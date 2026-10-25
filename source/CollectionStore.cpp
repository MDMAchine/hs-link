// HOT-STEP LINK -- CollectionStore.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#include "CollectionStore.h"
#include "DeletedFolder.h"
#include "PluginProcessor.h" // sanitizePresetName, audio format reading
#include <algorithm>
#include <cmath>

std::atomic<bool> CollectionStore::testDisabled { false };
std::atomic<bool> CollectionStore::testNoRootGuard { false };
std::atomic<bool> CollectionStore::testFailSidecar { false };
std::atomic<bool> CollectionStore::testFailSidecarMove { false };
std::atomic<bool> CollectionStore::testFailRemoveRollback { false };

// ---- TempoResolve: pure ----------------------------------------------------------

double TempoResolve::parseTempoFromFileName (const juce::String& name)
{
    const std::string s = name.toLowerCase().toStdString();
    const int n = (int) s.size();
    auto at = [&] (int i) { return i >= 0 && i < n ? s[(std::size_t) i] : '\0'; };
    auto digit = [&] (int i) { return at (i) >= '0' && at (i) <= '9'; };
    auto alnum = [&] (int i) { return digit (i) || (at (i) >= 'a' && at (i) <= 'z'); };
    auto sep = [&] (int i) { return at (i) == '_' || at (i) == '-' || at (i) == ' '; };
    auto bpmAt = [&] (int i) { return at (i) == 'b' && at (i + 1) == 'p' && at (i + 2) == 'm'; };
    // The number that starts at i: its end (one past), -1 when there is none.
    auto number = [&] (int i, double& value)
    {
        int e = i;
        while (digit (e))
            ++e;
        if (e == i)
            return -1;
        if (at (e) == '.' && digit (e + 1))
        {
            ++e;
            while (digit (e))
                ++e;
        }
        if (e - i > 16) // no tempo is that long; keeps the value exact and finite
            return -1;
        value = juce::String (s.substr ((std::size_t) i, (std::size_t) (e - i))).getDoubleValue();
        return e;
    };
    auto inRange = [] (double v) { return std::isfinite (v) && v >= kMinBpm && v <= kMaxBpm; };

    // A number written with a comma ("120,50") is not read as two numbers: neither
    // side of the comma is a tempo.
    auto commaBefore = [&] (int i) { return at (i - 1) == ',' && digit (i - 2); };
    auto commaAfter = [&] (int e) { return at (e) == ',' && digit (e + 1); };

    // The plugin's own stretched take, "..._st93.7bpm.wav": the tempo after the stretch
    // wins over any other token. "_st", the number, "bpm" with nothing between, then
    // the end of the name or not a letter or digit. The last one in range wins (a
    // stretch of a stretched take); one out of range or malformed is ignored.
    double stretched = 0.0;
    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        if (at (i) == '_' && at (i + 1) == 's' && at (i + 2) == 't')
        {
            const int e = number (i + 3, v);
            if (e > 0 && bpmAt (e) && ! alnum (e + 3) && inRange (v))
                stretched = v;
        }
    }
    if (stretched > 0.0)
        return stretched;

    for (int i = 0; i < n; ++i)
    {
        double v = 0.0;
        // N bpm. The number starts here: no letter or digit before it, and it is not
        // the decimal part of a longer number.
        if (digit (i) && ! alnum (i - 1) && ! (at (i - 1) == '.' && digit (i - 2)) && ! commaBefore (i))
        {
            const int e = number (i, v);
            if (e > 0)
            {
                const int b = sep (e) ? e + 1 : e;
                if (bpmAt (b) && ! alnum (b + 3) && inRange (v))
                    return v;
            }
        }
        // bpm N.
        if (bpmAt (i) && ! alnum (i - 1))
        {
            const int d = sep (i + 3) ? i + 4 : i + 3;
            const int e = number (d, v);
            if (e > 0 && ! alnum (e) && ! commaAfter (e) && inRange (v))
                return v;
        }
    }
    return 0.0;
}

