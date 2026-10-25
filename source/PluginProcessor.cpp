// HOT-STEP LINK -- PluginProcessor.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#include "TakeStretch.h"
#include "PluginProcessor.h"
#include "FactoryPresets.h"
#include "PluginEditor.h"
#include "ExclusiveFile.h"
#include "DeletedFolder.h"
#include "WavTempoTag.h"

namespace
{
    const juce::Identifier kServerUrlId ("serverUrl");
    const juce::Identifier kDawKeyId ("dawKey");   // legacy: only read, for migration
    const juce::Identifier kKeySetId ("keySet");
}

static const juce::Identifier kStarredId ("Starred"), kStarId ("Take"), kStarNameId ("name");

// Background thread of the take store, and the only thread that works on the lasting
// root: cleanup and rescue of earlier sessions at start, then every lasting copy (the
// first one of a new take and the retries of copies that failed). Never the message,
// the audio or the worker thread.
struct HotStepLinkProcessor::TakeSaver : public juce::Thread
{
    explicit TakeSaver (HotStepLinkProcessor& p) : juce::Thread ("HSLINK take saver"), owner (p) {}
    void takeRecorded() { fresh = true; notify(); }
    void retryNow()     { retryRequested = true; notify(); }
    static double nowMs() { return juce::Time::getMillisecondCounterHiRes(); }
    void run() override
    {
        ExclusiveFile::ScopedThreadTag tag (ExclusiveFile::tagSaver);
        // The destructor may cancel a file call this thread has in flight inside
        // ExclusiveFile (see ~HotStepLinkProcessor). Detached before the thread ends.
        struct Attach
        {
            explicit Attach (ExclusiveFile::IoCanceller& c) : canceller (c) { canceller.attachCurrentThread(); }
            ~Attach() { canceller.detach(); }
            ExclusiveFile::IoCanceller& canceller;
        } attach (owner.saverIo);
        // Cleanup first, then rescue. An aged folder with an unsaved take is kept by the
        // cleanup (holdsUnsavedTake) and swept by the rescue right after.
        DawWorker::cleanupStaleTemp (DawWorker::tempRoot(), DawWorker::staleAgeMs(), juce::Time::getCurrentTime(),
                                     [this] { return owner.closingNow(); });
        if (DawWorker::unsavedGuard.load())
        {
            // Adoption first: every dead locked folder joins the take list, and the
            // rescue below skips it (it handles legacy folders without owner.lock).
            if (owner.adoptDeadFolders() > 0)
                owner.ensureStableCopies();
            // Counted after the copy pass: an observer of "pass done" sees the copies.
            ++owner.rescuePasses;
        }
        // Sleeps until the next retry is due or a notify arrives (takeRecorded,
        // retryNow, close). A notify that arrives while a pass runs is kept by the
        // thread's event, so the next wait returns at once.
        int interval = owner.retryBaseMs.load();
        double retryAt = nowMs() + interval;
        while (! threadShouldExit())
        {
            wait (juce::jmax (1, (int) (retryAt - nowMs())));
            if (threadShouldExit())
                break;
            const int seq = owner.recordSeq.load(); // read before the pass lists the takes
            pass (interval, retryAt);
            owner.savedSeq = seq;
            owner.saverIdle.signal();
        }
        owner.saverIdle.signal();
    }
    void pass (int& interval, double& retryAt)
    {
        owner.retrySessionLock(); // only an instance that runs lockless does anything here
        const bool asked = retryRequested.exchange (false);
        const bool isFresh = fresh.exchange (false);
        const bool due = nowMs() >= retryAt;
        if (! DawWorker::unsavedGuard.load())
        {
            // Teeth: no retries and no rescue. First copies are still made.
            if (isFresh)
                owner.ensureStableCopies (true);
            if (due)
                retryAt = nowMs() + interval;
            return;
        }
        if (asked)
        {
            owner.adoptDeadFolders();
        }
        if (asked || due)
        {
            // The retry pass: every take without a lasting copy.
            if (owner.countUnsaved() > 0)
            {
                const int left = owner.ensureStableCopies();
                ++owner.retryPasses;
                interval = left > 0 && ! asked ? juce::jmin (interval * 2, owner.retryMaxMs.load())
                                               : owner.retryBaseMs.load();
            }
            else
                interval = owner.retryBaseMs.load();
            retryAt = nowMs() + interval;
            if (asked)
                ++owner.rescuePasses; // the adoption and its copy pass are both done
        }
        else if (isFresh)
        {
            // A take was just recorded: its first copy, now. Takes whose copy already
            // failed wait for their retry time. A first copy that fails starts the
            // retry schedule again.
            int failures = 0;
            owner.ensureStableCopies (true, &failures);
            if (failures > 0)
            {
                interval = owner.retryBaseMs.load();
                retryAt = nowMs() + interval;
            }
        }
    }
    HotStepLinkProcessor& owner;
    std::atomic<bool> retryRequested { false }, fresh { false };
};

void HotStepLinkProcessor::wakeSaverForNewTake()
{
    ++recordSeq;
    if (saver != nullptr)
        saver->takeRecorded();
}

bool HotStepLinkProcessor::waitSaverIdle (int timeoutMs)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (savedSeq.load() < recordSeq.load())
    {
        if (closingNow() || saver == nullptr || ! saver->isThreadRunning()
            || juce::Time::getMillisecondCounterHiRes() > end)
            return false;
        saverIdle.wait (20);
    }
    return true;
}

HotStepLinkProcessor::HotStepLinkProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "HSLINK", createLayout())
{
    standalone = juce::JUCEApplicationBase::isStandaloneApp();
    apvts.state.setProperty (kServerUrlId, kDefaultServerUrl, nullptr);
#if defined (HSLINK_TEETH_KEY_IN_STATE)
    apvts.state.setProperty (kDawKeyId, "", nullptr);
#else
    dawKey = loadStoredKey();
    apvts.state.setProperty (kKeySetId, dawKey.isNotEmpty(), nullptr);
#endif
    // One lasting folder per instance: the time has one-second resolution, so two
    // instances made in the same second (a project load) differ by the token.
    sessionName = "session_" + juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
    // One temp folder per instance, for takes and captures. Its name always carries
    // the token, so two instances never share it. The folder is made here and its
    // owner.lock is held until the destructor has dealt with the folder: rescue and
    // cleanup of other instances and processes leave a locked folder alone. Busy =
    // someone already holds a folder of this name: another name is tried. A lock
    // file that cannot be made (disk full, access denied) never fails construction:
    // the instance runs without a lock and its folder follows the rules for folders
    // of earlier builds.
    // After four Busy names a fifth, fresh one is used without a lock (never a folder
    // someone else holds).
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        tempSessionName = sessionName + "_" + ExclusiveFile::uniqueToken (6);
        if (auto* nameFor = testSessionNameFor.load()) // test hook only
        {
            const auto forced = nameFor (attempt);
            if (forced.isNotEmpty())
                tempSessionName = forced;
        }
        const auto dir = DawWorker::tempRoot().getChildFile (tempSessionName);
        dir.createDirectory();
        if (attempt == 4 || ownLock.tryAcquire (dir) != ExclusiveFile::SessionLock::Result::Busy)
            break;
    }
    ownLockHeld = ownLock.isHeld();
    if (! ownLock.isHeld())
    {
        // Lockless: siblings in this process leave the folder alone (the registry), and
        // the saver thread tries the lock again at its wake-ups (retrySessionLock).
        DawWorker::setLocklessFolder (DawWorker::tempRoot().getChildFile (tempSessionName), true);
        static std::atomic<bool> logged { false };
        if (! logged.exchange (true))
            juce::Logger::writeToLog ("HOT-STEP LINK: the temp session folder "
                                      + DawWorker::tempRoot().getChildFile (tempSessionName).getFullPathName()
                                      + " could not be locked; this instance runs without a session lock");
    }
    if (! ExclusiveFile::testLegacyOverwrite.load())
        sessionName = tempSessionName;
    daw.setTakesFolder (DawWorker::tempRoot().getChildFile (tempSessionName));
    daw.setEndpoint (kDefaultServerUrl, dawKey);
    daw.onTakeDownloaded = [this] (const DawJob& j, int takeIndex) { registerTake (j, takeIndex); };
    daw.start();
    levelMatchParam = apvts.getRawParameterValue ("levelMatch");
    followHostParam = apvts.getRawParameterValue ("followHost");
    // Test / development hook, read once: HSLINK_COLLECTION_ROOT points the Collection
    // at another folder, so a test run never touches the real Documents folder.
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("HSLINK_COLLECTION_ROOT", {});
        if (env.isNotEmpty() && juce::File::isAbsolutePath (env))
            setCollectionRoot (juce::File (env));
    }
    // Test / development hook, read once: HSLINK_STABLE_ROOT points the lasting take
    // folder elsewhere before the saver thread starts.
    {
        const auto env = juce::SystemStats::getEnvironmentVariable ("HSLINK_STABLE_ROOT", {});
        if (env.isNotEmpty() && juce::File::isAbsolutePath (env))
            setStableRoot (juce::File (env));
    }
    // The age cleanup of earlier sessions runs on the saver thread, before its rescue pass.
    saver = std::make_unique<TakeSaver> (*this);
    if (! saver->startThread())
        juce::Logger::writeToLog ("HOT-STEP LINK: the saver thread could not be started; takes of this session will be saved at the next start");
}

HotStepLinkProcessor::~HotStepLinkProcessor()
{
    // This thread makes no file call on the lasting root: every call below is in the
    // temp session folder. The saver thread is the only thread that works on the
    // lasting root.
    ExclusiveFile::ScopedThreadTag tag (ExclusiveFile::tagClosing);
    // First: from here on the saver starts no lasting copy (retry pass, rescue) and a
    // copy that is running stops before its next file call. The saver and the worker
    // are both told to end before the wait for the collection pool, so the worker
    // does not start another download during that wait.
    const bool closeGuard = shutdownGuard.load();
    destructorBegan = true;
    // Tempo measurements: told to stop first (a running one ends at its next poll), and
    // the gate is closed, so no job stores a result from here on.
    measureGate->stop = true;
    {
        std::lock_guard<std::mutex> g (measureGate->lock);
        measureGate->closed = true;
    }
    if (closeGuard)
    {
        shuttingDown = true;
        saver->signalThreadShouldExit();
        saver->notify();
        if (signalWorkerEarly.load())
            daw.signalStop();
    }
    if (auto* beforePool = testBeforePoolWait.load()) // test hook only
        beforePool (*this);
    stretchPool.removeAllJobs (true, 20000);    // stopped above: ends within its next poll, lists nothing (gate closed)
    measurePool.removeAllJobs (true, 20000);    // stopped above: ends within its next poll
    collectionPool.removeAllJobs (true, 20000); // a background rescan never outlives the processor
    // Deleted takes: the moves already committed finish on their pool thread (a waiting
    // job is not dropped), then the delete that is still inside its undo window is
    // moved to its Deleted folder here (one plain rename, no shell call). That is the
    // one call this thread can make on the lasting root, and only when a take was
    // deleted in the last seconds before the close.
    // Nothing stays pending past this point; a move that fails puts the take back.
    waitTakeDeleteIdle (20000);
    takeDeletePool.removeAllJobs (true, 20000);
    {
        std::vector<PendingTakeDelete> ps;
        if (takePendingDelete (ps))
            for (const auto& p : ps)
                finishTakeDelete (p);
    }
    daw.stop(); // the worker does HTTP and temp-folder file calls only
    shuttingDown = true;
    saver->signalThreadShouldExit();
    saver->notify();
    // The saver's byte copy stops at its next chunk (it reads closingNow()). Any other
    // file call it has in flight inside ExclusiveFile is cancelled here, every 50 ms
    // until the thread has ended: one call cancels only what is in flight at that
    // instant. Nothing outside ExclusiveFile is ever cancelled (IoCanceller), and the
    // saver detaches before it ends, so no call is made on a thread that is gone.
    if (closeGuard)
        while (! saver->waitForThreadToExit (50))
            saverIo.cancelInFlight();
    // No time limit: JUCE would force-kill the thread inside a file call (and it could
    // die holding a CRT or OS lock). The saver thread ends on its own.
    saver->stopThread (-1);
    if (! closeGuard)
        ensureStableCopies(); // teeth: the old last pass on the calling thread
    const bool guard = DawWorker::unsavedGuard.load();
    for (const auto& dir : getOwnTempFolders())
    {
        // Keep or delete, from memory: the folder is kept while the take list has an
        // entry in it without a lasting copy (stable is set by the saver only after a
        // verified copy). A starred take is treated like any other. No file is
        // looked at for this and nothing is done under takesLock but reading the list.
        bool keep = false;
        if (testDtorProbesLasting.load())
            keep = legacyFolderReferenced (dir); // teeth only: the old probing
        else
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (const auto& t : takes)
                keep = keep || (t.stable == juce::File() && t.temp.isAChildOf (dir));
        }
        // Temp folder: its marker, when present, also keeps it (a marker that a failed
        // sync left behind, or one for a take the list does not hold).
        if (! keep && dir.isDirectory()
            && ! (guard && dir.getChildFile (DawWorker::unsavedMarkerName()).existsAsFile()))
        {
            if (dir == DawWorker::tempRoot().getChildFile (tempSessionName))
                removeLockedFolder (dir, ownLock); // empties it, lets go of owner.lock, removes it
            else
            {
                ExclusiveFile::SessionLock none; // not held: release() does nothing
                removeLockedFolder (dir, none);
            }
        }
    }
    // Only now: the folder is decided and, if it went, gone. A kept folder is free for
    // the next start's rescue from here on.
    ownLock.release();
    ownLockHeld = false;
    releaseLegacyClaims();
    adoptedLocks.clear(); // adopted folders with unsaved takes: free for the next start
    adoptedKeepWhole.clear();
    DawWorker::setLocklessFolder (DawWorker::tempRoot().getChildFile (tempSessionName), false);
}

bool HotStepLinkProcessor::retrySessionLock()
{
    // Saver thread only (ownLock is not touched by any other thread while it runs).
    if (ownLock.isHeld())
        return true;
    const auto dir = DawWorker::tempRoot().getChildFile (tempSessionName);
    if (! dir.isDirectory() || ownLock.tryAcquire (dir) != ExclusiveFile::SessionLock::Result::Held)
        return false;
    ownLockHeld = true;
    DawWorker::setLocklessFolder (dir, false);
    juce::Logger::writeToLog ("HOT-STEP LINK: the temp session folder " + dir.getFullPathName() + " is locked now");
    return true;
}

// Deletes a session folder whose lock the caller holds (or that has none). The lock
// file cannot be deleted while it is held, so everything else goes first, then the
// lock is released, then the lock file and the folder. A delete that fails leaves a
// folder that a later start removes.
// The "Deleted" folder of deleted unsaved takes (DeletedFolder.h) is inside the session
// folder and goes with it: an unsaved take is a temp file, and no session folder is
// left behind in the system temp folder for it.
bool HotStepLinkProcessor::removeLockedFolder (const juce::File& dir, ExclusiveFile::SessionLock& lock)
{
    for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        if (f.getFileName() != ExclusiveFile::SessionLock::fileName())
            f.deleteRecursively();
    lock.release();
    return dir.deleteRecursively();
}

// W2: the allowed-file set of an adopted dead folder. The sweep lists only top-level
// *.wav and *.wav.part, so anything it could not have looked at keeps the folder.
bool HotStepLinkProcessor::adoptedFolderSafeToRemove (const juce::File& dir)
{
    return adoptedFolderBlocker (dir).isEmpty();
}

std::atomic<bool> HotStepLinkProcessor::keepLog { true };

// R7: one log line per kept folder per session (this process), naming the first
// entry that blocked removal. Called by finishAdoptedFolders and by age cleanup.
bool HotStepLinkProcessor::keptFolderCheck (const juce::File& dir)
{
    const auto blocker = adoptedFolderBlocker (dir);
    if (blocker.isEmpty())
        return true;
    static std::mutex m; // not the audio path: saver and cleanup threads only
    static juce::StringArray logged;
    bool first = false;
    {
        std::lock_guard<std::mutex> g (m);
        if (! logged.contains (dir.getFullPathName()))
        {
            logged.add (dir.getFullPathName());
            first = true;
        }
    }
    if (first && keepLog.load())
        juce::Logger::writeToLog ("HOT-STEP LINK: kept folder " + dir.getFullPathName()
                                  + " (not proven saved: " + blocker + ")");
    return false;
}

// The first entry outside the allowed-file set, or empty when the folder is safe.
juce::String HotStepLinkProcessor::adoptedFolderBlocker (const juce::File& dir)
{
    const auto tab = juce::String::charToString ((juce::juce_wchar) 9);
    const auto log = DawWorker::readSavedLog (dir);
    for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
    {
        const auto name = f.getFileName();
        if (f.isDirectory())
        {
            if (name == DeletedFolder::folderName())
                continue; // the user's deleted takes go with the session folder (design)
            return name + "/"; // any other folder: unknown
        }
        if (DawWorker::isBookkeepingName (name)) // owner.lock, saved.log, the legacy marker
            continue;
        if (DawWorker::isCaptureName (name))
            continue; // a capture, never a take
        const auto size = f.getSize();
        if (size <= 0)
            continue; // a zero-byte file is not a take and never recoverable
        const auto rel = f.getRelativePathFrom (dir);
        const auto key = tab + juce::String (size);
        if (name.endsWithIgnoreCase (".wav") && log.contains (rel + key))
            continue; // a take saved.log proves saved
        if (name.endsWithIgnoreCase (".wav.part")
            && (log.contains (rel + key) || log.contains (rel.dropLastCharacters (5) + key)))
            continue; // a take .part proven under its own or its .wav name
        return name; // an unlisted take or an unknown file
    }
    return {};
}

juce::Array<juce::File> HotStepLinkProcessor::getOwnTempFolders() const
{
    // Only session_* folders directly under the temp root, never a folder set elsewhere.
    juce::Array<juce::File> out;
    const auto root = DawWorker::tempRoot();
    for (const auto& d : { daw.getTakesFolder(), root.getChildFile (tempSessionName) })
        if (d.getParentDirectory() == root && d.getFileName().startsWith ("session_"))
            out.addIfNotAlreadyThere (d);
    return out;
}

std::atomic<int> EditorFs::testCalls[EditorFs::tagCount] {};
std::atomic<bool> TakeGrid::testEditorStats { false };
std::atomic<bool> TakeGrid::testThumbFromLasting { false };
std::atomic<bool> TakeGrid::testNoTempFallback { false };
std::atomic<int> EditorFs::testThumbTemp { 0 };
std::atomic<int> EditorFs::testThumbElsewhere { 0 };

void EditorFs::countThumbSource (const juce::File& f)
{
    if (f.isAChildOf (DawWorker::tempRoot()))
        ++testThumbTemp;
    else
        ++testThumbElsewhere;
}

juce::File TakeGrid::thumbSourceOf (const Cell& c)
{
    return testThumbFromLasting.load() ? c.file : c.temp;
}

juce::File TakeGrid::actionFileOf (const Cell& c, EditorFs::Tag tag, bool* found)
{
    bool ok = EditorFs::exists (c.file, tag);
    auto use = c.file;
    if (! ok && ! testNoTempFallback.load() && c.saved && c.temp != juce::File() && c.temp != c.file
        && EditorFs::exists (c.temp, tag))
    {
        use = c.temp;
        ok = true;
    }
    if (found != nullptr)
        *found = ok;
    return use;
}
std::atomic<int> HotStepLinkProcessor::testRescueLastingProbes { 0 };
std::atomic<void (*) (HotStepLinkProcessor&)> HotStepLinkProcessor::testBeforeRescueTake { nullptr };
std::atomic<bool> HotStepLinkProcessor::testRescueProbeBeforeClosing { false };

juce::String TakeGrid::keyOf (const HslinkTakeEntry& e)
{
    return e.name.isNotEmpty() ? e.name : DawWorker::takeNameOf (e.temp);
}

std::vector<TakeGrid::Cell> TakeGrid::Model::shown() const
{
    std::vector<Cell> out;
    for (const auto& c : cells)
        if (c.shown)
            out.push_back (c);
    return out;
}

const TakeGrid::Cell* TakeGrid::Model::find (const juce::String& key) const
{
    for (const auto& c : cells)
        if (c.key == key)
            return &c;
    return nullptr;
}

// Entry state only: whether a take is saved comes from stable / copyFailed, never from
// a stat. A lasting file removed by hand after its copy still has its cell.
TakeGrid::Model TakeGrid::build (const std::vector<HslinkTakeEntry>& takes, int selectedJob, bool showUnsaved,
                                 const juce::File& selected, const std::function<bool (const juce::File&)>& isStarred)
{
    Model m;
    m.cells.reserve (takes.size());
    for (const auto& t : takes)
    {
        Cell c;
        c.key = keyOf (t);
        c.label = c.key.containsChar ('.') ? c.key.upToLastOccurrenceOf (".", false, false) : c.key;
        c.jobId = t.jobId;
        c.takeIndex = t.takeIndex;
        c.temp = t.temp;
        c.stable = t.stable;
        c.saved = t.stable != juce::File();
        c.file = c.saved ? t.stable : t.temp;
        c.badge = ! c.saved && t.copyFailed;
        c.pending = ! c.saved && ! t.copyFailed;
        c.dragAllowed = c.saved;
        c.shown = (selectedJob == 0 || t.jobId == selectedJob) && (c.saved || showUnsaved);
        if (c.shown && testEditorStats.load() && ! EditorFs::exists (c.file, EditorFs::tagTick))
            c.shown = false; // test only: the old filter
        c.selected = selected != juce::File() && (selected == t.temp || (c.saved && selected == t.stable));
        c.starred = isStarred != nullptr && isStarred (c.file);
        c.tempoState = t.tempoState;
        c.tempo = t.tempo;
        c.resolved = t.resolvedTempo;
        c.durationS = t.durationS;
        m.cells.push_back (c);
    }
    return m;
}

int TakeGrid::fitToBarsScore (double lengthSec, double takeBpm, double hostBpm, int beatsPerBar)
{
    if (! std::isfinite (lengthSec) || ! std::isfinite (hostBpm) || ! std::isfinite (takeBpm)
        || lengthSec <= 0.0 || hostBpm <= 0.0 || beatsPerBar < 1)
        return -1;
    const double beatSec = 60.0 / hostBpm;
    const double barSec = beatSec * (double) beatsPerBar;
    const double bars = juce::jmax (1.0, (double) juce::roundToInt (lengthSec / barSec));
    const double errBeats = std::abs (lengthSec - bars * barSec) / beatSec;
    double score = 1.0 - juce::jmin (1.0, errBeats / kFitBeatTolerance);
    if (takeBpm > 0.0)
    {
        const double rel = std::abs (takeBpm - hostBpm) / hostBpm;
        score *= 1.0 - juce::jmin (1.0, rel / kFitTempoTolerance);
    }
    return juce::jlimit (0, 100, juce::roundToInt (100.0 * score));
}

TakeGrid::TempoTag TakeGrid::fitTagOf (int score)
{
    TempoTag t;
    if (score < 0)
        return t;
    t.text = "fit " + juce::String (score);
    t.colour = score >= 80 ? TempoTag::green : score >= 50 ? TempoTag::amber : TempoTag::red;
    return t;
}

TakeGrid::TempoTag TakeGrid::tempoTagOf (int tempoState, const TempoMeasure::Result& tempo)
{
    TempoTag tag;
    if (tempoState == HslinkTakeEntry::tempoMeasuring)
    {
        tag.text = "bpm ...";
        tag.colour = TempoTag::pending;
    }
    else if (tempoState == HslinkTakeEntry::tempoMeasured)
    {
        const auto ms = juce::String (juce::roundToInt (tempo.driftMs)) + " ms";
        if (tempo.valid && tempo.bpm > 0.0 && tempo.level >= TempoMeasure::Level::medium)
        {
            // The relation to the asked tempo, when it is an octave of it: "x2", "/2".
            const int oct = juce::jlimit (-3, 3, tempo.hintOctave);
            const auto mark = oct > 0 ? "x" + juce::String (1 << oct) + "  "
                              : oct < 0 ? "/" + juce::String (1 << -oct) + "  " : juce::String();
            tag.text = juce::String (tempo.bpm, 2) + " bpm  " + mark + ms;
            tag.colour = tempo.level == TempoMeasure::Level::high ? TempoTag::green : TempoTag::amber;
        }
        else
        {
            // No number here: the detector has none it stands behind.
            const bool wanders = tempo.level == TempoMeasure::Level::low && tempo.driftMs > TempoMeasure::kWanderMs;
            tag.text = wanders ? "wanders " + ms : juce::String ("tempo ?");
            tag.colour = TempoTag::red;
        }
    }
    return tag;
}

TakeGrid::TempoTag TakeGrid::tempoTagOf (int tempoState, const TempoMeasure::Result& tempo, const TempoResolve::Resolved& resolved)
{
    if (tempoState == HslinkTakeEntry::tempoMeasured && resolved.isFallback())
    {
        TempoTag tag;
        tag.text = TempoResolve::fallbackText (resolved);
        tag.colour = TempoTag::amber; // not a measurement: usable, check it by ear
        return tag;
    }
    return tempoTagOf (tempoState, tempo);
}

std::atomic<bool> HotStepLinkProcessor::tempoMeasureEnabled { true };
std::atomic<HotStepLinkProcessor::MeasureReadHook> HotStepLinkProcessor::measureReadHook { nullptr };
std::atomic<HotStepLinkProcessor::MeasureStepHook> HotStepLinkProcessor::measureStepHook { nullptr };

// The file is read whole and at once, so its handle is held for the read only, then
// decoded from memory. Before the download is published the bytes are in <take>.part.
// May throw (a failed allocation, a decoder): only measureTakeFile calls it.
static bool measureTakeFileUnguarded (const juce::File& take, double hintBpm, TempoMeasure::Result& out,
                                      const std::function<bool()>& shouldAbort)
{
    auto stopNow = [&shouldAbort] { return shouldAbort != nullptr && shouldAbort(); };
    const auto src = take.existsAsFile() ? take : DawWorker::partFileFor (take);
    if (stopNow() || ! src.existsAsFile() || src.getSize() > (juce::int64) 1024 * 1024 * 1024)
        return false;
    if (const auto hook = HotStepLinkProcessor::measureReadHook.load())
        hook (src);
    juce::MemoryBlock bytes;
    if (! src.loadFileAsData (bytes) || bytes.getSize() == 0 || stopNow())
        return false;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (std::make_unique<juce::MemoryInputStream> (bytes, false)));
    // The header is not trusted: a rate, a channel count or a length no take has, or
    // a length the bytes read cannot hold, is refused before anything is allocated.
    if (rd == nullptr || ! (rd->sampleRate >= 8000.0) || ! (rd->sampleRate <= 768000.0)
        || rd->lengthInSamples <= 0 || rd->numChannels == 0 || rd->numChannels > 64
        || rd->lengthInSamples > (juce::int64) bytes.getSize() * 1024)
        return false;
    // The first 10 minutes are plenty for a tempo.
    const auto total = (juce::int64) juce::jmin ((double) rd->lengthInSamples, rd->sampleRate * 600.0);
    const int chans = rd->numChannels > 1 ? 2 : 1;
    std::vector<float> mono ((size_t) total);
    const int chunk = 1 << 16;
    juce::AudioBuffer<float> buf (chans, chunk);
    for (juce::int64 pos = 0; pos < total; pos += chunk)
    {
        if (stopNow())
            return false;
        const int n = (int) juce::jmin ((juce::int64) chunk, total - pos);
        if (! rd->read (&buf, 0, n, pos, true, true))
            return false;
        const float* l = buf.getReadPointer (0);
        const float* r = buf.getReadPointer (chans - 1);
        float* d = mono.data() + pos;
        for (int i = 0; i < n; ++i)
            d[i] = 0.5f * (l[i] + r[i]);
    }
    const double rate = rd->sampleRate;
    rd.reset();
    bytes.reset();
    const auto r = TempoMeasure::analyse (mono.data(), mono.size(), rate, hintBpm, shouldAbort);
    if (stopNow())
        return false; // an aborted analysis reads as "none": not a result
    out = r;
    return true;
}

// Nothing may leave this by exception: it runs on a pool thread, and an exception
// that leaves a pool thread ends the host.
bool HotStepLinkProcessor::measureTakeFile (const juce::File& take, double hintBpm, TempoMeasure::Result& out,
                                            const std::function<bool()>& shouldAbort)
{
    try
    {
        return measureTakeFileUnguarded (take, hintBpm, out, shouldAbort);
    }
    catch (const std::exception&)
    {
    }
    catch (...)
    {
    }
    return false;
}

bool HotStepLinkProcessor::measureTakeForCell (const juce::File& take, double hintBpm, TempoMeasure::Result& out,
                                               const std::function<bool()>& shouldAbort)
{
    bool stopped = false;
    try
    {
        TempoMeasure::Result r;
        if (measureTakeFile (take, hintBpm, r, shouldAbort))
        {
            out = r;
            return true;
        }
        stopped = shouldAbort != nullptr && shouldAbort();
    }
    catch (const std::exception&)
    {
    }
    catch (...)
    {
    }
    out = TempoMeasure::Result();
    return ! stopped;
}

