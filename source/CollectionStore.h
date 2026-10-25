// HOT-STEP LINK -- CollectionStore.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_core/juce_core.h>
#include "TempoMeasure.h"
#include "TakeEdit.h"
#include <atomic>
#include <mutex>
#include <vector>

// Which tempo a take's grid uses, and where it came from. Pure, no IO, no statics.
// The cascade: a measured tempo the detector stands behind always wins; else a tempo
// token in the take's file name; else the host tempo; else none. The measurement and
// its certainty (TempoMeasure) are never changed here.
namespace TempoResolve
{
    enum class Source { none = 0, measured = 1, fileName = 2, host = 3 };
    // A file name or host tempo outside this range (both ends included) is not a tempo.
    constexpr double kMinBpm = 40.0, kMaxBpm = 300.0;

    struct Resolved
    {
        double bpm = 0.0;          // 0 for source none
        double downbeatSec = 0.0;  // the measured downbeat for source measured, else 0 (the file start)
        Source source = Source::none;
        bool snapEnabled = false;  // a grid exists: true for every source but none
        bool isFallback() const { return source == Source::fileName || source == Source::host; }
    };

    // The tempo named in a file name, 0 = none. Not case sensitive. Forms, N a number of
    // digits with an optional decimal point part ("120", "120.5"):
    //   N bpm:  "120bpm", "120_bpm", "120-bpm", "120 bpm"   (at most one of '_', '-', ' ' between)
    //   bpm N:  "bpm120", "bpm_120", "bpm-120", "bpm 120"
    // The whole match is one token: the character before it and the character after it
    // is the start or end of the name or not a letter or digit, and the number is not the
    // decimal part of a longer number ("1.120bpm" is 1.12, not 120). So "x2120bpm",
    // "t120bpm", "120bpms" and "st93.7bpm" do not match. A number without "bpm" is not a
    // tempo. N must lie in kMinBpm to kMaxBpm; a match outside it is skipped and the
    // scan goes on. The first match in range wins.
    // A number written with a comma is no tempo on either side: "120,50bpm" and
    // "bpm120,50" give 0 from that token.
    // Before all of that, the plugin's own stretched take: "_st" N "bpm" with nothing
    // between ("x_120bpm_ab12cd_st93.7bpm.wav" is 93.7), followed by the end of the name
    // or not a letter or digit. It wins over every other token; the last one in range
    // wins. Out of range or malformed, it is ignored and the rule above applies.
    double parseTempoFromFileName (const juce::String& name);
    // Finite and inside kMinBpm to kMaxBpm. 0, NaN, infinite or outside: absent.
    bool hostBpmUsable (double hostBpm);
    // True when the measurement may be used as a number and as a grid: valid, level
    // medium or high, and a grid TakeEdit accepts. The test the take cell and
    // requestTakeEdit already used.
    bool measuredCertain (const TempoMeasure::Result& measured);
    // The cascade. hostBpm: 0 = no host tempo.
    Resolved resolve (const TempoMeasure::Result& measured, const juce::String& fileName, double hostBpm);
    // "none" / "measured" / "file_name" / "host" (the sidecar key tempo_source), and back
    // (anything else: none).
    const char* sourceName (Source source);
    Source sourceFromName (const juce::String& name);
    // The text of a fallback tempo: "120 bpm (file name)", "98.5 bpm (host)". A whole
    // number has no decimals, else two. Empty for source none or measured.
    juce::String fallbackText (const Resolved& resolved);
}