bool TempoResolve::hostBpmUsable (double hostBpm)
{
    return std::isfinite (hostBpm) && hostBpm >= kMinBpm && hostBpm <= kMaxBpm;
}

bool TempoResolve::measuredCertain (const TempoMeasure::Result& m)
{
    return m.valid && m.level >= TempoMeasure::Level::medium && TakeEdit::gridUsable (m.bpm, m.downbeatSec);
}

TempoResolve::Resolved TempoResolve::resolve (const TempoMeasure::Result& measured, const juce::String& fileName, double hostBpm)
{
    Resolved r;
    if (measuredCertain (measured))
    {
        r.bpm = measured.bpm;
        r.downbeatSec = measured.downbeatSec;
        r.source = Source::measured;
    }
    else if (const double named = parseTempoFromFileName (fileName); named > 0.0)
    {
        r.bpm = named;
        r.source = Source::fileName;
    }
    else if (hostBpmUsable (hostBpm))
    {
        r.bpm = hostBpm;
        r.source = Source::host;
    }
    r.snapEnabled = r.source != Source::none;
    return r;
}

const char* TempoResolve::sourceName (Source source)
{
    switch (source)
    {
        case Source::measured: return "measured";
        case Source::fileName: return "file_name";
        case Source::host:     return "host";
        case Source::none:     break;
    }
    return "none";
}

TempoResolve::Source TempoResolve::sourceFromName (const juce::String& name)
{
    return name == "measured" ? Source::measured : name == "file_name" ? Source::fileName : name == "host" ? Source::host : Source::none;
}

juce::String TempoResolve::fallbackText (const Resolved& r)
{
    if (! r.isFallback() || ! (r.bpm > 0.0))
        return {};
    const double cents = std::round (r.bpm * 100.0) / 100.0;
    const auto number = cents == std::floor (cents) ? juce::String ((juce::int64) cents) : juce::String (cents, 2);
    return number + " bpm (" + (r.source == Source::fileName ? "file name" : "host") + ")";
}

namespace
{
juce::String isoOf (juce::int64 ms)
{
    return ms > 0 ? juce::Time (ms).toISO8601 (true) : juce::String();
}

juce::int64 msOf (const juce::var& v)
{
    const auto s = v.toString();
    return s.isEmpty() ? 0 : juce::Time::fromISO8601 (s).toMilliseconds();
}

// Duration from the audio file header, 0 when it is not readable audio.
double durationOfFile (const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr || rd->sampleRate <= 0.0)
        return 0.0;
    return (double) rd->lengthInSamples / rd->sampleRate;
}

void setError (juce::String* error, const juce::String& text)
{
    if (error != nullptr)
        *error = text;
}
}

juce::File CollectionStore::defaultRoot()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("MDMAchine").getChildFile ("HOT-STEP LINK").getChildFile ("collection");
}

void CollectionStore::setRoot (const juce::File& dir)
{
    std::lock_guard<std::mutex> g (ioLock);
    root = dir;
}

juce::File CollectionStore::getRoot() const
{
    std::lock_guard<std::mutex> g (ioLock);
    return root;
}