void HotStepLinkProcessor::requestTakeMeasure (const juce::File& tempFile, double hintBpm)
{
    if (! tempoMeasureEnabled.load() || closingNow() || measureGate->stop.load())
        return;
    {
        std::lock_guard<std::mutex> g (takesLock);
        bool found = false;
        for (auto& t : takes)
            if (t.temp == tempFile)
            {
                t.tempoState = TakeEntry::tempoMeasuring;
                found = true;
            }
        if (! found)
            return;
    }
    // The job reaches the processor only through storeTakeTempo, and only while the
    // gate is open (see MeasureGate). The measurement itself touches no processor.
    // Nothing may leave the job by exception (it would end the host), and nothing may
    // leave this function by exception either (the download worker calls it): a throw
    // at any step ends, where it still can, with the take stored as measured with
    // level none ("tempo ?"), never left at measuring.
    try
    {
        if (const auto hook = measureStepHook.load())
            hook (0);
        measurePool.addJob ([this, gate = measureGate, tempFile, hintBpm]
        {
            auto storeNone = [this, &gate, &tempFile]() noexcept
            {
                try
                {
                    std::lock_guard<std::mutex> g (gate->lock);
                    if (! gate->closed)
                        storeTakeTempo (tempFile, true, TempoMeasure::Result());
                }
                catch (...)
                {
                }
            };
            try
            {
                TempoMeasure::Result r;
                const bool measured = ! gate->stop.load()
                                      && measureTakeForCell (tempFile, hintBpm, r, [gate] { return gate->stop.load(); });
                std::lock_guard<std::mutex> g (gate->lock);
                if (! gate->closed)
                {
                    if (const auto hook = measureStepHook.load())
                        hook (1);
                    storeTakeTempo (tempFile, measured, r);
                }
            }
            catch (const std::exception&)
            {
                storeNone();
            }
            catch (...)
            {
                storeNone();
            }
        });
    }
    catch (...)
    {
        // Not queued (addJob allocates): no job will ever store a result for this take.
        // The caller is a member running on a live processor, so no gate is needed here.
        try
        {
            storeTakeTempo (tempFile, true, TempoMeasure::Result());
        }
        catch (...)
        {
        }
    }
}

juce::String HotStepLinkProcessor::stretchedTakeName (const juce::String& name, double targetBpm)
{
    const auto stem = name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
    return stem + "_st" + juce::String (std::round (targetBpm * 100.0) / 100.0) + "bpm.wav";
}

bool HotStepLinkProcessor::requestTakeStretch (const juce::File& tempFile, double fromBpm, double toBpm)
{
    if (closingNow() || measureGate->stop.load())
        return false;
    try
    {
        TakeEntry src;
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (const auto& t : takes)
                if (t.temp == tempFile && ! t.handingOver)
                    src = t;
        }
        if (src.temp == juce::File())
            return false; // not listed, or its file is not final yet
        // The stretch itself touches no processor (own reader, own buffers). The job
        // reaches the processor only to list the new take, under the gate and only
        // while it is open. Nothing may leave the job by exception.
        stretchPool.addJob ([this, gate = measureGate, src, fromBpm, toBpm]
        {
            try
            {
                TakeStretch::StretchRequest rq;
                rq.source = src.temp;
                rq.dest = src.temp.getSiblingFile (stretchedTakeName (src.name, toBpm));
                rq.sourceBpm = fromBpm;
                rq.targetBpm = toBpm;
                const auto res = TakeStretch::stretchTakeFile (rq, [gate] { return gate->stop.load(); });
                if (! res.ok)
                {
                    juce::Logger::writeToLog ("HOT-STEP LINK: stretch of " + src.name + " not done: " + res.error);
                    return;
                }
                std::lock_guard<std::mutex> g (gate->lock);
                if (gate->closed)
                    return; // closing: the file stays in the temp session folder, unlisted
                TakeEntry e = src;
                e.temp = rq.dest;
                e.name = rq.dest.getFileName();
                e.stable = juce::File();
                e.copyFailed = e.handingOver = false;
                e.bpm = toBpm;
                e.durationS = (double) res.outSamples / res.sampleRate;
                e.createdMs = juce::Time::currentTimeMillis();
                e.tempoState = TakeEntry::tempoNotMeasured;
                e.tempo = TempoMeasure::Result();
                e.resolvedTempo = TempoResolve::Resolved();
                {
                    std::lock_guard<std::mutex> tl (takesLock);
                    for (const auto& t : takes)
                        if (t.temp == e.temp || t.name == e.name)
                            return;
                    takes.push_back (e);
                    trimTakesLocked();
                }
                requestTakeMeasure (e.temp, e.bpm);
                wakeSaverForNewTake();
            }
            catch (...)
            {
            }
        });
        return true;
    }
    catch (...)
    {
    }
    return false;
}

bool HotStepLinkProcessor::waitTakeStretchIdle (int timeoutMs)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (stretchPool.getNumJobs() > 0)
    {
        if (juce::Time::getMillisecondCounterHiRes() > end)
            return false;
        juce::Thread::sleep (5);
    }
    return true;
}

juce::String HotStepLinkProcessor::editedTakeName (const juce::String& name, const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options)
{
    const auto stem = name.containsChar ('.') ? name.upToLastOccurrenceOf (".", false, false) : name;
    juce::String what;
    what << edit.cropStart << ' ' << edit.cropEnd << ' ' << edit.loopStart << ' ' << edit.loopEnd << ' ' << options.loopRepeats << ' '
         << (int) options.snapToGrid << (int) options.snapToZeroCrossing << ' ' << juce::String (options.fadeMs, 3) << ' '
         << juce::String (options.zeroCrossingMs, 3);
    for (const auto& m : options.warp)
        what << ' ' << (juce::int64) m.srcFrame << ':' << (juce::int64) m.dstFrame;
    if (! options.warp.empty())
        what << ' ' << juce::String (options.warpBpm, 3);
    const auto hash = juce::String::toHexString ((int) (what.hashCode64() & 0xffff)).paddedLeft ('0', 4);
    return stem + (options.loopRepeats >= 1 ? "_edl" + juce::String (options.loopRepeats) + "x"
                                             : juce::String (options.warp.empty() ? "_edc" : "_edw")) + hash + ".wav";
}

bool HotStepLinkProcessor::setTakeEdit (const juce::File& tempFile, const TakeEdit::EditData& edit)
{
    try
    {
        const auto clean = edit.sanitized();
        bool found = false;
        std::lock_guard<std::mutex> g (takesLock);
        for (auto& t : takes)
            if (t.temp == tempFile)
            {
                t.edit = clean;
                found = true;
            }
        return found;
    }
    catch (...)
    {
    }
    return false;
}

std::atomic<bool> HotStepLinkProcessor::editReportsByTicket { true };

HotStepLinkProcessor::TakeEditReport HotStepLinkProcessor::getLastTakeEditReport() const
{
    std::lock_guard<std::mutex> g (takesLock);
    return takeEditReports.empty() ? TakeEditReport() : takeEditReports.back();
}

bool HotStepLinkProcessor::getTakeEditReport (juce::uint64 ticket, TakeEditReport& out) const
{
    std::lock_guard<std::mutex> g (takesLock);
    if (! editReportsByTicket.load())
    {
        if (takeEditReports.empty())
            return false;
        out = takeEditReports.back(); // teeth: the old single slot
        return true;
    }
    if (ticket == 0)
        return false;
    for (const auto& r : takeEditReports)
        if (r.ticket == ticket)
        {
            out = r;
            return true;
        }
    return false;
}

std::atomic<juce::uint64> HotStepLinkProcessor::testHoldEditFromTicket { 0 };

HotStepLinkProcessor::TakeEditState HotStepLinkProcessor::getTakeEditState (juce::uint64 ticket) const
{
    if (ticket == 0)
        return TakeEditState::unknown;
    const bool closing = closingNow() || measureGate->stop.load();
    std::lock_guard<std::mutex> g (takesLock);
    for (const auto& r : takeEditReports)
        if (r.ticket == ticket)
            return TakeEditState::done;
    if (closing)
        return TakeEditState::unknown; // a job that has not reported by now lists nothing
    const auto it = takeEditJobs.find (ticket);
    if (it == takeEditJobs.end())
        return TakeEditState::unknown;
    return it->second ? TakeEditState::running : TakeEditState::queued;
}

void HotStepLinkProcessor::storeTakeEditReportLocked (TakeEditReport report)
{
    report.serial = ++takeEditSerial;
    takeEditReports.push_back (std::move (report));
    const size_t keep = editReportsByTicket.load() ? (size_t) kTakeEditReportsKept : (size_t) 1;
    while (takeEditReports.size() > keep)
        takeEditReports.pop_front();
}

void HotStepLinkProcessor::setTakeNameForTest (const juce::File& tempFile, const juce::String& name)
{
    std::lock_guard<std::mutex> g (takesLock);
    for (auto& t : takes)
        if (t.temp == tempFile)
            t.name = name;
}

juce::uint64 HotStepLinkProcessor::requestTakeEdit (const juce::File& tempFile, const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options,
                                                    juce::String* whyNot)
{
    auto refuse = [whyNot] (const char* why)
    {
        if (whyNot != nullptr)
            *whyNot = why;
        return (juce::uint64) 0;
    };
    if (closingNow() || measureGate->stop.load())
        return refuse ("Could not start: the plugin is closing.");
    juce::uint64 ticket = 0;
    try
    {
        TakeEntry src;
        {
            const auto clean = edit.sanitized();
            std::lock_guard<std::mutex> g (takesLock);
            for (auto& t : takes)
                if (t.temp == tempFile && ! t.handingOver)
                {
                    t.edit = clean;
                    src = t;
                }
            if (src.temp != juce::File())
            {
                ticket = nextEditTicket++;
                takeEditJobs[ticket] = false; // queued
            }
        }
        if (ticket == 0)
            return refuse ("Could not start: the take is no longer listed or not final.");
        // An entry without a name is known by its file name, as in the take grid (TakeGrid::keyOf).
        if (src.name.isEmpty() && editReportsByTicket.load())
            src.name = DawWorker::takeNameOf (src.temp);
        // The grid is the take's own measurement, never the caller's numbers.
        TakeEdit::EditRequest rq;
        rq.source = src.temp;
        rq.dest = src.temp.getSiblingFile (editedTakeName (src.name, src.edit, options));
        rq.edit = src.edit;
        rq.options = options;
        const bool haveGrid = src.tempoState == TakeEntry::tempoMeasured && src.tempo.valid
                              && TakeEdit::gridUsable (src.tempo.bpm, src.tempo.downbeatSec);
        const bool gridSkipped = options.snapToGrid && ! haveGrid;
        rq.options.snapToGrid = options.snapToGrid && haveGrid;
        rq.options.gridBpm = haveGrid ? src.tempo.bpm : 0.0;
        rq.options.gridDownbeatSec = haveGrid ? src.tempo.downbeatSec : 0.0;
        // The render itself touches no processor (own reader, own buffers). The job
        // reaches the processor only to store its report and list the new take, under
        // the gate and only while it is open. Nothing may leave the job by exception.
        // The state of the ticket (getTakeEditState) follows the job: running when the
        // pool thread takes it up, gone when it ends. As everything the job does to the
        // processor: under the gate and only while it is open.
        auto markJob = [this, gate = measureGate, ticket] (bool running)
        {
            try
            {
                std::lock_guard<std::mutex> g (gate->lock);
                if (gate->closed)
                    return;
                std::lock_guard<std::mutex> tl (takesLock);
                if (! running)
                    takeEditJobs.erase (ticket);
                else if (const auto it = takeEditJobs.find (ticket); it != takeEditJobs.end())
                    it->second = true;
            }
            catch (...)
            {
            }
        };
        stretchPool.addJob ([this, gate = measureGate, src, rq, gridSkipped, ticket, markJob]
        {
            struct Ended
            {
                std::function<void (bool)> mark;
                ~Ended() { mark (false); } // last, after the gate lock of the body is let go
            } ended { markJob };
            markJob (true);
            while (testHoldEditFromTicket.load() != 0 && ticket >= testHoldEditFromTicket.load() && ! gate->stop.load())
                juce::Thread::sleep (2); // tests only
            bool reported = false;
            try
            {
                if (gridSkipped)
                    juce::Logger::writeToLog ("HOT-STEP LINK: edit of " + src.name + ": no valid measured tempo, snap to grid skipped");
                const auto res = TakeEdit::renderEditFile (rq, [gate] { return gate->stop.load(); });
                if (! res.ok)
                    juce::Logger::writeToLog ("HOT-STEP LINK: edit of " + src.name + " not done: " + res.error);
                std::lock_guard<std::mutex> g (gate->lock);
                if (gate->closed)
                    return; // closing: a finished file stays in the temp session folder, unlisted
                bool listed = false;
                TakeEntry e = src;
                if (res.ok)
                {
                    e.temp = rq.dest;
                    e.name = rq.dest.getFileName();
                    e.stable = juce::File();
                    e.copyFailed = e.handingOver = false;
                    e.durationS = (double) res.outSamples / res.sampleRate;
                    e.createdMs = juce::Time::currentTimeMillis();
                    e.tempoState = TakeEntry::tempoNotMeasured;
                    e.tempo = TempoMeasure::Result();
                    e.resolvedTempo = TempoResolve::Resolved();
                    e.edit = TakeEdit::EditData(); // the points were positions in the source file
                }
                {
                    std::lock_guard<std::mutex> tl (takesLock);
                    if (res.ok)
                    {
                        listed = true;
                        for (const auto& t : takes)
                            if (t.temp == e.temp || t.name == e.name)
                                listed = false;
                        if (listed)
                        {
                            takes.push_back (e);
                            trimTakesLocked();
                        }
                    }
                    TakeEditReport rep;
                    rep.ticket = ticket;
                    rep.ok = res.ok;
                    rep.gridSkipped = gridSkipped;
                    rep.sourceName = src.name;
                    rep.newName = listed ? e.name : juce::String();
                    rep.result = res;
                    storeTakeEditReportLocked (std::move (rep));
                    reported = true;
                }
                if (! listed)
                    return;
                requestTakeMeasure (e.temp, e.bpm);
                wakeSaverForNewTake();
            }
            catch (...)
            {
                // A job that broke before its report still answers its ticket, so nobody
                // waits for it. Same rule as above: only while the gate is open.
                try
                {
                    std::lock_guard<std::mutex> g (gate->lock);
                    if (! reported && ! gate->closed)
                    {
                        TakeEditReport rep;
                        rep.ticket = ticket;
                        rep.gridSkipped = gridSkipped;
                        rep.sourceName = src.name;
                        rep.result.error = "The edit failed.";
                        std::lock_guard<std::mutex> tl (takesLock);
                        storeTakeEditReportLocked (std::move (rep));
                    }
                }
                catch (...)
                {
                }
            }
        });
        return ticket;
    }
    catch (...)
    {
    }
    try
    {
        std::lock_guard<std::mutex> g (takesLock);
        takeEditJobs.erase (ticket); // never queued: nobody may wait for it
    }
    catch (...)
    {
    }
    return refuse ("Could not start the edit.");
}

bool HotStepLinkProcessor::waitTakeEditIdle (int timeoutMs)
{
    return waitTakeStretchIdle (timeoutMs);
}

// Not measured (aborted) puts the take back to "not measured".
void HotStepLinkProcessor::storeTakeTempo (const juce::File& tempFile, bool measured, const TempoMeasure::Result& r)
{
    // The host tempo of this moment, from the snapshot the audio thread already keeps
    // (three atomic reads on this thread, never the audio thread). No host, or a host
    // that gives a position but no tempo: 0 = absent.
    const double hostBpm = hostSync.tempoForCascade();
    auto apply = [&] (TakeEntry& t)
    {
        t.tempoState = measured ? TakeEntry::tempoMeasured : TakeEntry::tempoNotMeasured;
        t.tempo = measured ? r : TempoMeasure::Result();
        // Resolved once, here: measured, else the file name, else the host. Kept with the
        // take; it does not follow the host afterwards.
        t.resolvedTempo = measured ? TempoResolve::resolve (r, t.name.isEmpty() ? t.temp.getFileName() : t.name, hostBpm)
                                   : TempoResolve::Resolved();
    };
    std::lock_guard<std::mutex> g (takesLock);
    for (auto& t : takes)
        if (t.temp == tempFile)
            apply (t);
    // A take inside its delete undo window is out of the list; Undo brings it back.
    if (pendingDelete.active && pendingDelete.entry.temp == tempFile)
        apply (pendingDelete.entry);
    for (auto& r : pendingRest)
        if (r.entry.temp == tempFile)
            apply (r.entry);
}

bool HotStepLinkProcessor::waitTakeMeasureIdle (int timeoutMs)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (measurePool.getNumJobs() > 0)
    {
        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) juce::jmax (0, timeoutMs))
            return false;
        juce::Thread::sleep (5);
    }
    return true;
}

std::atomic<bool> HotStepLinkProcessor::testDtorProbesLasting { false };
std::atomic<int> HotStepLinkProcessor::testDtorLastingProbes { 0 };
std::atomic<bool> HotStepLinkProcessor::signalWorkerEarly { true };
std::atomic<bool> HotStepLinkProcessor::testWorkerCopies { false };
std::atomic<bool> HotStepLinkProcessor::handOverGuard { true };
std::atomic<void (*) (HotStepLinkProcessor&)> HotStepLinkProcessor::testBeforePoolWait { nullptr };
std::atomic<juce::String (*) (int)> HotStepLinkProcessor::testSessionNameFor { nullptr };

// TEST ONLY (testDtorProbesLasting): the keep-or-delete decision as it was before the
// destructor decided from memory. It stats files under takesLock, one probe of the
// lasting root per starred take. The shipped destructor never calls it.
bool HotStepLinkProcessor::legacyFolderReferenced (const juce::File& dir) const
{
    std::lock_guard<std::mutex> g (takesLock);
    // A listed take whose lasting copy never succeeded exists only here: keep its folder.
    for (const auto& t : takes)
        if (t.stable == juce::File() && t.temp.existsAsFile() && t.temp.isAChildOf (dir))
            return true;
    // A starred take with no stable copy yet is still referenced: keep its folder.
    for (const auto& f : dir.findChildFiles (juce::File::findFiles, true, "*.wav"))
        if (isStarred (f))
        {
            auto it = stableByTemp.find (f.getFullPathName());
            if (it == stableByTemp.end())
                return true;
            ++testDtorLastingProbes;
            // The probe is a lasting-root step of the closing thread: counted and
            // passed to the step hook like every other one (a test slows it there).
            ++ExclusiveFile::testStepsByTag[ExclusiveFile::tagClosing];
            if (auto* hook = ExclusiveFile::testStepHook.load())
                hook ("close probe");
            if (! it->second.existsAsFile())
                return true;
        }
    return false;
}

juce::AudioProcessorValueTreeState::ParameterLayout HotStepLinkProcessor::createLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "backend", 1 }, "Backend",
        juce::StringArray { "Auto", "SA3", "YuE2", "ACE" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "aceModel", 1 }, "ACE model",
        juce::StringArray { "Auto", "Turbo", "Base" }, 0));
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "duration", 1 }, "Duration",
        juce::NormalisableRange<float> (5.0f, 240.0f, 1.0f), 30.0f));
    p.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "takes", 1 }, "Takes", 1, 8, 4));
    // VARIATION: the seed spread between repeat generates (SeedSpread::spread). 0 = the same seed.
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "variation", 1 }, "Variation",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f));
    // GUIDANCE: -0.5 = Auto (no field sent), else 0..20. STEPS: 0 = Auto, else 1..300.
    // Both default to Auto, so sessions saved before v0.5.14 load as Auto.
    p.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "guidance", 1 }, "Guidance",
        juce::NormalisableRange<float> (-0.5f, 20.0f, 0.5f), -0.5f,
        juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return v < 0.0f ? juce::String ("Auto") : juce::String (v, 1); })
            // "Auto" must parse back to the sentinel, or text -> value lands on 0.0 (clap-validator).
            .withValueFromStringFunction ([] (const juce::String& t)
            {
                const auto s = t.trim();
                return (s.isEmpty() || s.equalsIgnoreCase ("Auto")) ? -0.5f : s.getFloatValue();
            })));
    p.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "steps", 1 }, "Steps", 0, 300, 0,
        juce::AudioParameterIntAttributes().withStringFromValueFunction (
            [] (int v, int) { return v <= 0 ? juce::String ("Auto") : juce::String (v); })
            .withValueFromStringFunction ([] (const juce::String& t)
            {
                const auto s = t.trim();
                return s.equalsIgnoreCase ("Auto") ? 0 : s.getIntValue();
            })));
    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "levelMatch", 1 }, "Level match", true));
    p.push_back (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "followHost", 1 }, "Follow host transport", false));
    return { p.begin(), p.end() };
}

void HotStepLinkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    if (! capture.isCapturing())
        capture.prepare (sampleRate, juce::jmax (1, getTotalNumInputChannels()));
    audition.prepare (sampleRate, samplesPerBlock);
    editorAudition.prepare (sampleRate);
}

//==============================================================================
void CaptureEngine::prepare (double hostRate, int numChannels, double ringSeconds)
{
    jassert (! isCapturing());
    rate = hostRate > 0.0 ? hostRate : 48000.0;
    channels = juce::jlimit (1, 2, numChannels);
    const int cap = juce::jmax (8192, (int) (rate * ringSeconds));
    ring.setSize (channels, cap, false, true, false);
    fifo = std::make_unique<juce::AbstractFifo> (cap);
}

void CaptureEngine::push (const juce::AudioBuffer<float>& buf, int startSample, int numSamples)
{
    if (numSamples <= 0)
        return;
    if (! capturing.load (std::memory_order_acquire) || fifo == nullptr)
        return;
    const int n = juce::jmin (numSamples, fifo->getFreeSpace());
    if (n < numSamples)
        dropped.fetch_add (numSamples - n, std::memory_order_relaxed);
    if (n <= 0)
        return;
    int s1, n1, s2, n2;
    fifo->prepareToWrite (n, s1, n1, s2, n2);
    for (int ch = 0; ch < channels; ++ch)
    {
        const int src = juce::jmin (ch, buf.getNumChannels() - 1);
        if (src < 0)
            continue;
        if (n1 > 0) ring.copyFrom (ch, s1, buf, src, startSample, n1);
        if (n2 > 0) ring.copyFrom (ch, s2, buf, src, startSample + n1, n2);
    }
    fifo->finishedWrite (n1 + n2);
}

bool CaptureEngine::startCapture (const juce::File& wavOut)
{
    if (isCapturing() || fifo == nullptr)
        return false;
    fifo->reset();
    dropped.store (0);
    outFile = wavOut;
    take.assign ((size_t) channels, {});
    for (auto& t : take)
        t.reserve ((size_t) (rate * 60.0));
    capturing.store (true, std::memory_order_release);
    startThread();
    return true;
}

bool CaptureEngine::stopCapture()
{
    if (! isCapturing())
        return false;
    capturing.store (false, std::memory_order_release);
    signalThreadShouldExit();
    stopThread (10000);
    drain();
    return writeWav();
}

void CaptureEngine::run()
{
    while (! threadShouldExit())
    {
        drain();
        wait (20);
    }
}

void CaptureEngine::drain()
{
    int s1, n1, s2, n2;
    fifo->prepareToRead (fifo->getNumReady(), s1, n1, s2, n2);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* r = ring.getReadPointer (ch);
        take[(size_t) ch].insert (take[(size_t) ch].end(), r + s1, r + s1 + n1);
        take[(size_t) ch].insert (take[(size_t) ch].end(), r + s2, r + s2 + n2);
    }
    fifo->finishedRead (n1 + n2);
}

bool CaptureEngine::writeWav()
{
    const int inLen = take.empty() ? 0 : (int) take[0].size();
    if (inLen <= 0)
        return false;
#if defined (HSLINK_TEETH_NO_RESAMPLE)
    const int outLen = inLen;
    juce::AudioBuffer<float> out (channels, outLen);
    for (int ch = 0; ch < channels; ++ch)
        out.copyFrom (ch, 0, take[(size_t) ch].data(), outLen);
#else
    const double ratio = rate / kOutRate;
    const int outLen = (int) std::llround ((double) inLen / ratio);
    juce::AudioBuffer<float> out (channels, outLen);
    for (int ch = 0; ch < channels; ++ch)
    {
        // Pad so the interpolator never reads past the take.
        auto& src = take[(size_t) ch];
        src.resize ((size_t) inLen + 8, 0.0f);
        juce::LagrangeInterpolator li;
        li.process (ratio, src.data(), out.getWritePointer (ch), outLen);
    }
#endif
    outFile.deleteFile();
    std::unique_ptr<juce::OutputStream> os (outFile.createOutputStream());
    if (os == nullptr)
        return false;
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                          .withSampleRate (kOutRate)
                                          .withNumChannels (channels)
                                          .withBitsPerSample (32)
                                          .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (w == nullptr)
        return false;
    const bool ok = w->writeFromAudioSampleBuffer (out, 0, outLen);
    take.clear();
    return ok;
}

bool HotStepLinkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void HotStepLinkProcessor::updateHostSync (HostSyncState& s, juce::AudioPlayHead* ph, bool isStandalone)
{
    if (isStandalone || ph == nullptr)
    {
        s.hostSyncActive.store (false, std::memory_order_relaxed);
        s.isPlaying.store (false, std::memory_order_relaxed);
        return;
    }
    const auto pos = ph->getPosition();
    if (! pos.hasValue())
    {
        s.hostSyncActive.store (false, std::memory_order_relaxed);
        return;
    }
    const auto bpm = pos->getBpm();
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    if (bpm.hasValue()) // teeth: before the guard, any value the host hands over
        s.bpm.store (*bpm, std::memory_order_relaxed);
    s.bpmKnown.store (bpm.hasValue(), std::memory_order_relaxed);
#else
    // A tempo of 0, below 0, NaN or infinite is no tempo: not known, and the stored
    // value keeps what it had.
    const bool bpmOk = bpm.hasValue() && HostSyncState::hostBpmUsable (*bpm);
    if (bpmOk)
        s.bpm.store (*bpm, std::memory_order_relaxed);
    s.bpmKnown.store (bpmOk, std::memory_order_relaxed);
#endif
    if (auto ts = pos->getTimeSignature())
    {
        s.timeSigNum.store (ts->numerator, std::memory_order_relaxed);
#if defined (HSLINK_TEETH_BAD_SYNC)
        s.timeSigDen.store (ts->numerator, std::memory_order_relaxed);
#else
        s.timeSigDen.store (ts->denominator, std::memory_order_relaxed);
#endif
    }
    if (auto ppq = pos->getPpqPosition())
        s.ppqPosition.store (*ppq, std::memory_order_relaxed);
    s.isPlaying.store (pos->getIsPlaying(), std::memory_order_relaxed);
    s.isLooping.store (pos->getIsLooping(), std::memory_order_relaxed);
    if (auto loop = pos->getLoopPoints())
    {
        s.loopStartPpq.store (loop->ppqStart, std::memory_order_relaxed);
        s.loopEndPpq.store (loop->ppqEnd, std::memory_order_relaxed);
    }
    s.hostSyncActive.store (true, std::memory_order_relaxed);
    s.updateCounter.fetch_add (1, std::memory_order_release);
}

void HotStepLinkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());
    updateHostSync (hostSync, getPlayHead(), standalone);
    const int n = buffer.getNumSamples();
    {
        float pk = 0.0f;
        for (int ch = 0; ch < juce::jmin (getTotalNumInputChannels(), buffer.getNumChannels()); ++ch)
            pk = juce::jmax (pk, buffer.getMagnitude (ch, 0, n));
        float prev = inputPeak.load (std::memory_order_relaxed);
        while (pk > prev && ! inputPeak.compare_exchange_weak (prev, pk, std::memory_order_relaxed)) {}
    }
    const bool capturingNow = capture.isCapturing();
    const int phase = capPhase.load (std::memory_order_acquire);
    if (phase == capArmed || phase == capRecording)
    {
        // Armed source capture, gated by the host transport and loop.
        const double srNow = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
        const bool hostPlaying = ! standalone && hostSync.hostSyncActive.load (std::memory_order_relaxed)
                              && hostSync.isPlaying.load (std::memory_order_relaxed);
        const double ppq = hostSync.ppqPosition.load (std::memory_order_relaxed);
        const double bpmNow = juce::jmax (1.0, hostSync.bpm.load (std::memory_order_relaxed));
        bool recording = phase == capRecording;
        if (! recording && hostPlaying)
        {
            const bool looping = hostSync.isLooping.load (std::memory_order_relaxed);
            const double ls = hostSync.loopStartPpq.load (std::memory_order_relaxed);
            const double le = hostSync.loopEndPpq.load (std::memory_order_relaxed);
            if (! looping || le <= ls || (ppq >= ls && ppq < le))
            {
                captureStartPpq.store (ppq, std::memory_order_relaxed);
                haveCaptureStart.store (true, std::memory_order_release);
                capEndPpq.store (looping && le > ls ? le : -1.0, std::memory_order_relaxed);
                capLastPpq = ppq;
                recording = true;
                capPhase.store (capRecording, std::memory_order_release);
            }
        }
        if (recording)
        {
            bool stop = ! hostPlaying || ppq < capLastPpq - 1.0e-9; // stop or loop wrap
            int count = stop ? 0 : n;
            const double endPpq = capEndPpq.load (std::memory_order_relaxed);
#if ! defined (HSLINK_TEETH_NO_LOOP_END)
            if (! stop && endPpq > 0.0)
            {
                const double ppqPerSample = bpmNow / 60.0 / srNow;
                const auto remaining = (juce::int64) std::llround ((endPpq - ppq) / ppqPerSample);
                if (remaining <= (juce::int64) n)
                {
                    count = (int) juce::jmax ((juce::int64) 0, remaining);
                    stop = true;
                }
            }
#else
            juce::ignoreUnused (endPpq, bpmNow, srNow);
#endif
            capture.push (buffer, 0, count);
            capLastPpq = ppq;
            if (stop)
                capPhase.store (capFinished, std::memory_order_release);
        }
        wasCapturing = capturingNow;
    }
    else
    {
        if (capturingNow && ! wasCapturing && phase == capIdle)
        {
            captureStartPpq.store (hostSync.ppqPosition.load (std::memory_order_relaxed), std::memory_order_relaxed);
            haveCaptureStart.store (true, std::memory_order_release);
        }
        wasCapturing = capturingNow;
        if (phase == capIdle)
            capture.push (buffer, n);
    }

    // R5 audition: take position follows the host (or the standalone clock).
    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    bool playing = false;
    juce::int64 pos = 0;
    // L1: in host mode the preview runs on its own clock unless "Follow host transport" is on.
    const bool followHost = followHostParam != nullptr
                         && followHostParam->load (std::memory_order_relaxed) >= 0.5f;
    const bool hostClock  = ! standalone && followHost
                         && hostSync.hostSyncActive.load (std::memory_order_relaxed);
    if (! hostClock)
    {
        playing = previewPlay.load (std::memory_order_acquire);
        if (previewRestart.exchange (false, std::memory_order_acq_rel))
            previewClock = 0;
        if (playing && ! lastPreviewPlay)
            previewClock = 0;
        lastPreviewPlay = playing;
        pos = previewClock;
        if (playing)
            previewClock += n;
    }
    else
    {
        playing = hostSync.hostSyncActive.load (std::memory_order_relaxed)
               && hostSync.isPlaying.load (std::memory_order_relaxed);
        double anchor = 0.0;
        if (hostSync.isLooping.load (std::memory_order_relaxed))
            anchor = hostSync.loopStartPpq.load (std::memory_order_relaxed);
        else if (haveCaptureStart.load (std::memory_order_acquire))
            anchor = captureStartPpq.load (std::memory_order_relaxed);
        pos = AuditionMath::takeSamplePos (hostSync.ppqPosition.load (std::memory_order_relaxed), anchor,
                                           hostSync.bpm.load (std::memory_order_relaxed), sr);
    }
    const bool lm = levelMatchParam == nullptr || levelMatchParam->load (std::memory_order_relaxed) >= 0.5f;
    audition.render (buffer, n, pos, playing, lm);
    // Take editor audition: added on top, with its own clock. Idle = the buffer is not touched.
    editorAudition.render (buffer, n);
}

juce::String HotStepLinkProcessor::serverUrlOverride()
{
    // HSLINK_SERVER_URL: launch-time override, in memory only. Never written to state.
    return juce::SystemStats::getEnvironmentVariable ("HSLINK_SERVER_URL", {}).trim();
}

juce::String HotStepLinkProcessor::getSavedServerUrl() const
{
    return apvts.state.getProperty (kServerUrlId, kDefaultServerUrl).toString();
}

juce::String HotStepLinkProcessor::getServerUrl() const
{
    const auto ov = serverUrlOverride();
    return ov.isNotEmpty() ? ov : getSavedServerUrl();
}

void HotStepLinkProcessor::setServerUrl (const juce::String& url)
{
#if ! defined (HSLINK_TEETH_URL_TYPED_SAVE)
    if (serverUrlOverride().isNotEmpty())
        return; // env override active: typed text never reaches state or the endpoint
#endif
    apvts.state.setProperty (kServerUrlId, url, nullptr);
    daw.setEndpoint (url, getDawKey());
}

juce::File HotStepLinkProcessor::keyStoreFile()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("HSLINK_SETTINGS_FILE", {});
    if (env.isNotEmpty())
        return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("HOT-Step").getChildFile ("LINK.settings");
}

static juce::PropertiesFile::Options keyStoreOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "LINK";
    o.folderName = "HOT-Step";
    o.filenameSuffix = "settings";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = -1; // explicit save only
    return o;
}

juce::String HotStepLinkProcessor::loadStoredKey()
{
    const auto f = keyStoreFile();
    if (! f.existsAsFile())
        return {};
    juce::PropertiesFile props (f, keyStoreOptions());
    return props.getValue ("dawKey", {});
}

void HotStepLinkProcessor::saveStoredKey (const juce::String& key)
{
    const auto f = keyStoreFile();
    if (key.isEmpty() && ! f.existsAsFile())
        return;
    f.getParentDirectory().createDirectory();
    juce::PropertiesFile props (f, keyStoreOptions());
    props.setValue ("dawKey", key);
    props.saveIfNeeded();
}

juce::String HotStepLinkProcessor::getDawKey() const
{
#if defined (HSLINK_TEETH_KEY_IN_STATE)
    return apvts.state.getProperty (kDawKeyId, "").toString();
#else
    return dawKey;
#endif
}

void HotStepLinkProcessor::setDawKey (const juce::String& key)
{
#if defined (HSLINK_TEETH_KEY_IN_STATE)
    apvts.state.setProperty (kDawKeyId, key, nullptr); // teeth: old behaviour, key in state
#else
    if (key != dawKey)
    {
        dawKey = key;
        saveStoredKey (key);
    }
    apvts.state.setProperty (kKeySetId, key.isNotEmpty(), nullptr);
#endif
    daw.setEndpoint (getServerUrl(), key);
}

void HotStepLinkProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
#if defined (HSLINK_TEETH_BAD_R6)
    state.removeChild (state.getChildWithName (kStarredId), nullptr); // teeth: stars not saved
#endif
#if ! defined (HSLINK_TEETH_KEY_IN_STATE)
    state.removeProperty (kDawKeyId, nullptr); // the key never goes into a DAW project
    state.setProperty (kKeySetId, dawKey.isNotEmpty(), nullptr);
#endif
#if defined (HSLINK_TEETH_URL_LEAK)
    state.setProperty (kServerUrlId, getServerUrl(), nullptr); // teeth: override leaks into state
#endif
    writeTextState (state);
    writeSlotState (state);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void HotStepLinkProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    lastStateLoadOk.store (loadState (data, sizeInBytes));
}

bool HotStepLinkProcessor::loadState (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            if (! apvts.state.hasProperty (kServerUrlId))
                apvts.state.setProperty (kServerUrlId, kDefaultServerUrl, nullptr);
#if defined (HSLINK_TEETH_KEY_IN_STATE)
            if (! apvts.state.hasProperty (kDawKeyId))
                apvts.state.setProperty (kDawKeyId, "", nullptr);
#else
            // Migration: an old session carried the key in plain text. Move it to
            // the per-user settings file and drop it from state.
            const auto legacy = apvts.state.getProperty (kDawKeyId, "").toString();
            apvts.state.removeProperty (kDawKeyId, nullptr);
            if (legacy.isNotEmpty())
                setDawKey (legacy);
            else
                dawKey = loadStoredKey();
            apvts.state.setProperty (kKeySetId, dawKey.isNotEmpty(), nullptr);
#endif
            readTextState (apvts.state);
            readSlotState (apvts.state);
            syncStarMirror();
            daw.setEndpoint (getServerUrl(), getDawKey());
            return true;
        }
    }
    // Not our state (random bytes, truncated, another plugin's XML): change nothing.
    return false;
}

//==============================================================================
// R6 stable takes
juce::String StableTakes::slugify (const juce::String& text, int maxLen)
{
#if defined (HSLINK_TEETH_BAD_R6)
    return text.substring (0, maxLen); // teeth: no sanitising
#else
    juce::String out;
    bool pendingSep = false;
    const auto lower = text.toLowerCase(); // keep alive while iterating
    for (auto p = lower.getCharPointer(); ! p.isEmpty(); ++p)
    {
        const juce::juce_wchar c = *p;
        const bool keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (! keep)
        {
            pendingSep = out.isNotEmpty();
            continue;
        }
        if (pendingSep)
        {
            if (out.length() + 2 > maxLen)
                break;
            out << '_';
            pendingSep = false;
        }
        if (out.length() + 1 > maxLen)
            break;
        out << juce::String::charToString (c);
    }
    return out.isEmpty() ? juce::String ("take") : out;
#endif
}

juce::String StableTakes::makeName (const juce::String& caption, const juce::String& backend, int takeNumber, double bpm)
{
    const auto be = slugify (backend.isEmpty() ? juce::String ("auto") : backend, 12);
    const int b = bpm > 0.0 ? juce::roundToInt (bpm) : 0;
    return slugify (caption) + "_" + be + "_t" + juce::String (takeNumber) + "_" + juce::String (b) + "bpm.wav";
}

juce::String StableTakes::makeTakeName (const juce::String& caption, const juce::String& backend, int takeNumber, double bpm,
                                        const juce::String& id)
{
    const auto base = makeName (caption, backend, takeNumber, bpm);
    return base.upToLastOccurrenceOf (".wav", false, false) + "_"
           + (id.isEmpty() ? ExclusiveFile::uniqueToken (6) : id) + ".wav";
}

juce::File StableTakes::uniqueFile (const juce::File& folder, const juce::String& name)
{
    auto f = folder.getChildFile (name);
#if ! defined (HSLINK_TEETH_BAD_R6)
    const auto stem = f.getFileNameWithoutExtension();
    const auto ext = f.getFileExtension();
    for (int i = 2; f.exists() && i < 10000; ++i)
        f = folder.getChildFile (stem + "_" + juce::String (i) + ext);
#endif
    return f;
}

juce::File StableTakes::copyInto (const juce::File& src, const juce::File& folder, const juce::String& name,
                                  const ExclusiveFile::AbortFn& shouldAbort, ExclusiveFile::CopyResult* result)
{
#if defined (HSLINK_TEETH_BAD_R6)
    juce::ignoreUnused (shouldAbort);
    if (result != nullptr)
        *result = ExclusiveFile::CopyResult::Failed;
    if (! src.existsAsFile() || ! ExclusiveFile::ensureFolder (folder))
        return {};
    const auto dest = uniqueFile (folder, name);
    juce::MemoryBlock mb;
    src.loadFileAsData (mb);
    mb.setSize (mb.getSize() / 2); // teeth: truncated copy
    return dest.replaceWithData (mb.getData(), mb.getSize()) ? dest : juce::File();
#else
    // Never juce copyFileTo / moveFileTo onto the final name: both replace a file
    // that is already there. See ExclusiveFile.h. copyUnique probes the source and
    // makes the folder itself, reading shouldAbort before each of those calls.
    return ExclusiveFile::copyUnique (src, folder, name, shouldAbort, result);
#endif
}

juce::File StableTakes::defaultRoot()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("MDMAchine").getChildFile ("HOT-STEP LINK").getChildFile ("takes");
}

juce::File HotStepLinkProcessor::getStableSessionFolder() const
{
    std::lock_guard<std::mutex> g (takesLock);
    return stableRoot.getChildFile (sessionName);
}

// File copy outside takesLock. Caller: the saver thread (ensureStableCopies), or a test
// that calls ensureStableCopies itself. Not the worker, not the destructor, not the
// message thread. Only teeth hooks bring another thread here (testWorkerCopies,
// shutdownGuard off, unsavedGuard off, the take list off); no lock serialises the
// copies then, copyUnique picks names atomically.
juce::File HotStepLinkProcessor::copyStable (const juce::File& temp, const juce::String& name,
                                             ExclusiveFile::CopyResult* result)
{
    if (result != nullptr)
        *result = ExclusiveFile::CopyResult::Aborted;
    const auto folder = getStableSessionFolder();
    if (! countCopyAttempt())
        return {};
    return StableTakes::copyInto (temp, folder, name, [this] { return closingNow(); }, result);
}

bool HotStepLinkProcessor::closingNow() const
{
    return shutdownGuard.load() && shuttingDown.load();
}

std::atomic<bool> HotStepLinkProcessor::shutdownGuard { true };
std::atomic<int> HotStepLinkProcessor::testCopyDelayMs { 0 };
std::atomic<int> HotStepLinkProcessor::testAttemptsAfterClose { 0 };

// Before every lasting copy: false = closing, no attempt starts. Counts the attempt
// (tests) and holds the test delay.
bool HotStepLinkProcessor::countCopyAttempt()
{
    if (shutdownGuard.load() && shuttingDown.load())
        return false;
    ++copyAttempts;
    if (destructorBegan.load())
        ++testAttemptsAfterClose;
    const int delay = testCopyDelayMs.load();
    if (delay > 0)
        juce::Thread::sleep (delay);
    return true;
}

std::atomic<bool> HotStepLinkProcessor::testStopAfterRecord { false };
std::atomic<bool> HotStepLinkProcessor::recordBeforeCopy { true };
std::atomic<bool> HotStepLinkProcessor::testStopBeforePublish { false };
std::atomic<bool> HotStepLinkProcessor::recordBeforeMetadata { true };

// Invariant: the worker calls this as soon as a download is complete (the bytes are in
// <name>.wav.part), and the first things done are the list entry and the marker line;
// no file is opened before them. So from the moment a download is complete the take is
// in the marker, or the marker write has failed (then the marker is empty or missing).
// The rescue of a later start recovers it in every one of these states, whether its
// bytes are under <name>.wav or <name>.wav.part: a listed .wav that is missing is
// copied from its .part, and with an empty or missing marker a .part is copied when
// its header says it is a complete WAV (DawWorker::isTakePart).
void HotStepLinkProcessor::registerTake (const DawJob& job, int takeIndex)
{
    ExclusiveFile::ScopedThreadTag tag (ExclusiveFile::tagWorker);
    if (! takeStore.load() || takeIndex < 0 || takeIndex >= job.takeFiles.size())
        return;
    TakeEntry e;
    e.jobId = job.localId;
    e.takeIndex = takeIndex;
    e.temp = job.takeFiles[takeIndex];
    // The take's file name is its name everywhere. Teeth (takeIdInName off): the old
    // id-less lasting name.
    e.name = DawWorker::takeIdInName.load()
                 ? e.temp.getFileName()
                 : StableTakes::makeName (job.request.caption, job.request.expectedBackend, takeIndex + 1, job.request.bpm);
    const auto part = DawWorker::partFileFor (e.temp);
    const bool early = recordBeforeCopy.load();
    if (! early && ! DawWorker::publishDownload (part, e.temp))
        return; // teeth only: the old order publishes first and records last
    // The settings come from the job; the length is read from the file only after the
    // take is recorded (teeth, recordBeforeMetadata off: read here, as before).
    const bool recordFirst = early && recordBeforeMetadata.load();
    fillTakeMetadata (e, job, takeIndex, ! recordFirst);
    {
        std::lock_guard<std::mutex> g (takesLock);
        for (const auto& t : takes)
            if (t.temp == e.temp || t.temp == part)
                return;
    }
    const bool stopBeforePublish = testStopBeforePublish.load();
    if (stopBeforePublish && ! recordFirst)
        return; // test hook, old order: stopped after the metadata read, nothing recorded
    const bool stopHere = testStopAfterRecord.load();
    if (! early && stopHere)
        return; // teeth only: the old order stopped while its copy runs (file published, nothing recorded)
    if (early)
    {
        // Record first. The marker names the final file; until the rename the bytes
        // are in <name>.part, which is where the rescue looks when the listed file
        // is missing.
        {
            std::lock_guard<std::mutex> g (takesLock);
            takes.push_back (e);
            takes.back().handingOver = true; // the saver leaves it alone until handOver()
            trimTakesLocked();
        }
        // The entry is listed from here on, but its file is not final yet (length
        // read, rename tries). A saver pass woken for another take must not attempt
        // it in that window: the copy would find no file, fail, and the take would
        // show as not saved until a timed retry. handOver() ends the window; it runs
        // before the saver is woken and on every other way out of this block.
        auto handOver = [this, &e]
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (auto& t : takes)
                if (t.temp == e.temp)
                    t.handingOver = false;
        };
        struct AtExit
        {
            std::function<void()> fn;
            ~AtExit() { fn(); }
        } handOverAtExit { handOver };
        if (recordFirst)
        {
            if (stopBeforePublish)
                return; // test hook: recorded and listed, the bytes still in the .part
            // File IO outside takesLock; the lock is held for the one write only. Until
            // then the entry shows the requested length.
            const double secs = CollectionStore::testDisabled.load() ? 0.0 : readTakeDuration (e.temp);
            if (secs > 0.0)
            {
                e.durationS = secs;
                std::lock_guard<std::mutex> g (takesLock);
                for (auto& t : takes)
                    if (t.temp == e.temp)
                        t.durationS = secs;
            }
        }
        // Tempo tags (acid + smpl) at the requested tempo so a DAW warps the take at it.
        // Done on the .part before the rename, so the take file and every copy of it carry them.
        {
            juce::String tagError;
            const bool tagged = WavTempoTag::inject (part, e.bpm, &tagError);
            juce::Logger::writeToLog ("HOT-STEP LINK: take " + e.name + " tempo tag " + juce::String (e.bpm, 3) + " bpm: "
                                      + (tagged ? juce::String ("written") : tagError));
        }
        if (! DawWorker::publishDownload (part, e.temp, [this] { return closingNow(); }))
        {
            if (DawWorker::keepUnpublished.load() && part.existsAsFile())
            {
                // The rename kept failing (or the plugin is closing): the complete
                // bytes stay in the .part file and the take stays recorded, with that
                // file as its temp file. Its name is unchanged, so the marker line is
                // "<name>.part<TAB><name>" and the lasting copy gets the name.
                const auto finalFile = e.temp;
                e.temp = part;
                {
                    std::lock_guard<std::mutex> g (takesLock);
                    for (auto& t : takes)
                        if (t.temp == finalFile)
                            t.temp = part;
                    if (pinnedTake == finalFile.getFullPathName())
                        pinnedTake = part.getFullPathName();
                }
                daw.notePartKept (part);
                juce::Logger::writeToLog ("HOT-STEP LINK: take " + e.name + " was downloaded but its file could not be renamed; it is kept as "
                                          + part.getFullPathName() + " and saved from there");
            }
            else
            {
                // No take file came of it: drop the entry again. The worker reports the take as failed.
                {
                    std::lock_guard<std::mutex> g (takesLock);
                    for (size_t i = takes.size(); i-- > 0;)
                        if (takes[i].temp == e.temp)
                            takes.erase (takes.begin() + (std::ptrdiff_t) i);
                }
                return;
            }
        }
        // The file is final: measure its tempo in the background (the asked tempo is
        // only the hint). Reads the temp copy, read only; never the lasting one.
        requestTakeMeasure (e.temp, e.bpm);
        if (stopHere)
            return;
        if (! testWorkerCopies.load())
        {
            // Recorded, marked and published: this thread is done. The lasting copy is
            // the saver thread's, which is woken here and copies at once. While closing
            // it starts no copy: the take stays unsaved and marked for the next start.
            handOver();
            wakeSaverForNewTake();
            return;
        }
    }
    // TEETH ONLY from here on (testWorkerCopies, or the old order of recordBeforeCopy
    // off): the copy made on the calling thread, as before the saver made every copy.
    auto copyResult = ExclusiveFile::CopyResult::Aborted;
    if (! closingNow())
        e.stable = copyStable (e.temp, e.name, &copyResult);
    const bool closing = e.stable == juce::File() && copyResult == ExclusiveFile::CopyResult::Aborted;
    e.copyFailed = e.stable == juce::File() && ! closing;
    {
        std::lock_guard<std::mutex> g (takesLock);
        if (e.stable != juce::File())
            stableByTemp[e.temp.getFullPathName()] = e.stable;
        if (early)
        {
            // The entry is found again by its file (unique per take).
            for (auto& t : takes)
                if (t.temp == e.temp)
                {
                    t.stable = e.stable;
                    t.copyFailed = e.copyFailed;
                }
            if (e.stable != juce::File() && pinnedTake == e.temp.getFullPathName())
                pinnedTake = e.stable.getFullPathName();
        }
        else
        {
            takes.push_back (e); // teeth only: recorded after the copy returned
            trimTakesLocked();
        }
    }
    if (e.stable != juce::File())
        noteSaved (e.temp, e.stable); // saved.log, outside takesLock
    if (e.stable == juce::File())
    {
        // Once per failure; the retries do not log. Closing is not a failure: the
        // line says so and the retry schedule is left alone.
        if (closing)
            juce::Logger::writeToLog ("HOT-STEP LINK: take " + e.name + " was not saved because the plugin is closing; it is kept in "
                                      + e.temp.getFullPathName() + " and copied at the next start");
        else
            juce::Logger::writeToLog ("HOT-STEP LINK: take " + e.name + " could not be saved to "
                                      + getStableSessionFolder().getFullPathName() + "; it is kept in "
                                      + e.temp.getFullPathName() + " and the save is retried");
        if (saver != nullptr && ! closing)
            saver->retryNow();
    }
}

//==============================================================================
// Unsaved takes: marker file, background retry, rescue on a later start.
std::atomic<bool> DawWorker::unsavedGuard { true };

void HotStepLinkProcessor::requestSaveRetry()
{
    if (saver != nullptr)
        saver->retryNow();
}

int HotStepLinkProcessor::countUnsaved() const
{
    std::lock_guard<std::mutex> g (takesLock);
    int n = 0;
    for (const auto& t : takes)
        n += t.stable == juce::File() ? 1 : 0;
    return n;
}

// The session folder (a session_* folder directly under the temp root) that holds f,
// or {} when f is somewhere else. Path arithmetic only, no disk IO.
static juce::File unsavedSessionFolderOf (const juce::File& f, const juce::File& tempRoot)
{
    for (auto d = f.getParentDirectory(); d != d.getParentDirectory(); d = d.getParentDirectory())
        if (d.getParentDirectory() == tempRoot)
            return d.getFileName().startsWith ("session_") ? d : juce::File();
    return {};
}

// One saved.log line for a take of this instance whose lasting copy was just made. Only
// for a take file directly in a temp session folder. A line that cannot be written is
// not an error: the take is then "not proven saved" and its folder is kept.
void HotStepLinkProcessor::noteSaved (const juce::File& temp, const juce::File& lasting)
{
    const auto dir = unsavedSessionFolderOf (temp, DawWorker::tempRoot());
    if (dir != juce::File() && temp.getParentDirectory() == dir)
        DawWorker::appendSavedLog (dir, temp.getFileName(), temp.getSize(), lasting.getFullPathName());
}

HotStepLinkProcessor::SaveNotice HotStepLinkProcessor::getSaveNotice() const
{
    SaveNotice n;
    if (! DawWorker::unsavedGuard.load())
        return n;
    std::lock_guard<std::mutex> g (takesLock);
    for (const auto& t : takes)
        if (t.stable == juce::File() && t.copyFailed) // not "not copied yet": only after a failed attempt
        {
            if (n.unsaved++ == 0)
                n.tempFolder = t.temp.getParentDirectory().getFullPathName();
        }
    n.unprotected = false; // no marker any more: an unsaved take is always kept by its folder
    n.stableFolder = stableRoot.getChildFile (sessionName).getFullPathName();
    n.recovered = recoveredCount;
    n.recoveredFolder = recoveredFolder;
    return n;
}

juce::String HotStepLinkProcessor::SaveNotice::text() const
{
    juce::String s;
    if (unsaved > 0 && unprotected)
        s << unsaved << (unsaved == 1 ? " take" : " takes") << " could not be saved to " << stableFolder
          << " and could not be listed for recovery in the temporary folder " << tempFolder
          << ". That folder is not removed while " << (unsaved == 1 ? "it is" : "they are") << " unsaved. Export "
          << (unsaved == 1 ? "it" : "them") << " with Export WAV if the save keeps failing."
          << " The save is retried automatically.";
    else if (unsaved > 0)
        s << unsaved << (unsaved == 1 ? " take" : " takes") << " could not be saved to " << stableFolder
          << ". " << (unsaved == 1 ? "It is" : "They are") << " kept in the temporary folder " << tempFolder
          << ". The save is retried automatically.";
    if (recovered > 0)
        s << (s.isEmpty() ? "" : " ") << "Recovered " << recovered << (recovered == 1 ? " take" : " takes")
          << " from an earlier session into " << recoveredFolder << ".";
    return s;
}


std::atomic<int> HotStepLinkProcessor::rescueCalls { 0 };
std::atomic<int> HotStepLinkProcessor::rescueLockedSkips { 0 };
static std::mutex legacyRescueLock;     // guards legacyClaims
static juce::StringArray legacyClaims;  // legacy folders (no owner.lock) adopted by an instance in this process

std::atomic<int> HotStepLinkProcessor::adoptedTakes { 0 };
std::atomic<bool> HotStepLinkProcessor::adoptEnabled { true };
std::atomic<bool> HotStepLinkProcessor::keepGuard { true };

