// HOT-STEP LINK -- PluginProcessor.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include "WarpMarkers.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "CollectionStore.h"
#include "ExclusiveFile.h"
#include "TakeStretch.h"
#include "ClapStateVerdict.h"
#include <atomic>
#include <deque>
#include <chrono>
#include <vector>
#include <mutex>
#include <cmath>
#include <map>
#include <memory>
#include <functional>

#define HSLINK_VERSION "v" JucePlugin_VersionString  // from CMake project() VERSION, the only version source

// Host sync snapshot. Written by the audio thread, read lock-free by the UI.
struct HostSyncState
{
    std::atomic<double> bpm { 120.0 };
    std::atomic<int>    timeSigNum { 4 };
    std::atomic<int>    timeSigDen { 4 };
    std::atomic<double> ppqPosition { 0.0 };
    std::atomic<bool>   isPlaying { false };
    std::atomic<bool>   isLooping { false };
    std::atomic<double> loopStartPpq { 0.0 };
    std::atomic<double> loopEndPpq { 0.0 };
    std::atomic<bool>   hostSyncActive { false };
    std::atomic<unsigned int> updateCounter { 0 };
    // True only while the host really gives a tempo with its position. A host that
    // gives a position without one leaves bpm at its last value (120 at start), which
    // is then not a host tempo. Only meaningful while hostSyncActive is set.
    std::atomic<bool>   bpmKnown { false };

    // A tempo the host hands over counts only when it is a finite number above 0. Pure.
    static bool hostBpmUsable (double v) { return std::isfinite (v) && v > 0.0; }

    // The host tempo to hand to TempoResolve::resolve: bpm when the host is active and
    // its tempo is known, else 0 (absent). Pure.
    static double tempoForCascade (double bpm, bool known, bool active) { return active && known ? bpm : 0.0; }
    double tempoForCascade() const
    {
        return tempoForCascade (bpm.load (std::memory_order_relaxed), bpmKnown.load (std::memory_order_relaxed),
                                hostSyncActive.load (std::memory_order_relaxed));
    }
};

// Capture: lock-free SPSC ring fed by the audio thread, drained by a worker
// thread that keeps the take at the host rate, then on stop resamples offline
// to 48 kHz (LagrangeInterpolator) and writes a 32-bit float WAV.
class CaptureEngine : private juce::Thread
{
public:
    static constexpr double kOutRate = 48000.0;

    CaptureEngine() : juce::Thread ("HSLINK capture") {}
    ~CaptureEngine() override { stopCapture(); }

    // Message thread, not while capturing. Allocates the ring.
    void prepare (double hostRate, int numChannels, double ringSeconds = 32.0); // 32 s: loops survive faster-than-realtime bounces

    // Audio thread: no allocation, no locks.
    void push (const juce::AudioBuffer<float>& buf, int numSamples) { push (buf, 0, numSamples); }
    void push (const juce::AudioBuffer<float>& buf, int startSample, int numSamples);

    // Message thread.
    bool startCapture (const juce::File& wavOut);
    // Stops, drains, resamples, writes. Blocks until the file is written.
    bool stopCapture();
    bool isCapturing() const { return capturing.load (std::memory_order_acquire); }
    int getDroppedSamples() const { return dropped.load (std::memory_order_relaxed); }
    juce::File getLastFile() const { return outFile; }

private:
    void run() override;
    void drain();
    bool writeWav();

    double rate = 48000.0;
    int channels = 2;
    juce::AudioBuffer<float> ring;
    std::unique_ptr<juce::AbstractFifo> fifo;
    std::atomic<bool> capturing { false };
    std::atomic<int> dropped { 0 };
    std::vector<std::vector<float>> take; // worker thread only while running
    juce::File outFile;
};

// Snapshot of the server status, published by DawWorker.
struct DawEta
{
    double baseS = 0.0, perTakeS = 0.0, perAudioS = 0.0, coldLoadS = 0.0;
    juce::String source;
};

struct DawStatus
{
    bool online = false;
    bool everOnline = false;
    juce::String lastError;
    juce::String engineModel;
    juce::String etaBackend;    // status eta.backend: sa3, yue2, ace_turbo, ace_base; empty = not reported
    bool baseAvailable = false;
    bool nonTurboAvailable = false;
    std::map<juce::String, DawEta> etaByBackend;
    int heartbeatsOk = 0;       // successful heartbeats since start
    int consecutiveFailures = 0;
    juce::int64 lastOkMs = 0;   // Time::getMillisecondCounter domain
    // A request other than the heartbeat is on the wire and its response headers have
    // not arrived yet: when it was sent and its own timeout. waitingTimeoutMs 0 = none.
    // Set only when the worker was online at that moment, see DawWorker::computeOnline.
    juce::int64 waitingSinceMs = 0;
    int waitingTimeoutMs = 0;

    // Pure: the status pill text for an online server. etaBackend is eta.backend from
    // the status reply, engineModel is loaded_model (the ACE model name only).
    // "Online (YuE2)", "Online (SA3)", "Online (ACE: <model>)", "Online (ACE)"; with a
    // missing or unknown backend (older server) "Online (<model>)" or "Online (no model)".
    static juce::String onlinePillText (const juce::String& etaBackend, const juce::String& engineModel);
};

struct DawJob;
// Pure: the one state the header pill shows, from the heartbeat and the job list.
// offline when the server is not online; rendering when any job is running or being
// submitted; queued when any job waits; error when the job that finished last failed;
// idle otherwise.
enum class PillState { offline, idle, queued, rendering, error };
PillState pillStateFor (bool online, const std::vector<DawJob>& jobs);
const char* pillStateWord (PillState s); // "offline", "idle", "queued", "rendering", "error"

// R4a job client. One request to /api/v1/daw/generate.
struct DawJobRequest
{
    juce::String mode { "text2music" };
    juce::String caption, lyrics;
    juce::String expectedBackend;  // empty = Auto (whatever the server has active)
    int aceModel = 0;              // 0 Auto (omit), 1 Turbo, 2 Base; ACE only
    double durationS = 0.0;        // 0 = omit
    int takes = 1;
    double bpm = 0.0;              // 0 = omit
    juce::String timesig, key;     // empty = omit
    juce::int64 seed = -1;         // < 0 = random (omit)
    double guidance = -1.0;        // < 0 = Auto (omit); else clamped 0..20
    int steps = 0;                 // <= 0 = Auto (omit); else clamped 1..300
    juce::File sourceWav;          // set = multipart with src_audio
    bool hasRegion = false;        // repaint: region { start_s, end_s }
    double regionStartS = 0.0, regionEndS = 0.0;
#if defined (HSLINK_TEETH_BAD_FIELD)
    juce::String formField { "req" };     // teeth: wrong multipart field
#else
    juce::String formField { "request" }; // multipart field carrying the JSON
#endif
    // Pure: the DiT `model` field to send, empty = omit. Only ACE gets one; lego forces base.
    static juce::String resolveModel (const juce::String& mode, const juce::String& backend, int aceModel);
    // Pure: request JSON body.
    juce::String toJson() const;
};

// Snapshot of one job. kind generate or understand.
struct DawJob
{
    enum class State { pending, queued, running, done, partial, failed, cancelled };
    int localId = 0;
    bool isUnderstand = false;
    juce::String serverId;
    State state = State::pending;
    double progress = 0.0;
    double etaS = -1.0;
    int takeCount = 0;
    int takesReady = 0;            // reported ready by the server
    juce::Array<juce::File> takeFiles; // downloaded, index = take number
    bool downloadsComplete = false;
    bool cancelRequested = false;
    juce::String error;
    juce::String ditModel;         // server dit_model, empty = not reported
    juce::String phase;            // server phase (e.g. loading_model), empty = not reported
    juce::Array<juce::int64> takeSeeds; // takes[n].seed, -1 = not reported
    // understand result
    double bpm = 0.0;
    juce::String key, timesig;
    juce::StringArray tags;        // understand result tags, empty = none reported
    // worker-private
    DawJobRequest request;
    juce::File understandWav;
    juce::int64 nextPollMs = 0;
    int pollFailures = 0;
    bool cancelSent = false;
    // True from the moment the worker picks the job for its submit POST until that POST
    // has returned. Set and cleared under DawWorker::lock. A job in this window is in
    // flight: it is not waiting, cannot be removed, and Cancel takes the normal path.
    bool submitting = false;
    // Display name of the backend the worker is asking the server to make active for
    // this job ("SA3"), empty otherwise. Set and cleared under DawWorker::lock on the
    // listed entry only, around the switch request that runs before the submit POST.
    juce::String switchingTo;
    // Order in which the job reached a finished state (1 = first), 0 = not finished yet.
    // Set under DawWorker::lock; the highest one is the job that finished most recently.
    juce::uint64 finishSeq = 0;

    bool isTerminal() const { return state == State::done || state == State::partial
                                  || state == State::failed || state == State::cancelled; }
    bool isFinished() const { return isTerminal() && (downloadsComplete || isUnderstand
                                  || state == State::failed || state == State::cancelled); }
    static const char* stateName (State s);
    // The one word shown for a job, used by the queue row and by the job card header:
    // queued, switching, sending, running, done, partial, failed or cancelled. It reads the
    // flags first (waiting, switching, submitting, takes still downloading), then the state.
    static juce::String label (const DawJob& j);
    // The lead of the job card header: "Job " + label (j). The one difference from the
    // queue row: during a backend switch the header has room to name the backend and reads
    // "Switching to SA3...", where the row reads "switching".
    static juce::String headline (const DawJob& j);
    // Pure: the seed text of the job line from the seeds the server reported per take
    // (-1 = null or missing). More than one take with every seed the same is the request
    // seed repeated by the server, not per-take truth, so it is shown once and said so.
    // No seed reported at all (a random-seed job, or no takes listed) gives the empty
    // string: the job line then shows no seed part. The caller adds the separator only
    // for a non-empty text.
    //   {}            ""                      {7}          "seed 7"
    //   {5,5,5,5}     "seed 5 requested (per-take seeds not reported)"
    //   {11,12}       "seed 11, 12"           {5,-1,7}     "seed 5, unknown, 7"
    //   {-1,-1}       ""
    static juce::String seedText (const juce::Array<juce::int64>& takeSeeds);
    // Pure: status suffix for ETA or phase ("  Loading model...", "  ETA 42 s", or empty).
    static juce::String etaText (const DawJob& j);
    static State parseState (const juce::String& s, bool& ok);
};

// Background thread that owns all HTTP to the HOT-Step DAW facade. Never the
// audio or message thread. Offline after 3 failed heartbeats in a row or 10 s
// without a good response, measured with our own clock (socket timeouts can floor
// near 8 s on Windows). A good response is a parsed heartbeat or any other request
// that came back complete with HTTP 2xx (job poll, submit, cancel, backend call,
// take download): each sets lastOkMs and clears the failure count, see noteServerOk.
// A request also counts while it runs: when its headers arrive with HTTP 2xx and after
// each chunk of a take download. While a request waits for its headers the 10 s limit
// is held off, for at most that request's own timeout and only when the worker was
// online as the request was sent (see computeOnline).
// A failed request that is not a heartbeat changes nothing here. Offline retries
// back off 1, 2, 4 s, capped at 5 s.
class DawWorker : private juce::Thread
{
public:
    static constexpr int kHeartbeatMs = 3000;
    // Offline 10 s after the last good response at the latest (the deadline is read on
    // our own clock in getSnapshot, so a request that hangs cannot delay it). With a
    // server that refuses at once it is the third failed heartbeat, 6 to 9 s after the
    // server went away.
    static constexpr int kOfflineAfterMs = 10000;
    static constexpr int kOfflineAfterFailures = 3;
    static constexpr int kRequestTimeoutMs = 2500;   // connect + heartbeat
    static constexpr int kSubmitTimeoutMs = 120000;  // submit / upload (big src_audio, slow understand)
    static constexpr int kStallTimeoutMs = 15000;    // download: fail after 15 s without bytes

