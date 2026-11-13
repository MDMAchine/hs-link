// HOT-STEP LINK -- PluginEditor.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "TakeEditorPanel.h"
#include "TakePeaks.h"
#include <memory>
#include <mutex>

// The typed seed. Pure, no GUI: the editor's seed field and the tests call the same parse.
// The server holds the seed in a double and adds the take index to it, so the largest
// seed that stays exact for every take is 2^53 - 1 - 64.
namespace SeedInput
{
    constexpr juce::int64 kMaxSeed = 9007199254740927LL; // 2^53 - 1 - 64
    constexpr int kMaxDigits = 16;                       // digits of kMaxSeed
    constexpr juce::int64 kRandom = -1;                  // the "random seed" sentinel

    struct Result
    {
        bool ok = false;        // false = refused, value is not to be used
        juce::int64 value = kRandom;
    };

    // text is trimmed first. Empty: { true, kRandom }. Only the digits 0 to 9, at most
    // kMaxDigits of them (checked before the number is read, so nothing can wrap), and a
    // value of at most kMaxSeed: { true, value }. Anything else: { false, kRandom }.
    // Never clamps and never rounds.
    inline Result parse (const juce::String& text)
    {
        const auto t = text.trim();
        if (t.isEmpty())
            return { true, kRandom };
        if (t.length() > kMaxDigits || ! t.containsOnly ("0123456789"))
            return {};
        const auto v = t.getLargeIntValue();
        if (v < 0 || v > kMaxSeed)
            return {};
        return { true, v };
    }

    // The note shown when parse refuses a text.
    inline juce::String refusedText() { return "Seed too large: max " + juce::String (kMaxSeed); }
}

// VARIATION knob: seed spread done in the plugin (the server has no variation field). Pure.
// spread (base, variation, n): the seed of the n-th generate from one base seed.
//   variation <= 0, n <= 0 or base < 0 (random): base, unchanged.
//   else base + n * step, step = 8 + round (variation / 100 * 992), so variation 100
//   moves 1000 seeds per generate. step >= 8 keeps the takes of one job (the server
//   seeds take i with seed + i, up to 8 takes) apart from the next job's takes.
//   Past kMaxSeed the result wraps to 0 and up, so every seed stays exact.
namespace SeedSpread
{
    inline juce::int64 step (double variation)
    {
        return 8 + (juce::int64) juce::roundToInt (juce::jlimit (0.0, 100.0, variation) / 100.0 * 992.0);
    }
    inline juce::int64 spread (juce::int64 base, double variation, int n)
    {
        if (base < 0 || n <= 0 || ! (variation > 0.0))
            return base;
        const juce::int64 v = base + (juce::int64) n * step (variation);
        return v % (SeedInput::kMaxSeed + 1);
    }
}