juce::String CollectionStore::toJson (const CollectionItem& item)
{
    auto* o = new juce::DynamicObject();
    juce::var v (o);
    o->setProperty ("schema", kSchemaVersion);
    o->setProperty ("name", item.name);
    o->setProperty ("caption", item.caption);
    o->setProperty ("backend", item.backend);
    o->setProperty ("model", item.model);
    o->setProperty ("bpm", item.bpm);
    o->setProperty ("key", item.key);
    o->setProperty ("seed", item.seed); // -1 = random
    o->setProperty ("duration_s", item.durationS);
    o->setProperty ("mode", item.mode);
    o->setProperty ("created", isoOf (item.createdMs));
    o->setProperty ("added", isoOf (item.addedMs));
    o->setProperty ("starred", item.starred);
    juce::Array<juce::var> tags;
    for (const auto& t : item.tags)
        tags.add (t);
    o->setProperty ("tags", tags);
    o->setProperty ("source", item.sourceName);
    // Written only for a measured take, so a sidecar without them reads as not measured.
    if (item.tempoMeasured)
    {
        o->setProperty ("tempo_measured", true);
        o->setProperty ("measured_bpm", item.tempo.bpm);
        o->setProperty ("tempo_confidence", item.tempo.confidence);
        o->setProperty ("tempo_level", juce::String (TempoMeasure::levelName (item.tempo.level)));
        o->setProperty ("downbeat_s", item.tempo.downbeatSec);
        o->setProperty ("first_beat_s", item.tempo.firstBeatSec);
        o->setProperty ("drift_ms", item.tempo.driftMs);
        o->setProperty ("drift_end_ms", item.tempo.driftEndMs);
        o->setProperty ("beats", item.tempo.beats);
        o->setProperty ("hint_octave", item.tempo.hintOctave);
        // Where the tempo of the grid comes from. A file name or host tempo is kept
        // as its own number; measured_bpm stays the measurement.
        o->setProperty ("tempo_source", juce::String (TempoResolve::sourceName (item.resolvedTempo.source)));
        if (item.resolvedTempo.isFallback())
            o->setProperty ("tempo_fallback_bpm", item.resolvedTempo.bpm);
    }
    // Edit points: sample frames of the WAV. Only the set ones are written, so a
    // sidecar without them reads as not edited.
    const auto edit = item.edit.sanitized();
    if (edit.cropStart >= 0)
        o->setProperty ("crop_start", edit.cropStart);
    if (edit.cropEnd >= 0)
        o->setProperty ("crop_end", edit.cropEnd);
    if (edit.loopStart >= 0)
        o->setProperty ("loop_start", edit.loopStart);
    if (edit.loopEnd >= 0)
        o->setProperty ("loop_end", edit.loopEnd);
    return juce::JSON::toString (v);
}