    DawWorker() : juce::Thread ("HSLINK daw worker"), takesFolder (defaultTakesFolder()) {}
    // Take file names. A take is downloaded to "<name>.part" in the takes folder and
    // renamed to <name> once the download is complete; <name> is then the take's file
    // name everywhere (temp folder, lasting folder, recovered folder, marker, star key).
    // takeFileName: StableTakes::makeTakeName with a fresh id for take n (0-based) of j.
    static juce::String takeFileName (const DawJob& j, int n);
    static juce::File partFileFor (const juce::File& take);
    // Renames part to take without replacing. When part is not there (the callback
    // already renamed it, or a test made the take itself) the result is whether take
    // exists. A rename that fails (a scanner, an indexer or a sync agent holds the file
    // just closed) is tried again, kPublishTries tries in all with publishWaitMs between
    // them, about 1.5 s, in sleeps of 10 ms that end as soon as shouldStop returns
    // true. Worker thread or tests only. When every try failed the result is false and
    // part is left where it is: a complete download is never deleted here.
    static bool publishDownload (const juce::File& part, const juce::File& take,
                                 const std::function<bool()>& shouldStop = {});
    static constexpr int kPublishTries = 6;
    // The wait after failed try number tryIndex (0-based): 50, 100, 200, 400, 750 ms.
    static int publishWaitMs (int tryIndex);
    // Test hook (teeth): false = the old publishDownload (one try, a failed rename
    // deletes part) and the old callers (the entry is dropped, the take fails).
    static std::atomic<bool> keepUnpublished;
    // Test hooks: the next N renames of publishDownload fail without being tried;
    // the number of rename tries made so far.
    static std::atomic<int> testFailPublish;
    static std::atomic<int> publishAttempts;
    // Test hook, false in the plugin: the worker signals its own exit right after a
    // take's download request returned (a close that arrives as a download completes).
    static std::atomic<bool> testExitAfterDownload;
    // Offline switch for test and tool processes, false in the plugin. true: no worker
    // of this process opens a request to the live default server (a loopback host on
    // kLivePort, see isLiveDefaultBase): the heartbeat fails and a request returns 0
    // before any socket exists. A worker pointed anywhere else (a local mock, a closed
    // port) is not touched. gateRequest is the one check, called by heartbeat() and
    // request() before they build their stream; requestsOpened counts the requests that
    // passed it, requestsRefused the ones it stopped.
    static std::atomic<bool> testNoLiveServer;
    // Test hook (teeth), 0 in the plugin: with the switch off, a request for the live
    // default server goes to this loopback port instead, so a mutant run never reaches
    // the real server either.
    static std::atomic<int> testLiveRedirectPort;
    static std::atomic<int> requestsOpened, requestsRefused;
    static constexpr int kLivePort = 3001;
    // Pure: 127.x.x.x, localhost, 0.0.0.0 or [::1] on kLivePort, with or without a path.
    static bool isLiveDefaultBase (const juce::String& baseUrl);
    static bool gateRequest (juce::String& base);
    // A take whose rename kept failing stays in "<name>.part". takeNameOf gives the
    // take's name for either file: the file name without a trailing ".part".
    static juce::String takeNameOf (const juce::File& tempFile);
    // The callee of onTakeDownloaded says that it keeps the take in part, so the
    // worker does not try the rename again. Worker thread or tests.
    void notePartKept (const juce::File& part);
    bool isPartKept (const juce::File& part) const;
    // Capture files (audio recorded from the track) share the temp session folder with
    // the takes. Their name is exactly source_<decimal digits>.wav; isCaptureName is
    // the one test for it. A take name always has a letter between "source_" and
    // ".wav" (..._<n>bpm_<id>.wav, ..._<n>bpm.wav, job<id>_<server>_take<n>.wav), so no
    // take name is a capture name whatever its caption.
    static juce::String captureFileName (int serial);
    static bool isCaptureName (const juce::String& fileName);
    // Test hook (teeth): false = the old test, any name that starts with "source_".
    static std::atomic<bool> captureByForm;
    // Test hook (teeth): false = the old temp name job<id>_<serverId>_take<n>.wav and
    // the id-less lasting name StableTakes::makeName.
    static std::atomic<bool> takeIdInName;
    ~DawWorker() override { stop(); }

    void start() { if (! isThreadRunning()) startThread(); }
    void stop();
    // Tells the thread to end and cancels the request in flight, without waiting.
    void signalStop();
    // Tests: true while the worker thread runs.
    bool testWorkerRunning() const { return isThreadRunning(); }

    // Any thread except audio.
    void setEndpoint (const juce::String& baseUrl, const juce::String& dawKey);
    juce::String getBaseUrl() const;
    void retryNow(); // reset backoff and heartbeat immediately
    DawStatus getSnapshot() const;

    // Pure: parse a /status JSON body into s (fields only). False if not an object.
    static bool parseStatus (const juce::String& body, DawStatus& s);
    // Pure: apply the offline rules at time nowMs. Never online before the first parsed
    // heartbeat or at the failure limit. Past the time limit it is online only while
    // s.waitingTimeoutMs is set and nowMs is at most that long after s.waitingSinceMs.
    static bool computeOnline (const DawStatus& s, juce::int64 nowMs, bool useDeadline,
                               int offlineAfterMs = kOfflineAfterMs,
                               int offlineAfterFailures = kOfflineAfterFailures);

    std::atomic<bool> deadlineEnabled { true }; // test hook (teeth check)
    // Test seams, the constants in the plugin: the two offline limits this worker uses,
    // so a test need not wait 10 real seconds.
    std::atomic<int> offlineAfterMs { kOfflineAfterMs };
    std::atomic<int> offlineAfterFailures { kOfflineAfterFailures };
    // Test hook (teeth): false = the old rule, only a heartbeat is proof of life.
    std::atomic<bool> anyResponseIsLife { true };
    // Test hook (teeth): false = no proof of life from inside a request (2xx headers,
    // download chunks); only the complete response counts, as before.
    std::atomic<bool> chunkStampsLife { true };
    // Test hook (teeth): false = a request that waits for its headers does not hold off
    // the time limit.
    std::atomic<bool> waitingHoldsOnline { true };
    // Test hook (teeth): false = legacy blocking download, no length check.
    std::atomic<bool> interruptible { true };
    // Test hook (teeth): false = one short timeout for every request (the fb5ea05a regression).
    std::atomic<bool> splitTimeouts { true };