int HotStepLinkProcessor::adoptDeadFolders()
{
    ++rescueCalls;
    if (! DawWorker::unsavedGuard.load() || ! adoptEnabled.load() || ! takeStore.load())
    {
        return 0;
    }
    const auto tempRoot = DawWorker::tempRoot();
    const auto own = getOwnTempFolders();
    const auto root = getStableRoot();
    const auto tab = juce::String::charToString ((juce::juce_wchar) 9);
    const auto header = juce::String ("HSLINK-UNSAVED v1");
    // true = a dead folder is searched for take files whether or not it has a legacy
    // marker (every *.wav and complete *.wav.part that saved.log does not list).
    const bool sweep = DawWorker::protectByDefault.load();
    int added = 0;
    std::unique_lock<std::mutex> adoptGuard (adoptLock);
    for (const auto& dir : tempRoot.findChildFiles (juce::File::findDirectories, false, "session_*"))
    {
        if (shuttingDown.load())
            break;
        if (own.contains (dir) || DawWorker::isLocklessFolder (dir))
            continue;
        {
            std::lock_guard<std::mutex> g (takesLock);
            if (adoptedNames.contains (dir.getFullPathName()))
                continue;
        }
        const auto marker = dir.getChildFile (DawWorker::unsavedMarkerName());
        // A look without the lock: nothing is opened in a folder with nothing to adopt.
        if (! marker.existsAsFile() && (! sweep || ! DawWorker::holdsUnsavedTake (dir)))
            continue;
        // A folder with owner.lock is adopted only by the caller that wins the lock (its
        // owner is dead). One without (an older build, or a lockless instance that died)
        // is legacy: nobody can prove its owner is dead. With a legacy marker it is
        // adopted at once; without one only once its newest content is a day old.
        std::unique_ptr<ExclusiveFile::SessionLock> lock;
        bool legacy = ! dir.getChildFile (ExclusiveFile::SessionLock::fileName()).existsAsFile();
        if (! legacy)
        {
            lock = std::make_unique<ExclusiveFile::SessionLock>();
            const auto got = lock->tryAcquireExisting (dir);
            if (got == ExclusiveFile::SessionLock::Result::NoLockFile)
            {
                lock.reset(); // the lock file vanished between the look and the open
                legacy = true;
            }
            else if (got != ExclusiveFile::SessionLock::Result::Held)
            {
                if (got == ExclusiveFile::SessionLock::Result::Busy)
                    ++rescueLockedSkips; // its owner is alive: nothing is touched
                continue;
            }
        }
        if (legacy)
        {
            if (! marker.existsAsFile()
                && ! (DawWorker::newestContentTime (dir)
                      < juce::Time::getCurrentTime() - juce::RelativeTime::milliseconds (DawWorker::staleAgeMs())))
                continue;
            // No lock to hold: one instance per process claims such a folder.
            std::lock_guard<std::mutex> lg (legacyRescueLock);
            if (legacyClaims.contains (dir.getFullPathName()))
                continue;
            legacyClaims.add (dir.getFullPathName());
        }
        const auto releaseClaim = [&dir, legacy]
        {
            if (! legacy)
                return;
            std::lock_guard<std::mutex> lg (legacyRescueLock);
            legacyClaims.removeString (dir.getFullPathName());
        };
        // The legacy marker: read only, never written. An empty one lists nothing.
        juce::StringArray lines;
        if (marker.existsAsFile())
        {
            juce::String markerText;
            {
                juce::FileInputStream in (marker);
                if (! in.openedOk())
                {
                    releaseClaim(); // cannot be read now: leave everything as it is
                    continue;
                }
                markerText = in.readEntireStreamAsString();
            }
            if (markerText.trim().isNotEmpty())
                lines.addLines (markerText);
        }
        if (lines.isEmpty())
            lines.add (header);
        if (lines[0].trim() != header)
        {
            releaseClaim(); // not a marker this version understands: leave everything as it is
            continue;
        }
        const auto savedLog = DawWorker::readSavedLog (dir);
        const auto dest = root.getChildFile ("recovered_" + dir.getFileName());
        std::vector<TakeEntry> found;
        bool trusted = true, stop = false;
        // One candidate. false = stop looking in this folder (teeth: zeroByteSkip off).
        const auto consider = [&] (const juce::File& src, const juce::String& name, const juce::String& logKey)
        {
            for (const auto& e : found)
                if (e.temp == src)
                    return true;
            const auto size = src.getSize();
            if (size > 0 && (savedLog.contains (logKey + tab + juce::String (size))
                             || savedLog.contains (src.getRelativePathFrom (dir) + tab + juce::String (size))))
                return true; // saved.log proves this take has its lasting copy
            if (size <= 0)
            {
                if (! DawWorker::zeroByteSkip.load())
                    return false;
                // Not a take and never recoverable: it does not stop the takes after it.
                ++DawWorker::zeroByteSkips;
                juce::Logger::writeToLog ("HOT-STEP LINK: skipped the zero-byte file " + src.getFullPathName()
                                          + " during recovery (not a take)");
                return true;
            }
            TakeEntry e;
            e.temp = src;
            e.name = name;
            e.logKey = logKey;
            e.adopted = true;
            e.destFolder = dest;
            e.createdMs = src.getLastModificationTime().toMilliseconds();
            found.push_back (e);
            return true;
        };
        juce::StringArray listed; // full paths of the marker's lines
        for (int i = 1; i < lines.size(); ++i)
        {
            const auto rel = lines[i].upToFirstOccurrenceOf (tab, false, false).trim();
            if (rel.isNotEmpty())
                listed.add (dir.getChildFile (rel).getFullPathName());
        }
        // The marker's lines, under their listed names. A listed .wav that is missing is
        // taken from its .part (the process ended between the line and the rename).
        for (int i = 1; i < lines.size() && ! stop; ++i)
        {
            const auto rel = lines[i].upToFirstOccurrenceOf (tab, false, false).trim();
            auto name = lines[i].fromFirstOccurrenceOf (tab, false, false).trim();
            if (rel.isEmpty())
                continue;
            auto src = dir.getChildFile (rel);
            if (! src.isAChildOf (dir))
            {
                trusted = false; // a path that leaves the session folder: do not trust this marker
                break;
            }
            const auto logKey = src.getRelativePathFrom (dir);
            if (! src.existsAsFile())
            {
                const auto part = DawWorker::partFileFor (src);
                if (! DawWorker::partRescueEnabled.load() || ! rel.endsWithIgnoreCase (".wav")
                    || ! part.existsAsFile() || part.getSize() <= 0)
                    continue; // nothing left to recover for this line
                src = part;
            }
            if (name.isEmpty() || name != juce::File::createLegalFileName (name))
                name = src.getFileName();
            if (name.endsWithIgnoreCase (".part") && name.length() > 5)
                name = name.dropLastCharacters (5);
            stop = ! consider (src, name, logKey);
        }
        if (! trusted)
        {
            releaseClaim();
            continue;
        }
        if (sweep)
        {
            // Never a *.part file here and never a capture (DawWorker::isCaptureName:
            // exactly source_<digits>.wav). Takes of older builds have other names:
            // every other *.wav counts, under its own name.
            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
            {
                if (stop)
                    break;
                if (f.hasFileExtension ("wav") && ! ExclusiveFile::isPartial (f)
                    && ! DawWorker::isCaptureName (f.getFileName()) && ! listed.contains (f.getFullPathName()))
                    stop = ! consider (f, f.getFileName(), f.getFileName());
            }
            // A *.wav.part that holds a complete take (isTakePart, the old rescue's rule)
            // and has no line of its own: recovered and logged under its .wav name. One
            // the marker lists as its .wav, with that file missing, was taken above.
            if (DawWorker::partRescueEnabled.load())
                for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.part"))
                {
                    if (stop)
                        break;
                    const auto wavPath = f.getFullPathName().dropLastCharacters (5);
                    if (! f.getFileName().endsWithIgnoreCase (".wav.part") || listed.contains (f.getFullPathName())
                        || (listed.contains (wavPath) && ! juce::File (wavPath).existsAsFile()))
                        continue;
                    if (DawWorker::isTakePart (f, listed))
                        stop = ! consider (f, DawWorker::takeNameOf (f), f.getRelativePathFrom (dir).dropLastCharacters (5));
                }
        }
        int addedHere = 0;
        {
            std::lock_guard<std::mutex> g (takesLock);
            adoptedNames.add (dir.getFullPathName());
            for (const auto& e : found)
            {
                bool dup = false;
                for (const auto& t : takes)
                    dup = dup || t.temp == e.temp;
                if (! dup)
                {
                    takes.push_back (e); // at the end: no index of a pending delete shifts
                    ++addedHere;
                }
            }
        }
        added += addedHere;
        // W1: a scan that stopped early or did not sweep may leave unlisted takes behind.
        if (stop || ! sweep)
            adoptedKeepWhole.addIfNotAlreadyThere (dir.getFullPathName());
        adoptedLocks.emplace_back (dir, std::move (lock)); // nullptr: a legacy folder, claimed in legacyClaims
        if (addedHere > 0)
            juce::Logger::writeToLog ("HOT-STEP LINK: adopted " + juce::String (addedHere)
                                      + " unsaved take(s) from " + dir.getFullPathName());
    }
    adoptGuard.unlock();
    adoptedTakes += added;
    finishAdoptedFolders(); // a dead folder with nothing unsaved goes at once
    return added;
}

void HotStepLinkProcessor::releaseLegacyClaims()
{
    std::lock_guard<std::mutex> a (adoptLock);
    std::lock_guard<std::mutex> lg (legacyRescueLock);
    for (const auto& p : adoptedLocks)
        if (p.second == nullptr)
            legacyClaims.removeString (p.first.getFullPathName());
}

void HotStepLinkProcessor::finishAdoptedFolders()
{
    std::lock_guard<std::mutex> a (adoptLock);
    for (size_t i = 0; i < adoptedLocks.size();)
    {
        const auto dir = adoptedLocks[i].first;
        bool pending = false;
        int saved = 0;
        juce::String destPath;
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (const auto& t : takes)
                if (t.adopted && t.temp.isAChildOf (dir))
                {
                    pending = pending || t.stable == juce::File();
                    saved += t.stable != juce::File() ? 1 : 0;
                    destPath = t.destFolder.getFullPathName();
                }
        }
        if (pending || closingNow())
        {
            ++i;
            continue;
        }
        // W1 + W2: a folder that may still hold an unlisted take or an unknown file is
        // never emptied or removed; it stays held (and its marker stays) until close.
        if (keepGuard.load()
            && (adoptedKeepWhole.contains (dir.getFullPathName())
                || (adoptedLocks[i].second != nullptr && dir.isDirectory() && ! keptFolderCheck (dir))))
        {
            ++i;
            continue;
        }
        // Every adopted take there has its lasting copy and a saved.log line.
        if (saved > 0)
            juce::Logger::writeToLog ("HOT-STEP LINK: recovered " + juce::String (saved) + " unsaved take(s) from "
                                      + dir.getFullPathName() + " into " + destPath);
        if (adoptedLocks[i].second != nullptr)
        {
            if (dir.isDirectory())
                removeLockedFolder (dir, *adoptedLocks[i].second);
        }
        else
        {
            // Legacy (no owner.lock): its marker goes; the folder ages out by saved.log.
            dir.getChildFile (DawWorker::unsavedMarkerName()).deleteFile();
            std::lock_guard<std::mutex> lg (legacyRescueLock);
            legacyClaims.removeString (dir.getFullPathName());
        }
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (size_t k = 0; k < takes.size();)
                if (takes[k].adopted && takes[k].temp.isAChildOf (dir))
                    takes.erase (takes.begin() + (std::ptrdiff_t) k);
                else
                    ++k;
            adoptedNames.removeString (dir.getFullPathName());
        }
        adoptedLocks.erase (adoptedLocks.begin() + (std::ptrdiff_t) i);
    }
}




void HotStepLinkProcessor::fillTakeMetadata (TakeEntry& e, const DawJob& job, int takeIndex, bool readFile)
{
    if (CollectionStore::testDisabled.load())
        return; // teeth: no metadata
    const auto& rq = job.request;
    e.caption = rq.caption;
    // The server does not report which backend an Auto job ran on, so Auto stays "auto".
    e.backend = rq.expectedBackend.isEmpty() ? juce::String ("auto") : rq.expectedBackend;
    e.model = job.ditModel;
    e.bpm = rq.bpm;
    e.key = rq.key;
    const auto reported = takeIndex >= 0 && takeIndex < job.takeSeeds.size() ? job.takeSeeds[takeIndex] : (juce::int64) -1;
    e.seed = reported >= 0 ? reported : (rq.seed >= 0 ? rq.seed : (juce::int64) -1);
    e.mode = rq.mode;
    e.createdMs = juce::Time::currentTimeMillis();
    e.durationS = rq.durationS;
    if (readFile && takeIndex >= 0 && takeIndex < job.takeFiles.size())
    {
        const double secs = readTakeDuration (job.takeFiles[takeIndex]);
        if (secs > 0.0)
            e.durationS = secs;
    }
}

double HotStepLinkProcessor::readTakeDuration (const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    // Before the download is published the bytes are still in <take>.part; that
    // name has no audio extension, so it is opened as a stream.
    const auto part = DawWorker::partFileFor (f);
    std::unique_ptr<juce::AudioFormatReader> rd;
    if (f.existsAsFile())
        rd.reset (fm.createReaderFor (f));
    else if (auto in = part.createInputStream())
        rd.reset (fm.createReaderFor (std::move (in)));
    if (rd != nullptr && rd->sampleRate > 0.0 && rd->lengthInSamples > 0)
        return (double) rd->lengthInSamples / rd->sampleRate;
    return 0.0;
}

// Collection. The take is copied out of the list under takesLock, the lock is
// released, and only then is the store called: the two locks are never nested.
CollectionResult HotStepLinkProcessor::addTakeToCollection (const juce::File& stableTake)
{
    TakeEntry e;
    bool found = false;
    {
        std::lock_guard<std::mutex> g (takesLock);
        for (const auto& t : takes)
            if (t.stable != juce::File() && t.stable == stableTake)
            {
                e = t;
                found = true;
                break;
            }
    }
    if (! found)
    {
        CollectionResult r;
        r.error = "take not found, or it has no lasting file yet";
        return r;
    }
    const auto r = collection.add (e);
    if (r.ok)
        refreshCollection();
    return r;
}

std::vector<CollectionItem> HotStepLinkProcessor::getCollection() const
{
    std::lock_guard<std::mutex> g (collectionCacheLock);
    return collectionCache;
}

void HotStepLinkProcessor::refreshCollection()
{
    auto items = collection.list(); // store IO first, cache lock after
    {
        std::lock_guard<std::mutex> g (collectionCacheLock);
        collectionCache.swap (items);
    }
    ++collectionGeneration;
}

void HotStepLinkProcessor::requestCollectionRefresh()
{
    if (CollectionView::testDisabled.load())
        return; // teeth: no background rescan
    if (collectionPool.getNumJobs() > 1)
        return; // one running and one waiting already cover this request
    collectionPool.addJob ([this] { refreshCollection(); });
}

bool HotStepLinkProcessor::setCollectionStarred (const CollectionItem& item, bool starred)
{
    const bool ok = collection.setStarred (item, starred);
    if (ok)
        refreshCollection();
    return ok;
}

bool HotStepLinkProcessor::renameCollectionItem (const CollectionItem& item, const juce::String& newName)
{
    const bool ok = collection.rename (item, newName);
    if (ok)
        refreshCollection();
    return ok;
}

bool HotStepLinkProcessor::removeFromCollection (const CollectionItem& item, juce::String* note)
{
    const bool ok = collection.remove (item, note);
    if (ok)
        refreshCollection();
    return ok;
}

// Caller holds takesLock. Drops entries from the list only, never a file.
void HotStepLinkProcessor::trimTakesLocked()
{
    for (size_t i = 0; (int) takes.size() > kMaxTakes && i < takes.size();)
    {
        const auto& t = takes[i];
        const bool keep = t.stable == juce::File() // only copy is the temp file
                          || starredNames.contains (t.stable.getFileName())
                          || starredNames.contains (DawWorker::takeNameOf (t.temp)) // the same name unless a test made the files
                          || t.stable.getFullPathName() == pinnedTake;
        if (keep)
        {
            ++i;
            continue;
        }
        stableByTemp.erase (t.temp.getFullPathName());
        takes.erase (takes.begin() + (std::ptrdiff_t) i);
    }
}

int HotStepLinkProcessor::ensureStableCopies (bool freshOnly, int* firstFailures)
{
    std::vector<TakeEntry> todo;
    {
        std::lock_guard<std::mutex> g (takesLock);
        for (const auto& t : takes)
            if (t.stable == juce::File() && ! (freshOnly && t.copyFailed)
                && ! (t.handingOver && handOverGuard.load())) // registerTake is still publishing it
                todo.push_back (t);
    }
    int missing = 0;
    for (size_t i = 0; i < todo.size(); ++i)
    {
        const auto& t = todo[i];
        // Closing: no new attempt. What is left stays unsaved and marked.
        if (shutdownGuard.load() && shuttingDown.load())
        {
            missing += (int) (todo.size() - i);
            break;
        }
        auto copyResult = ExclusiveFile::CopyResult::Failed;
        juce::File dest;
        bool alreadyThere = false;
        if (t.adopted)
        {
            copyResult = ExclusiveFile::CopyResult::Aborted;
            if (auto* hook = testBeforeRescueTake.load())
                hook (*this); // tests only
            // A file under the final name with the same size is this take, copied by an
            // earlier pass (copies are published by an exclusive rename). Not a file call
            // on the lasting root once closing.
            const auto already = t.destFolder.getChildFile (t.name);
            const auto size = t.temp.getSize();
            if (! closingNow() || testRescueProbeBeforeClosing.load())
            {
                ++testRescueLastingProbes;
                if (! ExclusiveFile::isPartial (already) && already.existsAsFile())
                {
                    ++testRescueLastingProbes;
                    alreadyThere = already.getSize() == size;
                }
            }
            if (alreadyThere)
                dest = already;
            else if (size <= 0)
                copyResult = ExclusiveFile::CopyResult::Failed;
            else if (countCopyAttempt())
                dest = StableTakes::copyInto (t.temp, t.destFolder, t.name, [this] { return closingNow(); }, &copyResult);
        }
        else
            dest = copyStable (t.temp, t.name, &copyResult);
        if (dest == juce::File())
        {
            if (copyResult == ExclusiveFile::CopyResult::Aborted)
            {
                missing += (int) (todo.size() - i); // closing: this take and the rest stay unsaved and marked
                break;
            }
            ++missing;
            bool first = false;
            {
                std::lock_guard<std::mutex> g (takesLock);
                for (auto& e : takes)
                    if (e.temp == t.temp && e.stable == juce::File() && ! e.copyFailed)
                        e.copyFailed = first = true;
            }
            if (first && firstFailures != nullptr)
                ++*firstFailures;
            if (first) // the take's first failed attempt: logged once, the retries do not log
                juce::Logger::writeToLog ("HOT-STEP LINK: take " + t.name + " could not be saved to "
                                          + getStableSessionFolder().getFullPathName() + "; it is kept in "
                                          + t.temp.getFullPathName() + " and the save is retried");
            continue;
        }
        bool used = false;
        {
            std::lock_guard<std::mutex> g (takesLock);
            for (auto& e : takes)
                if (e.temp == t.temp && e.stable == juce::File())
                {
                    e.stable = dest;
                    e.copyFailed = false;
                    stableByTemp[e.temp.getFullPathName()] = dest;
                    used = true;
                    if (pinnedTake == e.temp.getFullPathName())
                        pinnedTake = dest.getFullPathName();
                    if (e.adopted && ! alreadyThere)
                    {
                        ++recoveredCount;
                        recoveredFolder = e.destFolder.getFullPathName();
                    }
                }
        }
        if (used && t.adopted)
            DawWorker::appendSavedLog (t.temp.getParentDirectory(), t.logKey.isNotEmpty() ? t.logKey : t.temp.getFileName(),
                                       t.temp.getSize(), dest.getFullPathName());
        else if (used)
            noteSaved (t.temp, dest); // saved.log, outside takesLock
        else
            dest.deleteFile(); // another thread made the copy first; drop our duplicate (outside takesLock)
    }
    if (! todo.empty())
        finishAdoptedFolders();
    return missing;
}

std::vector<HotStepLinkProcessor::TakeEntry> HotStepLinkProcessor::getTakes()
{
    if (! takeStore.load())
    {
        // Teeth / legacy: takes only from the job cards that are still alive.
        std::vector<TakeEntry> out;
        for (const auto& j : daw.getJobs())
            for (int i = 0; i < j.takeFiles.size(); ++i)
            {
                TakeEntry e;
                e.jobId = j.localId;
                e.takeIndex = i;
                e.temp = j.takeFiles[i];
                e.stable = stableTakeFor (j, i);
                out.push_back (e);
            }
        return out;
    }
    if (! DawWorker::unsavedGuard.load())
        ensureStableCopies(); // teeth: the old retry on the caller's thread
    std::lock_guard<std::mutex> g (takesLock);
    std::vector<TakeEntry> out;
    out.reserve (takes.size());
    for (const auto& t : takes)
        if (! t.adopted) // adopted takes count in the "Recovered N takes" line only
            out.push_back (t);
    return out;
}

void HotStepLinkProcessor::setPinnedTake (const juce::File& f)
{
    std::lock_guard<std::mutex> g (takesLock);
    pinnedTake = f == juce::File() ? juce::String() : f.getFullPathName();
}

//==============================================================================
// Take delete with a short undo. See the header for the rules.
std::atomic<bool (*) (const juce::File&)> HotStepLinkProcessor::testTrashFile { nullptr };

juce::String HotStepLinkProcessor::takeDeleteText (TakeDelete result)
{
    switch (result)
    {
        case TakeDelete::starred:  return "Unstar this take to delete it";
        case TakeDelete::busy:     return "This take is still being saved. Try again in a moment";
        case TakeDelete::notFound: return "This take is no longer in the list";
        case TakeDelete::closing:  return "The plugin is closing";
        case TakeDelete::deleted:  break;
    }
    return {};
}

juce::String HotStepLinkProcessor::takeDeleteFailedText (const juce::String& takeName)
{
    return "Could not move " + takeName + " to the Deleted folder. The take was kept.";
}

juce::String HotStepLinkProcessor::takeDeleteStarredText (const juce::String& takeName)
{
    return "Kept " + takeName + ": it is starred. Unstar it to delete it.";
}

void HotStepLinkProcessor::restoreTakeLocked (const PendingTakeDelete& p)
{
    // After the entry it followed when that one is still listed, else at its old index.
    size_t at = juce::jmin (p.index, takes.size());
    if (p.afterTemp.isNotEmpty())
        for (size_t i = 0; i < takes.size(); ++i)
            if (takes[i].temp.getFullPathName() == p.afterTemp)
                at = i + 1;
    takes.insert (takes.begin() + (std::ptrdiff_t) at, p.entry);
    if (p.entry.stable != juce::File())
        stableByTemp[p.entry.temp.getFullPathName()] = p.entry.stable;
}

bool HotStepLinkProcessor::takePendingDelete (std::vector<PendingTakeDelete>& out)
{
    std::lock_guard<std::mutex> g (takesLock);
    out.clear();
    if (! pendingDelete.active)
        return false;
    out.push_back (pendingDelete);
    out.insert (out.end(), pendingRest.begin(), pendingRest.end());
    pendingDelete = PendingTakeDelete();
    pendingRest.clear();
    return true;
}

// takesLock held. Stars are kept by take name (starredNames); every name the take can
// be starred under is checked.
bool HotStepLinkProcessor::takeStarredLocked (const TakeEntry& t) const
{
    return starredNames.contains (TakeGrid::keyOf (t)) || starredNames.contains (t.name)
           || starredNames.contains (DawWorker::takeNameOf (t.temp))
           || (t.stable != juce::File() && starredNames.contains (t.stable.getFileName()));
}

HotStepLinkProcessor::TakeDelete HotStepLinkProcessor::deleteTake (const juce::String& tempPath)
{
    if (shuttingDown.load())
        return TakeDelete::closing;
    TakeEntry gone;
    std::vector<PendingTakeDelete> before; // the delete (or batch) that was pending; taken out only when this one is accepted
    {
        std::lock_guard<std::mutex> g (takesLock);
        // The key is the full path of the take's temp file: every entry has one and no
        // two listed entries share it (registerTake refuses a second entry for a temp
        // file). Two takes can have the same name, so the name is not a key.
        size_t i = 0;
        for (; i < takes.size(); ++i)
            if (takes[i].temp.getFullPathName() == tempPath)
                break;
        if (tempPath.isEmpty() || i >= takes.size())
            return TakeDelete::notFound;
        const auto& t = takes[i];
        if (takeStarredLocked (t))
            return TakeDelete::starred;
        if (t.handingOver)
            return TakeDelete::busy; // registerTake still renames and records this file
        // Accepted. Every refusal is above this line and has changed nothing: a delete
        // that was pending is still pending, with its own deadline.
        if (pendingDelete.active) // one pending delete at a time: the one before is committed below
        {
            before.push_back (pendingDelete);
            before.insert (before.end(), pendingRest.begin(), pendingRest.end());
        }
        pendingDelete = PendingTakeDelete();
        pendingRest.clear();
        pendingDelete.active = true;
        pendingDelete.entry = t;
        pendingDelete.index = i;
        // The entry it follows: the nearest listed take before it that is not adopted.
        // Adopted entries are erased by the saver once saved, so they are no anchor.
        for (size_t k = i; k-- > 0;)
            if (! takes[k].adopted)
            {
                pendingDelete.afterTemp = takes[k].temp.getFullPathName();
                break;
            }
        pendingDelete.atMs = juce::Time::getMillisecondCounter();
        gone = t;
        if (pinnedTake == t.temp.getFullPathName() || pinnedTake == t.stable.getFullPathName())
            pinnedTake.clear();
        stableByTemp.erase (t.temp.getFullPathName());
        takes.erase (takes.begin() + (std::ptrdiff_t) i);
    }
    if (! before.empty()) // queued after the lock is released; the move runs on the pool thread
        takeDeletePool.addJob ([this, before] { for (const auto& b : before) finishTakeDelete (b); });
    // The selected take: audition off before its file can go. select publishes "no
    // take" through the engine's own retire list, so the audio thread fades off the
    // decoded buffer and it is freed by collectGarbage as for any other deselect. A
    // decode still running for it is dropped by the new generation.
    const auto sel = audition.getSelected();
    if (sel != juce::File() && (sel == gone.stable || sel == gone.temp || sel == DawWorker::partFileFor (gone.temp)))
        audition.select (juce::File(), getSourceFile());
    return TakeDelete::deleted;
}

bool HotStepLinkProcessor::undoTakeDelete()
{
    bool unsaved = false;
    {
        std::lock_guard<std::mutex> g (takesLock);
        if (! pendingDelete.active)
            return false;
        // A batch is restored oldest first: each entry's neighbour before it is then
        // already back, so every take lands at its old place.
        restoreTakeLocked (pendingDelete);
        unsaved = pendingDelete.entry.stable == juce::File();
        for (const auto& r : pendingRest)
        {
            restoreTakeLocked (r);
            unsaved = unsaved || r.entry.stable == juce::File();
        }
        pendingDelete = PendingTakeDelete();
        pendingRest.clear();
    }
    if (unsaved)
        requestSaveRetry(); // the saver thread copies it and syncs the marker
    return true;
}

juce::String HotStepLinkProcessor::getPendingTakeDelete() const
{
    std::lock_guard<std::mutex> g (takesLock);
    if (! pendingDelete.active)
        return {};
    return pendingRest.empty() ? pendingDelete.entry.name : juce::String ((int) pendingRest.size() + 1) + " unstarred takes";
}

juce::String HotStepLinkProcessor::takeTakeDeleteMessage()
{
    std::lock_guard<std::mutex> g (takesLock);
    const auto m = takeDeleteMessage;
    takeDeleteMessage.clear();
    return m;
}

void HotStepLinkProcessor::pollTakeDelete()
{
    {
        std::lock_guard<std::mutex> g (takesLock);
        if (! pendingDelete.active)
            return;
        const auto waited = juce::Time::getMillisecondCounter() - pendingDelete.atMs; // unsigned: wrap-safe
        if (waited < (juce::uint32) juce::jmax (0, takeDeleteUndoMs.load()))
            return;
    }
    commitTakeDelete();
}

void HotStepLinkProcessor::commitTakeDelete()
{
    std::vector<PendingTakeDelete> ps;
    if (takePendingDelete (ps))
        takeDeletePool.addJob ([this, ps] { for (const auto& p : ps) finishTakeDelete (p); });
}

int HotStepLinkProcessor::clearUnstarredTakes()
{
    if (shuttingDown.load())
        return 0;
    std::vector<PendingTakeDelete> before, batch;
    {
        std::lock_guard<std::mutex> g (takesLock);
        const auto now = juce::Time::getMillisecondCounter();
        for (size_t i = 0; i < takes.size(); ++i)
        {
            const auto& t = takes[i];
            if (takeStarredLocked (t) || t.handingOver)
                continue;
            PendingTakeDelete d;
            d.active = true;
            d.entry = t;
            d.index = i; // index in the list as it is now; undo restores oldest first
            d.afterTemp = i > 0 ? takes[i - 1].temp.getFullPathName() : juce::String();
            d.atMs = now;
            batch.push_back (d);
        }
        if (batch.empty())
            return 0;
        if (pendingDelete.active)
        {
            before.push_back (pendingDelete);
            before.insert (before.end(), pendingRest.begin(), pendingRest.end());
        }
        pendingDelete = batch.front();
        pendingRest.assign (batch.begin() + 1, batch.end());
        for (auto it = batch.rbegin(); it != batch.rend(); ++it) // erase from the back: indices stay valid
        {
            const auto& t = it->entry;
            if (pinnedTake == t.temp.getFullPathName() || pinnedTake == t.stable.getFullPathName())
                pinnedTake.clear();
            stableByTemp.erase (t.temp.getFullPathName());
            takes.erase (takes.begin() + (std::ptrdiff_t) it->index);
        }
    }
    if (! before.empty())
        takeDeletePool.addJob ([this, before] { for (const auto& b : before) finishTakeDelete (b); });
    const auto sel = audition.getSelected();
    if (sel != juce::File())
        for (const auto& d : batch)
            if (sel == d.entry.stable || sel == d.entry.temp || sel == DawWorker::partFileFor (d.entry.temp))
            {
                audition.select (juce::File(), getSourceFile());
                break;
            }
    return (int) batch.size();
}

bool HotStepLinkProcessor::waitTakeDeleteIdle (int timeoutMs)
{
    const auto start = juce::Time::getMillisecondCounter();
    while (takeDeletePool.getNumJobs() > 0)
    {
        if (juce::Time::getMillisecondCounter() - start > (juce::uint32) juce::jmax (0, timeoutMs))
            return false;
        juce::Thread::sleep (5);
    }
    return true;
}

// File IO. Never the audio thread, never under takesLock.
void HotStepLinkProcessor::finishTakeDelete (const PendingTakeDelete& p)
{
    const bool unsaved = p.entry.stable == juce::File();
    // One file: the lasting copy, or for a take without one its temp file (which may
    // still carry the .part ending). The temp copy of a saved take is left to the
    // normal removal of the temp session folder.
    auto target = unsaved ? p.entry.temp : p.entry.stable;
    if (unsaved && ! target.existsAsFile() && DawWorker::partFileFor (target).existsAsFile())
        target = DawWorker::partFileFor (target);
    // The star is checked again here, right before the move, on every commit path
    // (window lapse, next delete, editor closed: this pool job; processor destructor:
    // the closing thread). A take starred since its delete (a host state restore
    // refills the star list, or any other way) is not moved: it goes back into the
    // list like an undo, with a message. The check and the move are not one step (no
    // file call under takesLock), so a star set after this check does not stop the move.
    bool kept = false;
    {
        std::lock_guard<std::mutex> g (takesLock);
        if (takeStarredLocked (p.entry))
        {
            restoreTakeLocked (p);
            takeDeleteMessage = takeDeleteStarredText (p.entry.name);
            kept = true;
        }
    }
    if (kept)
        ++takeDeleteStarKeeps;
    // moved: done. kept: the file is where it was, the entry goes back. alreadyGone:
    // nothing to move, no error, and the entry is NOT put back (no take without a file).
    bool ok = true;
    if (! kept)
    {
        auto result = MoveToDeleted::alreadyGone;
        if (auto* fn = testTrashFile.load())
        {
            if (target.existsAsFile())
                result = fn (target) ? MoveToDeleted::moved : MoveToDeleted::kept;
        }
        else
            result = moveToDeletedFolder (target); // plain rename into <folder of the file>/Deleted
        if (result != MoveToDeleted::alreadyGone)
            ++takeDeleteMoves;
        ok = result != MoveToDeleted::kept;
    }
    if (! ok)
    {
        ++takeDeleteFailures;
        std::lock_guard<std::mutex> g (takesLock);
        restoreTakeLocked (p);
        takeDeleteMessage = takeDeleteFailedText (p.entry.name);
    }
    if (unsaved)
    {
        if ((kept || ! ok) && ! shuttingDown.load())
            requestSaveRetry();
    }
}