// Unknown fields are ignored and missing ones keep their defaults, so a sidecar
// written by a newer schema is read best-effort.
bool CollectionStore::fromJson (const juce::String& json, CollectionItem& item)
{
    juce::var v;
    if (juce::JSON::parse (json, v).failed() || v.getDynamicObject() == nullptr)
        return false;
    item.schema = (int) v.getProperty ("schema", 1);
    item.name = v.getProperty ("name", item.name).toString();
    item.caption = v.getProperty ("caption", {}).toString();
    item.backend = v.getProperty ("backend", {}).toString();
    item.model = v.getProperty ("model", {}).toString();
    item.bpm = (double) v.getProperty ("bpm", 0.0);
    item.key = v.getProperty ("key", {}).toString();
    item.seed = (juce::int64) v.getProperty ("seed", (juce::int64) -1);
    item.durationS = (double) v.getProperty ("duration_s", item.durationS);
    item.mode = v.getProperty ("mode", {}).toString();
    item.createdMs = msOf (v.getProperty ("created", {}));
    const auto added = msOf (v.getProperty ("added", {}));
    if (added > 0)
        item.addedMs = added;
    item.starred = (bool) v.getProperty ("starred", false);
    item.tags.clear();
    if (const auto* arr = v.getProperty ("tags", {}).getArray())
        for (const auto& t : *arr)
            item.tags.add (t.toString());
    item.sourceName = v.getProperty ("source", {}).toString();
    // Missing in every sidecar written before the measurement existed: not measured.
    item.tempoMeasured = (bool) v.getProperty ("tempo_measured", false);
    item.tempo = {};
    if (item.tempoMeasured)
    {
        const auto level = v.getProperty ("tempo_level", {}).toString();
        item.tempo.level = level == "high" ? TempoMeasure::Level::high
                           : level == "medium" ? TempoMeasure::Level::medium
                           : level == "low" ? TempoMeasure::Level::low : TempoMeasure::Level::none;
        // The number is shown only for medium and high, whatever the file says.
        item.tempo.valid = item.tempo.level >= TempoMeasure::Level::medium;
        item.tempo.bpm = (double) v.getProperty ("measured_bpm", 0.0);
        item.tempo.confidence = (double) v.getProperty ("tempo_confidence", 0.0);
        item.tempo.downbeatSec = (double) v.getProperty ("downbeat_s", 0.0);
        item.tempo.firstBeatSec = (double) v.getProperty ("first_beat_s", 0.0);
        item.tempo.driftMs = (double) v.getProperty ("drift_ms", 0.0);
        item.tempo.driftEndMs = (double) v.getProperty ("drift_end_ms", 0.0);
        item.tempo.beats = (int) v.getProperty ("beats", 0);
        item.tempo.hintOctave = (int) v.getProperty ("hint_octave", 0); // no key in older sidecars: 0
        // A tempo or a downbeat that no measurement writes (not finite, or a downbeat
        // further out than any take is long) is not a grid: no number, downbeat 0.
        if (! std::isfinite (item.tempo.bpm) || ! std::isfinite (item.tempo.downbeatSec)
            || std::abs (item.tempo.downbeatSec) > TakeEdit::kMaxGridDownbeatSec)
        {
            item.tempo.valid = false;
            item.tempo.bpm = 0.0;
            item.tempo.downbeatSec = 0.0;
        }
    }
    // The source of the grid tempo. No key (every sidecar before the cascade): measured
    // when the measurement is certain, else none, which is what such a take showed.
    // A file name or host source needs its own number in range, else it is none.
    item.resolvedTempo = {};
    if (item.tempoMeasured)
    {
        const bool certain = TempoResolve::measuredCertain (item.tempo);
        const auto stated = v.getProperty ("tempo_source", {});
        auto source = stated.isVoid() ? (certain ? TempoResolve::Source::measured : TempoResolve::Source::none)
                                      : TempoResolve::sourceFromName (stated.toString());
        const double fallback = (double) v.getProperty ("tempo_fallback_bpm", 0.0);
        if (source == TempoResolve::Source::measured && certain)
        {
            item.resolvedTempo.bpm = item.tempo.bpm;
            item.resolvedTempo.downbeatSec = item.tempo.downbeatSec;
        }
        else if ((source == TempoResolve::Source::fileName || source == TempoResolve::Source::host) && TempoResolve::hostBpmUsable (fallback))
            item.resolvedTempo.bpm = fallback;
        else
            source = TempoResolve::Source::none;
        item.resolvedTempo.source = source;
        item.resolvedTempo.snapEnabled = source != TempoResolve::Source::none;
    }
    // Missing, not a number, negative or absurdly large: not set. The pairs are then
    // checked as a whole (EditData::sanitized); the file length is checked at render.
    auto point = [&v] (const char* key)
    {
        const auto p = v.getProperty (key, -1);
        if (! (p.isInt() || p.isInt64() || p.isDouble()))
            return (juce::int64) -1;
        const double x = (double) p;
        return std::isfinite (x) && x >= 0.0 && x <= 1.0e12 ? (juce::int64) std::llround (x) : (juce::int64) -1;
    };
    TakeEdit::EditData edit;
    edit.cropStart = point ("crop_start");
    edit.cropEnd = point ("crop_end");
    edit.loopStart = point ("loop_start");
    edit.loopEnd = point ("loop_end");
    item.edit = edit.sanitized();
    return true;
}

// Atomic: the JSON goes to a temp file in the same folder, which then replaces
// the sidecar in one step, so a crash cannot leave a half-written sidecar.
bool CollectionStore::writeSidecar (const CollectionItem& item)
{
    if (testFailSidecar.load())
        return false;
    const auto dest = item.sidecar();
    const auto tmp = dest.getSiblingFile (dest.getFileName() + ".tmp");
    if (! tmp.replaceWithText (toJson (item), false, false, nullptr))
    {
        tmp.deleteFile();
        return false;
    }
    const bool ok = dest.existsAsFile() ? tmp.replaceFileIn (dest) : tmp.moveFileTo (dest);
    if (! ok)
        tmp.deleteFile(); // our own temp file, never an item
    return ok;
}