    // Jobs (any thread except audio). Returns the local job id.
    int submitGenerate (const DawJobRequest& r);
    // submitGenerate: generate jobs run one at a time, in submit order; a job waits in state
    // pending (not yet sent) until no earlier generate job is in flight.
    // Returns 0 when kMaxQueued jobs are already waiting.
    int submitUnderstand (const juce::File& wav);
    void cancelJob (int localId);
    // Drop a waiting (not yet sent) job. False if it is being sent, was already sent, or is unknown.
    bool removeQueued (int localId);
    // Pure-ish: true for a generate job still waiting its turn (not picked for its submit yet).
    static bool isWaiting (const DawJob& j) { return ! j.isUnderstand && j.state == DawJob::State::pending && ! j.submitting; }
    static constexpr int kMaxQueued = 8;    // waiting generate jobs
    static constexpr int kKeepFinished = 8; // finished generate job cards kept (cards only, takes live on)
    // Called on the worker thread right after take takeIndex of job has been downloaded,
    // before the job is published as finished and so before its card can be pruned.
    // job.takeFiles[takeIndex] is the take's final temp file; at this point the bytes
    // are still in partFileFor (that file). The callee may publish them itself
    // (publishDownload); the worker does it after the call when the part is still there.
    // Set before start().
    std::function<void (const DawJob&, int)> onTakeDownloaded;
    // Test hook (teeth): false = legacy prune, oldest finished card first whatever it holds.
    std::atomic<bool> pruneTakelessFirst { true };
    // Test hook (teeth): false = legacy, every job submitted at once, no limit.
    std::atomic<bool> serialQueue { true };
    // Test hook (teeth): false = legacy, prune only on the next pass (keeps one extra).
    std::atomic<bool> pruneOnFinish { true };
    // Test hook (teeth): false = legacy, a job stays "waiting" (removable) during its submit POST.
    std::atomic<bool> submitMark { true };
    // Test hook (teeth): false = legacy, a submit result with no matching job is dropped.
    std::atomic<bool> orphanGuard { true };
    // Test hook (teeth): false = legacy, the "newest finished card" of the prune is the last
    // finished card in submit order instead of the one that finished most recently.
    std::atomic<bool> finishSeqRule { true };
    // Backend switch before a submit. A generate job that names a backend asks the server
    // which one is active (GET /api/backends) and, when it is another one, asks it to
    // switch (POST /api/backends/active { id }) before the submit POST. The server answers
    // the switch at once (it is a settings write; the weights are freed and loaded inside
    // the job), so kSwitchTimeoutMs only bounds a server that does not answer. Only a job
    // the user queued causes a switch: nothing switches at load or on a timer.
    // A switch that fails ends that one job as failed with switchFailText and cancels the
    // jobs waiting behind it for the same backend, so they do not each fail on their own.
    static constexpr int kSwitchTimeoutMs = 10000;
    // Test hook (teeth): false = legacy, no question and no switch, the submit goes as it is.
    std::atomic<bool> switchBackend { true };
    // Test hook, null in the plugin: stands in for the two backend requests. Returns the
    // HTTP code (0 = no answer) and fills body. method is GET or POST, path as above.
    using BackendCallFn = int (*) (const juce::String& method, const juce::String& path,
                                   const juce::String& jsonBody, juce::String& body);
    static std::atomic<BackendCallFn> testBackendCall;
    // Test hook, null in the plugin: runs on the worker thread at the top of each backend
    // request (the GET and the POST), after the switch mark is set and the lock released,
    // before the cancel check and the publish of the request. Lock not held.
    using BeforeBackendCallFn = void (*) (const juce::String& method, const juce::String& path);
    static std::atomic<BeforeBackendCallFn> testBeforeBackendCall;
    // Test hook (teeth): false = legacy, a cancel that lands before the request is
    // published is not seen and the request goes out.
    std::atomic<bool> cancelBeforePublish { true };
    // Test hook, null in the plugin: runs on the worker thread in processJobs for each
    // picked job, after the locked step that picks it (submitting and the switch mark are
    // set, the lock is released) and before stepJob, so before ensureBackend. Lock not held.
    using BeforeStepFn = void (*) (int localId);
    static std::atomic<BeforeStepFn> testBeforeStep;
    // Test hook (teeth): false = legacy, processJobs does not set the switch mark when it
    // picks the job; ensureBackend sets the first one (the row can read sending first).
    std::atomic<bool> earlySwitchMark { true };
    // Test hook (teeth): false = legacy, ensureBackend does not mark the switch before the
    // question, only after it when another backend is active.
    std::atomic<bool> markBeforeQuestion { true };
    // Count of switch requests (the POST) sent so far.
    std::atomic<int> switchesSent { 0 };
    // Display name of the backend being switched to right now, empty when none. Any thread
    // except audio.
    juce::String getSwitchingTo() const;
    // Pure: "SA3", "YuE2", "ACE" for the ids the plugin sends, else the id itself.
    static juce::String backendLabel (const juce::String& id);
    // Pure: the one message of a failed switch. code 0 = no answer, 200 = answered but
    // another backend is still active; body is the server's reply.
    static juce::String switchFailText (const juce::String& label, int code, const juce::String& body);
    // Count of server jobs cancelled because their local job had gone when the submit returned.
    std::atomic<int> orphansCancelled { 0 };
    juce::String getLastOrphanId() const;
    void pruneFinishedLocked();
    std::vector<DawJob> getJobs() const;
    bool getJob (int localId, DawJob& out) const;
#if defined (HSLINK_TEST_HOOKS)
    // Test hook: put a job as it is (e.g. done, with takeSeeds) into the list; returns its local id.
    int addJobForTest (DawJob j) { std::lock_guard<std::mutex> g (lock); j.localId = nextLocalId++; jobs.push_back (j); return j.localId; }
#endif
    void setTakesFolder (const juce::File& f);
    mutable std::mutex keptLock; // guards keptPart only
    juce::File keptPart;
    juce::File getTakesFolder() const;
    // Default: <temp>/HOTSTEP_LINK/session_<ms>_<6 hex>/ (a bare worker; the processor
    // points its worker at its own temp session folder instead)
    static juce::File defaultTakesFolder();
    // <temp>/HOTSTEP_LINK, or HSLINK_TEMP_ROOT when set (tests).
    static juce::File tempRoot();
    // Delete session_* folders and capture WAVs under root older than maxAgeMs. Returns count removed.
    // A session folder goes only when all three hold: (1) its owner.lock could be
    // acquired (ExclusiveFile::SessionLock: no live owner), (2) its newest content file
    // (not owner.lock, saved.log or the marker) is older than maxAgeMs, (3)
    // holdsUnsavedTake is false. Runs on the saver thread at its start; tests call it.
    // A folder without owner.lock (an older build, or an instance that runs lockless)
    // is never given one here: once it is older than maxAgeMs it is treated as dead and
    // rule (3) alone decides. A folder in the lockless registry is always left alone.
    // shouldStop (empty = never) is read before each session folder and before the
    // capture WAVs; once it reads true the pass ends and what it has not reached stays
    // as it was. The saver passes the processor's closing flag.
    static int cleanupStaleTemp (const juce::File& root, juce::int64 maxAgeMs, juce::Time now,
                                 const std::function<bool()>& shouldStop = {});
    // Test hooks: session folders cleanupStaleTemp looked at (all calls); false (teeth)
    // = shouldStop is never read.
    static std::atomic<int> cleanupFoldersVisited;
    static std::atomic<bool> cleanupStopCheck;
    // The age both the cleanup and the rescue judge a session folder by: the time of
    // its newest content file, at any depth (owner.lock, saved.log and the marker
    // directly in it are not content); with no content, the folder's own time.
    static juce::Time newestContentTime (const juce::File& folder);
    static juce::int64 staleAgeMs() { return (juce::int64) 24 * 3600 * 1000; }
    // Session folders of live instances in THIS process that run without a session
    // lock (their owner.lock could not be created). Rescue and cleanup of every
    // instance in the process skip them. Another process cannot see this list.
    static void setLocklessFolder (const juce::File& folder, bool lockless);
    static bool isLocklessFolder (const juce::File& folder);
    // Test hook (teeth): false = as before the zero-byte rules. The rescue stops at a
    // zero-byte source and keeps the marker, and a 200 download with a zero-byte file
    // counts as a complete take.
    static std::atomic<bool> zeroByteSkip;
    // Test hook: > 0 = that many coming downloads that end with 200 have their .part
    // file emptied before the completeness test (a 200 with an empty body).
    static std::atomic<int> testEmptyDownloads;
    // Test hook: zero-byte sources the rescue skipped (one per file per pass).
    static std::atomic<int> zeroByteSkips;
    // "saved.log" in a temp session folder: append-only, one line per take file of that
    // folder whose lasting copy was made and verified:
    // <take file name><TAB><byte size><TAB><lasting full path><LF>.
    // Written by whoever holds the folder's lock, opened and closed per line. A missing
    // line only means "not proven saved". bytes <= 0 is never written.
    static const char* savedLogName() { return "saved.log"; }
    static bool appendSavedLog (const juce::File& folder, const juce::String& fileName, juce::int64 bytes,
                                const juce::String& lastingPath);
    // The log as keys "<name><TAB><bytes>"; blank and malformed lines are ignored.
    static juce::StringArray readSavedLog (const juce::File& folder);
    // True when folder holds a take that is not proven saved: a *.wav directly in it
    // that is not a capture, has bytes and has no saved.log line of its name and size;
    // a *.wav.part that isTakePart accepts and that has no saved.log line of its size
    // under its own name or its .wav name; a file the marker lists that exists, has
    // bytes and is not in saved.log; or a marker this version does not understand.
    // It counts exactly the files rescueMarkedFolders would copy.
    static bool holdsUnsavedTake (const juce::File& folder);
    // The one rule, used by the rescue and by holdsUnsavedTake, for "this *.wav.part
    // holds a complete take". True when part has bytes and (a) the marker lists it by
    // its own path, or (b) the marker lists the .wav it was to become and that file
    // does not exist (a line is written only after a complete download), or (c)
    // ExclusiveFile::isCompleteWav (part). listed = the marker's files as full paths;
    // empty for an empty or missing marker, where only (c) can hold. Any other .part
    // is a download that was cut off: never copied, never a reason to keep a folder.
    static bool isTakePart (const juce::File& part, const juce::StringArray& listed);
    // Test hook (teeth): false = as before the .part rescue. The rescue copies *.wav
    // files and .part files the marker lists by their own path only, a listed .wav
    // that is missing is skipped even when its .part is there, and holdsUnsavedTake
    // counts a .part when the marker lists it (either way) or the marker is empty.
    static std::atomic<bool> partRescueEnabled;
    // owner.lock, saved.log and the marker: never content, never a take.
    static bool isBookkeepingName (const juce::String& fileName);
    // Test hooks. protectByDefault false = the cleanup rule before saved.log ("no
    // marker and older than maxAgeMs") and a rescue that looks only at marked folders.
    // cleanupLockedSkips: old folders cleanupStaleTemp left alone because their lock is held.
    static std::atomic<bool> protectByDefault;
    // false (teeth only) = cleanupStaleTemp may delete by age a folder that has a
    // marker and no owner.lock, once saved.log proves every take in it.
    static std::atomic<bool> legacyMarkerKeeps;
    static std::atomic<int> cleanupLockedSkips;
    // Marker file in a temp session folder that holds a take with no lasting copy.
    // Text: first line "HSLINK-UNSAVED v1", then one line per take:
    // <path relative to the session folder><TAB><lasting file name>.
    static const char* unsavedMarkerName() { return "hslink_unsaved.txt"; }
    // Test hook (teeth): false = no marker is written, cleanup ignores markers, no
    // background retry, no rescue, no notice (the behaviour before the unsaved guard).
    static std::atomic<bool> unsavedGuard;
    static constexpr int kPollMs = 1000;
    static constexpr int kJobTimeoutMs = 15000;
    static constexpr int kMaxPollFailures = 5;

    // Pure: apply a GET /jobs/:id body to j. False if not an object.
    static bool applyJobJson (const juce::String& body, DawJob& j);
    static bool isSafeServerId (const juce::String& id);
    // Pure: job id from a 200/202 submit body. Understand: analysis_id, else id.
    static juce::String extractServerId (const juce::var& v, bool isUnderstand);
    // Pure: error text from an envelope { error: { code, message } } or the HTTP code.
    static juce::String errorFromBody (const juce::String& body, int code);

private:
    void run() override;
    bool heartbeat (juce::String& body, juce::String& err);
    // HTTP on the worker thread. code 0 = connection failed. fileField empty = no upload.
    int request (const juce::String& method, const juce::String& path, const juce::String& jsonBody,
                 const juce::String& fileField, const juce::File& file, const juce::String& formField,
                 juce::String& body, const juce::File& downloadTo = {}, int timeoutOverrideMs = 0,
                 int cancelLocalId = 0);
    // Lock held. True when the listed job has a cancel waiting. 0 = no job, never true.
    bool cancelWaitingLocked (int localId) const;
    // A request other than the heartbeat came back good: sets status.lastOkMs and clears
    // status.consecutiveFailures, as a good heartbeat does. Caller holds lock.
    void noteServerOkLocked();
    // Proof of life from inside a running request (one download chunk). Takes the lock
    // for the stamp only. Nothing when chunkStampsLife is off.
    void noteServerOkMidRequest();
    // Worker thread, lock not held. True = go on with the submit. False = j was ended
    // (failed or cancelled) or the thread is leaving.
    bool ensureBackend (DawJob& j);
    void processJobs();
    void stepJob (DawJob& j); // works on a copy, published after
    bool hasActiveJobs() const;

    mutable std::mutex lock;
    juce::String lastOrphanId; // guarded by lock
    juce::String url, key;
    DawStatus status;
    juce::WebInputStream* inFlight = nullptr; // guarded by lock
    bool skippedForCancel = false; // worker thread only: the last backend request was not sent
    std::atomic<bool> wakeForRetry { false };
    std::vector<DawJob> jobs; // guarded by lock
    int nextLocalId = 1;
    juce::uint64 nextFinishSeq = 1; // guarded by lock
    juce::File takesFolder;   // guarded by lock
    juce::int64 nextHeartbeatMs = 0; // worker thread only
    int backoffMs = 1000;            // worker thread only
};


// R5 audition. Pure helpers first (testable without a host).
namespace AuditionMath
{
    // Mean-square RMS over all channels and samples. 0 for empty.
    float rmsOf (const juce::AudioBuffer<float>& b);
    // gain = targetRms / takeRms, clamped to [-40, +24] dB. 1 when disabled or silent.
    float matchGain (float targetRms, float takeRms, bool enabled);
    // -18 dBFS RMS default target.
    constexpr float kDefaultTargetDb = -18.0f;
    float defaultTargetRms();
    // Offline resample (Lagrange). Same buffer copy when rates match.
    juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double inRate, double outRate);
    // Take position in samples: (hostPpq - anchorPpq) beats at bpm, at rate.
    juce::int64 takeSamplePos (double hostPpq, double anchorPpq, double bpm, double rate);
    // Crossfade length for 10 ms at rate (>= 1).
    int crossfadeSamples (double rate);
    // Preview waveform: per-bin min/max over all channels (bins >= 1, each
    // bin covers an equal slice of the buffer). Empty buffer = all zeros.
    void computePeaks (const juce::AudioBuffer<float>& b, int bins, std::vector<float>& mn, std::vector<float>& mx);
    // Playhead as 0..1 of len; -1 when pos is outside [0, len) or len <= 0.
    double playheadFraction (juce::int64 pos, juce::int64 len);
    // Playhead snapshot: pos (low 32 bits, 0xFFFFFFFF = not playing) and len
    // (high 32 bits) as frame counts in one word, so the UI never reads a torn pair.
    juce::uint64 packPlayhead (juce::int64 pos, juce::int64 len);
    double unpackPlayhead (juce::uint64 packed); // playheadFraction of the pair
    // Editor-side gate for the preview peaks fetch: true only when the selected
    // file or the engine's peaks generation changed since the last fetch.
    struct PeakPoll
    {
        juce::File file;
        juce::uint32 gen = 0xFFFFFFFFu;
        bool primed = false;
        bool needsFetch (const juce::File& sel, juce::uint32 g);
    };
    constexpr int kPreviewBins = 512;
}

// One decoded take, resampled to the host rate. Immutable once published.
struct AuditionTake
{
    juce::AudioBuffer<float> audio;
    float rms = 0.0f;       // of the take as decoded
    float targetRms = 0.0f; // from the captured source, else -18 dBFS
    juce::File file;
    std::vector<float> peakMin, peakMax; // preview waveform, kPreviewBins each
};

// Audio thread renders the active take. Message thread selects; decoding runs
// on a background pool. Takes are swapped through an atomic pointer; the owner
// list frees retired takes only after the audio thread has moved off them.
class AuditionEngine
{
public:
    AuditionEngine();
    ~AuditionEngine();