void HotStepLinkProcessor::syncStarMirror()
{
    juce::StringArray names;
    const auto list = apvts.state.getChildWithName (kStarredId);
    for (int i = 0; i < list.getNumChildren(); ++i)
        names.add (list.getChild (i).getProperty (kStarNameId).toString());
    std::lock_guard<std::mutex> g (takesLock);
    starredNames = names;
}

juce::File HotStepLinkProcessor::stableTakeFor (const DawJob& job, int takeIndex)
{
    const auto src = job.takeFiles[takeIndex];
    if (! src.existsAsFile())
        return {};
    const auto key = src.getFullPathName();
    {
        std::lock_guard<std::mutex> g (takesLock);
        auto it = stableByTemp.find (key);
        if (it != stableByTemp.end() && it->second.existsAsFile())
            return it->second;
    }
    const auto name = StableTakes::makeName (job.request.caption, job.request.expectedBackend,
                                             takeIndex + 1, job.request.bpm);
    const auto dest = copyStable (src, name);
    std::lock_guard<std::mutex> g (takesLock);
    if (dest != juce::File())
        stableByTemp[key] = dest;
    return dest;
}

bool HotStepLinkProcessor::isStarred (const juce::File& f) const
{
    const auto list = apvts.state.getChildWithName (kStarredId);
    return list.isValid() && list.getChildWithProperty (kStarNameId, DawWorker::takeNameOf (f)).isValid();
}

void HotStepLinkProcessor::setStarred (const juce::File& f, bool on)
{
    auto list = apvts.state.getOrCreateChildWithName (kStarredId, nullptr);
    const auto key = DawWorker::takeNameOf (f); // the take's name, also while its file is still <name>.part
    auto existing = list.getChildWithProperty (kStarNameId, key);
    if (on && ! existing.isValid())
    {
        juce::ValueTree t (kStarId);
        t.setProperty (kStarNameId, key, nullptr);
        list.appendChild (t, nullptr);
    }
    else if (! on && existing.isValid())
        list.removeChild (existing, nullptr);
    syncStarMirror();
}

juce::AudioProcessorEditor* HotStepLinkProcessor::createEditor()
{
    return new HotStepLinkEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HotStepLinkProcessor();
}

//==============================================================================
// DawWorker

void DawWorker::signalStop()
{
    signalThreadShouldExit();
    {
        std::lock_guard<std::mutex> g (lock);
        if (inFlight != nullptr)
            inFlight->cancel();
    }
    notify();
}

void DawWorker::stop()
{
    signalStop();
    // Every blocking call is interruptible (cancel above, exit checks between
    // read chunks), so this generous limit is never reached in practice.
    stopThread (kSubmitTimeoutMs + 15000);
}

void DawWorker::setEndpoint (const juce::String& baseUrl, const juce::String& dawKey)
{
    std::lock_guard<std::mutex> g (lock);
    if (url == baseUrl && key == dawKey)
        return;
    url = baseUrl;
    key = dawKey;
    status = DawStatus(); // new endpoint: forget the old server's state
    wakeForRetry = true;
    notify();
}

juce::String DawWorker::getBaseUrl() const
{
    std::lock_guard<std::mutex> g (lock);
    return url;
}

void DawWorker::retryNow()
{
    wakeForRetry = true;
    notify();
}

DawStatus DawWorker::getSnapshot() const
{
    std::lock_guard<std::mutex> g (lock);
    auto s = status;
    s.online = computeOnline (s, (juce::int64) juce::Time::getMillisecondCounter(), deadlineEnabled.load(),
                              offlineAfterMs.load(), offlineAfterFailures.load());
    return s;
}

bool DawWorker::computeOnline (const DawStatus& s, juce::int64 nowMs, bool useDeadline,
                               int limitMs, int limitFailures)
{
    if (s.heartbeatsOk == 0 || s.consecutiveFailures >= limitFailures)
        return false;
    if (useDeadline && nowMs - s.lastOkMs > limitMs)
    {
        // A request is waiting for its response headers. The blocking connect gives no
        // sign of life before they arrive, so the time limit is held off until the
        // request's own timeout has passed on our clock. The fields are set only when
        // the worker was online as the request was sent: this keeps a pill online, it
        // never turns one online.
        if (s.waitingTimeoutMs > 0 && nowMs - s.waitingSinceMs <= s.waitingTimeoutMs)
            return true;
        return false;
    }
    return true;
}

void DawWorker::noteServerOkMidRequest()
{
    if (! chunkStampsLife.load())
        return;
    std::lock_guard<std::mutex> g (lock);
    if (! threadShouldExit())
        noteServerOkLocked();
}

void DawWorker::noteServerOkLocked()
{
    // Proof of life from a request that is not the heartbeat. Only the two liveness
    // fields: heartbeatsOk stays, so the worker still needs one parsed status before it
    // is ever online, and the status fields keep the last heartbeat's values.
    if (! anyResponseIsLife.load())
        return;
    status.lastOkMs = (juce::int64) juce::Time::getMillisecondCounter();
    status.consecutiveFailures = 0;
}

bool DawWorker::parseStatus (const juce::String& body, DawStatus& s)
{
    const auto v = juce::JSON::parse (body);
    if (v.getDynamicObject() == nullptr)
        return false;
    s.engineModel = v["loaded_model"].isString() ? v["loaded_model"].toString() : juce::String();
    s.etaBackend = v["eta"]["backend"].isString() ? v["eta"]["backend"].toString().trim() : juce::String();
    s.baseAvailable = (bool) v["base_available"];
    s.nonTurboAvailable = (bool) v["non_turbo_available"];
    s.etaByBackend.clear();
    if (auto* eta = v["eta_by_backend"].getDynamicObject())
        for (auto& p : eta->getProperties())
        {
            DawEta e;
            e.baseS = (double) p.value["base_s"];
            e.perTakeS = (double) p.value["per_take_s"];
            e.perAudioS = (double) p.value["per_audio_s"];
            e.coldLoadS = (double) p.value["cold_load_s"];
            e.source = p.value["source"].toString();
            s.etaByBackend[p.name.toString()] = e;
        }
    return true;
}

bool DawWorker::heartbeat (juce::String& body, juce::String& err)
{
    juce::String base, k;
    {
        std::lock_guard<std::mutex> g (lock);
        base = url;
        k = key;
    }
    while (base.endsWithChar ('/'))
        base = base.dropLastCharacters (1);
    if (! gateRequest (base))
    {
        err = "the live server is switched off in this process";
        return false;
    }
    juce::WebInputStream in (juce::URL (base + "/api/v1/daw/status"), false);
    if (k.isNotEmpty())
        in.withExtraHeaders ("X-DAW-Key: " + k);
    in.withConnectionTimeout (kRequestTimeoutMs);
    {
        std::lock_guard<std::mutex> g (lock);
        if (threadShouldExit())
            return false;
        inFlight = &in;
    }
    const bool connected = in.connect (nullptr);
    const int code = connected ? in.getStatusCode() : 0;
    if (code == 200)
        body = in.readEntireStreamAsString();
    {
        std::lock_guard<std::mutex> g (lock);
        inFlight = nullptr;
    }
    if (! connected) { err = "connection failed"; return false; }
    if (code != 200) { err = "HTTP " + juce::String (code); return false; }
    return true;
}

void DawWorker::run()
{
    ExclusiveFile::ScopedThreadTag tag (ExclusiveFile::tagWorker);
    while (! threadShouldExit())
    {
        const auto now = (juce::int64) juce::Time::getMillisecondCounter();
        const bool retry = wakeForRetry.exchange (false);
        if (retry)
            backoffMs = 1000;
        if (retry || now >= nextHeartbeatMs)
        {
            juce::String body, err;
            bool ok = heartbeat (body, err);
            if (threadShouldExit())
                break;
            DawStatus parsed;
            if (ok && ! parseStatus (body, parsed))
            {
                ok = false;
                err = "bad status JSON";
            }
            int waitMs = kHeartbeatMs;
            {
                std::lock_guard<std::mutex> g (lock);
                if (ok)
                {
                    parsed.heartbeatsOk = status.heartbeatsOk + 1;
                    parsed.everOnline = true;
                    parsed.lastOkMs = (juce::int64) juce::Time::getMillisecondCounter();
                    status = parsed;
                    backoffMs = 1000;
                }
                else
                {
                    ++status.consecutiveFailures;
                    status.lastError = err;
                    if (! computeOnline (status, (juce::int64) juce::Time::getMillisecondCounter(), deadlineEnabled.load(),
                                         offlineAfterMs.load(), offlineAfterFailures.load()))
                    {
                        waitMs = backoffMs;
                        backoffMs = juce::jmin (5000, backoffMs * 2);
                    }
                }
            }
            nextHeartbeatMs = (juce::int64) juce::Time::getMillisecondCounter() + waitMs;
        }
        processJobs();
        if (threadShouldExit())
            break;
        auto waitMs = nextHeartbeatMs - (juce::int64) juce::Time::getMillisecondCounter();
        if (hasActiveJobs())
            waitMs = juce::jmin (waitMs, (juce::int64) 100);
        if (waitMs > 0)
            wait ((int) waitMs);
    }
}

//==============================================================================
// R4 job client

juce::String DawJobRequest::resolveModel (const juce::String& mode, const juce::String& backend, int aceModel)
{
    if (backend == "sa3" || backend == "yue2")
        return {};
    if (mode == "lego")
        return "base"; // turbo + lego is a 400 on the facade
    if (backend != "ace")
        return {};
    return aceModel == 1 ? juce::String ("turbo") : aceModel == 2 ? juce::String ("base") : juce::String();
}

juce::String DawJobRequest::toJson() const
{
    auto* o = new juce::DynamicObject();
    juce::var v (o);
    o->setProperty ("mode", mode);
    o->setProperty ("caption", caption);
    if (lyrics.isNotEmpty()) o->setProperty ("lyrics", lyrics);
    if (expectedBackend.isNotEmpty()) o->setProperty ("expected_backend", expectedBackend);
#if ! defined (HSLINK_TEETH_NO_MODEL)
    {
        const auto m = resolveModel (mode, expectedBackend, aceModel);
        if (m.isNotEmpty()) o->setProperty ("model", m);
    }
#endif
    if (durationS > 0.0) o->setProperty ("duration_s", durationS);
    o->setProperty ("takes", takes);
    if (bpm > 0.0) o->setProperty ("bpm", bpm);
    if (timesig.isNotEmpty()) o->setProperty ("timesig", timesig);
    if (key.isNotEmpty()) o->setProperty ("key", key);
    if (seed >= 0) o->setProperty ("seed", seed);
    // GUIDANCE / STEPS: Auto (sentinel) sends neither; the server 400s out of range, so clamp.
    if (guidance >= 0.0) o->setProperty ("guidance", juce::jlimit (0.0, 20.0, guidance));
    if (steps > 0) o->setProperty ("steps", juce::jlimit (1, 300, steps));
#if ! defined (HSLINK_TEETH_NO_REGION)
    if (hasRegion)
    {
        auto* rg = new juce::DynamicObject();
        rg->setProperty ("start_s", regionStartS);
        rg->setProperty ("end_s", regionEndS);
        o->setProperty ("region", juce::var (rg));
    }
#endif
    return juce::JSON::toString (v, true);
}

const char* DawJob::stateName (State s)
{
    switch (s)
    {
        case State::pending:   return "queued"; // not sent yet; same word as label()
        case State::queued:    return "queued";
        case State::running:   return "running";
        case State::done:      return "done";
        case State::partial:   return "partial";
        case State::failed:    return "failed";
        case State::cancelled: return "cancelled";
    }
    return "?";
}

juce::String DawJob::label (const DawJob& j)
{
    if (DawWorker::isWaiting (j))
        return j.cancelRequested ? "cancelled" : "queued";
    if (j.switchingTo.isNotEmpty())
        return "switching"; // asking the server for another backend, before the submit
    if (j.submitting)
        return "sending"; // the submit request is on its way, no server job id yet
    if (! j.isFinished())
        return "running";
    return stateName (j.state); // finished: done, partial, failed or cancelled
}

juce::String DawJob::headline (const DawJob& j)
{
    return j.switchingTo.isNotEmpty() ? "Switching to " + j.switchingTo + "..."
                                      : "Job " + label (j);
}

juce::String DawJob::etaText (const DawJob& j)
{
    if (j.isTerminal())
        return {};
    if (j.phase == "loading_model")
        return "  Loading model...";
    if (j.etaS >= 0.0 && std::isfinite (j.etaS))
        return "  ETA " + juce::String ((int) std::ceil (j.etaS)) + " s";
    return {};
}

juce::String DawJob::seedText (const juce::Array<juce::int64>& takeSeeds)
{
    int known = 0;
    bool allSame = true;
    for (auto sd : takeSeeds)
    {
        if (sd < 0) continue;
        ++known;
        if (takeSeeds[0] != sd) allSame = false;
    }
    if (known == 0)
        return {}; // nothing reported (random seed, or no takes listed): no seed part
    if (takeSeeds.size() > 1 && known == takeSeeds.size() && allSame)
        return "seed " + juce::String (takeSeeds[0]) + " requested (per-take seeds not reported)";
    juce::StringArray parts;
    for (auto sd : takeSeeds)
        parts.add (sd >= 0 ? juce::String (sd) : juce::String ("unknown"));
    return "seed " + parts.joinIntoString (", ");
}

PillState pillStateFor (bool online, const std::vector<DawJob>& jobs)
{
    if (! online)
        return PillState::offline;
    bool running = false, waiting = false;
    const DawJob* last = nullptr;
    for (const auto& j : jobs)
    {
        if (j.submitting || j.state == DawJob::State::running)
            running = true;
        else if (! j.isTerminal())
            waiting = true;
        if (j.finishSeq != 0 && (last == nullptr || j.finishSeq > last->finishSeq))
            last = &j;
    }
    if (running) return PillState::rendering;
    if (waiting) return PillState::queued;
    if (last != nullptr && last->state == DawJob::State::failed) return PillState::error;
    return PillState::idle;
}

const char* pillStateWord (PillState s)
{
    switch (s)
    {
        case PillState::offline:   return "offline";
        case PillState::queued:    return "queued";
        case PillState::rendering: return "rendering";
        case PillState::error:     return "error";
        case PillState::idle:      break;
    }
    return "idle";
}

juce::String DawStatus::onlinePillText (const juce::String& etaBackend, const juce::String& engineModel)
{
    const auto b = etaBackend.trim().toLowerCase();
    const auto m = engineModel.trim();
    if (b == "yue2") return "Online (YuE2)";
    if (b == "sa3")  return "Online (SA3)";
    if (b == "ace" || b.startsWith ("ace_"))
        return m.isNotEmpty() ? "Online (ACE: " + m + ")" : juce::String ("Online (ACE)");
    return m.isNotEmpty() ? "Online (" + m + ")" : juce::String ("Online (no model)");
}

DawJob::State DawJob::parseState (const juce::String& s, bool& ok)
{
    ok = true;
    if (s == "queued")    return State::queued;
    if (s == "running")   return State::running;
    if (s == "done")      return State::done;
    if (s == "partial")   return State::partial;
    if (s == "failed")    return State::failed;
    if (s == "cancelled") return State::cancelled;
    ok = false;
    return State::running;
}

juce::File DawWorker::tempRoot()
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("HSLINK_TEMP_ROOT", {});
    if (env.isNotEmpty() && juce::File::isAbsolutePath (env))
        return juce::File (env).getChildFile ("HOTSTEP_LINK"); // never the user folder itself
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("HOTSTEP_LINK");
}

std::atomic<bool> DawWorker::protectByDefault { true };
std::atomic<bool> DawWorker::legacyMarkerKeeps { true };
std::atomic<int> DawWorker::cleanupLockedSkips { 0 };
std::atomic<int> DawWorker::cleanupFoldersVisited { 0 };
std::atomic<bool> DawWorker::cleanupStopCheck { true };

bool DawWorker::isBookkeepingName (const juce::String& fileName)
{
    return fileName == ExclusiveFile::SessionLock::fileName() || fileName == savedLogName()
           || fileName == unsavedMarkerName();
}

bool DawWorker::appendSavedLog (const juce::File& folder, const juce::String& fileName, juce::int64 bytes,
                                const juce::String& lastingPath)
{
    if (fileName.isEmpty() || bytes <= 0 || ! folder.isDirectory())
        return false;
    // Opened at its end, one line, closed again: no handle is kept.
    juce::FileOutputStream os (folder.getChildFile (savedLogName()));
    if (! os.openedOk())
        return false;
    const auto tab = juce::String::charToString ((juce::juce_wchar) 9);
    os.writeText (fileName + tab + juce::String (bytes) + tab + lastingPath + juce::String::charToString ((juce::juce_wchar) 10),
                  false, false, nullptr);
    os.flush();
    return os.getStatus().wasOk();
}

juce::StringArray DawWorker::readSavedLog (const juce::File& folder)
{
    juce::StringArray keys, lines;
    const auto log = folder.getChildFile (savedLogName());
    if (! log.existsAsFile())
        return keys;
    const auto tab = juce::String::charToString ((juce::juce_wchar) 9);
    lines.addLines (log.loadFileAsString());
    for (const auto& line : lines)
    {
        // <name> TAB <bytes> TAB <lasting path>. Anything else is ignored.
        const auto name = line.upToFirstOccurrenceOf (tab, false, false);
        const auto rest = line.fromFirstOccurrenceOf (tab, false, false);
        const auto bytes = rest.upToFirstOccurrenceOf (tab, false, false);
        if (name.isEmpty() || bytes.isEmpty() || ! bytes.containsOnly ("0123456789") || ! rest.contains (tab)
            || rest.fromFirstOccurrenceOf (tab, false, false).trim().isEmpty())
            continue;
        keys.add (name + tab + juce::String (bytes.getLargeIntValue()));
    }
    return keys;
}

bool DawWorker::holdsUnsavedTake (const juce::File& folder)
{
    const auto tab = juce::String::charToString ((juce::juce_wchar) 9);
    const auto log = readSavedLog (folder);
    // A .part is also proven by a line under the .wav name it was to get.
    auto proven = [&] (const juce::File& f) {
        const auto rel = f.getRelativePathFrom (folder);
        const auto size = tab + juce::String (f.getSize());
        return log.contains (rel + size) || (rel.endsWithIgnoreCase (".wav.part") && log.contains (rel.dropLastCharacters (5) + size));
    };
    juce::StringArray listed;
    bool emptyMarker = false;
    const auto marker = folder.getChildFile (unsavedMarkerName());
    if (marker.existsAsFile())
    {
        const auto text = marker.loadFileAsString();
        juce::StringArray lines;
        lines.addLines (text);
        if (text.trim().isEmpty())
            emptyMarker = true; // lists nothing: stands for every take file here
        else if (lines[0].trim() != "HSLINK-UNSAVED v1")
            return true;        // a marker this version does not understand: keep the folder
        for (int i = 1; i < lines.size(); ++i)
        {
            const auto rel = lines[i].upToFirstOccurrenceOf (tab, false, false).trim();
            if (rel.isEmpty())
                continue;
            const auto src = folder.getChildFile (rel);
            if (! src.isAChildOf (folder))
                return true;    // a list the rescue does not trust: keep the folder
            listed.add (src.getFullPathName());
            if (src.existsAsFile() && src.getSize() > 0 && ! proven (src))
                return true;
        }
    }
    for (const auto& f : folder.findChildFiles (juce::File::findFiles, false, "*"))
    {
        const auto name = f.getFileName();
        const bool part = name.endsWithIgnoreCase (".wav.part");
        if (! part && ! name.endsWithIgnoreCase (".wav"))
            continue;
        if (isCaptureName (name) || f.getSize() <= 0)
            continue; // a capture, or a zero-byte file, which cannot be a complete take
        // A .part counts exactly when the rescue would copy it (isTakePart). Hook off:
        // the rule before it (listed either way, or any .part under an empty marker).
        if (part && (partRescueEnabled.load()
                         ? ! isTakePart (f, listed)
                         : (! emptyMarker && ! listed.contains (f.getFullPathName())
                            && ! listed.contains (f.getFullPathName().dropLastCharacters (5)))))
            continue;
        if (! proven (f))
            return true;
    }
    return false;
}

std::atomic<bool> DawWorker::partRescueEnabled { true };

bool DawWorker::isTakePart (const juce::File& part, const juce::StringArray& listed)
{
    const auto path = part.getFullPathName();
    if (! path.endsWithIgnoreCase (".wav.part") || ! part.existsAsFile() || part.getSize() <= 0)
        return false;
    if (listed.contains (path))
        return true;
    const auto wavPath = path.dropLastCharacters (5);
    if (listed.contains (wavPath) && ! juce::File (wavPath).existsAsFile())
        return true;
    return ExclusiveFile::isCompleteWav (part);
}

std::atomic<bool> DawWorker::zeroByteSkip { true };
std::atomic<int> DawWorker::testEmptyDownloads { 0 };
std::atomic<int> DawWorker::zeroByteSkips { 0 };

namespace
{
std::mutex locklessLock;
juce::StringArray locklessFolders;
}

void DawWorker::setLocklessFolder (const juce::File& folder, bool lockless)
{
    std::lock_guard<std::mutex> g (locklessLock);
    if (lockless)
        locklessFolders.addIfNotAlreadyThere (folder.getFullPathName());
    else
        locklessFolders.removeString (folder.getFullPathName());
}

bool DawWorker::isLocklessFolder (const juce::File& folder)
{
    if (ExclusiveFile::testNoLegacyGuard.load())
        return false; // teeth: no registry
    std::lock_guard<std::mutex> g (locklessLock);
    return locklessFolders.contains (folder.getFullPathName());
}

juce::Time DawWorker::newestContentTime (const juce::File& d)
{
    // With no content, the folder's own time. Folder times are unreliable on Windows
    // (children bump them, setting can fail).
    const auto folderTime = d.getLastModificationTime();
    juce::Time newest;
    bool anyContent = false;
    for (const auto& f : d.findChildFiles (juce::File::findFilesAndDirectories, true))
    {
        if (f.getParentDirectory() == d && isBookkeepingName (f.getFileName()))
            continue;
        anyContent = true;
        newest = juce::jmax (newest, f.getLastModificationTime());
    }
    return anyContent ? newest : folderTime;
}

int DawWorker::cleanupStaleTemp (const juce::File& root, juce::int64 maxAgeMs, juce::Time now,
                                 const std::function<bool()>& shouldStop)
{
    // Only ever inside a folder named HOTSTEP_LINK, never recursing outside it.
    if (! root.isDirectory() || root.getFileName() != "HOTSTEP_LINK")
        return 0;
    const auto cutoff = now - juce::RelativeTime::milliseconds (maxAgeMs);
    int removed = 0;
    auto stopped = [&shouldStop] { return shouldStop && cleanupStopCheck.load() && shouldStop(); };
    for (const auto& d : root.findChildFiles (juce::File::findDirectories, false, "session_*"))
    {
        // Closing: the folders not looked at yet stay exactly as they are.
        if (stopped())
            return removed;
        ++cleanupFoldersVisited;
        // Rule 2, age: the newest content file inside (so a long-running session is
        // kept). owner.lock, saved.log and the marker are not content. With no content,
        // the folder's own time, read before a lock file may be made in it. Folder
        // times are unreliable on Windows (children bump them, setting can fail).
        if (! (newestContentTime (d) < cutoff))
            continue;
        if (isLocklessFolder (d))
            continue; // a live instance in this process that runs without a lock
        const bool guard = unsavedGuard.load();
        const bool protect = guard && protectByDefault.load();
        // Hook off: the rule before saved.log (a marked folder is never deleted by age).
        if (guard && ! protect && d.getChildFile (unsavedMarkerName()).existsAsFile())
            continue;
        // Rule 1, the lock: only a folder whose owner.lock can be acquired has no live
        // owner. Busy = alive, whatever the file times say. Failed = cannot tell: kept. NoLockFile =
        // a folder of an older build (or a lockless instance): no lock file is made in
        // it; being this old it counts as dead and rule 3 decides.
        ExclusiveFile::SessionLock lock;
        const auto got = lock.tryAcquireExisting (d);
        if (got != ExclusiveFile::SessionLock::Result::Held && got != ExclusiveFile::SessionLock::Result::NoLockFile)
        {
            if (got == ExclusiveFile::SessionLock::Result::Busy)
                ++cleanupLockedSkips;
            continue;
        }
        if (got == ExclusiveFile::SessionLock::Result::NoLockFile)
        {
            if (! d.isDirectory())
                continue; // gone since the listing (another instance removed it): not counted
            // A folder with a marker and no lock file is never deleted by age: it can
            // be the folder of an older build that is still running.
            if (guard && legacyMarkerKeeps.load() && d.getChildFile (unsavedMarkerName()).existsAsFile())
                continue;
        }
        // Rule 3: age never deletes a take that is not proven saved.
        // W2 for age: an unknown file, an unknown subfolder or an unlisted (cut-short) .part
        // keeps the folder too. keepGuard false = teeth (NO_KEEP_GUARD): the old rule.
        if (protect && (holdsUnsavedTake (d)
                        || (HotStepLinkProcessor::keepGuard.load() && ! HotStepLinkProcessor::keptFolderCheck (d))))
            continue;
        if (HotStepLinkProcessor::removeLockedFolder (d, lock))
            ++removed;
    }
    // Stray capture WAVs (plugin-made source_*.wav only) left directly under the root.
    // Not recursive: nothing inside a session folder is matched here.
    if (stopped())
        return removed;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, false, "source_*.wav"))
        if (isCaptureName (f.getFileName()) && f.getLastModificationTime() < cutoff && f.deleteFile())
            ++removed;
    return removed;
}

juce::File DawWorker::defaultTakesFolder()
{
    return tempRoot()
        .getChildFile ("session_" + juce::String (juce::Time::currentTimeMillis()) + "_" + ExclusiveFile::uniqueToken (6));
}

std::atomic<bool> DawWorker::takeIdInName { true };

juce::String DawWorker::takeFileName (const DawJob& j, int n)
{
    if (! takeIdInName.load()) // teeth only: the old temp name
        return "job" + juce::String (j.localId) + "_" + j.serverId + "_take" + juce::String (n) + ".wav";
    return StableTakes::makeTakeName (j.request.caption, j.request.expectedBackend, n + 1, j.request.bpm);
}

juce::File DawWorker::partFileFor (const juce::File& take)
{
    return take.getSiblingFile (take.getFileName() + ".part");
}

std::atomic<bool> DawWorker::keepUnpublished { true };
std::atomic<int> DawWorker::testFailPublish { 0 };
std::atomic<int> DawWorker::publishAttempts { 0 };
std::atomic<bool> DawWorker::testExitAfterDownload { false };
std::atomic<bool> DawWorker::testNoLiveServer { false };
std::atomic<int> DawWorker::testLiveRedirectPort { 0 };
std::atomic<int> DawWorker::requestsOpened { 0 };
std::atomic<int> DawWorker::requestsRefused { 0 };

bool DawWorker::isLiveDefaultBase (const juce::String& baseUrl)
{
    auto rest = baseUrl.trim().toLowerCase();
    if (rest.contains ("://"))
        rest = rest.fromFirstOccurrenceOf ("://", false, false);
    rest = rest.upToFirstOccurrenceOf ("/", false, false);
    if (rest.contains ("@"))
        rest = rest.fromLastOccurrenceOf ("@", false, false);
    const int colon = rest.lastIndexOfChar (':');
    if (colon < 0)
        return false;
    const auto port = rest.substring (colon + 1);
    if (port.isEmpty() || ! port.containsOnly ("0123456789") || port.getIntValue() != kLivePort)
        return false;
    const auto host = rest.substring (0, colon);
    return host == "localhost" || host == "[::1]" || host == "0.0.0.0" || host.startsWith ("127.");
}

bool DawWorker::gateRequest (juce::String& base)
{
    if (isLiveDefaultBase (base))
    {
        if (testNoLiveServer.load())
        {
            ++requestsRefused;
            return false;
        }
        if (const int port = testLiveRedirectPort.load())
            base = "http://127.0.0.1:" + juce::String (port);
    }
    ++requestsOpened;
    return true;
}
std::atomic<bool> DawWorker::captureByForm { true };

int DawWorker::publishWaitMs (int tryIndex)
{
    static const int waits[] = { 50, 100, 200, 400, 750 };
    return waits[juce::jlimit (0, 4, tryIndex)];
}

bool DawWorker::publishDownload (const juce::File& part, const juce::File& take, const std::function<bool()>& shouldStop)
{
    if (! part.existsAsFile())
        return take.existsAsFile();
    const bool keep = keepUnpublished.load();
    const int tries = keep ? kPublishTries : 1;
    for (int i = 0; i < tries; ++i)
    {
        ++publishAttempts;
        int forced = testFailPublish.load();
        while (forced > 0 && ! testFailPublish.compare_exchange_weak (forced, forced - 1)) {}
        if (forced <= 0 && ExclusiveFile::moveNoReplace (part, take))
            return true;
        // The rename reported failure but took effect (the part is gone): the answer
        // is whether the take file is there, and no further try is made.
        if (! part.existsAsFile())
            return take.existsAsFile();
        if (i + 1 >= tries)
            break;
        // The file is usually held for tens to hundreds of milliseconds. Sleeps of
        // 10 ms, so a close is seen within one of them.
        for (int waited = 0, total = publishWaitMs (i); waited < total; waited += 10)
        {
            if (shouldStop && shouldStop())
                return false;
            juce::Thread::sleep (10);
        }
        if (shouldStop && shouldStop())
            return false;
    }
    if (! keep)
        part.deleteFile(); // teeth only: the old behaviour
    return false;
}