bool CollectionStore::isInsideRootLocked (const juce::File& f) const
{
    // Parent compare on normalised paths: "root/../x.wav" resolves to a file whose
    // parent is not the root, and a file in a sub-folder is not ours either.
    return root != juce::File() && f != juce::File() && f.getParentDirectory() == root;
}

// Caller holds ioLock. False = no such WAV. corrupt = a sidecar exists but cannot be parsed.
bool CollectionStore::readItemLocked (const juce::File& wav, CollectionItem& out, bool& corrupt) const
{
    corrupt = false;
    if (! wav.existsAsFile())
        return false;
    out = {};
    out.wav = wav;
    out.name = wav.getFileNameWithoutExtension();
    out.sourceName = wav.getFileName();
    out.addedMs = wav.getLastModificationTime().toMilliseconds();
    const auto side = out.sidecar();
    if (side.existsAsFile() && fromJson (side.loadFileAsString(), out))
        return true;
    corrupt = side.existsAsFile();
    out.sidecarMissing = true;
    out.createdMs = out.addedMs;
    out.durationS = durationOfFile (wav);
    return true;
}

CollectionResult CollectionStore::add (const HslinkTakeEntry& take)
{
    CollectionResult r;
    if (testDisabled.load())
    {
        r.error = "collection disabled";
        return r;
    }
    if (take.stable == juce::File() || ! take.stable.existsAsFile())
    {
        r.error = "take has no lasting file";
        return r;
    }
    std::lock_guard<std::mutex> g (ioLock);
    if (! root.createDirectory().wasOk())
    {
        r.error = "cannot create the collection folder";
        return r;
    }
    auto stem = HotStepLinkProcessor::sanitizePresetName (take.stable.getFileNameWithoutExtension());
    if (stem.isEmpty())
        stem = "take";
    // First free base name: neither the WAV nor a sidecar (even an orphan one) may
    // exist, so nothing is ever overwritten. Adding the same take twice therefore
    // makes a second item, <name>_2.
    juce::File dest;
    for (int i = 1; i < 10000; ++i)
    {
        const auto f = root.getChildFile (stem + (i == 1 ? juce::String() : "_" + juce::String (i)) + ".wav");
        if (! f.exists() && ! f.withFileExtension ("json").exists())
        {
            dest = f;
            break;
        }
    }
    if (dest == juce::File())
    {
        r.error = "no free name in the collection folder";
        return r;
    }
    if (! take.stable.copyFileTo (dest))
    {
        r.error = "could not copy the take into the collection";
        return r;
    }
    CollectionItem it;
    it.wav = dest;
    it.name = take.stable.getFileNameWithoutExtension();
    it.caption = take.caption;
    it.tempoMeasured = take.tempoState == HslinkTakeEntry::tempoMeasured;
    if (it.tempoMeasured)
    {
        it.tempo = take.tempo;
        it.resolvedTempo = take.resolvedTempo;
    }
    it.edit = take.edit.sanitized();
    it.backend = take.backend;
    it.model = take.model;
    it.bpm = take.bpm;
    it.key = take.key;
    it.seed = take.seed;
    it.durationS = take.durationS > 0.0 ? take.durationS : durationOfFile (dest);
    it.mode = take.mode;
    it.createdMs = take.createdMs;
    it.addedMs = juce::jmax (juce::Time::currentTimeMillis(), lastAddedMs + 1);
    it.sourceName = take.stable.getFileName();
    if (! writeSidecar (it))
    {
        dest.deleteFile(); // the copy made a few lines up, in this call: no orphan WAV
        r.error = "could not write the collection sidecar";
        return r;
    }
    lastAddedMs = it.addedMs;
    r.ok = true;
    r.item = it;
    return r;
}