    // Any non-audio-render thread (prepareToPlay). The re-decode it may need
    // is deferred to the message thread; selected is message-thread only.
    void prepare (double hostRate, int maxBlock);
    void select (const juce::File& take, const juce::File& sourceForTarget); // empty file = off
    juce::File getSelected() const { return selected; }
    void collectGarbage(); // message thread, call periodically

    // Pure-ish loader used by the pool, public for tests.
    // shouldAbort (optional) is polled between read chunks; true = give up (nullptr).
    static std::unique_ptr<AuditionTake> loadTake (juce::AudioFormatManager& fm, const juce::File& f,
                                                   const juce::File& source, double hostRate,
                                                   const std::function<bool()>& shouldAbort = nullptr);
    std::atomic<bool> interruptible { true }; // test hook (teeth): false = no abort polling
    // Test hook: publish a prepared take directly (message thread).
    void publish (std::unique_ptr<AuditionTake> t);
    void publishIfCurrent (std::unique_ptr<AuditionTake> t, int gen); // any thread except audio

    // Audio thread: no allocation, no locks. Writes the take over buf when a
    // take is active. playing false or position outside the take = silence of
    // the take. Pass-through when no take is active (and none fading).
    void render (juce::AudioBuffer<float>& buf, int numSamples, juce::int64 takePos, bool playing, bool levelMatch);

    std::atomic<float> lastGain { 1.0f }; // for UI readout
    // Playhead readout (audio thread writes, UI reads). -1 = not playing a take.
    std::atomic<juce::uint64> playhead { AuditionMath::packPlayhead (-1, 0) };
    // Bumped whenever a take is published or a decode fails (UI peaks refetch gate).
    std::atomic<juce::uint32> peaksGeneration { 0 };
    // Message thread: copy the preview peaks of the published take for f.
    bool getPeaks (const juce::File& f, std::vector<float>& mn, std::vector<float>& mx);

private:
    float sampleOf (const AuditionTake* t, int ch, juce::int64 pos, float gain, float input) const;

    void reselectForRate(); // message thread
    std::atomic<double> rate { 48000.0 };
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>> (true) };
    int xfadeLen = 480;
    juce::AudioFormatManager formats;
    juce::ThreadPool pool { 1 };
    std::atomic<AuditionTake*> requested { nullptr };
    // Audio thread state, mirrored to atomics so GC knows what is in use.
    AuditionTake* cur = nullptr;
    AuditionTake* prev = nullptr;
    int fadePos = 0;
    std::atomic<AuditionTake*> usedCur { nullptr }, usedPrev { nullptr };
    std::atomic<juce::uint32> blockCount { 0 }; // bumped at the end of each render
    // Message thread owner list. Freed only when retired, unused, and the audio
    // thread has finished 2 blocks since retirement.
    struct Owned { std::unique_ptr<AuditionTake> take; bool retired = false; juce::uint32 retiredAt = 0; };
    std::mutex ownLock; // never taken by the audio thread
    std::vector<Owned> owned;
    juce::File selected, selectedSource;
    std::atomic<int> generation { 0 };
};

// Take editor audition: Play, Stop and Loop of the take editor, with a clock of its
// own (the host transport is not read). The message thread asks; the decode runs on a
// pool thread through AuditionEngine::loadTake, so the audio is at the host rate. A
// clip goes to the audio thread through an atomic pointer. The audio thread adds the
// clip to the output and names the clip it reads in hazard; a retired clip is freed
// on the message thread, and only while hazard does not name it.
class EditorAudition
{
public:
    EditorAudition();
    ~EditorAudition();

    enum State { idle = 0, loading, playing };
    // prepareToPlay, any thread but audio: ends the audition; a new rate drops the cache.
    void prepare (double hostRate);
    // releaseResources, any thread but audio: ends the audition. After prepare() and
    // release() the clip is dropped without a fade (the host may stop processing, and
    // a fade in the first block after the gap would be a tick); stop() fades.
    void release();
    // Message thread. start, end and length are frames of the file; end <= start or
    // length <= 0 = the whole take. Plays from start, also while a clip is playing
    // (that one is faded out first).
    void play (const juce::File& take, juce::int64 start, juce::int64 end, juce::int64 length, bool loop);
    void stop(); // any thread but audio
    // Message thread, before play(): the warp markers of the take in frames of the file
    // (round 2b). Not empty: the next play() renders the piecewise stretched take on
    // its decode job (off the audio thread) and plays that; its length stays the take
    // length (anchors at 0 and the end), so start, end and the play fraction are dst
    // time. Empty: the plain take.
    void setWarp (std::vector<WarpMarkers::Marker> markers);
    // Loop on: the region repeats. Off: it plays to its end once. Takes effect at the
    // next pass through the end of the region.
    void setLoop (bool on) { loopOn.store (on, std::memory_order_relaxed); }
    State getState() const;
    // Any thread: where the clip plays, as a fraction of the decoded take (0..1), the
    // last position the audio thread wrote; -1 = no clip has a position. The editor
    // maps it to frames of the file (the same fraction, since the decode keeps length).
    double getPlayFraction() const { return playFrac.load (std::memory_order_relaxed); }
    // Message thread, once per tick: ends a clip that has played to its end and frees
    // retired clips.
    void poll();
    void dropCache(); // the decoded take kept for the next Play; any thread but audio

    // Audio thread: no allocation, no locks. Idle (no clip, or the clip has ended, and
    // the duck released) is one pointer load and compare, and buf is not touched. Else
    // what is in buf (the main output) is ducked to silence over the duck length while
    // a clip is wanted, and back after it; the clip is added on top. A clip taken
    // while the main output is not fully down waits until it is, then fades in.
    // hostBypassed (from processBlockBypassed, on the bypassed buffer): no clip is
    // wanted in this call, also one published during this block, and with a clip
    // playing or asked for bypassStop is raised (no owner lock). The clip that plays
    // fades out as after a stop() and the main output comes back over the duck length;
    // with no audition the buffer is not touched. getState() is idle from then on;
    // poll() or the next play() makes it a stop().
    void render (juce::AudioBuffer<float>& buf, int numSamples, bool hostBypassed = false);

    // Fade in at Play, fade out at Stop and at the end of a clip that does not loop.
    static constexpr double kFadeSeconds = 0.005;
    static int fadeSamples (double rate);
    // The duck of the main output, down and back: a cubic smoothstep over this length,
    // in samples at the host rate (set in prepare(), not the fade of the clip).
    static constexpr double kDuckSeconds = 0.012;
    static int duckSamples (double rate);
    // The gain of the main output at ramp position pos of total: exactly 1 at pos <= 0,
    // exactly 0 at pos >= total, between them 1 - (3x^2 - 2x^3) with x = pos / total.
    // Pure, no state: the one place the taper is written.
    static float duckGain (int pos, int total);
    std::atomic<bool> interruptible { true }; // test hook, as AuditionEngine::interruptible
    // Test hooks, empty in the plugin (one test each per call, off the audio thread):
    // in play() right after its generation, in poll() between its read and its lock.
    std::function<void()> afterGenForTest, pollGapForTest;
    std::function<void()> jobThrowForTest; // test hook: called at the top of the preview job (may throw)
#if defined (HSLINK_TEST_HOOKS)
    std::function<void()> bypassGapForTest; // test build only: in play() and poll(), between the stop() and the flag coming down
#endif

private:
    using Audio = std::shared_ptr<const juce::AudioBuffer<float>>;
    struct Clip // immutable once published
    {
        Audio audio;         // the whole take at the host rate
        int start = 0, end = 0; // the region in frames of audio
        int fade = 1;
        juce::uint32 serial = 0;
    };
    void publish (const Audio& a, juce::int64 start, juce::int64 end, juce::int64 length, double atRate, int gen);

    std::atomic<double> rate { 48000.0 };
    std::atomic<bool> loopOn { false };
    std::atomic<double> playFrac { -1.0 }; // audio thread writes, see getPlayFraction()
    std::atomic<int> generation { 0 };
    std::atomic<Clip*> requested { nullptr };
    std::atomic<Clip*> hazard { nullptr };        // audio thread writes: the clip it may read
    std::atomic<juce::uint32> doneSerial { 0 };   // audio thread writes: the clip that played to its end
    std::atomic<juce::uint32> activeSerial { 0 }; // the published clip, 0 = none
    std::atomic<bool> loadingNow { false };
    std::atomic<juce::uint32> dropGen { 0 };      // prepare() and release() count up
    std::atomic<bool> bypassStop { false };       // audio thread raises: stopped by host bypass
    std::atomic<int> duckLen { 576 };             // prepare() writes: the duck ramp in samples
    // Audio thread only.
    juce::uint32 seenDrop = 0; // dropGen as last read, read only while a clip is wanted or held
    Clip* cur = nullptr;
    int pos = 0;
    float gain = 0.0f;
    // The duck of the main output is a position on its ramp: 0 = released (gain 1),
    // duckN = ducked (gain 0), between them gain = duckGain (duckPos, duckN).
    // A change of direction walks the same curve back from where it stands: no step.
    // duckPos is written by duckNext() and duckSync() only.
    int duckPos = 0;
    int duckN = 576;   // the ramp length in use; follows duckLen only at an end of the ramp
    bool hold = false; // the clip taken waits until the main output is down
    bool ended = false, tailLoop = false;
    void duckSync();              // takes a new duckLen when the ramp is at one of its ends
    float duckNext (bool down);   // one sample along the ramp, returns the gain there
    // Moves the duck one step per sample over [from, to) of buf, down or up, and
    // applies it. Released and going up: buf is not touched.
    void duckMain (juce::AudioBuffer<float>& buf, int from, int to, bool down);

    struct Owned { std::unique_ptr<Clip> clip; bool retired = false; };
    std::mutex ownLock; // never taken by the audio thread
    std::vector<Owned> owned;   // ownLock
    juce::uint32 nextSerial = 0; // ownLock
    Audio cache;                // ownLock
    juce::File cacheFile;       // ownLock
    double cacheRate = 0.0;     // ownLock
    std::vector<WarpMarkers::Marker> warpMap; // ownLock, frames of the file; empty = unwarped
    juce::AudioFormatManager formats; // the pool thread only, after construction
    juce::ThreadPool pool { 1 };      // declared last, stopped first
};

// Round 6: every file-system call the editor makes goes through here, counted by
// what triggered it. The periodic tick and paint make none (tagTick and tagPaint stay
// 0 unless TakeGrid::testEditorStats is on). Tests read and reset the counters.
namespace EditorFs
{
    enum Tag { tagTick = 0, tagPaint, tagDrag, tagExport, tagCell, tagSelect, tagCount };
    extern std::atomic<int> testCalls[tagCount];
    inline void count (Tag t) { ++testCalls[t]; }
    inline bool exists (const juce::File& f, Tag t) { count (t); return f.existsAsFile(); }
    // Round 7: every file a take cell's waveform thumbnail was given, sorted by where it
    // is: under DawWorker::tempRoot() or anywhere else (the lasting root). The second
    // stays 0 in the plugin. Path compare only, no file call. Tests read and reset both.
    extern std::atomic<int> testThumbTemp, testThumbElsewhere;
    void countThumbSource (const juce::File& f);
}