juce::String DawWorker::takeNameOf (const juce::File& tempFile)
{
    const auto n = tempFile.getFileName();
    return n.endsWithIgnoreCase (".wav.part") ? n.dropLastCharacters (5) : n;
}

void DawWorker::notePartKept (const juce::File& part)
{
    std::lock_guard<std::mutex> g (keptLock);
    keptPart = part;
}

bool DawWorker::isPartKept (const juce::File& part) const
{
    std::lock_guard<std::mutex> g (keptLock);
    return keptPart == part;
}

juce::String DawWorker::captureFileName (int serial)
{
    return "source_" + juce::String (serial) + ".wav";
}

bool DawWorker::isCaptureName (const juce::String& fileName)
{
    if (! captureByForm.load())
        return fileName.startsWith ("source_"); // teeth only: the old test
    if (! fileName.startsWith ("source_") || ! fileName.endsWith (".wav"))
        return false;
    const auto digits = fileName.substring (7, fileName.length() - 4);
    return digits.isNotEmpty() && digits.containsOnly ("0123456789");
}

void DawWorker::setTakesFolder (const juce::File& f)
{
    std::lock_guard<std::mutex> g (lock);
    takesFolder = f;
}

juce::File DawWorker::getTakesFolder() const
{
    std::lock_guard<std::mutex> g (lock);
    return takesFolder;
}

int DawWorker::submitGenerate (const DawJobRequest& r)
{
    DawJob j;
    j.request = r;
    int id;
    {
        std::lock_guard<std::mutex> g (lock);
        if (serialQueue.load())
        {
            int waiting = 0;
            for (auto& q : jobs)
                if (isWaiting (q) && ! q.cancelRequested)
                    ++waiting;
            if (waiting >= kMaxQueued)
                return 0;
        }
        id = j.localId = nextLocalId++;
        jobs.push_back (j);
    }
    notify();
    return id;
}

int DawWorker::submitUnderstand (const juce::File& wav)
{
    DawJob j;
    j.isUnderstand = true;
    j.understandWav = wav;
    int id;
    {
        std::lock_guard<std::mutex> g (lock);
        id = j.localId = nextLocalId++;
        jobs.push_back (j);
    }
    notify();
    return id;
}

void DawWorker::cancelJob (int localId)
{
    {
        std::lock_guard<std::mutex> g (lock);
        for (auto& j : jobs)
            if (j.localId == localId)
            {
                j.cancelRequested = true;
                // The only request in flight while this is set is the job's own backend
                // switch: end it now so Cancel does not wait for the server's answer.
                if (j.switchingTo.isNotEmpty() && inFlight != nullptr)
                    inFlight->cancel();
            }
    }
    notify();
}

bool DawWorker::removeQueued (int localId)
{
    std::lock_guard<std::mutex> g (lock);
    for (auto it = jobs.begin(); it != jobs.end(); ++it)
        if (it->localId == localId)
        {
            if (! isWaiting (*it))
                return false; // being sent, sent, or finished: Cancel is the way out
            jobs.erase (it);
            return true;
        }
    return false;
}

juce::String DawWorker::getLastOrphanId() const
{
    std::lock_guard<std::mutex> g (lock);
    return lastOrphanId;
}

std::vector<DawJob> DawWorker::getJobs() const
{
    std::lock_guard<std::mutex> g (lock);
    return jobs;
}

bool DawWorker::getJob (int localId, DawJob& out) const
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& j : jobs)
        if (j.localId == localId)
        {
            out = j;
            return true;
        }
    return false;
}

bool DawWorker::hasActiveJobs() const
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& j : jobs)
        if (! j.isFinished())
            return true;
    return false;
}

juce::String DawWorker::extractServerId (const juce::var& v, bool isUnderstand)
{
    if (isUnderstand)
    {
        const auto a = v["analysis_id"].toString();
        if (a.isNotEmpty())
            return a;
    }
    return v["id"].toString();
}

bool DawWorker::isSafeServerId (const juce::String& id)
{
    // ^[A-Za-z0-9-]{1,64}$ : the id goes into a file name and a URL path.
    if (id.isEmpty() || id.length() > 64)
        return false;
    for (auto c : id)
        if (! ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    return true;
}

bool DawWorker::applyJobJson (const juce::String& body, DawJob& j)
{
    const auto v = juce::JSON::parse (body);
    if (v.getDynamicObject() == nullptr)
        return false;
    bool ok = false;
    const auto st = DawJob::parseState (v["state"].toString(), ok);
    if (! ok)
        return false;
    j.state = st;
    if (! v["progress"].isVoid())   j.progress = juce::jlimit (0.0, 1.0, (double) v["progress"]);
    if (! v["eta_s"].isVoid())      j.etaS = (double) v["eta_s"];
#if ! defined (HSLINK_TEETH_NO_PHASE)
    // Optional (newer servers): absent on old servers, so phase stays empty and ETA shows as before.
    j.phase = v["phase"].isString() ? v["phase"].toString() : juce::String();
#endif
    if (! v["take_count"].isVoid()) j.takeCount = (int) v["take_count"];
    if (! v["takes_done"].isVoid())
        j.takesReady = (int) v["takes_done"];
    else if (const auto* takes = v["takes"].getArray())
    {
        // Non-sequential jobs carry no takes_done: count ready entries (or all, if none say).
        int ready = 0, flagged = 0;
        for (const auto& t : *takes)
            if (! t["ready"].isVoid()) { ++flagged; if ((bool) t["ready"]) ++ready; }
        j.takesReady = flagged > 0 ? ready : takes->size();
    }
    if (v["error"].isString())      j.error = v["error"].toString();
    if (! v["bpm"].isVoid())        j.bpm = (double) v["bpm"];
    if (v["key"].isString())        j.key = v["key"].toString();
    if (v["dit_model"].isString())  j.ditModel = v["dit_model"].toString();
    if (const auto* tk = v["takes"].getArray())
    {
        j.takeSeeds.clearQuick();
        for (const auto& t : *tk)
            j.takeSeeds.add (t["seed"].isVoid() || t["seed"].isUndefined() || t["seed"].isString()
                                 ? (juce::int64) -1 : (juce::int64) t["seed"]);
    }
    if (v["timesig"].isString())    j.timesig = v["timesig"].toString();
    if (const auto* tg = v["tags"].getArray())
    {
        j.tags.clearQuick();
        for (const auto& t : *tg)
            if (t.toString().trim().isNotEmpty())
                j.tags.addIfNotAlreadyThere (t.toString().trim());
    }
    else if (v["tags"].isString())
    {
        j.tags.clearQuick();
        j.tags.addTokens (v["tags"].toString(), ",", "\"");
        j.tags.trim();
        j.tags.removeEmptyStrings();
        j.tags.removeDuplicates (false);
    }
    if (st == DawJob::State::done)
        j.progress = 1.0;
    return true;
}

juce::String DawWorker::errorFromBody (const juce::String& body, int code)
{
    if (code == 0)
        return "connection failed";
    const auto v = juce::JSON::parse (body);
    const juce::String m = v["error"]["message"].toString();
    const juce::String c = v["error"]["code"].toString();
    juce::String t ("HTTP " + juce::String (code));
    if (c.isNotEmpty()) t << " " << c;
    if (m.isNotEmpty()) t << ": " << m;
    return t;
}

int DawWorker::request (const juce::String& method, const juce::String& path, const juce::String& jsonBody,
                        const juce::String& fileField, const juce::File& file, const juce::String& formField,
                        juce::String& body, const juce::File& downloadTo, int timeoutOverrideMs,
                        int cancelLocalId)
{
    juce::String base, k;
    {
        std::lock_guard<std::mutex> g (lock);
        base = url;
        k = key;
    }
    while (base.endsWithChar ('/'))
        base = base.dropLastCharacters (1);
    if (! gateRequest (base))
        return 0; // reads as "connection failed"
    const bool post = method == "POST";
    juce::URL u (base + path);
    juce::String headers;
    if (k.isNotEmpty())
        headers << "X-DAW-Key: " << k << "\r\n";
    if (fileField.isNotEmpty())
    {
        if (formField.isNotEmpty())
            u = u.withParameter (formField, jsonBody);
        u = u.withFileToUpload (fileField, file, "audio/wav");
    }
    else if (post)
    {
        u = u.withPOSTData (jsonBody.isNotEmpty() ? jsonBody : juce::String ("{}"));
        headers << "Content-Type: application/json\r\n";
    }
    juce::WebInputStream in (u, post);
    if (headers.isNotEmpty())
        in.withExtraHeaders (headers);
    // Short timeouts are for connect/heartbeat only; shutdown goes through cancel + exit checks.
    int timeoutMs = kJobTimeoutMs;
    if (interruptible.load())
        timeoutMs = ! splitTimeouts.load() ? kRequestTimeoutMs
                  : downloadTo != juce::File() ? kStallTimeoutMs // per read: no bytes for 15 s = fail
                  : post ? kSubmitTimeoutMs
                  : kJobTimeoutMs;
    if (timeoutOverrideMs > 0)
        timeoutMs = timeoutOverrideMs;
    in.withConnectionTimeout (timeoutMs);
    {
        std::lock_guard<std::mutex> g (lock);
        if (threadShouldExit())
            return 0;
        // cancelJob ends a published request. A cancel that came before this point found
        // none, so it is read here, in the locked step that publishes: nothing is sent.
        if (cancelWaitingLocked (cancelLocalId))
        {
            skippedForCancel = true;
            return 0;
        }
        inFlight = &in;
        // From here until connect returns nothing can be learned about the server. An
        // online worker stays online for that wait, at most this request's timeout.
        if (waitingHoldsOnline.load())
        {
            const auto sentMs = (juce::int64) juce::Time::getMillisecondCounter();
            if (computeOnline (status, sentMs, deadlineEnabled.load(), offlineAfterMs.load(), offlineAfterFailures.load()))
            {
                status.waitingSinceMs = sentMs;
                status.waitingTimeoutMs = timeoutMs;
            }
        }
    }
    const bool connected = in.connect (nullptr);
    int code = connected ? in.getStatusCode() : 0;
    {
        // Headers are in, or the connect failed: the wait is over either way. Cleared
        // in the same locked step as the stamp, so no snapshot sees neither.
        std::lock_guard<std::mutex> g (lock);
        status.waitingSinceMs = 0;
        status.waitingTimeoutMs = 0;
        if (connected && code >= 200 && code < 300 && chunkStampsLife.load() && ! threadShouldExit())
            noteServerOkLocked();
    }
    if (connected)
    {
        if (code == 200 && downloadTo != juce::File())
        {
            downloadTo.deleteFile();
            bool ok = false;
            {
                juce::FileOutputStream os (downloadTo);
                if (os.openedOk())
                {
                    if (! interruptible.load())
                    {
                        os.writeFromInputStream (in, -1); // test hook: legacy path (teeth)
                        ok = true;
                    }
                    else
                    {
                        // Chunked so shutdown never waits on a whole download.
                        const juce::int64 expected = in.getTotalLength(); // -1 when no Content-Length
                        juce::HeapBlock<char> chunk (65536);
                        juce::int64 got = 0;
                        bool writeOk = true;
                        while (! threadShouldExit() && ! in.isExhausted())
                        {
                            const int n = in.read (chunk.get(), 65536);
                            if (n <= 0)
                                break;
                            if (! os.write (chunk.get(), (size_t) n))
                            {
                                writeOk = false;
                                break;
                            }
                            got += n;
                            noteServerOkMidRequest(); // a slow download is a live server
                        }
                        os.flush();
                        ok = writeOk && ! threadShouldExit() && ! in.isError() && os.getStatus().wasOk()
                             && (expected < 0 || got == expected);
                    }
                }
            }
            if (! ok)
            {
                downloadTo.deleteFile(); // never leave a partial take on disk
                body = "download incomplete";
                code = 0;
            }
        }
        else
            body = in.readEntireStreamAsString();
    }
    {
        std::lock_guard<std::mutex> g (lock);
        inFlight = nullptr;
        // The one place every non-heartbeat response returns. Good = the server answered
        // HTTP 2xx and the answer was read to the end without a stream error (a download
        // that came up short was set to code 0 above). Anything else leaves the liveness
        // fields alone: only a failed heartbeat counts as a failure.
        if (code >= 200 && code < 300 && ! in.isError() && ! threadShouldExit())
            noteServerOkLocked();
    }
    return code;
}

std::atomic<DawWorker::BackendCallFn> DawWorker::testBackendCall { nullptr };
std::atomic<DawWorker::BeforeBackendCallFn> DawWorker::testBeforeBackendCall { nullptr };
std::atomic<DawWorker::BeforeStepFn> DawWorker::testBeforeStep { nullptr };

bool DawWorker::cancelWaitingLocked (int localId) const
{
    if (localId == 0 || ! cancelBeforePublish.load())
        return false;
    for (auto& s : jobs)
        if (s.localId == localId)
            return s.cancelRequested;
    return false;
}

juce::String DawWorker::backendLabel (const juce::String& id)
{
    if (id == "sa3")  return "SA3";
    if (id == "yue2") return "YuE2";
    if (id == "ace")  return "ACE";
    return id;
}

juce::String DawWorker::switchFailText (const juce::String& label, int code, const juce::String& body)
{
    const auto v = juce::JSON::parse (body);
    // The main API answers { error: "text" }, the facade { error: { message } }.
    juce::String m = v["error"].isString() ? v["error"].toString() : v["error"]["message"].toString();
    juce::String t ("Could not switch to " + label + ": ");
    if (code == 0)
        return t + "no answer from HOT-Step within " + juce::String (kSwitchTimeoutMs / 1000)
                 + " s. Check that HOT-Step is running, then press Generate again.";
    if (code == 404)
        return t + "this HOT-Step has no " + label + " backend. Pick another backend or Auto.";
    if (code == 200)
        m = "it still reports " + (v["activeId"].isString() ? backendLabel (v["activeId"].toString())
                                                             : juce::String ("another backend")) + " as active";
    const bool busy = code == 409 || code == 423 || code == 429 || code == 503;
    t << (busy ? "HOT-Step is busy" : "HOT-Step refused");
    if (code != 200)
        t << " (HTTP " << code << (m.isNotEmpty() ? ": " + m : juce::String()) << ")";
    else
        t << " (" << m << ")";
    return t + (busy ? ". Wait for it to finish, then press Generate again."
                     : ". Switch the backend in HOT-Step or pick Auto, then press Generate again.");
}

juce::String DawWorker::getSwitchingTo() const
{
    std::lock_guard<std::mutex> g (lock);
    for (auto& j : jobs)
        if (j.switchingTo.isNotEmpty())
            return j.switchingTo;
    return {};
}

bool DawWorker::ensureBackend (DawJob& j)
{
    const auto want = j.request.expectedBackend;
    if (want.isEmpty() || ! switchBackend.load())
        return true;
    const auto call = [this, &j] (const char* method, const char* path, const juce::String& json, juce::String& body)
    {
        skippedForCancel = false;
        if (auto* hook = testBeforeBackendCall.load())
            hook (method, path);
        if (auto* fn = testBackendCall.load())
        {
            // The stand-in publishes nothing, so the same cancel check is made here.
            {
                std::lock_guard<std::mutex> g (lock);
                skippedForCancel = cancelWaitingLocked (j.localId);
            }
            return skippedForCancel ? 0 : fn (method, path, json, body);
        }
        return request (method, path, json, {}, {}, {}, body, {}, kSwitchTimeoutMs, j.localId);
    };
    const auto label = backendLabel (want);
    // Shows the switch on the listed entry (queue row, status pill) and reads a cancel.
    const auto mark = [this, &j] (const juce::String& text)
    {
        std::lock_guard<std::mutex> g (lock);
        bool cancel = false;
        for (auto& s : jobs)
            if (s.localId == j.localId)
            {
                s.switchingTo = text;
                cancel = s.cancelRequested;
            }
        return cancel;
    };
    // Marked before the question, so the row goes queued, switching, sending and never
    // reads "sending" while the backend list is being asked for. Every return below this
    // line is preceded by mark ({}), which clears it again.
    bool cancelled = markBeforeQuestion.load() ? mark (label) : false; // false: teeth only
    juce::String body;
    int code = 0;
    if (! cancelled)
    {
        code = call ("GET", "/api/backends", {}, body);
        if (threadShouldExit())
        {
            mark ({});
            return false;
        }
        // No backend list (an older server, the contract stub, no answer): submit as before
        // and let the server's own check speak. Same when the wanted backend is active.
        const auto listed = juce::JSON::parse (body);
        if (code != 200 || ! listed["activeId"].isString() || listed["activeId"].toString() == want)
        {
            if (! mark ({}))
                return true;
            j.state = DawJob::State::cancelled; // cancelled while the question was out
            return false;
        }
        cancelled = mark (label); // reads a cancel that arrived during the question
    }
    if (! cancelled)
    {
        ++switchesSent;
        juce::DynamicObject::Ptr o (new juce::DynamicObject());
        o->setProperty ("id", want);
        code = call ("POST", "/api/backends/active", juce::JSON::toString (juce::var (o.get()), true), body);
        if (skippedForCancel)
            --switchesSent; // cancelled before it was published: no switch went out
    }
    cancelled = mark ({}) || cancelled;
    if (threadShouldExit())
        return false;
    if (cancelled)
    {
        j.state = DawJob::State::cancelled;
        return false;
    }
    if (code == 200 && juce::JSON::parse (body)["activeId"].toString() == want)
        return true;

    j.state = DawJob::State::failed;
    j.error = switchFailText (label, code, body);
    // The jobs waiting behind it for the same backend would each ask again and fail the
    // same way. They are cancelled here with the reason; nothing is retried without the
    // user, because a retry is a new switch request to a server that just said no.
    int held = 0;
    {
        std::lock_guard<std::mutex> g (lock);
        for (auto& s : jobs)
            if (s.localId != j.localId && isWaiting (s) && ! s.cancelRequested && s.request.expectedBackend == want)
            {
                ++held;
                s.state = DawJob::State::cancelled;
                s.error = "Not sent: the switch to " + label + " failed (see job #" + juce::String (j.localId) + ").";
                s.finishSeq = nextFinishSeq++; // before the failed job, whose message stays the newest
            }
    }
    if (held > 0)
        j.error << " " << held << (held == 1 ? " job" : " jobs") << " queued behind it for " << label
                << (held == 1 ? " was" : " were") << " cancelled, not sent.";
    return false;
}

void DawWorker::processJobs()
{
    std::vector<DawJob> work;
    {
        std::lock_guard<std::mutex> g (lock);
        const bool serial = serialQueue.load();
        bool busy = false;
        for (auto& j : jobs)
            if (! j.isUnderstand && ! j.isFinished() && ! isWaiting (j))
                busy = true;
        for (auto& j : jobs)
        {
            if (j.isFinished())
                continue;
            if (serial && isWaiting (j) && ! j.cancelRequested)
            {
                if (busy)
                    continue; // waits its turn
                busy = true; // the first waiting job goes now
            }
            // Picked for its submit POST: from here it is in flight, not waiting, so it
            // cannot be removed while the POST runs without the lock.
            const bool picked = j.state == DawJob::State::pending && ! j.cancelRequested && submitMark.load();
            if (picked)
                j.submitting = true;
            const bool first = work.empty();
            work.push_back (j);
            // The switch mark goes on the listed entry in the same locked step, so the row
            // never reads "sending" between here and ensureBackend's own first mark. Set
            // after the copy: the work copy stays unmarked, ensureBackend clears the listed
            // entry on every way out, and the write-back after stepJob clears it too. Only
            // for the first job of the pass: cancelJob ends the request in flight for a
            // marked job, and that request must be this job's own, not an earlier job's.
            if (picked && first && ! j.isUnderstand && j.request.expectedBackend.isNotEmpty() && switchBackend.load()
                && earlySwitchMark.load())
                j.switchingTo = backendLabel (j.request.expectedBackend);
        }
        pruneFinishedLocked();
    }
    for (auto& j : work)
    {
        if (threadShouldExit())
            return;
        if (auto* hook = testBeforeStep.load())
            hook (j.localId);
        stepJob (j);
        j.submitting = false;
        bool found = false;
        {
            std::lock_guard<std::mutex> g (lock);
            for (auto& s : jobs)
                if (s.localId == j.localId)
                {
                    const bool cancel = s.cancelRequested || j.cancelRequested;
                    s = j;
                    s.cancelRequested = cancel; // a cancel may arrive while stepJob ran
                    if (s.isFinished() && s.finishSeq == 0)
                        s.finishSeq = nextFinishSeq++; // finish order, read by the prune
                    found = true;
                }
            if (pruneOnFinish.load())
                pruneFinishedLocked(); // prune as soon as a job finishes, not on the next pass
        }
        // The local job went away while its request ran. A live server job with no local
        // owner would never be cancelled or downloaded, so cancel it here (lock not held).
        if (! found && ! j.isUnderstand && ! j.isTerminal() && j.serverId.isNotEmpty()
            && isSafeServerId (j.serverId) && orphanGuard.load() && ! threadShouldExit())
        {
            juce::String cb;
            request ("POST", "/api/v1/daw/jobs/" + juce::URL::addEscapeChars (j.serverId, true) + "/cancel", "{}", {}, {}, {}, cb);
            juce::Logger::writeToLog ("HOT-STEP LINK: job #" + juce::String (j.localId)
                                      + " was gone when its request returned; cancelled server job " + j.serverId);
            {
                std::lock_guard<std::mutex> g (lock);
                lastOrphanId = j.serverId;
            }
            ++orphansCancelled;
        }
    }
}

// Caller holds lock. Keeps at most kKeepFinished finished generate job CARDS. Takes are
// not owned here (the processor's take list has them), so this never loses audio.
// Order: takeless failed / cancelled cards go first, oldest first, except the card of
// the job that finished most recently (highest finishSeq, so the error of the job that
// just ended stays readable); then the oldest finished cards. Only finished jobs are
// ever erased: a waiting, sending, running or still downloading job is never dropped.
void DawWorker::pruneFinishedLocked()
{
    int finished = 0, newestId = 0;
    juce::uint64 newestSeq = 0;
    const bool bySeq = finishSeqRule.load();
    for (auto& j : jobs)
        if (! j.isUnderstand && j.isFinished())
        {
            ++finished;
            if (j.finishSeq == 0)
                j.finishSeq = nextFinishSeq++; // finished outside the publish path
            if (! bySeq || j.finishSeq >= newestSeq)
            {
                newestSeq = j.finishSeq;
                newestId = j.localId;
            }
        }
    if (pruneTakelessFirst.load())
        for (auto it = jobs.begin(); finished > kKeepFinished && it != jobs.end();)
            if (! it->isUnderstand && it->isFinished() && it->takeFiles.isEmpty() && it->localId != newestId
                && (it->state == DawJob::State::failed || it->state == DawJob::State::cancelled))
            {
                it = jobs.erase (it);
                --finished;
            }
            else
                ++it;
    for (auto it = jobs.begin(); finished > kKeepFinished && it != jobs.end();)
        if (! it->isUnderstand && it->isFinished())
        {
            it = jobs.erase (it);
            --finished;
        }
        else
            ++it;
}

void DawWorker::stepJob (DawJob& j)
{
    using S = DawJob::State;
    const auto now = (juce::int64) juce::Time::getMillisecondCounter();
    juce::String body;

    if (j.state == S::pending)
    {
        if (j.cancelRequested)
        {
            j.state = S::cancelled;
            return;
        }
        if (! j.isUnderstand && ! ensureBackend (j))
            return;
        int code;
        if (j.isUnderstand)
            code = request ("POST", "/api/v1/daw/understand", {}, "audio", j.understandWav, {}, body);
        else if (j.request.sourceWav.existsAsFile())
            code = request ("POST", "/api/v1/daw/generate", j.request.toJson(), "src_audio",
                            j.request.sourceWav, j.request.formField, body);
        else
            code = request ("POST", "/api/v1/daw/generate", j.request.toJson(), {}, {}, {}, body);
        if (threadShouldExit())
            return;
        if (code == 200 || code == 202)
        {
            const auto v = juce::JSON::parse (body);
            j.serverId = extractServerId (v, j.isUnderstand);
            if (v["dit_model"].isString())
                j.ditModel = v["dit_model"].toString();
            if (j.serverId.isEmpty())
            {
                j.state = S::failed;
                j.error = "no job id in response";
                return;
            }
            if (! isSafeServerId (j.serverId))
            {
                j.state = S::failed;
                j.error = "bad job id from server";
                return;
            }
            j.state = j.isUnderstand ? S::running : S::queued;
            if (! v["eta_s"].isVoid()) j.etaS = (double) v["eta_s"];
            j.takeCount = j.request.takes;
            j.nextPollMs = now + kPollMs;
        }
        else
        {
            j.state = S::failed;
            j.error = errorFromBody (body, code);
        }
        return;
    }

    if (j.state == S::queued || j.state == S::running)
    {
        bool pollNow = now >= j.nextPollMs;
        if (j.cancelRequested && ! j.cancelSent && ! j.isUnderstand)
        {
#if ! defined (HSLINK_TEETH_NO_CANCEL)
            juce::String cb;
            request ("POST", "/api/v1/daw/jobs/" + juce::URL::addEscapeChars (j.serverId, true) + "/cancel", "{}", {}, {}, {}, cb);
#endif
            j.cancelSent = true;
            pollNow = true;
        }
        if (! pollNow)
            return;
        const auto path = j.isUnderstand ? "/api/v1/daw/understand/" + juce::URL::addEscapeChars (j.serverId, true)
                                         : "/api/v1/daw/jobs/" + juce::URL::addEscapeChars (j.serverId, true);
        const int code = request ("GET", path, {}, {}, {}, {}, body);
        if (threadShouldExit())
            return;
        j.nextPollMs = (juce::int64) juce::Time::getMillisecondCounter() + kPollMs;
        if (code == 200 && applyJobJson (body, j))
        {
            j.pollFailures = 0;
            if (j.state == S::failed && j.error.isEmpty())
                j.error = "job failed";
        }
        else if (code >= 400 && code < 500)
        {
            j.state = S::failed;
            j.error = errorFromBody (body, code);
        }
        else if (++j.pollFailures >= kMaxPollFailures)
        {
            j.state = S::failed;
            j.error = "lost the job: " + errorFromBody (body, code);
        }
        if (! j.isTerminal())
            return;
    }

    if ((j.state == S::done || j.state == S::partial) && ! j.isUnderstand && ! j.downloadsComplete)
    {
        const auto folder = getTakesFolder();
        folder.createDirectory();
        for (int n = j.takeFiles.size(); n < j.takesReady; ++n)
        {
            // The take's name, with its id, is chosen once, here. The bytes go to
            // <name>.part and get the name only when the download is complete.
            auto f = folder.getChildFile (takeFileName (j, n));
            const bool ids = takeIdInName.load();
            for (int tries = 0; ids && tries < 8 && (f.exists() || partFileFor (f).exists()); ++tries)
                f = folder.getChildFile (takeFileName (j, n));
            const auto part = partFileFor (f);
            juce::String eb;
            const int code = request ("GET", "/api/v1/daw/jobs/" + juce::URL::addEscapeChars (j.serverId, true) + "/audio?take=" + juce::String (n),
                                      {}, {}, {}, {}, eb, part);
            if (testExitAfterDownload.load())
                signalThreadShouldExit(); // test hook: the close arrives as the download ends
            // request() gives 200 with the file only for a download that ran to its
            // end (every byte of Content-Length written, no exit seen up to the last
            // check). Such a file is a complete take even when the exit is signalled
            // right after: it is recorded below, never deleted.
            const bool exiting = threadShouldExit();
            if (code == 200 && testEmptyDownloads.load() > 0 && testEmptyDownloads.fetch_sub (1) > 0)
            {
                part.deleteFile(); // test hook: a 200 with an empty body
                part.create();
            }
            // A 200 with no bytes (or no file) is not a take: it fails like any incomplete download.
            const bool empty = code == 200 && zeroByteSkip.load() && (! part.existsAsFile() || part.getSize() <= 0);
            const bool complete = code == 200 && part.existsAsFile() && ! empty;
            if (exiting && ! complete)
            {
                part.deleteFile(); // a download cut short never becomes a take
                return;
            }
            if (! complete)
            {
                part.deleteFile();
                if (j.error.isEmpty())
                    j.error = "take " + juce::String (n) + ": "
                              + (empty || (code == 0 && eb == "download incomplete") ? juce::String ("download incomplete")
                                                                          : errorFromBody (eb, code));
                break;
            }
            j.takeFiles.add (f);
            notePartKept ({}); // the slot is for this take only
            if (onTakeDownloaded)
                onTakeDownloaded (j, j.takeFiles.size() - 1); // record, publish, lasting copy: now, before any prune
            if (isPartKept (part))
                j.takeFiles.set (j.takeFiles.size() - 1, part); // the callback keeps the take in its .part file
            else if (! publishDownload (part, f, [this] { return threadShouldExit(); })) // no callback, or one that left the part alone
            {
                if (keepUnpublished.load() && part.existsAsFile())
                {
                    // Complete bytes that could not be renamed: the .part file is the take's file.
                    j.takeFiles.set (j.takeFiles.size() - 1, part);
                }
                else
                {
                    j.takeFiles.remove (j.takeFiles.size() - 1);
                    if (j.error.isEmpty())
                        j.error = "take " + juce::String (n) + ": the downloaded file could not be put in place";
                    break;
                }
            }
        }
        if (j.takeFiles.size() < j.takesReady)
            j.state = j.takeFiles.isEmpty() ? S::failed : S::partial;
        j.downloadsComplete = true;
    }
}

//==============================================================================
// Host metadata for job requests

void HotStepLinkProcessor::fillHostMetadata (DawJobRequest& r, const HostSyncState& s, bool isStandalone, double manualBpm)
{
    if (! isStandalone && s.hostSyncActive.load (std::memory_order_relaxed))
    {
        // No host tempo: bpm stays 0 so the request omits it, and no loop duration is
        // derived from the stored default.
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
        const double bpm = s.bpm.load (std::memory_order_relaxed); // teeth: before 72fa3a8, the stored value whatever bpmKnown says
#else
        const double bpm = s.bpmKnown.load (std::memory_order_relaxed) ? s.bpm.load (std::memory_order_relaxed) : 0.0;
#endif
        if (bpm > 0.0)
            r.bpm = bpm;
        r.timesig = juce::String (s.timeSigNum.load (std::memory_order_relaxed)) + "/"
                    + juce::String (s.timeSigDen.load (std::memory_order_relaxed));
        const double a = s.loopStartPpq.load (std::memory_order_relaxed);
        const double b = s.loopEndPpq.load (std::memory_order_relaxed);
        if (s.isLooping.load (std::memory_order_relaxed) && b > a && bpm > 0.0)
            r.durationS = (b - a) * 60.0 / bpm;
        return;
    }
    if (manualBpm > 0.0)
        r.bpm = manualBpm;
}

DawJobRequest HotStepLinkProcessor::makeRequest (const juce::String& caption, double manualBpm) const
{
    DawJobRequest r;
    r.caption = caption;
    if (auto* t = apvts.getRawParameterValue ("takes"))
        r.takes = juce::jlimit (1, 8, (int) std::lround (t->load()));
    if (auto* d = apvts.getRawParameterValue ("duration"))
        r.durationS = (double) d->load();
    if (auto* g = apvts.getRawParameterValue ("guidance"))
    {
        const float gv = g->load();
        r.guidance = gv < 0.0f ? -1.0 : (double) gv;
    }
    if (auto* st = apvts.getRawParameterValue ("steps"))
        r.steps = (int) std::lround (st->load());
    if (auto* b = apvts.getRawParameterValue ("backend"))
    {
        static constexpr const char* kIds[] = { "", "sa3", "yue2", "ace" };
        r.expectedBackend = kIds[juce::jlimit (0, 3, (int) std::lround (b->load()))];
    }
    if (auto* m = apvts.getRawParameterValue ("aceModel"))
        r.aceModel = juce::jlimit (0, 2, (int) std::lround (m->load()));
#if ! defined (HSLINK_TEETH_NO_SEED_KEY)
    r.seed = getSeed();
    if (auto* k = keyName (getKeyIndex()))
        r.key = k;
#endif
#if ! defined (HSLINK_TEETH_NO_LYRICS)
    if (! getInstrumental())
        r.lyrics = getLyrics().trim(); // empty = omitted; the server then treats the take as instrumental
#endif
    fillHostMetadata (r, hostSync, standalone, manualBpm);
    return r;
}

static const juce::Identifier kSeedId ("seed"), kKeyIndexId ("keyIndex"), kLyricsId ("lyrics"), kInstrumentalId ("instrumental"), kCaptionId ("caption");

const char* HotStepLinkProcessor::keyName (int index)
{
    static constexpr const char* kKeys[kNumKeys] = {
        "C major", "C# major", "D major", "Eb major", "E major", "F major",
        "F# major", "G major", "Ab major", "A major", "Bb major", "B major",
        "C minor", "C# minor", "D minor", "Eb minor", "E minor", "F minor",
        "F# minor", "G minor", "G# minor", "A minor", "Bb minor", "B minor" };
    return index >= 1 && index <= kNumKeys ? kKeys[index - 1] : nullptr;
}

#if defined (HSLINK_TEETH_NO_STATE_SYNC)
// Teeth: old behaviour, values live in apvts.state, no generation bump.
void HotStepLinkProcessor::writeTextState (juce::ValueTree&) const {}
void HotStepLinkProcessor::readTextState (juce::ValueTree&) {}
juce::int64 HotStepLinkProcessor::getSeed() const
{
    const auto v = apvts.state.getProperty (kSeedId);
    if (v.isVoid())
        return -1;
    const auto n = v.toString().getLargeIntValue();
    return n < 0 ? -1 : n;
}
void HotStepLinkProcessor::setSeed (juce::int64 seed) { apvts.state.setProperty (kSeedId, juce::String (seed < 0 ? (juce::int64) -1 : seed), nullptr); }
int HotStepLinkProcessor::getKeyIndex() const { return juce::jlimit (0, kNumKeys, (int) apvts.state.getProperty (kKeyIndexId, 0)); }
void HotStepLinkProcessor::setKeyIndex (int index) { apvts.state.setProperty (kKeyIndexId, juce::jlimit (0, kNumKeys, index), nullptr); }
juce::String HotStepLinkProcessor::getCaption() const { return apvts.state.getProperty (kCaptionId, "").toString(); }
void HotStepLinkProcessor::setCaption (const juce::String& text) { apvts.state.setProperty (kCaptionId, text, nullptr); }
juce::String HotStepLinkProcessor::getLyrics() const { return apvts.state.getProperty (kLyricsId, "").toString(); }
void HotStepLinkProcessor::setLyrics (const juce::String& text) { apvts.state.setProperty (kLyricsId, text, nullptr); }
bool HotStepLinkProcessor::getInstrumental() const { return (bool) apvts.state.getProperty (kInstrumentalId, false); }
void HotStepLinkProcessor::setInstrumental (bool on) { apvts.state.setProperty (kInstrumentalId, on, nullptr); }
#else
// Copies the members into a state copy (any thread). Same property names as
// before, so old projects load unchanged.
void HotStepLinkProcessor::writeTextState (juce::ValueTree& state) const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    state.setProperty (kSeedId, juce::String (textSeed), nullptr);
    state.setProperty (kKeyIndexId, textKeyIndex, nullptr);
    state.setProperty (kCaptionId, textCaption, nullptr);
    state.setProperty (kLyricsId, textLyrics, nullptr);
    state.setProperty (kInstrumentalId, textInstrumental, nullptr);
}