// One downloaded take (see HotStepLinkProcessor::TakeEntry, which is this type).
// The settings fields are filled once, in registerTake, from the job that made it.
struct HslinkTakeEntry
{
    int jobId = 0;       // DawJob::localId, for the queue-card filter
    int takeIndex = 0;
    juce::File temp;     // download in the temp session folder
    juce::File stable;   // lasting copy, {} while the copy has not succeeded
    juce::String name;   // the take's file name, the same in the temp and the lasting folder
    // True only after a lasting-copy attempt for this take has failed. An entry with an
    // empty stable and copyFailed false is not copied yet: the saver thread has not
    // made its first attempt (or the plugin closed before it).
    bool copyFailed = false;
    // True from the moment registerTake adds the entry until it hands the take to
    // the saver thread (the file may still be <name>.part or in a rename try). The
    // saver makes no copy attempt for such an entry.
    bool handingOver = false;
    // True for a take adopted at start from the temp session folder of a dead owner
    // (adoptDeadFolders). Its lasting copy goes to destFolder (recovered_<session>),
    // not to this instance's session folder. Not shown by getTakes.
    bool adopted = false;
    juce::File destFolder;
    juce::String logKey;         // adopted: the saved.log key (a .wav.part is logged under its .wav name)
    // Generate settings known when the download completed.
    juce::String caption;
    juce::String backend;        // requested backend id ("sa3", "yue2", "ace"), "auto" when none was asked for
    juce::String model;          // server dit_model, empty = not reported
    double bpm = 0.0;            // 0 = not set
    juce::String key;            // empty = not set
    juce::int64 seed = -1;       // per-take seed from the server, else the requested seed, else -1 = random
    double durationS = 0.0;      // from the downloaded file, else the requested duration
    juce::String mode;           // text2music / repaint / lego
    juce::int64 createdMs = 0;   // wall clock (ms since epoch) when the take was registered
    // Measured tempo (TempoMeasure). bpm above is the tempo that was asked for and is
    // never changed by the measurement. tempo is meaningful only in state measured.
    enum TempoState { tempoNotMeasured = 0, tempoMeasuring = 1, tempoMeasured = 2 };
    int tempoState = tempoNotMeasured;
    TempoMeasure::Result tempo;
    // The tempo the grid uses and its source (TempoResolve::resolve), set once when the
    // measurement is stored, with the host tempo of that moment. It does not follow the
    // host afterwards. Source none while the take is not measured.
    TempoResolve::Resolved resolvedTempo;
    // Crop and loop points of this take (TakeEdit), in sample frames of its own file,
    // -1 = not set. Kept beside the take; the audio file is never changed by them.
    TakeEdit::EditData edit;
};

// One saved take in the Collection: <base>.wav plus the sidecar <base>.json.
struct CollectionItem
{
    juce::File wav;              // inside the collection root
    int schema = 1;
    juce::String name;           // display name; rename changes only this, never the file
    juce::String caption, backend, model, key, mode, sourceName;
    double bpm = 0.0, durationS = 0.0;
    juce::int64 seed = -1;
    juce::int64 createdMs = 0, addedMs = 0;
    bool starred = false;
    juce::StringArray tags;      // always written, empty for now
    // Measured tempo of the take, copied from it at add(). tempoMeasured false = not
    // measured (every sidecar written before the measurement existed reads like that).
    bool tempoMeasured = false;
    TempoMeasure::Result tempo;
    // The tempo the grid uses and its source, copied from the take at add(). Sidecar keys
    // tempo_source and, for a file name or host tempo, tempo_fallback_bpm. A sidecar
    // without tempo_source reads as measured when its measurement is certain, else none.
    TempoResolve::Resolved resolvedTempo;
    // Crop and loop points of the take (TakeEdit), copied from it at add(): sample
    // frames of the WAV, -1 = not set. Sidecar keys crop_start, crop_end, loop_start,
    // loop_end, each written only when set.
    TakeEdit::EditData edit;
    bool sidecarMissing = false; // no readable sidecar: the fields above are derived from the file
    juce::File sidecar() const { return wav.withFileExtension ("json"); }
};

struct CollectionQuery
{
    juce::String text;           // case-insensitive substring over name, caption and tags; empty = all
    juce::String backend;        // "sa3" / "yue2" / "ace"; empty = All
    bool starredOnly = false;
};

struct CollectionResult
{
    bool ok = false;
    juce::String error;          // set when ok is false
    CollectionItem item;         // set when ok is true
};

// The Collection: a persistent, user-curated library of saved takes, global across
// projects and plugin instances. A take enters it only through add(). It is separate
// from the session take list and from the starred list in the plugin state.
//
// Threading: message thread or worker thread, NEVER the audio thread. All IO is
// serialised by the store's own mutex. Callers must not hold takesLock,
// DawWorker::lock or the audition lock while calling in: copy what you need from
// the take list first, release, then call the store.
class CollectionStore
{
public:
    static constexpr int kSchemaVersion = 1;

    // userDocuments/MDMAchine/HOT-STEP LINK/collection
    static juce::File defaultRoot();
    void setRoot (const juce::File& dir);  // tests: never the real Documents folder
    juce::File getRoot() const;

    // Copies the take's lasting WAV into the root under a sanitized unique name and
    // writes the sidecar. Never overwrites: adding the same take twice makes a second
    // item (name_2). A take with no lasting file is refused. If the sidecar cannot be
    // written the copied WAV is removed again, so no orphan is left.
    CollectionResult add (const HslinkTakeEntry& take);

    // Scans the root: every *.wav, newest added first. A WAV whose sidecar is missing
    // or unreadable is listed with sidecarMissing set and metadata derived from the
    // file; list() never deletes or rewrites anything. A sidecar with no WAV is ignored.
    std::vector<CollectionItem> list() const;