// Round 6: the take grid as the editor shows it, built from the processor's take
// entries and nothing else (no file-system call). GUI-free: tests call build directly.
namespace TakeGrid
{
    struct Cell
    {
        juce::String key;      // the take's file name: the same before and after its lasting copy
        juce::String label;    // key without its extension, painted in the cell
        int jobId = 0, takeIndex = 0;
        juce::File temp, stable;
        juce::File file;       // what audition, star and Export act on: stable when saved, else temp
        bool shown = false;    // false: filtered out (another queue card, or teeth)
        bool saved = false;    // stable is set (a verified lasting copy was made)
        bool badge = false;    // NOT SAVED: a lasting-copy attempt failed
        bool pending = false;  // no lasting copy yet and no failed attempt (no badge)
        bool dragAllowed = false; // == saved
        bool selected = false, starred = false;
        int tempoState = HslinkTakeEntry::tempoNotMeasured; // HslinkTakeEntry::TempoState
        TempoMeasure::Result tempo;                         // meaningful in state measured
        TempoResolve::Resolved resolved;                    // the tempo of the grid and its source (HslinkTakeEntry::resolvedTempo)
        double durationS = 0.0;                             // HslinkTakeEntry::durationS, 0 = unknown
        int fitScore = -1;                                  // fitToBarsScore at the host tempo, -1 = no score (set by the editor)
    };
    // Fit to bars. Pure. How well a take lines up with whole host bars at the host tempo,
    // 0..100, -1 = no score (no host tempo, no length, or a bad argument).
    // Rationale: a take that drops into the arrangement without a cut or a stretch is
    // one whose length is a whole number of bars and whose tempo is the host tempo.
    //   length part: the distance from the nearest whole-bar length (at least one bar),
    //     in beats. 0 beats = 1.0, falling linearly to 0.0 at kFitBeatTolerance beats.
    //   tempo part (only when takeBpm > 0, e.g. the measured tempo): the relative error
    //     against the host tempo. 0 = 1.0, falling linearly to 0.0 at kFitTempoTolerance.
    //   score = round (100 * length part * tempo part). takeBpm <= 0 = length part only.
    constexpr double kFitBeatTolerance  = 1.0;  // beats off a whole bar for a score of 0
    constexpr double kFitTempoTolerance = 0.03; // 3 % tempo error for a score of 0
    int fitToBarsScore (double lengthSec, double takeBpm, double hostBpm, int beatsPerBar);

    // The tempo tag painted in a take cell. Pure.
    //   not measured: no text.            measuring: "bpm ..." (pending)
    //   level high:   "128.00 bpm  3 ms" green      level medium: the same, amber
    //   an octave of the asked tempo (Result::hintOctave): "140.02 bpm  x2  4 ms", "/2"
    //   level low with a drift above TempoMeasure::kWanderMs: "wanders 85 ms" red
    //   level low or none otherwise: "tempo ?" red. A number is shown only for a
    //   result that is valid (medium or high).
    struct TempoTag
    {
        enum Colour { blank = 0, pending = 1, green = 2, amber = 3, red = 4 };
        juce::String text;
        int colour = blank;
        bool operator== (const TempoTag& o) const { return text == o.text && colour == o.colour; }
        bool operator!= (const TempoTag& o) const { return ! (*this == o); }
    };
    TempoTag tempoTagOf (int tempoState, const TempoMeasure::Result& tempo);
    // The same with the tempo cascade (TempoResolve): a measured take whose tempo comes
    // from its file name or from the host shows "120 bpm (file name)" or "98 bpm (host)",
    // amber, instead of "tempo ?" or "wanders". Every other case is the tag above.
    TempoTag tempoTagOf (int tempoState, const TempoMeasure::Result& tempo, const TempoResolve::Resolved& resolved);
    // The fit tag of a take cell: "fit 87", green at 80 and up, amber at 50 and up, else
    // red. score < 0: an empty tag.
    TempoTag fitTagOf (int score);
    struct Model
    {
        std::vector<Cell> cells;  // one per take entry, in list order, shown or not
        std::vector<Cell> shown() const;
        const Cell* find (const juce::String& key) const;
    };
    // Test only. true = the old per-take existsAsFile filter in build (and the old
    // stats in the editor's paint and Export enable), through EditorFs so it is counted.
    extern std::atomic<bool> testEditorStats;
    // Test only (teeth THUMB_FROM_LASTING). true = the old thumbnail source: Cell::file,
    // which is the lasting copy for a saved take.
    extern std::atomic<bool> testThumbFromLasting;
    // The file a cell's waveform thumbnail reads: always the take's temp copy, never the
    // lasting one (JUCE opens the source on the message thread, and again per pixel
    // column in paint for a very short take). Empty temp = no source, a blank waveform.
    juce::File thumbSourceOf (const Cell& c);
    // Test only (teeth NO_TEMP_FALLBACK). true = the old behaviour: a click and Export
    // use Cell::file only, with no fallback to the temp copy.
    extern std::atomic<bool> testNoTempFallback;
    // The file a click (select / audition) or Export acts on. Called on a user action
    // only, never from the refresh or paint. Cell::file when it exists (one check); for a
    // saved take whose lasting file is gone, the temp copy when that exists (a second
    // check). Neither there: Cell::file, with *found = false. Checks go through EditorFs
    // under tag.
    juce::File actionFileOf (const Cell& c, EditorFs::Tag tag, bool* found = nullptr);
    juce::String keyOf (const HslinkTakeEntry& e);
    // selected: the audition selection (a path; compared, never opened).
    Model build (const std::vector<HslinkTakeEntry>& takes, int selectedJob, bool showUnsaved,
                 const juce::File& selected, const std::function<bool (const juce::File&)>& isStarred);
}

// R6: stable take folder. Hosts reference dragged files after the plugin
// closes, so takes are copied out of the temp session folder before any drag.
namespace StableTakes
{
    // Pure: lowercase [a-z0-9] runs joined by '_', trimmed, max maxLen chars.
    // Empty result becomes "take".
    juce::String slugify (const juce::String& text, int maxLen = 40);
    // Pure: <caption-slug>_<backend>_t<n>_<bpm>bpm.wav (backend empty = auto).
    juce::String makeName (const juce::String& caption, const juce::String& backend, int takeNumber, double bpm);
    // The file name of a take: makeName with a take id before the extension,
    // <caption-slug>_<backend>_t<n>_<bpm>bpm_<6 hex>.wav. id empty = a fresh
    // ExclusiveFile::uniqueToken (6).
    juce::String makeTakeName (const juce::String& caption, const juce::String& backend, int takeNumber, double bpm,
                               const juce::String& id = {});
    // First free name in folder: name, then name_2, name_3 ... before the extension.
    juce::File uniqueFile (const juce::File& folder, const juce::String& name);
    // Copies src into folder under a unique name (ExclusiveFile::copyUnique: .part file,
    // then an exclusive rename). Never replaces a file. Returns the new file, or {}.
    // shouldAbort and result as in ExclusiveFile::copyUnique.
    juce::File copyInto (const juce::File& src, const juce::File& folder, const juce::String& name,
                         const ExclusiveFile::AbortFn& shouldAbort = {},
                         ExclusiveFile::CopyResult* result = nullptr);
    // userDocuments/MDMAchine/HOT-STEP LINK/takes
    juce::File defaultRoot();
}