// Fresh look-and-feel for the mock-up B style (dark plates, pink accent).
class HslinkLook : public juce::LookAndFeel_V4
{
public:
    HslinkLook();
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool, bool) override;
    void drawComboBox (juce::Graphics&, int, int, bool, int, int, int, int, juce::ComboBox&) override;
    void fillTextEditorBackground (juce::Graphics&, int, int, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int, int, juce::TextEditor&) override;
    void drawLinearSlider (juce::Graphics&, int, int, int, int, float, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    void drawRotarySlider (juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
    juce::Font getLabelFont (juce::Label&) override;
};

// Banner over the Generate controls while the server is unreachable (the rest of the
// editor stays usable). Retry wakes the worker.
class OfflineOverlay : public juce::Component
{
public:
    OfflineOverlay();
    void setMessage (const juce::String& url, const juce::String& err, const juce::StringArray& attempts = {});
    void paint (juce::Graphics&) override;
    void resized() override;
    juce::TextButton retryButton { "Retry" };
private:
    juce::Label message, detail, attemptsLabel;
};

// R5: one take in the grid. Mini waveform, star toggle, click to select.
class TakeCell : public juce::Component, public juce::SettableTooltipClient, private juce::ChangeListener
{
public:
    // key: the take's name (TakeGrid::Cell::key), the cell's identity for its whole life.
    // thumbSource: the file the mini waveform is read from, fixed when the cell is made.
    // Always the take's temp copy (TakeGrid::thumbSourceOf), never the lasting file.
    TakeCell (const juce::String& key, const juce::String& label, const juce::File& thumbSource, int index,
              juce::AudioFormatManager& fm, juce::AudioThumbnailCache& cache);
    ~TakeCell() override { thumb.removeChangeListener (this); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override; // R6: external drag of the stable file
    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    void setStarred (bool s) { starButton.setToggleState (s, juce::dontSendNotification); }
    // A take with no lasting copy: file is its temp file. It shows a NOT SAVED badge,
    // can be auditioned, starred and exported, and cannot be dragged to the DAW.
    // u: a copy attempt failed (badge, tooltip, no drag). pending: no lasting copy yet
    // and no failed attempt (no badge; the drag is still refused).
    void setUnsaved (bool u, const juce::String& why, bool pending = false);
    bool isUnsaved() const { return unsaved; }
    // Restored take whose file is gone: dimmed, FILE MISSING label, no waveform.
    void setMissing (bool m) { if (m != missing) { missing = m; setAlpha (m ? 0.55f : 1.0f); repaint(); } }
    bool isMissing() const { return missing; }
    void setIndex (int i) { if (i != index) { index = i; repaint(); } }
    // The measured-tempo tag in the waveform's lower left corner (TakeGrid::tempoTagOf).
    void setTempoTag (const TakeGrid::TempoTag& t) { if (t != tempoTag) { tempoTag = t; repaint(); } }
    void setFitTag (const TakeGrid::TempoTag& t) { if (t != fitTag) { fitTag = t; repaint(); } }
    const TakeGrid::TempoTag& getFitTag() const { return fitTag; }
    const TakeGrid::TempoTag& getTempoTag() const { return tempoTag; }
    // The take length in the waveform's lower right corner (TakeGrid::lengthTextOf).
    void setLengthText (const juce::String& t) { if (t != lengthText) { lengthText = t; repaint(); } }
    const juce::String& getLengthText() const { return lengthText; }
    // true when the length plate is drawn: inside the waveform, its text not cut, and
    // clear of the tempo plate. plateOut/tempoOut: where they are ({} = not drawn).
    bool lengthFitsForTest (juce::Rectangle<int>* plateOut = nullptr, juce::Rectangle<int>* tempoOut = nullptr) const;
    juce::String tempoShownForTest() const { return tempoShown(); }
    static float lengthTextWidthForTest (const juce::String& t) { return lengthTextWidth (t); }
    const juce::String key, label;
    // What the "x" button hands to onDelete: the full path of the take's temp file
    // (the processor's delete key). Two takes can share a name, so key is not used.
    juce::String deleteKey;
    // The file the waveform thumbnail was given when the cell was made ({} = none).
    // A kept cell is re-pointed only when this file is gone (repointThumbIfGone).
    juce::File thumbFile;
    // src: TakeGrid::thumbSourceOf now. Re-points the waveform when thumbFile no longer
    // exists and src does (the temp copy was cleaned up after the cell was made).
    bool repointThumbIfGone (const juce::File& src);
    // Every action names the take by its key; the file is looked up by the owner then.
    std::function<void (const juce::String&)> onSelect;
    std::function<void (const juce::String&, bool)> onStar;
    std::function<void (const juce::String&)> onAdd; // "+": keep this take in the Collection
    std::function<void (const juce::String&)> onDelete; // "x": delete this take (Deleted folder, short undo); gets deleteKey
    std::function<void (const juce::String&, int)> onLoadSlot; // "A"/"B": load this take's settings into slot 0/1
    // The lasting file to hand to the host, asked once per drag gesture. {} = refuse.
    std::function<juce::File (const juce::String&)> dragFileFor;
    // The completion callback handed to the external drag. It holds the cell by a safe
    // pointer, so it may run after the cell was destroyed and then does nothing.
    static std::function<void()> dragDoneFor (TakeCell* cell);
    // The check made once per drag gesture before the external drag starts: the file
    // exists and is not empty (counted as one EditorFs::tagDrag call). false = refuse.
    static bool dragFileUsable (const juce::File& f);
    bool isDragRefusedForTest() const { return dragRefused; }
    bool isDragMissingForTest() const { return dragMissing; }
    bool isDragStartedForTest() const { return dragStarted; }
    void setDragStartedForTest (bool b) { dragStarted = b; }
private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
    int index;
    juce::AudioThumbnail thumb;
    juce::TextButton starButton { "*" }, addButton { "+" }, deleteButton { "x" };
    juce::TextButton slotAButton { "A" }, slotBButton { "B" };
    bool selected = false;
    bool dragStarted = false;
    bool unsaved = false, notCopiedYet = false, dragRefused = false;
    bool missing = false;
    bool dragMissing = false; // the refusal is for a lasting file that is gone or empty
    TakeGrid::TempoTag tempoTag, fitTag;
    juce::String lengthText;
    juce::Rectangle<int> waveArea() const;       // the waveform rectangle paint() draws in
    juce::Rectangle<int> tempoPlate() const;     // {} = no tempo tag
    juce::Rectangle<int> lengthPlate() const;    // {} = no length (or no room in the whole waveform width)
    juce::String tempoShown() const;             // the tempo tag, shortened so the length always shows
    static float lengthTextWidth (const juce::String& t);
};

// R5: scrollable grid of takes; columns follow the width, rows scroll.
class TakeListComponent : public juce::Component
{
public:
    TakeListComponent();
    // shownCells: TakeGrid::Model::shown(). Cells are keyed by Cell::key: a take keeps
    // its cell (highlight, star, waveform) when its lasting copy lands; the cell list
    // changes only when the list of keys does, and then cells whose key stays are reused.
    void setTakes (const std::vector<TakeGrid::Cell>& shownCells, const juce::String& unsavedWhy = {});
    void resized() override;
    void paint (juce::Graphics&) override;
    std::function<void (const juce::String&)> onSelect;
    std::function<void (const juce::String&, bool)> onStar;
    std::function<void (const juce::String&)> onAdd;
    std::function<void (const juce::String&)> onDelete;
    std::function<void (const juce::String&, int)> onLoadSlot;
    std::function<juce::File (const juce::String&)> dragFileFor;
    const juce::StringArray& getCellKeys() const { return current; }
    // Tests: the address of the cell showing key (nullptr = none), to prove a cell is kept.
    const TakeCell* cellForKey (const juce::String& key) const;
    // Pure: grid columns for a width (>= 1).
    static int columnsFor (int width) { return juce::jmax (1, width / kMinCellW); }
    static constexpr int kMinCellW = 170, kCellH = 86;
    int getNumCells() const { return cells.size(); }
private:
    void layoutCells();
    juce::AudioFormatManager formats;
    juce::AudioThumbnailCache cache { 64 };
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<TakeCell> cells;
    juce::StringArray current; // keys, in cell order
};

// Queue list: one compact card per generate job (newest at the bottom).
class QueueListComponent : public juce::Component
{
public:
    struct Card
    {
        int id = 0;
        juce::String caption, state;
        int takes = 0, takeCount = 0;
        bool removable = false, finished = false;
        bool operator== (const Card& o) const
        {
            return id == o.id && caption == o.caption && state == o.state && takes == o.takes
                && takeCount == o.takeCount && removable == o.removable && finished == o.finished;
        }
    };
    void setCards (const std::vector<Card>& c, int selectedId);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    std::function<void (int)> onRemove, onSelect;
    // Pure: card state label for a job (queued, running, done, failed, cancelled).
    static juce::String stateLabel (const DawJob& j);
    static constexpr int kRowH = 22;
    int getNumCards() const { return (int) cards.size(); }
    // Queued plus running jobs: the QUEUE header count. Finished cards stay listed but do not count.
    int getNumActiveCards() const
    {
        int n = 0;
        for (const auto& c : cards)
            n += c.finished ? 0 : 1;
        return n;
    }
    // The empty-list hint and the lines paint() may wrap it to.
    static constexpr const char* kEmptyHint = "No jobs yet. Generate while a job runs to queue more.";
    static constexpr int kEmptyHintLines = 2;
    static constexpr float kEmptyHintFontH = 11.0f;
    static constexpr int kEmptyHintInsetX = 6;
private:
    juce::Rectangle<int> rowBounds (int i) const;
    std::vector<Card> cards;
    int selected = 0;
};

// Collection panel: one saved take. Name, backend badge, "104 BPM - Am - 0:30",
// star and remove. Click selects (audition), drag starts an external file drag.
class CollectionRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit CollectionRow (const CollectionItem& it);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void setSelected (bool s) { if (s != selected) { selected = s; repaint(); } }
    // Tests: what mouseDrag does once the pointer has moved far enough. True = the file
    // was there at the one check this gesture makes (the drag itself is not started here).
    bool dragCheckForTest() { return checkDragFile(); }
    const CollectionItem item;
    std::function<void (const CollectionItem&)> onSelect, onRemove;
    std::function<void (const CollectionItem&, bool)> onStar;
    static constexpr int kRowH = 38;
private:
    enum Zone { zoneBody, zoneStar, zoneRemove };
    Zone zoneAt (juce::Point<int> p) const;
    Zone pressed = zoneBody;
    bool selected = false, dragStarted = false;
    bool dragChecked = false, dragFileOk = false; // one file check per drag gesture
    bool checkDragFile();
};

// Collection bar (bottom of the editor, Mockup A): title with a count, search box,
// filter chips and a sideways list of saved takes. Holds no files of its own: the editor feeds it the
// processor's cached list and it filters through CollectionStore::filter.
class CollectionPanel : public juce::Component
{
public:
    CollectionPanel();
    void setItems (const std::vector<CollectionItem>& items); // also ends the "Loading" state
    void setSelectedFile (const juce::File& f);
    void setChips (const CollectionChips& c);
    const CollectionChips& getChips() const { return chips; }
    // Footer message, held until it expires (tick) so nothing overwrites it early.
    void showNote (const juce::String& text, bool isError, int holdMs = 4500);
    void tick();
    void paint (juce::Graphics&) override;
    void resized() override;
    int getNumRows() const { return rows.size(); }
    std::function<void (const CollectionItem&)> onSelect, onRemove;
    std::function<void (const CollectionItem&, bool)> onStar;
private:
    void rebuild();
    void layoutRows();
    void syncChipButtons();
    std::vector<CollectionItem> all;
    CollectionChips chips;
    bool loaded = false;
    juce::TextEditor search;
    juce::TextButton chipAll { "ALL" }, chipSa3 { "SA3" }, chipYue2 { "YuE2" }, chipAce { "ACE" },
                     chipMiniMax { "MiniMax" }, chipHeart { "HeartMuLa" }, chipStar { "* starred" };
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<CollectionRow> rows;
    juce::File selectedFile;
    juce::String note;
    bool noteError = false;
    juce::uint32 noteUntilMs = 0;
    juce::Rectangle<int> headerArea, listArea, footerArea;
    static constexpr int kChipW = 210, kChipGap = 6;
};

// Role cards: three workflow shortcuts at the top of the Generate plate.
// The lit card is set by the editor from the backend setting; nothing is stored here.
class RoleCardRow : public juce::Component
{
public:
    RoleCardRow() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
    void setActive (int card) { if (card != active) { active = card; repaint(); } }
    int getActive() const { return active; }
    void paint (juce::Graphics&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::function<void (int)> onPick;
    static constexpr int kHeight = 36;
private:
    juce::Rectangle<int> cardBounds (int i) const;
    int cardAt (juce::Point<int> p) const;
    int active = -1, hover = -1;
};

// SOURCE panel waveform (Mockup A): min/max peaks of the source clip, bar ruler under
// it. Peaks are built on the view's own pool thread (never the audio or message thread)
// and handed over under a lock; the editor's VBlank tick calls poll(), which repaints
// when a build lands. No Timer.
class SourceWaveView : public juce::Component, public juce::SettableTooltipClient
{
public:
    SourceWaveView() = default;
    // Header capture-state look (pure, testable without a GUI).
    enum class CaptureState { idle, recording, captured };
    struct HeaderStyle { juce::Colour text; bool dot = false; };
    static HeaderStyle headerStyleFor (CaptureState s);
    ~SourceWaveView() override;
    // Message thread. Starts a peak build when file or length changed; empty file clears.
    void setSource (const juce::File& file, double lengthS, double bpm);
    // Message thread (VBlank tick): picks up a finished build. True when it repainted.
    bool poll();
    void setRecording (bool r) { if (r != recording) { recording = r; repaint(); } }
    void paint (juce::Graphics&) override;
    bool hasPeaksForTest() const { return peaks != nullptr && peaks->ok; }

private:
    struct Shared
    {
        std::mutex lock;
        std::shared_ptr<TakePeaks::Data> result; // set by the pool thread
        std::atomic<bool> cancel { false };
    };
    std::shared_ptr<Shared> pending;
    std::shared_ptr<TakePeaks::Data> peaks;
    juce::File file;
    double lengthS = 0.0, bpm = 0.0;
    bool recording = false;
    juce::ThreadPool pool { 1 }; // declared last, stopped first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SourceWaveView)
};

// Settings popover (Mockup A): holds the former SETUP form. The controls are
// reparented here once; they keep their values and attachments across open/close.
class SettingsPopover : public juce::Component
{
public:
    juce::TextButton closeButton { "Close" };
    SettingsPopover() { addAndMakeVisible (closeButton); }
    void paint (juce::Graphics&) override;
};

// What the MIDI file build needs about one take (all values, safe to copy to a thread).
struct MidiBuildInputs
{
    juce::File src;                          // the WAV to read (warped copy when a warp is in effect)
    TakeEditorMath::Grid grid;               // bpm/downbeat of src
    std::vector<WarpMarkers::Marker> markers; // effective warp (empty = none)
    double srcBpm = 0.0;                     // unwarped tempo, for the per-segment map
    bool hasTempo = false;
    int beatsPerBar = 4, sigDen = 4;
    int mutant = 0;                          // test mutants (0 = real)
    int quantDiv = 0;                        // export quantize grid: 0 off, 16, 8
    double quantStrength = 1.0;              // 0..1
    // The loop (else the crop) of the take in frames of src (through the warp when one is
    // in effect); end exclusive. -1 = file start / file end. Only hits inside are kept,
    // timed from regionStart, and the tempo map is shifted to match.
    juce::int64 regionStart = -1, regionEnd = -1;
};
struct MidiBuildResult
{
    juce::File tempo, hits, notes;           // {} = not written (notes: only for tonal takes)
    juce::String err;                        // empty = ok
};
// Reads in.src, writes <name>_hits.mid (and <name>_tempo.mid when hasTempo, and
// <name>_melody.mid when the take is tonal, see MidiExport::isTonal) into destDir.
// Blocking file I/O: call from a background thread. Editor-free, so tests can call it.
MidiBuildResult buildMidiFiles (const MidiBuildInputs& in, const juce::File& destDir);
// buildMidiFiles, returning the full paths of the files that exist afterwards ({} on failure).
juce::StringArray buildMidiFilesForDrag (const MidiBuildInputs& in, const juce::File& destDir);
// A fresh unique folder under the system temp dir for drag-out MIDI files (created).
juce::File midiDragTempDir();
// The root midiDragTempDir() makes its folders in (temp/HOTSTEP_LINK_midi).
juce::File midiDragTempRoot();
// Removes everything inside root (the root itself stays). Refuses (returns -1) unless
// root is named HOTSTEP_LINK_midi; never touches anything outside root. Returns the
// number of children it could not remove.
int clearMidiDragTempRoot (const juce::File& root, juce::RelativeTime minAge = juce::RelativeTime::hours (1.0),
                           juce::Time now = juce::Time());

// Drag source for the MIDI files of the selected take. When the files are missing the
// chip asks the editor to build them (background thread) and starts the drag once ready.
class MidiDragChip : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<juce::StringArray()> filesFor; // {} = nothing to drag yet
    std::function<bool()> requestBuild;          // start a background build; false = cannot
    void paint (juce::Graphics&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override { dragStarted = false; }
    // Message thread, when a requested build ends: drags the files if the mouse is still down.
    void buildFinished (const juce::StringArray& files);
    bool isBuilding() const { return building; }
private:
    bool dragStarted = false, building = false;
};

class HotStepLinkEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, public juce::KeyListener
{
public:
    explicit HotStepLinkEditor (HotStepLinkProcessor&);
    ~HotStepLinkEditor() override;

    // Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z for the open take editor, caught at the top-level window
    // (key listener) so they work whatever component holds focus, unless a TextEditor has it.
    using juce::AudioProcessorEditor::keyPressed;
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;
    void parentHierarchyChanged() override;
    static std::atomic<bool> testNoTopLevelKeys; // teeth: the top-level undo/redo route is off

    void paint (juce::Graphics&) override;
    void resized() override;

    // The text of the sync readout while a host is active (not standalone): pure, the
    // label shows exactly this. "no host tempo" in place of the BPM when bpmKnown is off.
    static juce::String syncReadoutText (const HostSyncState& s);
    // The three HOST SYNC panel cells (TEMPO, METER, BARS). Pure; "--" while not live
    // (standalone or host sync inactive) or the field is unknown. BARS shows the loop
    // range "a-b" while looping, else the current bar.
    static juce::String syncTempoCellText (bool isStandalone, const HostSyncState& s);
    static juce::String syncMeterCellText (bool isStandalone, const HostSyncState& s);
    static juce::String syncBarsCellText (bool isStandalone, const HostSyncState& s);
    // The tempo the Fit to bars field of the take editor starts from: the manual tempo
    // in the standalone and while host sync is not active; else the host tempo, 0 while
    // the host gives none (never the stored last value or 120). Pure.
    static double fitHostBpm (bool isStandalone, const HostSyncState& s, double manualBpm);

    // Drop a WAV: the repaint / lego source, or a take on the TAKES plate (all formats).
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // Test access (the test binary has no display). runRefreshForTest runs one VBlank
    // tick including the part that runs on every 10th tick. getTakeGridForTest is the
    // model the grid was last filled from; getTakeCellKeysForTest the keys of the cells.
    void runRefreshForTest() { tick (true); }
    // Label of the "+4 more like" button for a take label ({} = none selected); short so it fits.
    // availWidth = button width minus the 4 px side margins; the take id is dropped when it would not fit.
    static juce::String moreLikeTextFor (const juce::String& takeLabel, float availWidth = 1.0e9f);
    static float moreLikeLabelWidth (const juce::String& text);
    juce::TextButton& getMoreLikeButtonForTest() { return moreLikeButton; }
    const TakeGrid::Model& getTakeGridForTest() const { return takeModel; }
    juce::StringArray getTakeCellKeysForTest() const { return takeList.getCellKeys(); }
    const void* getTakeCellForTest (const juce::String& key) const { return takeList.cellForKey (key); }
    // The file the cell's waveform thumbnail was given ({} = no cell, or no source).
    juce::File getTakeThumbSourceForTest (const juce::String& key) const
    {
        const auto* c = takeList.cellForKey (key);
        return c != nullptr ? c->thumbFile : juce::File();
    }
    void selectTakeForTest (const juce::String& key) { selectTake (key); }
    void setSettingsOpenForTest (bool open) { setSettingsOpen (open); }
    bool settingsOpenForTest() const { return settingsPopover.isVisible(); }
    juce::File dragFileForTest (const juce::String& key) { return dragFileFor (key); }
    bool exportEnabledForTest() const { return exportWavButton.isEnabled(); }
    // The file Export WAV would read now ({} = nothing selected); no chooser is opened.
    juce::File exportSourceForTest() { return exportSource(); }
    // Teeth (HSLINK_TEETH=NO_WARP_EXPORT): Export and drag ignore the editor's warp.
    bool noWarpExportForTest = false;
    // Test mutants: skip the cache cap / the tempo tag on a newly written warped file.
    bool noWarpCacheCapForTest = false;
    bool noWarpTagForTest = false;
    // Keep at most kWarpCacheMax files in <root>_warped and drop any older than
    // kWarpCacheMaxAgeDays, oldest first; keep is never removed.
    static constexpr int kWarpCacheMax = 64;
    static constexpr int kWarpCacheMaxAgeDays = 7;
    static void trimWarpCache (const juce::File& dir, const juce::File& keep);
    juce::File warpedFileForTest (const juce::File& temp, const juce::File& src) { return warpedFileFor (temp, src); }
    // The "x" of a take cell and the Undo button, without a click.
    void deleteTakeForTest (const juce::String& tempPath) { deleteTake (tempPath); }
    void undoDeleteForTest() { undoDelete(); }
    bool moreLikeEnabledForTest() const { return moreLikeButton.isEnabled(); }
    // The "+4 more like take N" click for a take key, without a click.
    void startMoreLikeForTest (const juce::String& key) { startMoreLike (key); }
    void loadTakeIntoSlotForTest (const juce::String& key, int slot) { loadTakeIntoSlot (key, slot); }
    bool undoRowVisibleForTest() const { return undoDeleteButton.isVisible(); }
    // The "Edit take" button without a click (false = no take cell is selected), and the panel.
    bool openTakeEditorForTest() { return openTakeEditor(); }
    TakeEditorPanel& getTakeEditorForTest() { return takeEditor; }
    juce::Component::SafePointer<juce::Component> keyHost; // top-level component our KeyListener is on
    bool editTakeEnabledForTest() const { return editTakeButton.isEnabled(); }
    // -1 = no cell for key, else 1 when the cell shows missing and is dimmed.
    int takeCellMissingForTest (const juce::String& key) const;
    juce::Rectangle<int> editTakeBoundsForTest() const { return editTakeButton.getBounds(); }
    juce::Rectangle<int> reanalyzeBoundsForTest() const { return reanalyzeButton.getBounds(); }
    juce::Rectangle<int> setupPlateForTest() const { return setupPlate; }
    juce::Rectangle<int> generateBoundsForTest() const { return generateButton.getBounds(); }
    // Offline banner (covers only the Generate controls) and the WAV-to-take path.
    // Mutant: mutOverlayFullWindow = the banner covers the whole window again.
    enum OfflineMutant { mutOfflineNone = 0, mutOverlayFullWindow = 1 };
    int offlineMutantForTest = mutOfflineNone; // call resized() after setting
    juce::Rectangle<int> offlineBannerBoundsForTest() const { return overlay.getBounds(); }
    bool offlineBannerVisibleForTest() const { return overlay.isVisible(); }
    juce::Rectangle<int> takesPlateForTest() const { return queuePlate; }
    juce::Rectangle<int> addTakeBoundsForTest() const { return addTakeButton.getBounds(); }
    bool generateEnabledForTest() const { return generateButton.isEnabled(); }
    bool addTakeFromFileForTest (const juce::File& f) { return addTakeFromFile (f); }
    // 0 take list, 1 Edit take, 2 audition Play, 3 Export WAV, 4 collection panel.
    juce::Component* offlineTargetForTest (int which)
    {
        juce::Component* c[] = { &takeList, &editTakeButton, &playButton, &exportWavButton, &collectionPanel };
        return which >= 0 && which < 5 ? c[which] : nullptr;
    }
    bool forceStandaloneForTest = false; // the test exe is not a standalone app: lets filesDropped run
    // Teeth: true = Add as take and WAV drop are Standalone only again (the pre-E1 gate).
    bool addTakeStandaloneOnlyForTest = false;
    bool addTakeVisibleForTest() const { return addTakeButton.isVisible(); }
    void applyAddTakeGateForTest() { addTakeButton.setVisible (addTakeAllowed()); resized(); }
    bool noAutoSelectForTest = false; // teeth: a newly arrived take is not selected by refreshTakes
    // Header counters (b1e6a71): the TAKES and QUEUE counts the editor last repainted for.
    // Teeth HEADER_NO_REPAINT: noHeaderRepaintForTest restores the old code (no repaint
    // after setCards, none on the auto-select early return of refreshTakes).
    bool noHeaderRepaintForTest = false;
    int queueCountShownForTest() const { return lastQueueCountShown; }
    int takeCountShownForTest() const { return lastTakeCountShown; }
    bool openEditorBlocksSelectForTest = false; // teeth: an open take editor blocks the auto-select (the v0.5.19 bug)
    int takeCellCountForTest() const { return takeList.getNumCells(); }
    juce::Rectangle<int> exportWavBoundsForTest() const { return exportWavButton.getBounds(); }
    // Smallest window with the whole Preview section. Height: the rows of the middle plate
    // take 700 px, and previewWaveArea() needs a card of 60 px. Hosts clamp to this too.
    static constexpr int kMinWindowW = 1080, kMinWindowH = 760;
    juce::Rectangle<int> exportMidiBoundsForTest() const { return exportMidiButton.getBounds(); }
    juce::Rectangle<int> quantizeBoundsForTest() const { return quantizeBox.getBounds(); }
    juce::Rectangle<int> midiDragBoundsForTest() const { return midiDrag.getBounds(); }
    bool exportMidiEnabledForTest() const { return exportMidiButton.isEnabled(); }
    // Export MIDI without a click. midiSyncForTest = run the export and its completion on
    // this thread (no worker, no callAsync). Mutants: 1 = the writer uses a fixed 120 BPM,
    // 2 = every hit one onset hop (2.5 ms) late.
    bool midiSyncForTest = false;
    int midiMutantForTest = 0;
    void exportMidiForTest() { exportMidi(); }
    juce::File lastMidiTempoForTest() const { return lastMidiTempo; }
    juce::File lastMidiHitsForTest() const { return lastMidiHits; }
    bool midiDragEnabledForTest() const { return midiDrag.isEnabled(); }
    bool midiDragBuildingForTest() const { return midiDrag.isBuilding(); }
    juce::Rectangle<int> playBoundsForTest() const { return playButton.getBounds(); }
    juce::Rectangle<int> gainLabelBoundsForTest() const { return gainLabel.getBounds(); }
    juce::Rectangle<int> followHostBoundsForTest() const { return followHostButton.getBounds(); }
    juce::Rectangle<int> levelMatchBoundsForTest() const { return levelMatchButton.getBounds(); }
    juce::Rectangle<int> previewCardForTest() const { return previewCard; }
    juce::Rectangle<int> previewWaveForTest() const { return previewWaveArea(); }
    juce::Rectangle<int> genPlateForTest() const { return genPlate; }
    juce::Rectangle<int> previewHeaderForTest() const { return previewHeaderArea(); }
    juce::Rectangle<int> queueListBoundsForTest() const { return queueList.getBounds(); }
    // True while the seed field holds a text that was refused (over the limit).
    bool isSeedRefusedForTest() const { return seedRefused; }
    // Types into the seed field without committing it (no Return, no focus loss).
    void typeSeedForTest (const juce::String& t) { seedEditor.setText (t, false); }
    // Header bar animation phase (advanced by the VBlank tick at about 20 fps).
    int getHeaderAnimFrameForTest() const { return headerAnimFrame; }

private:
    HslinkLook look; // declared first so it outlives every child
    juce::Rectangle<int> headerArea, statusArea, collectionArea, setupPlate, genPlate, queuePlate, jobCard, previewCard, sourceSlot;
    void paintSection (juce::Graphics&, juce::Rectangle<int> plate, const juce::String& title, int count) const;
    void refreshSyncReadout();
    void refreshSource();
    void useSourceFile (const juce::File& f);
    void refreshServerStatus();
    void applyModeGating (const DawStatus& st);
    // takesOverride > 0 and seedOverride >= 0 replace the takes count and the seed of
    // this one request only (the "+4 more like take N" button); the settings stay as they are.
    void startJob (int takesOverride = 0, juce::int64 seedOverride = -1);
    // "+4 more like take N": four new takes seeded just past the seed the server reported
    // for this take (takeSeed + 1, so none of the four repeats take N exactly).
    void startMoreLike (const juce::String& key);
    void loadTakeIntoSlot (const juce::String& key, int slot); // per-row A/B: take settings into the header A/B slot
    void seedDebugTakes();                                      // standalone debug hook only (HSLINK_DEBUG_SEED_TAKES)
    void refreshJob();
    void refreshTakes();
    void tick (bool periodic);
    // The grid model from the processor's take entries (no file-system call).
    TakeGrid::Model buildTakeModel();
    TakeGrid::Model takeModel;
    juce::StringArray knownTakeKeys; // keys seen by the last refreshTakes (new-take auto-select)
    bool takeKeysSeeded = false;
    int lastTakeCountShown = -1;
    int lastQueueCountShown = -1;
    int lastJobCardShown = -1;
    void selectTake (const juce::String& key);
    juce::File dragFileFor (const juce::String& key);
    juce::File exportSource(); // the file Export reads, decided on the button press
    // src, or when the take editor holds a warp in effect for temp, a rendered copy of
    // src with that warp (cached by take and marker list); {} when the render fails.
    juce::File warpedFileFor (const juce::File& temp, const juce::File& src);
    void exportWav();
    // Writes <take>_tempo.mid and <take>_hits.mid beside the file Export reads (the
    // lasting take, or its warped copy) on a worker thread; announced in the job label.
    void exportMidi();
    juce::StringArray midiFilesForSelection() const;
    bool midiInputsForSelection (MidiBuildInputs& out); // false = nothing selected
    bool buildMidiForDrag();                            // chip request: background build to temp

    // Source clip (plugin: Capture arm; standalone: Drop WAV + region fields)
    juce::TextButton captureButton { "Capture" }, dropButton { "Drop WAV..." };
    // All formats: list a WAV as a take (no server). Drop WAV keeps setting the Source.
    juce::TextButton addTakeButton { "Add as take..." };
    std::unique_ptr<juce::FileChooser> addTakeChooser;
    // Add as take + WAV drop: every format (VST3, CLAP, Standalone) since E1.
    bool addTakeAllowed() const { return standalone || forceStandaloneForTest || ! addTakeStandaloneOnlyForTest; }
    bool addTakeFromFile (const juce::File& f); // false = not a readable audio file / already listed
    bool lastServerOnline = true;               // false = known offline: Generate is blocked
    // CAPTURE (Mockup A): stop-and-keep while recording (armCapture(false) keeps the
    // take); otherwise a disabled state readout (armed / captured N bars).
    juce::TextButton captureNowButton { "CAPTURE" };
    void refreshCaptureNow();
    // HOST SYNC grid (Mockup A): TEMPO / METER / BARS, "--" when unknown.
    juce::Rectangle<int> syncPanel;
    juce::String syncTempoText { "--" }, syncMeterText { "--" }, syncBarsText { "--" };
    juce::Label sourceCaption, sourceLabel, regionCaption, regionInfo;
    SourceWaveView sourceWave;
    juce::Rectangle<int> sourcePanel, knobPanel;
    // UNDERSTAND + INPUT boxes (Mockup A, left column). Key / tempo come from the
    // newest finished understand job; tags and input level are editor-side text
    // state only (no backing state in the processor yet).
    juce::Rectangle<int> understandPanel, inputPanel;
    juce::Rectangle<int> previewHeaderArea() const;
    juce::String understandKeyText { "--" }, understandTempoText { "--" };
    juce::StringArray understandTags;
    float inputLevelShown = 0.0f;
    juce::TextButton reanalyzeButton { "RE-ANALYZE CAPTURE" };
    void refreshUnderstand();
    // SETTINGS button + popover holding the SETUP form.
    juce::TextButton settingsButton { "SETTINGS" };
    SettingsPopover settingsPopover;
    void setSettingsOpen (bool open);
    juce::TextEditor regionStartEditor, regionEndEditor;
    std::unique_ptr<juce::FileChooser> sourceChooser;
    juce::String lastSourceText;

    // R6 export
    juce::TextButton exportWavButton { "Export WAV" }, exportMidiButton { "Export MIDI" };
    juce::ComboBox quantizeBox;
    juce::Slider quantStrengthSlider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    std::unique_ptr<juce::FileChooser> exportChooser;
    MidiDragChip midiDrag;
    bool midiBusy = false;
    // Where the last export went: full folder path plus a reveal button (hidden until an export).
    juce::Label exportPathLabel;
    juce::TextButton showExportButton { "Show in folder" };
    juce::File lastExportFile;
    void showExportResult (const juce::File& written);
    juce::File lastMidiSelection;          // the selected take the files below belong to
    juce::File lastMidiTempo, lastMidiHits, lastMidiNotes; // {} = not written (tempo: the take had no tempo)

    // R5 take list + audition
    TakeListComponent takeList;
    // Unsaved / recovered takes. Its own label: the job label is rewritten on every refresh.
    juce::Label saveNotice;
    juce::TextButton retrySaveButton { "Retry save" };
    juce::String lastNoticeText;
    bool lastNoticeUnsaved = false;
    // Take delete: one row under the Takes grid while a delete can be undone.
    juce::Label deleteNotice;
    juce::TextButton undoDeleteButton { "Undo" };
    juce::String lastPendingDelete;
    void deleteTake (const juce::String& tempPath);
    void undoDelete();
    juce::uint32 lastTakesRestoreGen = 0; // D2: proc.getTakesRestoreGeneration() at the last refreshTakes
    juce::ToggleButton levelMatchButton { "Level match" };
    juce::TextButton playButton { "Play" }; // preview clock (all modes)
    juce::ToggleButton followHostButton { "Follow host" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> followHostAtt;
    juce::Label gainLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> levelMatchAtt;
    float lastGainShown = -1.0f;
    // Preview waveform (message thread copies of the take's peaks).
    AuditionMath::PeakPoll peakPoll;
    std::vector<float> peakMin, peakMax;
    double lastPlayhead = -2.0;
    juce::Rectangle<int> previewWaveArea() const;
    void refreshPreview();

    // R4 job hookup
    juce::Label captionCaption, jobLabel;
    juce::TextEditor captionEditor;
    juce::Label lyricsCaption;
    juce::TextEditor lyricsEditor;
    juce::ToggleButton instrumentalButton { "Instrumental" };
    juce::TextButton generateButton { "Generate" }, cancelButton { "Cancel" };
    juce::Rectangle<int> progressArea;
    double jobProgress = 0.0;
    int currentJob = 0;
    int selectedJob = 0; // queue card whose takes the TAKES area shows, 0 = all
    QueueListComponent queueList;
    void refreshQueue();
    juce::String lastJobText;

    HotStepLinkProcessor& proc;
    const bool standalone;
    // Dev shot hooks (Standalone --shot-* flags, see parseShotArgs). step 0 = idle.
    struct ShotPlan { int step = 0, take = 0, marker = -1, waitTicks = 0; double dstSec = 0.0, zoomStart = 0.0, zoomEnd = 0.0; bool snap = false; } shot;
    void parseShotArgs (const juce::String& cmd);
    void runShotStep();

    // Take editor screen: an overlay on the Generate plate for the selected take. Declared
    // after proc: its callbacks reach the processor.
    TakeEditorPanel takeEditor;
    juce::TextButton editTakeButton { "Edit take" };
    // "+4 MORE LIKE TAKE N" under the queue: acts on the selected take (moreLikeKey), off with none.
    juce::TextButton moreLikeButton { "+4 MORE LIKE" };
    juce::String moreLikeKey;
    juce::String moreLikeLabel; // selected take's label, kept so resized() can re-fit the button text
    // "CLEAR UNSTARRED" beside it: every unstarred take to Deleted as one batch, one Undo.
    juce::TextButton clearUnstarredButton { "CLEAR UNSTARRED" };
    bool openTakeEditor();
    void closeTakeEditor (bool restore);

    juce::Label titleLabel, versionLabel, syncLabel, urlCaption, keyCaption;
    juce::Label modeCaption, serverLabel;
    juce::ComboBox modeBox; // UI-only for now (not automatable), wired to jobs in R4
    OfflineOverlay overlay;
    int vblankTick = 0;
    int headerAnimFrame = 0; // header bar animation phase, 20 fps from the VBlank tick
    // Opaque child over the animated beat-grid bars, so the 20 fps tick repaints only it
    // and never runs the editor's full paint().
    struct HeaderBars : juce::Component
    {
        explicit HeaderBars (HotStepLinkEditor& e) : ed (e) { setOpaque (true); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override { g.setOrigin (-getPosition()); ed.paintHeaderBars (g); }
        HotStepLinkEditor& ed;
    };
    HeaderBars headerBars { *this };
    void paintHeaderBars (juce::Graphics& g);
    // Header animation switch; HSLINK_NO_HEADER_ANIM=1 turns it off (for CPU A/B).
    const bool headerAnimOn = juce::SystemStats::getEnvironmentVariable ("HSLINK_NO_HEADER_ANIM", {}).isEmpty();
    juce::String lastServerText;
    PillState pillState = PillState::offline;
public:
    PillState getPillStateForTest() const { return pillState; }
    juce::String getServerTextForTest() const { return lastServerText; }
private:
    bool lastOnline = true, lastBase = true, lastNonTurbo = true, gatingInit = false;
    juce::Label backendCaption, durationCaption, takesCaption, variationCaption, guidanceCaption, stepsCaption, aceModelCaption, bpmCaption;
    juce::ComboBox aceModelBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> aceModelAtt;
    juce::String lastModelGateMode;
    void applyAceModelGating (bool baseOk);
    juce::TextEditor urlEditor, keyEditor;
    juce::ComboBox backendBox;
    juce::Slider durationSlider, takesSlider, variationKnob, guidanceKnob, stepsKnob;
    int generateCount = 0; // generates sent from this editor: n of SeedSpread::spread
    juce::Slider manualBpm; // standalone only, app-owned
    // Seed + key: app settings in plugin state (not automatable).
    juce::Label seedCaption, songKeyCaption, peakModeCaption;
    juce::TextEditor seedEditor;
    juce::ToggleButton seedRandomButton { "Random" };
    juce::TextButton diceButton { "Dice" };
    juce::ComboBox songKeyBox;
    juce::ComboBox peakModeBox; // PeakGuard::Mode + 1 (KNOWN_ISSUES row 15)
    juce::Random diceRng; // message thread only
    void commitSeed();
    // The field holds a refused text: it keeps the text, shows the error outline and
    // startJob sends nothing. Not saved; a reopened editor shows the seed in effect.
    bool seedRefused = false;
    void setSeedRefused (bool refused);
    // Presets + A/B (header)
    juce::ComboBox presetBox;
    juce::TextButton presetSaveButton { "Save" }, presetDeleteButton { "Del" };
    juce::TextButton slotAButton { "A" }, slotBButton { "B" }, copyAbButton { "A=B" };
    void refreshPresetList (const juce::String& select);
    void refreshSlotButtons();
    void switchSlot (int slot);
    void refreshSeedUi();
    void refreshTextState(); // reload controls after a host state restore
    juce::uint32 lastStateGen = 0;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> backendAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> durationAtt, takesAtt, variationAtt, guidanceAtt, stepsAtt;

    RoleCardRow roleCards;

    // Collection panel (Setup plate). The list is picked up by polling the
    // processor's generation counter in the VBlank tick; no disk IO happens here.
    CollectionPanel collectionPanel;
    juce::uint32 lastCollectionGen = 0;
    void refreshCollectionPanel();
    void confirmRemoveFromCollection (const CollectionItem& item);

    // Planned features (mock-up B), painted greyed, no behaviour yet.
    juce::Rectangle<int> soonPresets;
    void paintSoon (juce::Graphics&, juce::Rectangle<int>, const juce::String& title, const juce::String& hint) const;

    unsigned int lastCounter = 0xffffffffu;
    bool drawnOnce = false;

    juce::TooltipWindow tooltipWindow { this, 500 };
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HotStepLinkEditor)
};