std::vector<CollectionItem> CollectionStore::list() const
{
    std::vector<CollectionItem> out;
    if (testDisabled.load())
        return out;
    std::lock_guard<std::mutex> g (ioLock);
    if (! root.isDirectory())
        return out;
    for (const auto& f : root.findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        CollectionItem it;
        bool corrupt = false;
        if (readItemLocked (f, it, corrupt))
            out.push_back (it);
    }
    std::sort (out.begin(), out.end(), [] (const CollectionItem& a, const CollectionItem& b)
    {
        if (a.addedMs != b.addedMs)
            return a.addedMs > b.addedMs;
        return a.wav.getFileName().compareNatural (b.wav.getFileName()) > 0;
    });
    return out;
}

bool CollectionStore::setStarred (const CollectionItem& item, bool starred, juce::String* error)
{
    if (testDisabled.load())
        return false;
    std::lock_guard<std::mutex> g (ioLock);
    CollectionItem cur;
    bool corrupt = false;
    if (! isInsideRootLocked (item.wav) || ! readItemLocked (item.wav, cur, corrupt))
    {
        setError (error, "not a collection item");
        return false;
    }
    if (corrupt)
    {
        setError (error, "the sidecar is unreadable and was left alone");
        return false;
    }
    cur.sidecarMissing = false;
    cur.starred = starred;
    if (! writeSidecar (cur))
    {
        setError (error, "could not write the collection sidecar");
        return false;
    }
    return true;
}

bool CollectionStore::rename (const CollectionItem& item, const juce::String& newDisplayName, juce::String* error)
{
    if (testDisabled.load())
        return false;
    const auto name = newDisplayName.trim().substring (0, 120);
    if (name.isEmpty())
    {
        setError (error, "empty name");
        return false;
    }
    std::lock_guard<std::mutex> g (ioLock);
    CollectionItem cur;
    bool corrupt = false;
    if (! isInsideRootLocked (item.wav) || ! readItemLocked (item.wav, cur, corrupt))
    {
        setError (error, "not a collection item");
        return false;
    }
    if (corrupt)
    {
        setError (error, "the sidecar is unreadable and was left alone");
        return false;
    }
    cur.sidecarMissing = false;
    cur.name = name; // display name only: the file on disk keeps its name
    if (! writeSidecar (cur))
    {
        setError (error, "could not write the collection sidecar");
        return false;
    }
    return true;
}

bool CollectionStore::remove (const CollectionItem& item, juce::String* error)
{
    if (testDisabled.load())
        return false;
    std::lock_guard<std::mutex> g (ioLock);
    if (! testNoRootGuard.load()
        && (! isInsideRootLocked (item.wav) || ! item.wav.hasFileExtension ("wav")))
    {
        setError (error, "refused: not a file directly inside the collection folder");
        return false;
    }
    if (! item.wav.existsAsFile())
    {
        setError (error, "no such item");
        return false;
    }
    // Plain renames into <collection folder>/Deleted, never a delete (DeletedFolder.h).
    // The WAV goes first: it is the file a host or the audition can hold open, so the
    // likely failure changes nothing. End states:
    //   1. WAV and sidecar in Deleted (or no sidecar to move)   -> true.
    //   2. WAV kept                                             -> false, nothing moved.
    //   3. WAV moved, sidecar kept: the WAV is renamed back to its own path (same
    //      no-replace rename). Both in place again              -> false.
    //   4. as 3 but the rename back failed too: the WAV stays in Deleted, the sidecar
    //      stays in the root. list() goes by WAV, so no item is shown for it, and
    //      nothing is deleted to tidy up                        -> true, with a note.
    // alreadyGone for the WAV: the item is off the list either way, so the sidecar is
    // still moved and that counts as removed.
    juce::File wavInDeleted;
    const auto wavResult = moveToDeletedFolder (item.wav, &wavInDeleted);
    if (wavResult == MoveToDeleted::kept)
    {
        setError (error, removeNoteText (RemoveNote::audioKept));
        return false;
    }
    const auto side = item.wav.withFileExtension ("json");
    const auto sideResult = testFailSidecarMove.load() ? MoveToDeleted::kept : moveToDeletedFolder (side);
    if (sideResult != MoveToDeleted::kept)
        return true;
    if (wavResult == MoveToDeleted::moved && ! testFailRemoveRollback.load()
        && DeletedFolder::renameBack (wavInDeleted, item.wav))
    {
        setError (error, removeNoteText (RemoveNote::infoKept));
        return false;
    }
    // The WAV is in Deleted only when it was moved; alreadyGone means it was not at
    // its path any more and nothing of it went to Deleted.
    setError (error, removeNoteText (wavResult == MoveToDeleted::moved ? RemoveNote::infoLeftAudioInDeleted
                                                                       : RemoveNote::infoLeftAudioGone));
    return true;
}