    // Sidecar only. A missing sidecar is created from the derived metadata; a sidecar
    // that exists but cannot be parsed is left alone and the call fails.
    bool setStarred (const CollectionItem& item, bool starred, juce::String* error = nullptr);
    bool rename (const CollectionItem& item, const juce::String& newDisplayName, juce::String* error = nullptr);

    // Moves the WAV and its sidecar into <root>/Deleted by plain rename (DeletedFolder.h).
    // Never deletes. false: the WAV and the sidecar are both where they were and the
    // item stays listed. true: the item is off the list; *error is set on true only in
    // the one case that the sidecar had to stay behind (see the .cpp).
    // Refuses anything that is not a .wav directly inside the root.
    bool remove (const CollectionItem& item, juce::String* error = nullptr);
    // The notes remove() sets for its end states (the .cpp lists the states).
    //   audioKept:              false. The WAV could not be moved; nothing was moved.
    //   infoKept:               false. The sidecar could not be moved; the WAV was put back.
    //   infoLeftAudioInDeleted: true. The WAV is in Deleted, the sidecar is in the root.
    //   infoLeftAudioGone:      true. The WAV was no longer at its path (not moved by
    //                           this call), the sidecar is in the root.
    enum class RemoveNote { audioKept, infoKept, infoLeftAudioInDeleted, infoLeftAudioGone };
    static juce::String removeNoteText (RemoveNote note);
    // Pure. The footer line for a remove: removed is the result of remove(), note its text.
    //   true,  no note: "Moved to the Deleted folder: <name>"
    //   true,  note:    "Removed <name>: <note>"
    //   false, note:    "Could not remove <name>: <note>"   (no note: without the colon)
    static juce::String removeResultText (const juce::String& name, bool removed, const juce::String& note);

    // Pure, no IO.
    static std::vector<CollectionItem> filter (const std::vector<CollectionItem>& items, const CollectionQuery& query);
    static juce::String toJson (const CollectionItem& item);
    static bool fromJson (const juce::String& json, CollectionItem& item); // false = not a sidecar object

    // Test hooks (teeth), set from the HSLINK_TEETH env var by the tests only.
    // testDisabled: add / list / remove / star / rename do nothing, filter returns its input.
    // testNoRootGuard: remove skips the inside-the-root check.
    // testFailSidecar: the next sidecar writes fail (exercises the no-orphan path).
    static std::atomic<bool> testDisabled, testNoRootGuard, testFailSidecar;
    // remove() only, false in the plugin. testFailSidecarMove: the sidecar is not moved
    // and counts as kept. testFailRemoveRollback: the WAV is not put back after that.
    static std::atomic<bool> testFailSidecarMove, testFailRemoveRollback;

private:
    bool isInsideRootLocked (const juce::File& f) const;
    bool readItemLocked (const juce::File& wav, CollectionItem& out, bool& corrupt) const;
    static bool writeSidecar (const CollectionItem& item);

    mutable std::mutex ioLock;
    juce::File root { defaultRoot() };
    juce::int64 lastAddedMs = 0; // keeps "added" strictly increasing inside one instance
};

// Collection panel view-model: pure, GUI-free, no IO. The chip row and the search
// box map to a CollectionQuery; a row's second line is built from its metadata.
struct CollectionChips
{
    enum Backend { all = 0, sa3 = 1, yue2 = 2, ace = 3 };
    int backend = all;           // one of Backend
    bool starredOnly = false;    // the starred toggle
    juce::String search;         // the search box, as typed
};

struct CollectionView
{
    static CollectionQuery queryFor (const CollectionChips& chips);
    static bool isFiltering (const CollectionChips& chips); // anything but ALL, no star, empty search
    // "104 BPM - Am - 0:30". An unknown part (bpm <= 0, empty key, duration <= 0) is left out.
    static juce::String subtitle (double bpm, const juce::String& key, double durationS);
    static juce::String subtitle (const CollectionItem& it) { return subtitle (it.bpm, it.key, it.durationS); }
    static juce::String badge (const juce::String& backend); // "SA3" / "YuE2" / "ACE" / "AUTO" / upper case, empty = none
    static juce::String countText (int shown, int total, bool filtering); // "8" or "3 / 8"
    // Test hook (teeth), HSLINK_TEETH=NO_COLL_VIEW: subtitle returns an empty string,
    // queryFor returns the default query, the processor's background rescan does nothing.
    static std::atomic<bool> testDisabled;
};