// Moves the restored properties into the members and drops them from
// apvts.state, then bumps the generation for the editor.
void HotStepLinkProcessor::readTextState (juce::ValueTree& state)
{
    const auto v = state.getProperty (kSeedId);
    const auto n = v.isVoid() ? (juce::int64) -1 : v.toString().getLargeIntValue();
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        textSeed = n < 0 ? -1 : n;
        textKeyIndex = juce::jlimit (0, kNumKeys, (int) state.getProperty (kKeyIndexId, 0));
        textCaption = state.getProperty (kCaptionId, "").toString();
        textLyrics = state.getProperty (kLyricsId, "").toString();
        textInstrumental = (bool) state.getProperty (kInstrumentalId, false);
    }
    for (auto id : { kSeedId, kKeyIndexId, kCaptionId, kLyricsId, kInstrumentalId })
        state.removeProperty (id, nullptr);
    ++stateGeneration;
}

juce::int64 HotStepLinkProcessor::getSeed() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textSeed;
}

void HotStepLinkProcessor::setSeed (juce::int64 seed)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textSeed = seed < 0 ? (juce::int64) -1 : seed;
}

int HotStepLinkProcessor::getKeyIndex() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textKeyIndex;
}

void HotStepLinkProcessor::setKeyIndex (int index)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textKeyIndex = juce::jlimit (0, kNumKeys, index);
}

juce::String HotStepLinkProcessor::getCaption() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textCaption;
}

void HotStepLinkProcessor::setCaption (const juce::String& text)
{
#if defined (HSLINK_TEETH_NO_CAPTION_STATE)
    juce::ignoreUnused (text);
#else
    const juce::SpinLock::ScopedLockType sl (textLock);
    textCaption = text;
#endif
}

juce::String HotStepLinkProcessor::getLyrics() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textLyrics;
}

void HotStepLinkProcessor::setLyrics (const juce::String& text)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textLyrics = text;
}

bool HotStepLinkProcessor::getInstrumental() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textInstrumental;
}

void HotStepLinkProcessor::setInstrumental (bool on)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textInstrumental = on;
}
#endif

//==============================================================================
// R5 audition
float AuditionMath::rmsOf (const juce::AudioBuffer<float>& b)
{
    const int nc = b.getNumChannels(), ns = b.getNumSamples();
    if (nc <= 0 || ns <= 0)
        return 0.0f;
    double acc = 0.0;
    for (int c = 0; c < nc; ++c)
    {
        const float* d = b.getReadPointer (c);
        for (int i = 0; i < ns; ++i)
            acc += (double) d[i] * (double) d[i];
    }
    return (float) std::sqrt (acc / ((double) nc * (double) ns));
}

float AuditionMath::defaultTargetRms()
{
    return juce::Decibels::decibelsToGain (kDefaultTargetDb);
}

float AuditionMath::matchGain (float targetRms, float takeRms, bool enabled)
{
#if defined (HSLINK_TEETH_NO_GAIN)
    juce::ignoreUnused (targetRms, takeRms, enabled);
    return 1.0f;
#endif
    if (! enabled || takeRms <= 1.0e-6f || targetRms <= 0.0f)
        return 1.0f;
    return juce::jlimit (juce::Decibels::decibelsToGain (-40.0f), juce::Decibels::decibelsToGain (24.0f),
                         targetRms / takeRms);
}

juce::AudioBuffer<float> AuditionMath::resample (const juce::AudioBuffer<float>& in, double inRate, double outRate)
{
#if defined (HSLINK_TEETH_NO_AUD_RESAMPLE)
    return in;
#endif
    if (inRate <= 0.0 || outRate <= 0.0 || std::abs (inRate - outRate) < 1.0e-6)
        return in;
    const int nIn = in.getNumSamples();
    const int nOut = (int) std::ceil ((double) nIn * outRate / inRate);
    juce::AudioBuffer<float> out (in.getNumChannels(), juce::jmax (0, nOut));
    out.clear();
    const double ratio = inRate / outRate;
    for (int c = 0; c < in.getNumChannels(); ++c)
    {
        juce::LagrangeInterpolator li;
        li.process (ratio, in.getReadPointer (c), out.getWritePointer (c), nOut, nIn, 0);
    }
    return out;
}

juce::int64 AuditionMath::takeSamplePos (double hostPpq, double anchorPpq, double bpm, double rate)
{
    if (bpm <= 0.0 || rate <= 0.0)
        return 0;
#if defined (HSLINK_TEETH_BAD_PPQ)
    const double secs = (hostPpq - anchorPpq - 1.0) * 60.0 / bpm;
#else
    const double secs = (hostPpq - anchorPpq) * 60.0 / bpm;
#endif
    return (juce::int64) std::floor (secs * rate + 0.5);
}

int AuditionMath::crossfadeSamples (double rate)
{
    return juce::jmax (1, (int) std::lround (rate * 0.010));
}

AuditionEngine::AuditionEngine()
{
    formats.registerBasicFormats();
}

AuditionEngine::~AuditionEngine()
{
    alive->store (false);
    generation.fetch_add (1);
    pool.removeAllJobs (true, 10000);
}

void AuditionEngine::prepare (double hostRate, int)
{
    const double r = hostRate > 0.0 ? hostRate : 48000.0;
    const bool changed = std::abs (r - rate.exchange (r)) > 1.0e-6;
    xfadeLen = AuditionMath::crossfadeSamples (r);
    if (! changed)
        return;
    // selected/selectedSource belong to the message thread: re-decode there.
    if (juce::MessageManager::existsAndIsCurrentThread())
        reselectForRate();
    else
        juce::MessageManager::callAsync ([this, a = alive]
        {
            if (a->load())
                reselectForRate();
        });
}

void AuditionEngine::reselectForRate()
{
    if (selected.existsAsFile())
        select (selected, selectedSource); // re-decode at the new host rate
}

std::unique_ptr<AuditionTake> AuditionEngine::loadTake (juce::AudioFormatManager& fm, const juce::File& f,
                                                         const juce::File& source, double hostRate,
                                                         const std::function<bool()>& shouldAbort)
{
    // Chunked read so a huge decode can be abandoned at shutdown.
    auto readAll = [&] (juce::AudioFormatReader& r, juce::AudioBuffer<float>& b, int nch) -> bool
    {
        const int total = b.getNumSamples();
        const int step = shouldAbort ? 65536 : total;
        for (int pos = 0; pos < total; pos += step)
        {
            if (shouldAbort && shouldAbort())
                return false;
            const int n = juce::jmin (step, total - pos);
            if (! r.read (&b, pos, n, pos, true, nch > 1))
                return false;
        }
        return true;
    };
    constexpr juce::int64 kMaxLen = (juce::int64) 1 << 28;
    // A take that was never renamed is "<name>.wav.part": no audio extension, so it
    // is opened as a stream (as fillTakeMetadata does).
    std::unique_ptr<juce::AudioFormatReader> rd;
    if (! ExclusiveFile::isPartial (f))
        rd.reset (fm.createReaderFor (f));
    else if (auto in = f.createInputStream())
        rd.reset (fm.createReaderFor (std::move (in)));
    if (rd == nullptr || rd->lengthInSamples <= 0 || rd->lengthInSamples > kMaxLen)
        return nullptr;
    const int nc = juce::jlimit (1, 2, (int) rd->numChannels);
    juce::AudioBuffer<float> raw (nc, (int) rd->lengthInSamples);
    if (! readAll (*rd, raw, nc))
        return nullptr;
    if (shouldAbort && shouldAbort())
        return nullptr;
    auto t = std::make_unique<AuditionTake>();
    t->file = f;
    t->rms = AuditionMath::rmsOf (raw);
    AuditionMath::computePeaks (raw, AuditionMath::kPreviewBins, t->peakMin, t->peakMax);
    t->audio = AuditionMath::resample (raw, rd->sampleRate, hostRate);
    t->targetRms = AuditionMath::defaultTargetRms();
    if (source.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> sr (fm.createReaderFor (source));
        if (sr != nullptr && sr->lengthInSamples > 0 && sr->lengthInSamples <= kMaxLen)
        {
            const int sc = juce::jlimit (1, 2, (int) sr->numChannels);
            juce::AudioBuffer<float> sb (sc, (int) sr->lengthInSamples);
            if (readAll (*sr, sb, sc))
            {
                const float srms = AuditionMath::rmsOf (sb);
                if (srms > 1.0e-6f)
                    t->targetRms = srms;
            }
        }
    }
    return t;
}

void AuditionEngine::publish (std::unique_ptr<AuditionTake> t)
{
    publishIfCurrent (std::move (t), generation.load());
}

void AuditionEngine::publishIfCurrent (std::unique_ptr<AuditionTake> t, int gen)
{
    std::lock_guard<std::mutex> g (ownLock);
    if (generation.load() != gen)
        return; // a newer selection superseded this decode
    const auto now = blockCount.load (std::memory_order_acquire);
    for (auto& o : owned)
        if (! o.retired)
        {
            o.retired = true;
            o.retiredAt = now;
        }
    AuditionTake* raw = t.get();
    if (t != nullptr)
        owned.push_back ({ std::move (t), false, 0 });
    requested.store (raw, std::memory_order_release);
    peaksGeneration.fetch_add (1, std::memory_order_release);
}

void AuditionEngine::select (const juce::File& take, const juce::File& sourceForTarget)
{
    selected = take;
    selectedSource = sourceForTarget;
    const int gen = generation.fetch_add (1) + 1;
    if (! take.existsAsFile())
    {
        publishIfCurrent (nullptr, gen);
        return;
    }
    const double r = rate.load();
    const bool canAbort = interruptible.load();
    pool.addJob ([this, take, sourceForTarget, r, gen, canAbort]
    {
        // formats is only used from this single pool thread after construction.
        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        std::function<bool()> abort;
        if (canAbort)
            abort = [this, job, gen]
            {
                return (job != nullptr && job->shouldExit()) || generation.load() != gen;
            };
        auto t = loadTake (formats, take, sourceForTarget, r, abort);
        if (t != nullptr)
            publishIfCurrent (std::move (t), gen);
        else
            peaksGeneration.fetch_add (1, std::memory_order_release); // failed or aborted
    });
}

bool AuditionEngine::getPeaks (const juce::File& f, std::vector<float>& mn, std::vector<float>& mx)
{
    std::lock_guard<std::mutex> g (ownLock);
    for (auto& o : owned)
        if (! o.retired && o.take != nullptr && o.take->file == f && ! o.take->peakMin.empty())
        {
            mn = o.take->peakMin;
            mx = o.take->peakMax;
            return true;
        }
    return false;
}

void AuditionMath::computePeaks (const juce::AudioBuffer<float>& b, int bins, std::vector<float>& mn, std::vector<float>& mx)
{
    bins = juce::jmax (1, bins);
    mn.assign ((size_t) bins, 0.0f);
    mx.assign ((size_t) bins, 0.0f);
    const juce::int64 len = b.getNumSamples();
    if (len <= 0 || b.getNumChannels() <= 0)
        return;
    for (int k = 0; k < bins; ++k)
    {
        const int s0 = (int) (len * k / bins);
        const int s1 = juce::jmax (s0 + 1, (int) (len * (k + 1) / bins));
        if (s0 >= len)
            continue;
        const int n = (int) juce::jmin ((juce::int64) s1, len) - s0;
        float lo = 0.0f, hi = 0.0f;
        bool first = true;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
#if defined (HSLINK_TEETH_NO_PEAKS)
            const auto r = juce::Range<float> (0.0f, 0.0f);
#else
            const auto r = juce::FloatVectorOperations::findMinAndMax (b.getReadPointer (c, s0), n);
#endif
            lo = first ? r.getStart() : juce::jmin (lo, r.getStart());
            hi = first ? r.getEnd() : juce::jmax (hi, r.getEnd());
            first = false;
        }
        mn[(size_t) k] = lo;
        mx[(size_t) k] = hi;
    }
}

double AuditionMath::playheadFraction (juce::int64 pos, juce::int64 len)
{
    if (len <= 0 || pos < 0 || pos >= len)
        return -1.0;
    return (double) pos / (double) len;
}

juce::uint64 AuditionMath::packPlayhead (juce::int64 pos, juce::int64 len)
{
    const juce::uint64 p = (pos < 0 || pos >= 0xFFFFFFFFLL) ? 0xFFFFFFFFull : (juce::uint64) pos;
    const juce::uint64 l = len <= 0 ? 0ull : (juce::uint64) juce::jmin (len, (juce::int64) 0xFFFFFFFFLL);
   #if HSLINK_TEETH_BAD_PACK
    return (p << 32) | l; // halves swapped
   #else
    return (l << 32) | p;
   #endif
}

double AuditionMath::unpackPlayhead (juce::uint64 packed)
{
    const juce::uint64 p = packed & 0xFFFFFFFFull;
    const juce::int64 len = (juce::int64) (packed >> 32);
    return p == 0xFFFFFFFFull ? -1.0 : playheadFraction ((juce::int64) p, len);
}

bool AuditionMath::PeakPoll::needsFetch (const juce::File& sel, juce::uint32 g)
{
   #if HSLINK_TEETH_NO_POLL_GATE
    juce::ignoreUnused (sel, g);
    return true;
   #else
    if (primed && sel == file && g == gen)
        return false;
    primed = true;
    file = sel;
    gen = g;
    return true;
   #endif
}

void AuditionEngine::collectGarbage()
{
    std::lock_guard<std::mutex> g (ownLock);
    const auto now = blockCount.load (std::memory_order_acquire);
    const auto* uc = usedCur.load (std::memory_order_acquire);
    const auto* up = usedPrev.load (std::memory_order_acquire);
    owned.erase (std::remove_if (owned.begin(), owned.end(), [&] (const Owned& o)
    {
        return o.retired && (juce::uint32) (now - o.retiredAt) >= 2u
            && o.take.get() != uc && o.take.get() != up;
    }), owned.end());
}

float AuditionEngine::sampleOf (const AuditionTake* t, int ch, juce::int64 pos, float gain, float input) const
{
    if (t == nullptr)
        return input; // no take: pass-through
    if (pos < 0 || pos >= t->audio.getNumSamples())
        return 0.0f;
    const int c = juce::jmin (ch, t->audio.getNumChannels() - 1);
    return t->audio.getSample (c, (int) pos) * gain;
}

void AuditionEngine::render (juce::AudioBuffer<float>& buf, int numSamples, juce::int64 takePos, bool playing, bool levelMatch)
{
    AuditionTake* want = requested.load (std::memory_order_acquire);
    if (want != cur)
    {
        prev = cur;
        cur = want;
        fadePos = 0;
    }
    if (prev == cur)
        prev = nullptr;
    if (cur == nullptr && prev == nullptr)
    {
        usedCur.store (nullptr, std::memory_order_release);
        usedPrev.store (nullptr, std::memory_order_release);
        playhead.store (AuditionMath::packPlayhead (-1, 0), std::memory_order_relaxed);
        blockCount.fetch_add (1, std::memory_order_acq_rel);
        return; // pure pass-through
    }
    const float gCur = cur != nullptr ? AuditionMath::matchGain (cur->targetRms, cur->rms, levelMatch) : 1.0f;
    const float gPrev = prev != nullptr ? AuditionMath::matchGain (prev->targetRms, prev->rms, levelMatch) : 1.0f;
    const int nc = buf.getNumChannels();
    const int n = juce::jmin (numSamples, buf.getNumSamples());
    int fp = fadePos;
    for (int i = 0; i < n; ++i)
    {
        const juce::int64 p = playing ? takePos + i : -1;
#if defined (HSLINK_TEETH_NO_XFADE)
        const bool inFade = false;
#else
        const bool inFade = prev != nullptr && fp < xfadeLen;
#endif
        const float w = inFade ? (float) fp / (float) xfadeLen : 1.0f;
        for (int c = 0; c < nc; ++c)
        {
            float* d = buf.getWritePointer (c);
            const float in = d[i];
            const float b = sampleOf (cur, c, p, gCur, in);
            d[i] = inFade ? b * w + sampleOf (prev, c, p, gPrev, in) * (1.0f - w) : b;
        }
        if (fp < xfadeLen)
            ++fp;
    }
    fadePos = fp;
    if (fadePos >= xfadeLen)
        prev = nullptr;
    lastGain.store (gCur, std::memory_order_relaxed);
    playhead.store (AuditionMath::packPlayhead (playing && cur != nullptr ? takePos : -1,
                                                cur != nullptr ? cur->audio.getNumSamples() : 0),
                    std::memory_order_relaxed);
    usedCur.store (cur, std::memory_order_release);
    usedPrev.store (prev, std::memory_order_release);
    blockCount.fetch_add (1, std::memory_order_acq_rel);
}

//==============================================================================
// Take editor audition
EditorAudition::EditorAudition()
{
    formats.registerBasicFormats();
}

EditorAudition::~EditorAudition()
{
    generation.fetch_add (1);
    pool.removeAllJobs (true, 10000);
}

int EditorAudition::fadeSamples (double rate)
{
    return juce::jmax (1, (int) std::lround (rate * kFadeSeconds));
}

int EditorAudition::duckSamples (double rate)
{
    return juce::jmax (1, (int) std::lround (rate * kDuckSeconds));
}

void EditorAudition::prepare (double hostRate)
{
    const double r = hostRate > 0.0 ? hostRate : 48000.0;
    const bool changed = std::abs (r - rate.exchange (r)) > 1.0e-6;
    duckLen.store (duckSamples (r), std::memory_order_relaxed);
    release();
    if (changed)
        dropCache();
}

void EditorAudition::release()
{
    stop(); // first: a render() in between must not take the clip again
    dropGen.fetch_add (1, std::memory_order_release); // the clip in render() goes without a fade
}

void EditorAudition::dropCache()
{
    std::lock_guard<std::mutex> g (ownLock);
    cache.reset();
    cacheFile = juce::File();
    cacheRate = 0.0;
}

EditorAudition::State EditorAudition::getState() const
{
    if (bypassStop.load (std::memory_order_acquire))
        return idle; // stopped by host bypass; poll() or play() does the stop()
    if (loadingNow.load (std::memory_order_acquire))
        return loading;
    return activeSerial.load (std::memory_order_acquire) != 0 ? playing : idle;
}

void EditorAudition::stop()
{
    generation.fetch_add (1); // a decode that is running publishes nothing
    std::lock_guard<std::mutex> g (ownLock);
    for (auto& o : owned)
        o.retired = true;
    requested.store (nullptr);
    activeSerial.store (0, std::memory_order_release);
    loadingNow.store (false, std::memory_order_release);
}

void EditorAudition::publish (const Audio& a, juce::int64 start, juce::int64 end, juce::int64 length, double atRate, int gen)
{
    std::lock_guard<std::mutex> g (ownLock);
    if (generation.load() != gen)
        return; // stopped, or a newer Play
    const juce::int64 n = a != nullptr ? (juce::int64) a->getNumSamples() : 0;
    loadingNow.store (false, std::memory_order_release);
    if (n <= 0 || a->getNumChannels() <= 0)
        return;
    auto c = std::make_unique<Clip>();
    c->audio = a;
    c->end = (int) n;
    if (length > 0 && end > start)
    {
        // File frames to frames of the decoded take (the host rate).
        const double k = (double) n / (double) length;
        c->start = (int) juce::jlimit ((juce::int64) 0, n - 1, (juce::int64) std::llround ((double) start * k));
        c->end = (int) juce::jlimit ((juce::int64) c->start + 1, n, (juce::int64) std::llround ((double) end * k));
    }
    c->fade = fadeSamples (atRate);
    c->serial = ++nextSerial;
    if (c->serial == 0)
        c->serial = ++nextSerial;
    for (auto& o : owned)
        o.retired = true;
    Clip* raw = c.get();
    const auto serial = c->serial;
    owned.push_back ({ std::move (c), false });
    requested.store (raw);
    activeSerial.store (serial, std::memory_order_release);
}

void EditorAudition::setWarp (std::vector<WarpMarkers::Marker> markers)
{
    std::lock_guard<std::mutex> g (ownLock);
    warpMap = std::move (markers);
}

void EditorAudition::play (const juce::File& take, juce::int64 start, juce::int64 end, juce::int64 length, bool loop)
{
    if (bypassStop.load (std::memory_order_acquire))
    {
        // A bypass stop poll() has not seen yet: done here, before this Play. The flag
        // comes down after the stop(), so render() never sees it down with the old
        // clip still asked for.
#if defined (HSLINK_TEETH_EA_FLAG_FIRST)
        bypassStop.store (false, std::memory_order_release); // teeth: the order before e8228b5
#else
        stop();
#endif
#if defined (HSLINK_TEST_HOOKS)
        if (bypassGapForTest) bypassGapForTest();
#endif
#if defined (HSLINK_TEETH_EA_FLAG_FIRST)
        stop();
#endif
        bypassStop.store (false, std::memory_order_release);
    }
    const int gen = generation.fetch_add (1) + 1;
    if (afterGenForTest)
        afterGenForTest();
    loopOn.store (loop, std::memory_order_relaxed);
    const double r = rate.load();
    Audio have;
    std::vector<WarpMarkers::Marker> warp;
    {
        std::lock_guard<std::mutex> g (ownLock);
        if (cache != nullptr && cacheFile == take && std::abs (cacheRate - r) < 1.0e-6)
            have = cache;
        warp = warpMap;
        if (generation.load() == gen) // else a stop() came in between: stay as it left it
            loadingNow.store (true, std::memory_order_release);
    }
    if (have != nullptr && warp.empty())
    {
        publish (have, start, end, length, r, gen);
        return;
    }
    const bool canAbort = interruptible.load();
    pool.addJob ([this, take, start, end, length, r, gen, canAbort, have, warp]
    {
        auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        std::function<bool()> abort;
        if (canAbort)
            abort = [this, job, gen]
            {
                return (job != nullptr && job->shouldExit()) || generation.load() != gen;
            };
#if ! defined (HSLINK_TEETH_EA_NO_TRY)
      try // v0.5.9: a throw in the decode or the stretch must still end the loading state
      {
#endif
        if (jobThrowForTest)
            jobThrowForTest();
        // Decoded and resampled to the host rate here, off the audio thread.
        Audio a = have;
        if (a == nullptr)
        {
            auto t = AuditionEngine::loadTake (formats, take, juce::File(), r, abort);
            if (t != nullptr)
                a = std::make_shared<const juce::AudioBuffer<float>> (std::move (t->audio));
            std::lock_guard<std::mutex> g (ownLock);
            if (generation.load() != gen)
                return;
            if (a != nullptr)
            {
                cache = a;
                cacheFile = take;
                cacheRate = r;
            }
        }
        if (a != nullptr && ! warp.empty() && length > 0)
        {
            // Round 2b: the piecewise stretch, here on the job. The markers are frames
            // of the file, scaled to frames of the decoded take.
            const double k = (double) a->getNumSamples() / (double) length;
            auto m = warp;
            for (auto& w : m)
            {
                w.srcFrame = std::llround ((double) w.srcFrame * k);
                w.dstFrame = std::llround ((double) w.dstFrame * k);
            }
            m.back().srcFrame = m.back().dstFrame = a->getNumSamples();
            auto out = TakeStretch::renderWarped (*a, r, m, abort);
            if (generation.load() != gen)
                return;
            if (out.getNumSamples() > 0)
                a = std::make_shared<const juce::AudioBuffer<float>> (std::move (out));
        }
        publish (a, start, end, length, r, gen); // no audio: only ends the loading state
#if ! defined (HSLINK_TEETH_EA_NO_TRY)
      }
      catch (...)
      {
          publish (nullptr, start, end, length, r, gen); // ends loading; the generation check is in publish
      }
#endif
    });
}