class HotStepLinkProcessor : public juce::AudioProcessor,
                             public clap_juce_state_load_verdict
{
public:
    HotStepLinkProcessor();
    ~HotStepLinkProcessor() override;
    // Own temp session folders (DAW takes + captures); deleted on destruction unless referenced.
    juce::Array<juce::File> getOwnTempFolders() const;

    // Takes outlive job cards. One entry per downloaded take, added on the DawWorker
    // thread when the download completes; the lasting copy (stable) is made right
    // after by the saver thread. Guarded by takesLock, never touched by the audio thread.
    // Declared in CollectionStore.h: ids, files and the generate settings of the job.
    using TakeEntry = HslinkTakeEntry;
    // Pure apart from reading the take file's header for the duration: the settings
    // part of a TakeEntry for take takeIndex of job.
    // readFile false = no file is opened: durationS is the requested length.
    static void fillTakeMetadata (TakeEntry& e, const DawJob& job, int takeIndex, bool readFile = true);
    // The length in seconds read from the header of take, or of <take>.part when take
    // is not there; 0 when neither can be read. File IO: never under takesLock.
    static double readTakeDuration (const juce::File& take);
    // Tempo measurement of a take (TempoMeasure). Reads the file read only, whole, into
    // memory, sums it to mono and analyses that copy; nothing is shared with the audio
    // thread and the file is never written. Never the audio or the message thread.
    // false = aborted or not readable (out is left alone). Static: touches no processor.
    // Never throws: a broken header, a file that goes away during the read or a failed
    // allocation gives false.
    static bool measureTakeFile (const juce::File& take, double hintBpm, TempoMeasure::Result& out,
                                 const std::function<bool()>& shouldAbort = nullptr);
    // What the pool job stores. true = the take counts as measured: out is the result,
    // or a level none result when the file could not be read (the cell shows "tempo ?").
    // false = aborted: the take goes back to not measured. Never throws.
    static bool measureTakeForCell (const juce::File& take, double hintBpm, TempoMeasure::Result& out,
                                    const std::function<bool()>& shouldAbort = nullptr);
    // Tests only, null in the plugin: called with the file about to be read, after it
    // was found and before its bytes are loaded. May remove the file or throw.
    using MeasureReadHook = void (*) (const juce::File&);
    static std::atomic<MeasureReadHook> measureReadHook;
    // Tests only, null in the plugin: called in requestTakeMeasure with step 0 right
    // before the job is queued (calling thread) and with step 1 in the job right before
    // the result is stored (pool thread). May throw.
    using MeasureStepHook = void (*) (int step);
    static std::atomic<MeasureStepHook> measureStepHook;
    // Queues the measurement of the listed take with this temp file on the measure pool
    // and marks it measuring. Any thread but the audio thread; returns at once. The
    // result lands in the take entry (tempoState, tempo), which the editor reads at its
    // next refresh. Nothing is queued when the take is not listed or the plugin is closing.
    // Never throws: when the job cannot be queued, or a step of it throws, the take ends
    // as measured with level none ("tempo ?") instead of staying at measuring.
    void requestTakeMeasure (const juce::File& tempFile, double hintBpm);
    // Pure: the file name of the copy of take name at targetBpm,
    // <stem>_st<target>bpm.wav (target with up to 2 decimals, "130", "129.5").
    static juce::String stretchedTakeName (const juce::String& name, double targetBpm);
    // Queues a time stretch (TakeStretch, pitch kept) of the listed take with this temp
    // file from fromBpm to toBpm on its own pool thread. Any thread but the audio
    // thread; returns at once, true = queued. The stretched file is written into the
    // take's temp session folder under stretchedTakeName and listed as a NEW take the
    // way a download is: unsaved, marker synced, tempo measured (asked tempo = target),
    // saver woken for the lasting copy. The original entry and its file are untouched.
    // Nothing is listed when the stretch fails, the name exists already, or the plugin
    // is closing. Never throws. Shares the gate of the tempo job (MeasureGate).
    bool requestTakeStretch (const juce::File& tempFile, double fromBpm, double toBpm);
    // Tests: waits until no stretch is waiting or running.
    bool waitTakeStretchIdle (int timeoutMs = 60000);
    // Pure: the file name of the edited copy of take name, <stem>_edc<hash>.wav for a
    // crop and <stem>_edl<repeats>x<hash>.wav for a loop render. hash = 4 hex digits of
    // the edit points and options as asked, so the same request gives the same name.
    static juce::String editedTakeName (const juce::String& name, const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options);
    // Stores the crop and loop points of the listed take with this temp file (sample
    // frames of that file, see TakeEdit::EditData; bad values are taken out). Nothing is
    // rendered and the file is not touched. false = not listed. Never throws.
    bool setTakeEdit (const juce::File& tempFile, const TakeEdit::EditData& edit);
    // What a finished edit job did. ticket is the number requestTakeEdit gave for that
    // job. serial counts the finished jobs (0 = none yet). gridSkipped: snap to grid was
    // asked for but the take has no valid measured tempo, so the points were used as
    // given. sourceName is the take's name, or its temp file name when the entry has none.
    struct TakeEditReport
    {
        int serial = 0;
        juce::uint64 ticket = 0;
        bool ok = false, gridSkipped = false;
        juce::String sourceName, newName; // newName is empty when nothing was listed
        TakeEdit::EditResult result;
    };
    // The report of the job that finished last, whatever its ticket.
    TakeEditReport getLastTakeEditReport() const;
    // The report of the job with this ticket. false = that job has not finished yet, or
    // its report is older than the last kTakeEditReportsKept. Never another job's report.
    bool getTakeEditReport (juce::uint64 ticket, TakeEditReport& out) const;
    static constexpr int kTakeEditReportsKept = 16;
    // Where the edit job of a ticket is. queued = asked for, waiting on the pool thread
    // behind another take job (a stretch or an edit). running = its render has started.
    // done = its report can be read with getTakeEditReport. unknown = no such job: never
    // asked, ended without a report, its report is older than the last
    // kTakeEditReportsKept, or the plugin is closing. Any thread but the audio thread.
    enum class TakeEditState { unknown = 0, queued, running, done };
    TakeEditState getTakeEditState (juce::uint64 ticket) const;
    // Test hook, 0 in the plugin. An edit job whose ticket is this number or higher waits
    // right after it was marked running, until the number is changed or the plugin closes.
    static std::atomic<juce::uint64> testHoldEditFromTicket;
    // Test hook (teeth), true in the plugin. false = the old report store: one slot, read
    // whatever the ticket asked for, named by the entry's name alone.
    static std::atomic<bool> editReportsByTicket;
    // Tests: gives the listed take with this temp file another name (may be empty).
    void setTakeNameForTest (const juce::File& tempFile, const juce::String& name);
    // Queues a crop or loop render (TakeEdit) of the listed take with this temp file on
    // the pool thread of the stretch (one heavy take job at a time). Any thread but the
    // audio thread; returns at once with the job's ticket (never 0), the key of its
    // report in getTakeEditReport. 0 = not queued, whyNot (if given) says why in plain
    // words. edit is stored on the take as
    // setTakeEdit does. options.gridBpm and options.gridDownbeatSec are ignored: with
    // snapToGrid the grid is the take's own measured tempo and downbeat when the take is
    // measured and the result is valid, else the grid snap is skipped (see
    // TakeEditReport::gridSkipped; also logged). The file is written into the take's
    // temp session folder under editedTakeName and listed as a NEW take the way a
    // stretched one is: unsaved, marker synced, tempo measured, saver woken, no edit
    // points of its own. The original entry's audio file is untouched. Nothing is listed
    // when the render fails, the name exists already, or the plugin is closing. Never
    // throws. Shares the gate of the tempo job (MeasureGate).
    juce::uint64 requestTakeEdit (const juce::File& tempFile, const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options,
                                  juce::String* whyNot = nullptr);
    // Tests: waits until no edit (and no stretch: the same pool) is waiting or running.
    bool waitTakeEditIdle (int timeoutMs = 60000);
    // Tests: waits until no measurement is waiting or running.
    bool waitTakeMeasureIdle (int timeoutMs = 10000);
    // Test hook (teeth), true in the plugin. false: no take is measured.
    static std::atomic<bool> tempoMeasureEnabled;
    // Bound of the take list: 256 = 32 full jobs of 8 takes, far past what the grid
    // can usefully show, at about 1 KB per entry. Past it the oldest take that is
    // unstarred, not pinned and has its lasting copy leaves the LIST only; no file is
    // deleted. Starred takes never leave, so the list can exceed the bound if more
    // than 256 takes are starred.
    static constexpr int kMaxTakes = 256;
    // Worker thread (DawWorker::onTakeDownloaded) or tests. Order: the entry is added
    // to the list as unsaved with the settings of its job (no file is opened), the
    // marker is synced, the take's length is read from the file and written to the
    // entry, the download is published (<take>.part renamed to the take file, when a
    // part is there), then the saver thread is woken. This call makes no lasting copy
    // and no file call outside the temp session folder: the saver thread makes the
    // copy and sets stable on the entry. The entry's name is the take's file name.
    void registerTake (const DawJob& job, int takeIndex);
    // Tests: registerTake, then waitSaverIdle. To the caller this is the old
    // registerTake that returned after the copy attempt.
    void registerTakeAndWait (const DawJob& job, int takeIndex) { registerTake (job, takeIndex); waitSaverIdle(); }
    // Tests: waits until the saver thread has finished a pass that began after the
    // last recorded take. false = closing, the saver is not running, or the time ran out.
    bool waitSaverIdle (int timeoutMs = 30000);
    // Teeth only. true = registerTake makes the lasting copy itself on the calling
    // thread, as before the saver made every copy (the saver is not woken).
    static std::atomic<bool> testWorkerCopies;
    // Teeth only. true = the destructor decides keep-or-delete with the old probing
    // (legacyFolderReferenced: file stats under takesLock, one probe of the lasting
    // root per starred take). testDtorLastingProbes counts those lasting-root probes;
    // the shipped destructor makes none.
    static std::atomic<bool> testDtorProbesLasting;
    static std::atomic<int> testDtorLastingProbes;
    // false (teeth only) = the worker is told to end only after the wait for the
    // collection pool, as before.
    static std::atomic<bool> signalWorkerEarly;
    // false (teeth only) = the saver may attempt a take that registerTake is still
    // publishing (TakeEntry::handingOver is ignored): the attempt finds no file,
    // fails, and the take shows as not saved until a timed retry.
    static std::atomic<bool> handOverGuard;
    // Test hook, null in the plugin: called by the destructor right before its wait
    // for the collection pool (after the early signals). A test holds the close there.
    static std::atomic<void (*) (HotStepLinkProcessor&)> testBeforePoolWait;
    // Test hook, null in the plugin: the name of the temp session folder for try
    // number attempt (0 to 4) of the constructor; an empty answer = the normal name.
    static std::atomic<juce::String (*) (int attempt)> testSessionNameFor;
    // Test hooks. Defaults are the shipped behaviour.
    // true = registerTake returns after the entry is recorded and the marker synced,
    // before the length is read and before the rename (as if the process died there):
    // the complete bytes are in <name>.wav.part and the marker line names <name>.wav.
    // With recordBeforeMetadata false it returns after the metadata was read and before
    // the entry is added: a complete .part with no entry and no marker line.
    static std::atomic<bool> testStopBeforePublish;
    // false = the order before this rule (teeth only): the metadata, with the length
    // read from the .part file, is filled first and the entry is recorded after it.
    static std::atomic<bool> recordBeforeMetadata;
    // true = registerTake returns right after the entry is recorded, the marker synced
    // and the file published, without waking the saver thread (as if the process died
    // there). A timed retry pass of the saver can still copy the take.
    static std::atomic<bool> testStopAfterRecord;
    // false = the old order (teeth only): publish, copy, and only then add the entry.
    // With testStopAfterRecord also set, the old order returns after the publish and
    // before the copy and the entry (as if the process died while the copy ran).
    static std::atomic<bool> recordBeforeCopy;
    // Any thread except audio. Oldest first. A copy of the list only: no disk IO.
    std::vector<TakeEntry> getTakes();
    // One copy pass over the takes with no lasting copy; returns how many of them
    // still have none. freshOnly = only takes whose copy was never attempted (the pass
    // after a take was recorded); firstFailures, when given, is raised once per take
    // whose first attempt failed in this pass. File IO: the saver thread, or a test
    // that calls it from its own thread (then nothing serialises it with the saver;
    // the copies are still safe, a duplicate is dropped). Never the message thread.
    // Stops between takes once the processor is closing.
    int ensureStableCopies (bool freshOnly = false, int* firstFailures = nullptr);

    // Takes with no lasting copy. They stay listed (stable empty, temp valid), their
    // temp session folder carries the marker file, and a saver thread retries the copy
    // every retryBaseMs, doubling up to retryMaxMs while it keeps failing.
    struct SaveNotice
    {
        int unsaved = 0, recovered = 0;
        // True when no marker could be put into a temp folder that holds an unsaved
        // take: nothing on disk keeps that folder from a later clean-up.
        bool unprotected = false;
        juce::String stableFolder, tempFolder, recoveredFolder;
        juce::String text() const; // empty when there is nothing to tell
    };
    SaveNotice getSaveNotice() const; // any thread except audio; no disk IO
    void requestSaveRetry();          // any thread except audio: the saver thread retries now
    // Saver thread, at start (and on retryNow). Walks the temp root; for every
    // session_* folder that has an owner.lock this call can win (its owner is dead), it
    // keeps the lock and pushes every take WAV there (not source_*, not zero bytes,
    // not in saved.log) into the take list as adopted, saved by ensureStableCopies
    // into recovered_<session>. The folder is removed once every adopted take in it is
    // saved. Returns takes adopted.
    int adoptDeadFolders();
    static std::atomic<int> adoptedTakes;   // test hook: takes pushed by adoptDeadFolders
    static std::atomic<bool> adoptEnabled;  // false = teeth: no adoption, the old rescue only
    static std::atomic<bool> keepGuard;     // false = teeth (NO_KEEP_GUARD): W1 and W2 keep logic off
    // Test hooks for the marker and for closing. Defaults are the shipped behaviour.
    // false = as before the shutdown guard: the closing flag is set after the worker
    // stopped, a retry pass never stops early, registerTake copies while closing, and
    // the destructor runs a copy pass on its own thread.
    static std::atomic<bool> shutdownGuard;
    // Every lasting-copy attempt waits this long first (a stand-in for a share that
    // does not answer).
    static std::atomic<int> testCopyDelayMs;
    // Lasting-copy attempts started after any processor's destructor began.
    static std::atomic<int> testAttemptsAfterClose;
    // Tests: sets the closing flag without destroying the processor.
    void testBeginShutdown() { shuttingDown = true; }
    // F1 hooks (rescueMarkedFolders, the loop over a marker's listed takes).
    // testRescueLastingProbes: file calls the loop made on the lasting root to see if a
    // take is already recovered (2 per probed take at most). testBeforeRescueTake: called
    // before each listed take is looked at. testRescueProbeBeforeClosing: true (teeth
    // only) = the old order, the probe runs without a closing check before it.
    static std::atomic<int> testRescueLastingProbes;
    static std::atomic<void (*) (HotStepLinkProcessor&)> testBeforeRescueTake;
    static std::atomic<bool> testRescueProbeBeforeClosing;
    // Test hooks: calls that entered rescueMarkedFolders; folders it left alone because
    // their owner.lock is held by someone alive.
    static std::atomic<int> rescueCalls;
    static std::atomic<int> rescueLockedSkips;
    // Empties dir except owner.lock, releases lock, then removes the lock file and dir.
    static bool removeLockedFolder (const juce::File& dir, ExclusiveFile::SessionLock& lock);
    // W2: true only when every entry directly in an adopted dead folder is in the
    // allowed set (adoptedFolderAllowed in the .cpp): owner.lock, saved.log, the legacy
    // marker, a capture (source_<digits>.wav), a zero-byte file, the "Deleted" folder,
    // or a *.wav / *.wav.part that saved.log proves saved. Anything else keeps the folder.
    static bool adoptedFolderSafeToRemove (const juce::File& dir);
    // The first blocking entry name (a folder ends in "/"), empty when safe.
    static juce::String adoptedFolderBlocker (const juce::File& dir);
    // Same test as above; a kept folder is logged once per session with its blocker.
    static bool keptFolderCheck (const juce::File& dir);
    static std::atomic<bool> keepLog; // false = teeth (NO_KEEP_LOG): the kept-folder line is off
    // Tests: true while this instance holds the lock of its temp session folder.
    bool holdsSessionLock() const { return ownLockHeld.load(); }
    // An instance whose owner.lock could not be created tries again here, on the saver
    // thread at each of its wake-ups, until it holds the lock. Returns true when held.
    bool retrySessionLock();
    std::atomic<int> copyAttempts { 0 };   // every lasting-copy attempt (tests)
    std::atomic<int> retryPasses { 0 };    // background retry passes run (tests)
    std::atomic<int> rescuePasses { 0 };   // rescue passes finished (tests)
    std::atomic<int> retryBaseMs { 10000 }, retryMaxMs { 60000 };
    // The take that is selected / auditioning: never dropped by the bound. {} = none.
    void setPinnedTake (const juce::File& stableTake);

    // Delete a take, with a short undo. deleteTake takes the entry out of the list at
    // once and holds it as the one pending delete; its files are not touched. The move
    // happens on the delete pool thread when the delete is committed: moveToDeletedFolder
    // (DeletedFolder.h), a plain rename into a folder named "Deleted" in the same
    // directory as the file. No OS trash, no shell call, nothing is deleted and LINK
    // never empties that folder. When the rename cannot be made the file is left where
    // it is, the take is kept and a message is shown. Committed
    // by pollTakeDelete once takeDeleteUndoMs has
    // passed, by the next deleteTake, by commitTakeDelete (the editor calls it when it
    // closes), or by the destructor (there on the closing thread). The file moved is
    // the lasting copy when the take has one, else the temp file; a file that is
    // already gone is not an error. When the move fails the entry goes back into the
    // list at its old place and a message is left for takeTakeDeleteMessage.
    // The key is the full path of the take's temp file (TakeEntry::temp): every entry
    // has one, and no two listed entries share it (registerTake refuses a second entry
    // for the same temp file). The lasting path is empty until the copy succeeds and
    // two takes can have the same name, so neither is the key. Stars stay keyed by name.
    // Refused, with nothing changed: a starred take, a take registerTake is still
    // handing over, an unknown key, a closing processor. All refusals are decided
    // before anything is touched, so a refused call also leaves an earlier pending
    // delete pending with its own deadline; only an accepted delete commits the one
    // before it. A take that was added to the
    // Collection is NOT refused: the Collection holds its own copy of the WAV.
    // The star is checked again right before the move, on every commit path (in
    // finishTakeDelete). A take that got a star while its delete was pending or
    // queued is not moved: the entry goes back into the list as for an undo, a
    // message is left for takeTakeDeleteMessage and takeDeleteStarKeeps counts it.
    // In the destructor the entry is put back too (as for a failed move there) and
    // no message is read by anyone. The check and the move are two steps: a star set
    // between them does not stop the move.
    // deleteTake and undoTakeDelete: message thread (deleteTake turns audition off
    // when the take is the selected one). The others: any thread except audio.
    enum class TakeDelete { deleted, starred, busy, notFound, closing };
    TakeDelete deleteTake (const juce::String& tempPath); // key: TakeEntry::temp.getFullPathName()
    bool undoTakeDelete();                           // false = nothing pending any more
    // Clear unstarred: every listed take that is not starred and not still handing over
    // leaves the list as ONE pending batch delete (same window, same commit paths and
    // Deleted-folder move as deleteTake). One undoTakeDelete puts the whole batch back
    // at its old places. A delete pending before it is committed first. Starred takes
    // are untouched. Returns the number of takes taken out (0 = nothing changed).
    // Message thread (turns audition off when the selected take is in the batch).
    int clearUnstarredTakes();
    void pollTakeDelete();                           // commits the pending delete when its window has passed
    void commitTakeDelete();                         // commits it now (returns at once, the move runs on the pool)
    juce::String getPendingTakeDelete() const;       // the pending take's name, empty = none
    juce::String takeTakeDeleteMessage();            // the message of a failed move or of a take kept for its star, handed out once
    // Pure. The line the user sees for a deleteTake result (empty for deleted).
    static juce::String takeDeleteText (TakeDelete result);
    static juce::String takeDeleteFailedText (const juce::String& takeName);
    static juce::String takeDeleteStarredText (const juce::String& takeName); // starred when its delete was committed
    std::atomic<int> takeDeleteUndoMs { 8000 };      // the undo window; tests shorten it
    std::atomic<int> takeDeleteMoves { 0 };          // moves asked for a file that was there (tests)
    std::atomic<int> takeDeleteFailures { 0 };       // moves that failed and put the take back (tests)
    std::atomic<int> takeDeleteStarKeeps { 0 };      // commits that found the take starred and put it back, no move (tests)
    // Test hook, null in the plugin (moveToDeletedFolder is called). A test points it
    // at its own function in place of the move; false = failed, the take is kept.
    // The seam of the move itself is DeletedFolder::testFailRename.
    static std::atomic<bool (*) (const juce::File&)> testTrashFile;
    // Tests: waits until no committed delete is waiting or running on the pool.
    bool waitTakeDeleteIdle (int timeoutMs = 10000);
    // Test hook (teeth): false = legacy, no take list; takes come from the live job
    // cards and the lasting copy is made lazily by getTakes().
    std::atomic<bool> takeStore { true };

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { editorAudition.release(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // Host bypass: the take editor audition stops; the buffer is JUCE's default. Only
    // when a clip played or the main output was ducked at that moment is the buffer
    // written: the clip fades out over its fade and the main output comes back over
    // the duck ramp, as after a Stop, instead of a cut and a step.
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::AudioProcessor::processBlockBypassed (buffer, midi);
        editorAudition.render (buffer, buffer.getNumSamples(), true);
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HOT-STEP LINK"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    // Applies a saved state. Returns false, and changes nothing, for malformed data.
    bool loadState (const void* data, int sizeInBytes);
    // CLAP stateLoad verdict: false when the last setStateInformation rejected the chunk.
    bool clapLastStateLoadAccepted() const override { return lastStateLoadOk.load(); }

    // Message thread only.
    juce::String getServerUrl() const;
    juce::String getSavedServerUrl() const;
    static juce::String serverUrlOverride();
    void setServerUrl (const juce::String& url);
    // The DAW key is per user, stored in userApplicationDataDirectory/HOT-Step/LINK.settings
    // (env HSLINK_SETTINGS_FILE overrides, for tests). Plugin state keeps only a keySet flag.
    juce::String getDawKey() const;
    void setDawKey (const juce::String& key);
    static juce::File keyStoreFile();
    static juce::String loadStoredKey();
    static void saveStoredKey (const juce::String& key);

    // Message thread: request with host metadata filled (bpm, timesig, duration
    // from the loop region when looping). manualBpm used in standalone.
    DawJobRequest makeRequest (const juce::String& caption, double manualBpm = 0.0) const;
    // Pure: fill r from a host sync snapshot.
    static void fillHostMetadata (DawJobRequest& r, const HostSyncState& s, bool isStandalone, double manualBpm);

    // Pure helper so tests can drive it with a fake play head.
    static void updateHostSync (HostSyncState& s, juce::AudioPlayHead* ph, bool standalone);

    // Source clip for repaint / lego. Message thread unless noted.
    enum CapPhase { capIdle = 0, capArmed = 1, capRecording = 2, capFinished = 3 };
    // Arm: capture starts when the host plays inside the loop (or at once with
    // no loop) and stops at the loop end or transport stop. Off cancels.
    bool armCapture (bool on);
    bool isArmed() const { const int p = capPhase.load(); return p == capArmed || p == capRecording; }
    bool isRecordingSource() const { return capPhase.load() == capRecording; }
    // Finalises a finished take into the source. Returns true when the source changed.
    bool pollCapture();
    // Standalone drop / chooser (also used for finished captures).
    bool setSourceFile (const juce::File& wav);
    void clearSource();
    juce::File getSourceFile() const { return sourceFile; }
    // Input peak since the editor last read it (audio thread max-stores, editor exchanges to 0).
    std::atomic<float> inputPeak { 0.0f };
    double getSourceLengthS() const { return sourceLenS; }
    // Host tempo the source was captured at; 0 = unknown (no host tempo at capture).
    double getSourceBpm() const { return sourceBpm; }
    bool sourceReady() const { return sourceLenS > 0.0 && sourceFile.existsAsFile(); }
    // The same from state, no file call: a source clip was loaded. For the editor's
    // tick; a clip deleted since is reported by prepareSourceJob when Generate is clicked.
    bool sourceLoaded() const { return sourceLenS > 0.0 && sourceFile != juce::File(); }
    juce::String getSourceStateText() const;
    static bool modeNeedsSource (const juce::String& mode) { return mode == "repaint" || mode == "lego"; }
    // Pure: host loop (ppq) to seconds relative to the capture start.
    static bool loopRegionSeconds (double loopStartPpq, double loopEndPpq, double capStartPpq, double bpm,
                                   double& startS, double& endS);
    // Pure: 0 <= start < end <= length (1 ms tolerance on the end).
    static bool validateRegion (double startS, double endS, double lengthS, juce::String& err);
    // Fills sourceWav and (repaint) region. Plugin: region from the host loop;
    // standalone: manualStartS / manualEndS. False + err = blocked client-side.
    bool prepareSourceJob (DawJobRequest& r, double manualStartS, double manualEndS, juce::String& err) const;

    juce::AudioProcessorValueTreeState apvts;
    HostSyncState hostSync;
    CaptureEngine capture;
    DawWorker daw;
    AuditionEngine audition;
    EditorAudition editorAudition;

    // R6: starred takes, keyed by the take's file name (the same in the temp and the
    // lasting folder, so a star set on an unsaved take stays), saved in plugin state
    // (ValueTree child of apvts.state). Message thread only.
    bool isStarred (const juce::File& stableTake) const;
    void setStarred (const juce::File& stableTake, bool on);
    // R6: stable copy of a downloaded temp take (copies once, cached). Any thread
    // except audio. Returns {} if the copy failed.
    juce::File stableTakeFor (const DawJob& job, int takeIndex);
    // Tests. HSLINK_STABLE_ROOT (read once at start) does the same before the saver thread starts.
    void setStableRoot (const juce::File& root) { std::lock_guard<std::mutex> g (takesLock); stableRoot = root; }
    juce::File getStableRoot() const { std::lock_guard<std::mutex> g (takesLock); return stableRoot; }
    juce::File getStableSessionFolder() const;
    // Standalone audition transport (UI writes, audio reads).
    std::atomic<bool> previewPlay    { false };   // message thread writes, audio reads
    std::atomic<bool> previewRestart { false };   // set by Play: restart preview at 0
    std::atomic<double> captureStartPpq { 0.0 };
    std::atomic<bool> haveCaptureStart { false };
    bool isStandalone() const { return standalone; }

    // Seed and musical key: app settings saved in plugin state (apvts.state
    // properties, not automatable). Message thread only.
    static constexpr int kNumKeys = 24;
    static const char* keyName (int index); // 1..24 -> "C major" .. "B minor"; else nullptr
    juce::int64 getSeed() const;            // -1 = random
    void setSeed (juce::int64 seed);
    int getKeyIndex() const;                // 0 = Auto
    void setKeyIndex (int index);
    juce::String getCaption() const;        // stored in state, not automatable
    void setCaption (const juce::String& text);
    juce::String getLyrics() const;         // stored in state, not automatable
    void setLyrics (const juce::String& text);
    bool getInstrumental() const;           // true = lyrics are not sent
    void setInstrumental (bool on);
    // Bumped (message thread) on every setStateInformation so an open editor
    // reloads caption/lyrics/seed/key/instrumental without echoing writes back.
    juce::uint32 getStateGeneration() const { return stateGeneration.load(); }

    // Mode combo id (1 text2music, 2 repaint, 3 lego) and standalone BPM:
    // UI settings kept with the text state so A/B and presets carry them.
    int getModeId() const;
    void setModeId (int id);
    double getManualBpm() const;
    void setManualBpm (double bpm);

    // One snapshot of the generate settings (A/B slots and presets).
    struct GenSettings
    {
        juce::String caption, lyrics;
        bool instrumental = false;
        double bpm = 120.0;
        float duration = 30.0f;
        int takes = 4;
        juce::int64 seed = -1;
        int keyIndex = 0, modeId = 1, backend = 0, aceModel = 0;
        juce::ValueTree toTree (const juce::Identifier& type) const;
        static GenSettings fromTree (const juce::ValueTree& t);
        bool operator== (const GenSettings& o) const;
    };
    // Message thread only. capture reads the live values; apply writes them
    // (params via the host-notifying path) and bumps the state generation.
    GenSettings captureSettings() const;
    void applySettings (const GenSettings& s);
    // A/B: slot 0 = A, 1 = B. The active slot is the live settings.
    int getActiveSlot() const;
    void selectSlot (int slot);       // saves live into the old slot, loads the other
    void copyActiveToOther();         // A=B (or B=A)
    GenSettings getSlot (int slot) const; // active slot returns live values

    // Role cards (message thread only): applyRoleCard writes just the params
    // the card changes, through the host-notifying path, like a manual edit.
    void applyRoleCard (int card);
    int getActiveRoleCard() const;    // derived from the backend param, never stored

    // Presets: one XML file per preset in the preset folder.
    static juce::String sanitizePresetName (const juce::String& name); // empty = invalid
    juce::File getPresetFolder() const;
    void setPresetFolder (const juce::File& dir) { presetFolder = dir; } // tests
    juce::StringArray listPresets() const;
    bool savePreset (const juce::String& name);   // live settings
    bool loadPreset (const juce::String& name);   // into the active slot
    bool deletePreset (const juce::String& name);

    // Collection: the global library of saved takes (CollectionStore.h). Any thread
    // except audio; nothing here runs in prepareToPlay or processBlock. The cached
    // list is empty until the first refreshCollection(); every call below that
    // changes the folder rescans it and bumps the generation, which the editor polls.
    CollectionStore collection;
    void setCollectionRoot (const juce::File& dir) { collection.setRoot (dir); } // tests
    CollectionResult addTakeToCollection (const juce::File& stableTake); // a take from getTakes(), by its lasting file
    std::vector<CollectionItem> getCollection() const;                   // cached, newest added first
    juce::uint32 getCollectionGeneration() const { return collectionGeneration.load(); }
    void refreshCollection();                                            // rescan the folder (file IO, blocks)
    // Rescans on the processor's own background thread and returns at once. The
    // result shows up as a new getCollectionGeneration(); the job is joined in the
    // destructor, so it can never outlive the processor. Safe from the message thread.
    void requestCollectionRefresh();
    bool setCollectionStarred (const CollectionItem& item, bool starred);
    bool renameCollectionItem (const CollectionItem& item, const juce::String& newName);
    // To the Deleted folder beside it. note: the text of CollectionStore::remove; on
    // true it is set only when the info file had to stay in the Collection folder.
    bool removeFromCollection (const CollectionItem& item, juce::String* note = nullptr);

    static constexpr const char* kDefaultServerUrl = "http://127.0.0.1:3001";

private:
    std::atomic<bool> lastStateLoadOk { true };
    // User text/settings kept outside apvts.state so a host-thread
    // getStateInformation never reads a ValueTree the UI is writing. Never
    // touched by the audio thread.
    mutable juce::SpinLock textLock;
    juce::String textCaption, textLyrics;
    juce::int64 textSeed = -1;
    int textKeyIndex = 0;
    bool textInstrumental = false;
    int textModeId = 1;
    double textBpm = 120.0;
    int activeSlot = 0;               // under textLock
    GenSettings otherSlot;            // the inactive slot, under textLock
    bool otherSlotSet = false;        // under textLock
    juce::File presetFolder;          // empty = default app-data folder
    std::atomic<juce::uint32> stateGeneration { 0 };
    void writeTextState (juce::ValueTree& state) const;
    void readTextState (juce::ValueTree& state);
    void writeSlotState (juce::ValueTree& state) const;
    void readSlotState (juce::ValueTree& state);

    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    bool standalone = false;
    juce::String dawKey;              // message thread; never written to plugin state
    std::atomic<float>* levelMatchParam = nullptr; // cached in ctor
    std::atomic<float>* followHostParam = nullptr; // cached in prepareToPlay (L230)
    bool wasCapturing = false;        // audio thread
    std::atomic<int> capPhase { capIdle };
    std::atomic<double> capEndPpq { -1.0 }; // audio thread writes at start
    double capLastPpq = 0.0;          // audio thread
    juce::File sourceFile;            // message thread
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    double sourceLenS = 0.0, sourceStartPpq = 0.0, sourceBpm = 120.0; // teeth: before round 15
#else
    double sourceLenS = 0.0, sourceStartPpq = 0.0, sourceBpm = 0.0;   // sourceBpm 0 = unknown, never 120
#endif
    int captureSerial = 0;
    juce::int64 previewClock = 0;     // audio thread
    bool lastPreviewPlay = false;     // audio thread
    void trimTakesLocked();
    void syncStarMirror();             // message thread, after any change to the starred list
    // result: Aborted when the processor is closing (no attempt started, or the copy
    // stopped between two file calls).
    juce::File copyStable (const juce::File& temp, const juce::String& name,
                           ExclusiveFile::CopyResult* result = nullptr);
    int countUnsaved() const;
    struct TakeSaver;                  // background thread: cleanup and rescue at start, then every lasting copy
    void wakeSaverForNewTake();        // worker thread: a take was recorded
    std::atomic<int> recordSeq { 0 }, savedSeq { 0 }; // takes recorded / the count the saver's last pass covered
    juce::WaitableEvent saverIdle;     // signalled after every saver pass (waitSaverIdle)
    bool legacyFolderReferenced (const juce::File& dir) const; // TEST ONLY, see testDtorProbesLasting
    // Declared before saver: the saver thread attaches to it for its whole run.
    ExclusiveFile::IoCanceller saverIo;
    std::unique_ptr<TakeSaver> saver;
    std::atomic<bool> shuttingDown { false };
    std::atomic<bool> destructorBegan { false }; // only for testAttemptsAfterClose
    bool countCopyAttempt();           // before every lasting copy; false = closing, do not copy
    bool closingNow() const;           // the abort predicate of every lasting copy
    void noteSaved (const juce::File& temp, const juce::File& lasting); // one saved.log line; never under takesLock
    ExclusiveFile::SessionLock ownLock; // owner.lock of the temp session folder: ctor to the end of the dtor
    std::atomic<bool> ownLockHeld { false }; // ownLock.isHeld(), readable from any thread
    mutable std::mutex takesLock;      // guards everything from stableRoot to recoveredFolder
    juce::File stableRoot { StableTakes::defaultRoot() };
    juce::String sessionName;          // lasting folder name; set in ctor before the worker starts
    juce::String tempSessionName;      // the one temp folder of this instance (takes and captures); always has the token
    std::map<juce::String, juce::File> stableByTemp;
    std::vector<TakeEntry> takes;      // oldest first
    juce::StringArray starredNames;    // mirror of the starred list in apvts.state
    juce::String pinnedTake;           // full path
    int recoveredCount = 0;            // takes rescued from earlier sessions by this instance
    juce::String recoveredFolder;
    // Folders this instance adopted, with the owner.lock it holds on each (saver
    // thread, then the destructor after the saver stopped). adoptedNames mirrors the
    // paths under takesLock for rescueMarkedFolders.
    std::vector<std::pair<juce::File, std::unique_ptr<ExclusiveFile::SessionLock>>> adoptedLocks;
    juce::StringArray adoptedNames;
    // W1: adopted folders whose scan stopped early or did not sweep (adoptLock). They
    // are never emptied or removed (unlisted takes may remain); the lock is kept to close.
    juce::StringArray adoptedKeepWhole;
    void finishAdoptedFolders();       // saver thread: removes adopted folders whose takes are all saved
    void releaseLegacyClaims();        // destructor: legacy folders still held go back to the next start
    std::mutex adoptLock;              // adoptedLocks; adoption and finishing, one at a time; never inside takesLock
    // The one pending take delete (takesLock). entry is out of takes while active.
    struct PendingTakeDelete
    {
        bool active = false;
        TakeEntry entry;
        size_t index = 0;          // its place in takes when it was taken out
        juce::String afterTemp;    // full path of the temp file of the entry before it, empty = it was first
        juce::uint32 atMs = 0;     // juce::Time::getMillisecondCounter at the delete
    };
    PendingTakeDelete pendingDelete;
    std::vector<PendingTakeDelete> pendingRest; // takesLock; the rest of a clearUnstarredTakes batch (oldest first), only while pendingDelete.active
    juce::String takeDeleteMessage;    // takesLock
    void restoreTakeLocked (const PendingTakeDelete& p);
    bool takeStarredLocked (const TakeEntry& t) const;  // takesLock held
    void finishTakeDelete (const PendingTakeDelete& p); // file IO: delete pool thread, or the destructor
    bool takePendingDelete (std::vector<PendingTakeDelete>& out); // moves the pending delete (and its batch) out; false = none
    mutable std::mutex collectionCacheLock; // guards collectionCache only; never held across store IO
    std::vector<CollectionItem> collectionCache;
    std::atomic<juce::uint32> collectionGeneration { 0 };
    juce::ThreadPool collectionPool { 1 }; // background rescans only; declared last of the collection state
    juce::ThreadPool takeDeletePool { 1 }; // Deleted-folder moves of deleted takes only; drained in the destructor
    // Tempo measurement. A job holds the gate by shared pointer: it polls stop while it
    // works, and it stores its result under lock only while closed is false. The
    // destructor sets both before it waits for the pool, so a job that outlasts that
    // wait ends without touching the processor.
    struct MeasureGate
    {
        std::atomic<bool> stop { false };
        std::mutex lock;       // taken before takesLock, never inside it
        bool closed = false;   // lock
    };
    std::shared_ptr<MeasureGate> measureGate { std::make_shared<MeasureGate>() };
    void storeTakeTempo (const juce::File& tempFile, bool measured, const TempoMeasure::Result& r);
    juce::ThreadPool measurePool { 1 };    // take tempo measurements only; stopped before the pools above
    std::deque<TakeEditReport> takeEditReports; // takesLock; newest last, at most kTakeEditReportsKept
    int takeEditSerial = 0;                // takesLock
    juce::uint64 nextEditTicket = 1;       // takesLock
    std::map<juce::uint64, bool> takeEditJobs; // takesLock; ticket -> its job has started; gone once the job has ended
    void storeTakeEditReportLocked (TakeEditReport report);
    juce::ThreadPool stretchPool { 1 };    // take stretches and edits only (requestTakeStretch, requestTakeEdit); declared last, stopped first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotStepLinkProcessor)
};

// Role cards: workflow shortcuts on top of the existing backend / takes /
// ACE model settings. Pure, no state of their own.
namespace RoleCards
{
    enum Card { kNone = -1, kSa3 = 0, kYue2 = 1, kAce = 2, kCount = 3 };
    // Backend param indices ("Auto", "SA3", "YuE2", "ACE") and ACE model Base.
    constexpr int kBackendAuto = 0, kBackendSa3 = 1, kBackendYue2 = 2, kBackendAce = 3, kAceModelBase = 2;
    const char* title (int card);
    const char* subtitle (int card);
    // The settings after clicking a card. Unknown card = unchanged.
    HotStepLinkProcessor::GenSettings apply (int card, HotStepLinkProcessor::GenSettings s);
    // The card to light for these settings (by backend), kNone for Auto.
    int activeFor (const HotStepLinkProcessor::GenSettings& s);
    int activeForBackend (int backend);
}