juce::String CollectionStore::removeNoteText (RemoveNote note)
{
    switch (note)
    {
        case RemoveNote::audioKept:              return "audio file in use or could not be moved, both files kept";
        case RemoveNote::infoKept:               return "info file could not be moved, both files kept";
        case RemoveNote::infoLeftAudioInDeleted: return "audio is in the Deleted folder, info file still in the Collection folder";
        case RemoveNote::infoLeftAudioGone:      return "audio file was already gone, info file still in the Collection folder";
    }
    return {};
}

juce::String CollectionStore::removeResultText (const juce::String& name, bool removed, const juce::String& note)
{
    if (removed)
        return note.isEmpty() ? "Moved to the Deleted folder: " + name : "Removed " + name + ": " + note;
    return note.isEmpty() ? "Could not remove " + name : "Could not remove " + name + ": " + note;
}

std::vector<CollectionItem> CollectionStore::filter (const std::vector<CollectionItem>& items, const CollectionQuery& query)
{
    if (testDisabled.load())
        return items;
    const auto text = query.text.trim();
    std::vector<CollectionItem> out;
    for (const auto& it : items)
    {
        if (query.starredOnly && ! it.starred)
            continue;
        if (query.backend.isNotEmpty() && ! it.backend.equalsIgnoreCase (query.backend))
            continue;
        if (text.isNotEmpty() && ! it.name.containsIgnoreCase (text) && ! it.caption.containsIgnoreCase (text)
            && ! it.tags.joinIntoString (" ").containsIgnoreCase (text))
            continue;
        out.push_back (it);
    }
    return out;
}

//==============================================================================
std::atomic<bool> CollectionView::testDisabled { false };

CollectionQuery CollectionView::queryFor (const CollectionChips& chips)
{
    CollectionQuery q;
    if (testDisabled.load())
        return q;
    q.text = chips.search.trim();
    q.backend = chips.backend == CollectionChips::sa3 ? "sa3"
              : chips.backend == CollectionChips::yue2 ? "yue2"
              : chips.backend == CollectionChips::ace ? "ace" : "";
    q.starredOnly = chips.starredOnly;
    return q;
}

bool CollectionView::isFiltering (const CollectionChips& chips)
{
    return chips.backend != CollectionChips::all || chips.starredOnly || chips.search.trim().isNotEmpty();
}

juce::String CollectionView::subtitle (double bpm, const juce::String& key, double durationS)
{
    if (testDisabled.load())
        return {};
    juce::StringArray parts;
    if (bpm > 0.0)
    {
        const double r = std::round (bpm * 10.0) / 10.0;
        const bool whole = std::abs (r - std::round (r)) < 1.0e-6;
        parts.add ((whole ? juce::String ((int) std::round (r)) : juce::String (r, 1)) + " BPM");
    }
    if (key.trim().isNotEmpty())
        parts.add (key.trim());
    if (durationS > 0.0)
    {
        const int total = juce::jmax (1, (int) std::round (durationS));
        parts.add (juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2));
    }
    return parts.joinIntoString (" - ");
}

juce::String CollectionView::badge (const juce::String& backend)
{
    const auto b = backend.trim().toLowerCase();
    if (b == "sa3")  return "SA3";
    if (b == "yue2") return "YuE2";
    if (b == "ace")  return "ACE";
    return b.toUpperCase();
}

juce::String CollectionView::countText (int shown, int total, bool filtering)
{
    return filtering ? juce::String (shown) + " / " + juce::String (total) : juce::String (total);
}