void EditorAudition::poll()
{
    if (bypassStop.load (std::memory_order_acquire))
    {
        // The audio thread fades the clip out under host bypass. The flag comes down
        // after the stop(): see play().
#if defined (HSLINK_TEETH_EA_FLAG_FIRST)
        bypassStop.store (false, std::memory_order_release); // teeth: the order before e8228b5
#else
        stop();
#endif
#if defined (HSLINK_TEST_HOOKS)
        if (bypassGapForTest) bypassGapForTest();
#endif
#if defined (HSLINK_TEETH_EA_FLAG_FIRST)
        stop();
#endif
        bypassStop.store (false, std::memory_order_release);
    }
    const auto as = activeSerial.load (std::memory_order_acquire);
    const bool atEnd = as != 0 && doneSerial.load (std::memory_order_acquire) == as;
    if (pollGapForTest)
        pollGapForTest();
    std::lock_guard<std::mutex> g (ownLock);
    if (atEnd && activeSerial.load (std::memory_order_acquire) == as)
    {
        // Played to its end, and no newer clip was published since. The generation
        // stays: a newer Play that is still decoding publishes later.
        for (auto& o : owned)
            o.retired = true;
        requested.store (nullptr);
        activeSerial.store (0, std::memory_order_release);
    }
    const auto* hz = hazard.load();
    owned.erase (std::remove_if (owned.begin(), owned.end(), [&] (const Owned& o)
    {
        return o.retired && o.clip.get() != hz;
    }), owned.end());
}

void EditorAudition::duckSync()
{
    // A new length (prepare() at another rate) is taken only where the gain does not
    // depend on it: released, or fully down. In the middle the ramp ends on the old one.
    const int dl = duckLen.load (std::memory_order_relaxed);
    if (dl == duckN)
        return;
    if (duckPos <= 0)
        duckN = dl;
    else if (duckPos >= duckN)
        duckN = duckPos = dl;
}

float EditorAudition::duckNext (bool down)
{
    if (down)
    {
        if (duckPos < duckN)
            ++duckPos;
    }
    else if (duckPos > 0)
        --duckPos;
    return duckGain (duckPos, duckN);
}

float EditorAudition::duckGain (int pos, int total)
{
    // The two ends by value, not by arithmetic.
    if (pos <= 0)
        return 1.0f;
    if (pos >= total)
        return 0.0f;
    // Cubic smoothstep 3x^2 - 2x^3 on the way left to full duck, x = (total - pos) / total:
    // flat at both ends, so no corner where the ramp starts or lands. Taken on the
    // integer complement, the gain at pos and the gain at total - pos add up to 1.
    const float x = (float) (total - pos) / (float) total;
    return x * x * (3.0f - 2.0f * x);
}

void EditorAudition::duckMain (juce::AudioBuffer<float>& buf, int from, int to, bool down)
{
    if (! down && duckPos <= 0)
        return;
    const int nc = buf.getNumChannels();
    float* const* out = buf.getArrayOfWritePointers();
    for (int i = from; i < to; ++i)
    {
        const float d = duckNext (down);
        if (duckPos <= 0)
            break; // released: the rest of buf stays as it is
        for (int c = 0; c < nc; ++c)
            out[c][i] *= d;
    }
}

void EditorAudition::render (juce::AudioBuffer<float>& buf, int numSamples, bool hostBypassed)
{
    // Stopped by host bypass and the message thread has not done its stop() yet: the
    // clip that is still asked for is not wanted. One that plays fades out below.
    Clip* want = nullptr;
    if (hostBypassed)
    {
        // A bypassed block wants no clip whatever the flag says: a clip published at
        // any moment of this block is not taken, so it cannot sound. No lock here, and
        // the clip is not let go: the one that plays fades out as after a stop() and
        // hazard names it until its fade has ended. The message thread does the stop().
#if defined (HSLINK_TEETH_EA_BYPASS_PUBLISH)
        // teeth: the hole before 0be1dd1. The old bypassed() saw no clip and left the
        // flag down, the Play landed before the old render() read requested, and that
        // render() took the clip. Here: no clip current and the flag down = take it.
        if (cur == nullptr && ! bypassStop.load (std::memory_order_acquire))
            want = requested.load (std::memory_order_acquire);
        else
#endif
        if (cur != nullptr || requested.load (std::memory_order_acquire) != nullptr)
        {
            bypassStop.store (true, std::memory_order_release);
#if defined (HSLINK_TEETH_EA_BYPASS_CUT)
            cur = nullptr; // teeth: the cut before 6e32220, the clip is dropped at the bypass sample
            hazard.store (nullptr);
#endif
        }
    }
    else if (! bypassStop.load (std::memory_order_acquire))
        want = requested.load (std::memory_order_acquire);
    if (want == cur && (cur == nullptr || ended) && duckPos <= 0)
        return; // idle
    const int n = juce::jmin (numSamples, buf.getNumSamples());
    duckSync();
    if (want == cur && (cur == nullptr || ended))
    {
        duckMain (buf, 0, n, false); // no clip any more: the main output comes back
        return;
    }
    const auto dg = dropGen.load (std::memory_order_acquire);
    if (dg != seenDrop)
    {
        seenDrop = dg;
        if (cur != nullptr && ! ended)
        {
            // prepare() or release() since this clip was taken: the host may have left
            // a gap, so a fade from the old level would be a tick out of silence. The
            // clip is dropped and none of it is written; a newer clip starts next
            // block. The main output ramps back from where the duck was.
            cur = nullptr;
            hazard.store (nullptr);
            duckMain (buf, 0, n, want != nullptr);
            return;
        }
    }
    if (cur == nullptr || ended)
    {
        // Take the new clip: name it in hazard, then look again. A clip that was
        // replaced in between may be freed already and is not read; the next block
        // tries again.
        cur = nullptr;
        ended = false;
        hazard.store (want);
        if (want == nullptr || requested.load() != want)
        {
            hazard.store (nullptr);
            duckMain (buf, 0, n, false);
            return;
        }
        cur = want;
        pos = cur->start;
        gain = 0.0f;
        tailLoop = false;
        // Play: the clip is not heard before the main output is fully down, so the
        // two never add up. Already down (a Play over a playing clip): no wait.
        hold = duckPos < duckN;
    }
    const bool stopping = want != cur;
    // Main output: down while a clip is asked for (this one, or the one that replaces
    // it), up again while this one fades out after a stop.
    const bool duckDown = want != nullptr;
    const auto& src = *cur->audio;
    const int sc = src.getNumChannels();
    const int nc = buf.getNumChannels();
    const int start = cur->start, end = cur->end;
    const int xf = juce::jmin (cur->fade, (end - start) / 2);
    const bool preRoll = start >= xf; // audio before the region to fade the wrap into
    const bool loop = loopOn.load (std::memory_order_relaxed);
    const float step = 1.0f / (float) cur->fade;
    const float* s0 = src.getReadPointer (0);
    const float* s1 = src.getReadPointer (sc > 1 ? 1 : 0);
    float* const* out = buf.getArrayOfWritePointers();
    float g = gain;
    int p = pos;
    bool stopped = false, finished = false;
    int i = 0;
    for (; i < n; ++i)
    {
        const float d = duckNext (duckDown);
        if (duckPos > 0)
            for (int c = 0; c < nc; ++c)
                out[c][i] *= d; // the main output, before the clip is added
        if (hold && ! stopping)
        {
            hold = duckPos < duckN; // the sample that reaches silence is the last of the wait
            continue;
        }
        if (stopping)
        {
            g -= step;
            if (g <= 0.0f)
            {
                stopped = true;
                break;
            }
        }
        else if (g < 1.0f)
            g = juce::jmin (1.0f, g + step);
        float l = s0[p], r = s1[p];
        const int tail = p - (end - xf);
        if (xf > 0 && tail >= 0)
        {
            if (tail == 0)
                tailLoop = loop;
            const float t = (float) (tail + 1) / (float) xf;
            if (tailLoop && preRoll)
            {
                // Loop wrap: the end of the region fades into the audio just before
                // its start, which runs on into the start without a step.
                const int q = p - (end - start);
                l += (s0[q] - l) * t;
                r += (s1[q] - r) * t;
            }
            else
            {
                l *= 1.0f - t; // the end of the clip, or a wrap with nothing before the start
                r *= 1.0f - t;
            }
        }
        if (nc == 1)
            out[0][i] += 0.5f * (l + r) * g;
        else
            for (int c = 0; c < nc; ++c)
                out[c][i] += (c == 0 ? l : r) * g;
        if (++p >= end)
        {
            if (xf > 0 ? tailLoop : loop)
            {
                p = start;
                if (! preRoll)
                    g = 0.0f; // faded out above: fade in again
            }
            else
            {
                finished = true;
                break;
            }
        }
    }
    gain = g;
    pos =juce::jmin (p, end - 1);
    playFrac.store ((stopped || finished) ? -1.0 : (double) pos / (double) juce::jmax (1, src.getNumSamples()), std::memory_order_relaxed);
    // The rest of the block after a stop or the end of the clip (sample i is done).
    if (stopped || finished)
        duckMain (buf, i + 1, n, want != nullptr && ! (finished && want == cur));
    if (stopped)
    {
        cur = nullptr;
        hazard.store (nullptr);
    }
    else if (finished)
    {
        ended = true; // hazard keeps the clip until another one is asked for
        doneSerial.store (cur->serial, std::memory_order_release);
    }
}

//==============================================================================
// Source clip for repaint / lego

bool HotStepLinkProcessor::armCapture (bool on)
{
    const int phase = capPhase.load();
    if (on)
    {
        if (phase != capIdle)
            return phase != capFinished;
        if (capture.isCapturing())
            return false;
        auto f = DawWorker::tempRoot()
                     .getChildFile (tempSessionName)
                     .getChildFile (DawWorker::captureFileName (++captureSerial));
        f.getParentDirectory().createDirectory();
        if (! capture.startCapture (f))
            return false;
        capPhase.store (capArmed, std::memory_order_release);
        return true;
    }
    if (phase == capRecording)
    {
        // Disarm mid-take: keep what was recorded.
        capPhase.store (capFinished, std::memory_order_release);
        pollCapture();
    }
    else if (phase == capArmed)
    {
        capPhase.store (capIdle, std::memory_order_release);
        capture.stopCapture(); // nothing recorded, no file
    }
    return true;
}

bool HotStepLinkProcessor::pollCapture()
{
    if (capPhase.load (std::memory_order_acquire) != capFinished)
        return false;
    const bool wrote = capture.stopCapture();
    const double startPpq = captureStartPpq.load();
    const double bpm = hostSync.bpm.load();
    const bool bpmKnown = hostSync.bpmKnown.load();
    capPhase.store (capIdle, std::memory_order_release);
    if (! wrote || ! setSourceFile (capture.getLastFile()))
        return false;
    sourceStartPpq = startPpq;
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    juce::ignoreUnused (bpmKnown);
    sourceBpm = bpm > 0.0 ? bpm : 120.0; // teeth: the stored value whatever bpmKnown says
#else
    // No host tempo at capture: the take's tempo is unknown (0), never 120 and never
    // the last value the host once gave.
    sourceBpm = bpmKnown && HostSyncState::hostBpmUsable (bpm) ? bpm : 0.0;
#endif
    return true;
}

bool HotStepLinkProcessor::setSourceFile (const juce::File& wav)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (wav));
    if (rd == nullptr || rd->sampleRate <= 0.0 || rd->lengthInSamples <= 0)
        return false;
    sourceFile = wav;
    sourceLenS = (double) rd->lengthInSamples / rd->sampleRate;
    sourceStartPpq = 0.0;
#if ! defined (HSLINK_TEETH_NO_TEMPO_GATE)
    // A loaded file has no known tempo: not 120 and not the tempo of the capture before.
    // pollCapture sets the tempo of its own take after this call.
    sourceBpm = 0.0;
#endif
    return true;
}

void HotStepLinkProcessor::clearSource()
{
    sourceFile = juce::File();
    sourceLenS = 0.0;
}

juce::String HotStepLinkProcessor::getSourceStateText() const
{
    const int phase = capPhase.load();
    if (phase == capRecording) return "Source: capturing...";
    if (phase == capArmed)     return "Source: armed, waiting for play";
    if (phase == capFinished)  return "Source: writing...";
    if (sourceLoaded())        return "Source: ready: " + juce::String (sourceLenS, 3) + " s";
    return "Source: none";
}

bool HotStepLinkProcessor::loopRegionSeconds (double loopStartPpq, double loopEndPpq, double capStartPpq,
                                              double bpm, double& startS, double& endS)
{
    if (! (bpm > 0.0) || ! (loopEndPpq > loopStartPpq))
        return false;
    startS = (loopStartPpq - capStartPpq) * 60.0 / bpm;
    endS = (loopEndPpq - capStartPpq) * 60.0 / bpm;
    return true;
}

bool HotStepLinkProcessor::validateRegion (double startS, double endS, double lengthS, juce::String& err)
{
    if (! std::isfinite (startS) || ! std::isfinite (endS) || startS < -1.0e-3)
        err = "region start must be >= 0";
    else if (! (startS < endS))
        err = "region start must be before its end";
    else if (endS > lengthS + 1.0e-3)
        err = "region end " + juce::String (endS, 3) + " s is past the source (" + juce::String (lengthS, 3) + " s)";
    else
        return true;
    return false;
}

bool HotStepLinkProcessor::prepareSourceJob (DawJobRequest& r, double manualStartS, double manualEndS,
                                             juce::String& err) const
{
    if (! modeNeedsSource (r.mode))
        return true;
#if ! defined (HSLINK_TEETH_NO_SOURCE_GATE)
    if (! sourceReady())
    {
        err = r.mode + " needs a source clip: " + (standalone ? "drop a WAV" : "arm Capture and play");
        return false;
    }
#endif
    r.sourceWav = sourceFile;
    if (r.mode != "repaint")
        return true;
    double s = manualStartS, e = manualEndS;
#if ! defined (HSLINK_TEETH_NO_TEMPO_GATE)
    // A take captured with no host tempo: the host loop is in beats and cannot be
    // turned into seconds. The job is refused: a repaint with no region would not be
    // what the user selected. A new capture with a host tempo repaints as before.
    if (! standalone && ! (sourceBpm > 0.0))
    {
        err = "repaint needs a host tempo";
        return false;
    }
#endif
    if (! standalone)
    {
        if (! hostSync.isLooping.load()
            || ! loopRegionSeconds (hostSync.loopStartPpq.load(), hostSync.loopEndPpq.load(), sourceStartPpq,
                                    sourceBpm, s, e))
        {
            err = "repaint needs a host loop over the part to repaint";
            return false;
        }
    }
    if (! validateRegion (s, e, sourceLenS, err))
        return false;
    r.hasRegion = true;
    r.regionStartS = juce::jmax (0.0, s);
    r.regionEndS = juce::jmin (e, sourceLenS);
    return true;
}

//==============================================================================
// Mode / BPM / A-B slots / presets (message thread, never the audio thread).
static const juce::Identifier kSlotsId ("abSlots"), kSlotAId ("slotA"), kSlotBId ("slotB"),
    kActiveId ("active"), kPresetId ("hslinkPreset");

int HotStepLinkProcessor::getModeId() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textModeId;
}

void HotStepLinkProcessor::setModeId (int id)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textModeId = juce::jlimit (1, 3, id);
}

double HotStepLinkProcessor::getManualBpm() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return textBpm;
}

void HotStepLinkProcessor::setManualBpm (double bpm)
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    textBpm = juce::jlimit (40.0, 240.0, bpm);
}

juce::ValueTree HotStepLinkProcessor::GenSettings::toTree (const juce::Identifier& type) const
{
    juce::ValueTree t (type);
    t.setProperty ("caption", caption, nullptr);
    t.setProperty ("lyrics", lyrics, nullptr);
    t.setProperty ("instrumental", instrumental, nullptr);
    t.setProperty ("bpm", bpm, nullptr);
    t.setProperty ("duration", duration, nullptr);
    t.setProperty ("takes", takes, nullptr);
    t.setProperty ("seed", juce::String (seed), nullptr);
    t.setProperty ("keyIndex", keyIndex, nullptr);
    t.setProperty ("mode", modeId, nullptr);
    t.setProperty ("backend", backend, nullptr);
    t.setProperty ("aceModel", aceModel, nullptr);
    return t;
}

HotStepLinkProcessor::GenSettings HotStepLinkProcessor::GenSettings::fromTree (const juce::ValueTree& t)
{
    GenSettings s;
    s.caption = t.getProperty ("caption", "").toString();
    s.lyrics = t.getProperty ("lyrics", "").toString();
    s.instrumental = (bool) t.getProperty ("instrumental", false);
    s.bpm = juce::jlimit (40.0, 240.0, (double) t.getProperty ("bpm", 120.0));
    s.duration = (float) juce::jlimit (5.0, 240.0, (double) t.getProperty ("duration", 30.0));
    s.takes = juce::jlimit (1, 8, (int) t.getProperty ("takes", 4));
    const auto sd = t.getProperty ("seed", "-1").toString().getLargeIntValue();
    s.seed = sd < 0 ? (juce::int64) -1 : sd;
    s.keyIndex = juce::jlimit (0, kNumKeys, (int) t.getProperty ("keyIndex", 0));
    s.modeId = juce::jlimit (1, 3, (int) t.getProperty ("mode", 1));
    s.backend = juce::jlimit (0, 3, (int) t.getProperty ("backend", 0));
    s.aceModel = juce::jlimit (0, 2, (int) t.getProperty ("aceModel", 0));
    return s;
}

bool HotStepLinkProcessor::GenSettings::operator== (const GenSettings& o) const
{
    return caption == o.caption && lyrics == o.lyrics && instrumental == o.instrumental
        && std::abs (bpm - o.bpm) < 1.0e-6 && std::abs (duration - o.duration) < 1.0e-4f
        && takes == o.takes && seed == o.seed && keyIndex == o.keyIndex && modeId == o.modeId
        && backend == o.backend && aceModel == o.aceModel;
}

HotStepLinkProcessor::GenSettings HotStepLinkProcessor::captureSettings() const
{
    GenSettings s;
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        s.caption = textCaption;
        s.lyrics = textLyrics;
        s.instrumental = textInstrumental;
        s.bpm = textBpm;
        s.seed = textSeed;
        s.keyIndex = textKeyIndex;
        s.modeId = textModeId;
    }
    auto rd = [this] (const char* id, float def)
    {
        auto* v = apvts.getRawParameterValue (id);
        return v != nullptr ? v->load() : def;
    };
    s.duration = rd ("duration", 30.0f);
    s.takes = juce::roundToInt (rd ("takes", 4.0f));
    s.backend = juce::roundToInt (rd ("backend", 0.0f));
    s.aceModel = juce::roundToInt (rd ("aceModel", 0.0f));
    return s;
}

void HotStepLinkProcessor::applySettings (const GenSettings& s)
{
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        textCaption = s.caption;
        textLyrics = s.lyrics;
        textInstrumental = s.instrumental;
        textBpm = s.bpm;
        textSeed = s.seed;
        textKeyIndex = s.keyIndex;
        textModeId = s.modeId;
    }
    auto wr = [this] (const char* id, float v)
    {
        if (auto* prm = apvts.getParameter (id))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    };
    wr ("duration", s.duration);
    wr ("takes", (float) s.takes);
    wr ("backend", (float) s.backend);
    wr ("aceModel", (float) s.aceModel);
    ++stateGeneration;
}

const char* RoleCards::title (int card)
{
    switch (card)
    {
        case kSa3:  return "SA3";
        case kYue2: return "YuE2";
        case kAce:  return "ACE base";
        default:    return "";
    }
}

const char* RoleCards::subtitle (int card)
{
    switch (card)
    {
        case kSa3:  return "quick sketch";
        case kYue2: return "batch-4 take garden";
        case kAce:  return "lego / repaint";
        default:    return "";
    }
}

HotStepLinkProcessor::GenSettings RoleCards::apply (int card, HotStepLinkProcessor::GenSettings s)
{
#if ! defined (HSLINK_TEETH_NO_ROLE)
    switch (card)
    {
        case kSa3:  s.backend = kBackendSa3;  s.takes = 1; break;
        case kYue2: s.backend = kBackendYue2; s.takes = 4; break;
        case kAce:  s.backend = kBackendAce;  s.aceModel = kAceModelBase; break;
        default:    break;
    }
#else
    juce::ignoreUnused (card);
#endif
    return s;
}

int RoleCards::activeForBackend (int backend)
{
#if ! defined (HSLINK_TEETH_NO_ROLE)
    switch (backend)
    {
        case kBackendSa3:  return kSa3;
        case kBackendYue2: return kYue2;
        case kBackendAce:  return kAce;
        default:           break;
    }
#else
    juce::ignoreUnused (backend);
#endif
    return kNone;
}

int RoleCards::activeFor (const HotStepLinkProcessor::GenSettings& s)
{
    return activeForBackend (s.backend);
}

void HotStepLinkProcessor::applyRoleCard (int card)
{
    const auto cur = captureSettings();
    const auto next = RoleCards::apply (card, cur);
    auto wr = [this] (const char* id, int before, int after)
    {
        if (before == after)
            return;
        if (auto* prm = apvts.getParameter (id))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (prm->convertTo0to1 ((float) after));
            prm->endChangeGesture();
        }
    };
    wr ("backend", cur.backend, next.backend);
    wr ("takes", cur.takes, next.takes);
    wr ("aceModel", cur.aceModel, next.aceModel);
}

int HotStepLinkProcessor::getActiveRoleCard() const
{
    auto* b = apvts.getRawParameterValue ("backend");
    return RoleCards::activeForBackend (b != nullptr ? juce::roundToInt (b->load()) : 0);
}

int HotStepLinkProcessor::getActiveSlot() const
{
    const juce::SpinLock::ScopedLockType sl (textLock);
    return activeSlot;
}

void HotStepLinkProcessor::selectSlot (int slot)
{
    slot = slot != 0 ? 1 : 0;
    const auto live = captureSettings();
    GenSettings next = live; // a never-used slot starts as a copy
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        if (slot == activeSlot)
            return;
        activeSlot = slot;
#if ! defined (HSLINK_TEETH_NO_AB)
        if (otherSlotSet)
            next = otherSlot;
        otherSlot = live;
        otherSlotSet = true;
#endif
    }
    applySettings (next);
}

void HotStepLinkProcessor::copyActiveToOther()
{
    const auto live = captureSettings();
    const juce::SpinLock::ScopedLockType sl (textLock);
    otherSlot = live;
    otherSlotSet = true;
}

HotStepLinkProcessor::GenSettings HotStepLinkProcessor::getSlot (int slot) const
{
    const auto live = captureSettings();
    const juce::SpinLock::ScopedLockType sl (textLock);
    if ((slot != 0 ? 1 : 0) == activeSlot || ! otherSlotSet)
        return live;
    return otherSlot;
}

// Any thread (host save). Reads lock-guarded members and atomics only.
void HotStepLinkProcessor::writeSlotState (juce::ValueTree& state) const
{
#if defined (HSLINK_TEETH_NO_AB)
    juce::ignoreUnused (state);
#else
    const auto live = captureSettings();
    GenSettings other;
    int act = 0;
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        other = otherSlotSet ? otherSlot : live;
        act = activeSlot;
    }
    juce::ValueTree slots (kSlotsId);
    slots.setProperty (kActiveId, act == 0 ? "A" : "B", nullptr);
    slots.appendChild ((act == 0 ? live : other).toTree (kSlotAId), nullptr);
    slots.appendChild ((act == 0 ? other : live).toTree (kSlotBId), nullptr);
    state.removeChild (state.getChildWithName (kSlotsId), nullptr);
    state.appendChild (slots, nullptr);
#endif
}

void HotStepLinkProcessor::readSlotState (juce::ValueTree& state)
{
    auto slots = state.getChildWithName (kSlotsId);
    {
        const juce::SpinLock::ScopedLockType sl (textLock);
        activeSlot = 0;
        otherSlotSet = false;
        textModeId = 1;
        textBpm = 120.0;
        if (slots.isValid())
        {
            activeSlot = slots.getProperty (kActiveId, "A").toString() == "B" ? 1 : 0;
            const auto a = slots.getChildWithName (kSlotAId);
            const auto b = slots.getChildWithName (kSlotBId);
            const auto liveT = activeSlot == 0 ? a : b;
            const auto otherT = activeSlot == 0 ? b : a;
            if (liveT.isValid())
            {
                const auto ls = GenSettings::fromTree (liveT);
                textModeId = ls.modeId;
                textBpm = ls.bpm;
            }
            if (otherT.isValid())
            {
                otherSlot = GenSettings::fromTree (otherT);
                otherSlotSet = true;
            }
        }
    }
    if (slots.isValid())
        state.removeChild (slots, nullptr);
    ++stateGeneration;
}

juce::String HotStepLinkProcessor::sanitizePresetName (const juce::String& name)
{
#if defined (HSLINK_TEETH_NO_SANITIZE)
    return name;
#else
    // ASCII letters, digits, space, '-' and '_' only: no dots, no separators,
    // so a name can never leave the preset folder.
    juce::String out;
    for (auto c : name)
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
            || c == ' ' || c == '-' || c == '_')
            out << juce::String::charToString (c);
    return out.trim().substring (0, 64).trim();
#endif
}

juce::File HotStepLinkProcessor::getPresetFolder() const
{
    if (presetFolder != juce::File())
        return presetFolder;
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("HOT-STEP LINK").getChildFile ("Presets");
}

juce::StringArray HotStepLinkProcessor::listPresets() const
{
    juce::StringArray names;
    for (const auto& f : getPresetFolder().findChildFiles (juce::File::findFiles, false, "*.xml"))
        names.add (f.getFileNameWithoutExtension());
    names.sortNatural();
    juce::StringArray all; // factory first, in table order, then user files
    for (const auto& e : FactoryPresets::kEntries)
        all.add (e.name);
    all.addArray (names);
    return all;
}

static juce::File presetFileFor (const juce::File& dir, const juce::String& clean)
{
    if (clean.isEmpty())
        return {};
#if defined (HSLINK_TEETH_NO_SANITIZE)
    return dir.getChildFile (clean + ".xml");
#else
    auto f = dir.getChildFile (clean + ".xml");
    return f.getParentDirectory() == dir ? f : juce::File();
#endif
}

bool HotStepLinkProcessor::savePreset (const juce::String& name)
{
#if ! defined (HSLINK_TEETH_PRESETS)
    if (FactoryPresets::isFactoryName (name))
        return false; // factory presets are read-only
#endif
    const auto dir = getPresetFolder();
    const auto f = presetFileFor (dir, sanitizePresetName (name));
    if (f == juce::File() || ! dir.createDirectory())
        return false;
    auto xml = captureSettings().toTree (kPresetId).createXml();
    return xml != nullptr && xml->writeTo (f);
}

bool HotStepLinkProcessor::loadPreset (const juce::String& name)
{
    if (const int fi = FactoryPresets::indexOf (name); fi >= 0)
    {
        applySettings (FactoryPresets::toSettings (FactoryPresets::kEntries[fi]));
        return true;
    }
    const auto f = presetFileFor (getPresetFolder(), sanitizePresetName (name));
    if (f == juce::File() || ! f.existsAsFile())
        return false;
    auto xml = juce::XmlDocument::parse (f);
    if (xml == nullptr || ! xml->hasTagName (kPresetId.toString()))
        return false;
    applySettings (GenSettings::fromTree (juce::ValueTree::fromXml (*xml)));
    return true;
}

bool HotStepLinkProcessor::deletePreset (const juce::String& name)
{
    if (FactoryPresets::isFactoryName (name))
        return false; // factory presets are read-only
    const auto f = presetFileFor (getPresetFolder(), sanitizePresetName (name));
    return f != juce::File() && f.existsAsFile() && f.deleteFile();
}
