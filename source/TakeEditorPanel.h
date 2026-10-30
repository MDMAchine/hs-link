// HOT-STEP LINK -- TakeEditorPanel.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "PluginProcessor.h"
#include "TakePeaks.h"
#include "WarpMarkers.h"

// Pure helpers of the take editor (no GUI): pixel to frame mapping, the crop clamp,
// the handle pick rule and the result text. Tests call them directly.
namespace TakeEditorMath
{
    struct View
    {
        juce::int64 firstFrame = 0;   // the frame at x = 0
        double framesPerPixel = 1.0;
        int widthPx = 0;
        juce::int64 length = 0;       // frames of the take
    };
    // The whole take across widthPx (slice 1 has no zoom).
    View wholeTake (juce::int64 length, int widthPx);
    // clamp (firstFrame + llround (x * framesPerPixel), 0, length).
    juce::int64 frameAtX (const View& v, double x);
    double xOfFrame (const View& v, juce::int64 frame);

    // The shortest crop: max (2, round (0.020 * rate)) frames.
    juce::int64 minCropLength (double sampleRate);
    // Puts start and end in order inside 0..length with at least minLen between them.
    // movedEnd says which of the two gives way: false = the start was moved and is
    // limited to 0 .. end - minLen, true = the end is limited to start + minLen .. length.
    // A take shorter than minLen gives 0 and length.
    void clampCrop (juce::int64& start, juce::int64& end, juce::int64 length, juce::int64 minLen, bool movedEnd);
    // Handle frames to stored points and back: a start of 0 and an end of length are
    // stored as -1, so "no crop" stays an empty EditData. The loop points of base are kept.
    TakeEdit::EditData toEditData (juce::int64 start, juce::int64 end, juce::int64 length, const TakeEdit::EditData& base);
    void fromEditData (const TakeEdit::EditData& edit, juce::int64 length, juce::int64& start, juce::int64& end);

    // Which crop handle a pointer at x takes, from the x of the two lines. A handle is
    // taken within kGrabPx either side of its line. Both in reach: the nearer line; on
    // a tie the pointer left of the middle of the pair takes the start, else the end.
    enum Handle { handleNone = 0, handleStart = 1, handleEnd = 2 };
    constexpr double kGrabPx = 6.0;
    constexpr int kTabPx = 14; // grip tab: start at the top of the waveform, end at the bottom
    Handle pickHandle (double xStart, double xEnd, double x);

    // Loop flags (slice 2). The same frames, mapping and clamp as the crop handles
    // (clampCrop with minCropLength); -1 and -1 = no loop. loopFromEditData gives -1, -1
    // for an edit without a loop, else the two points inside 0..length.
    void loopFromEditData (const TakeEdit::EditData& edit, juce::int64 length, juce::int64& start, juce::int64& end);
    // e with these loop points; anything that is not start >= 0 and end > start stores -1, -1.
    TakeEdit::EditData withLoop (TakeEdit::EditData e, juce::int64 start, juce::int64 end);
    // The loop a click at frame in the empty loop lane makes: from the click on, one
    // eighth of the take long (at least minLen), moved back when it would pass the end.
    void loopAtClick (juce::int64 frame, juce::int64 length, juce::int64 minLen, juce::int64& start, juce::int64& end);
    constexpr int kDefaultRepeats = 4, kMinRepeats = 1, kMaxRepeats = 64;
    // What a mouse press in the loop lane does (o135). popup is ModifierKeys::isPopupMenu
    // of the press: the right button, and on macOS Ctrl + left button as well. left, right
    // and middle are the buttons down, dragOn says a drag was still on at the press.
    // laneClear: a context click with no drag on (not with the middle button, not with
    // left and right together). laneTake: the left button alone and not a context click
    // (a flag is taken or a loop made). laneNothing: everything else; a context click
    // never takes a flag, and one that ends a drag does not clear.
    enum LanePress { laneNothing = 0, laneClear, laneTake };
    LanePress lanePress (bool popup, bool left, bool right, bool middle, bool dragOn);

    // Beat grid of the take (slice 3): beats at downbeatSec + k * 60 / bpm, in frames
    // llround (downbeatSec * rate + k * rate * 60 / bpm), the numbers of
    // TakeEdit::snapToGrid. measured = the take's tempo state is measured and its result
    // valid (the test of the processor before it snaps).
    struct Grid
    {
        double bpm = 0.0, downbeatSec = 0.0;
        bool measured = false;
        // The tempo is not the take's measurement: it comes from the file name or the
        // host (TempoResolve), downbeatSec 0. The grid is usable for the ruler and for
        // dragged points; the render is never asked to snap on it (its grid is the take's
        // own measurement), so the points are used as the drag left them.
        bool fallback = false;
    };
    // measured or fallback, TakeEdit::gridUsable (bpm, downbeatSec) and a positive finite rate.
    bool gridUsable (const Grid& grid, double sampleRate);
    // The frame of beat k. Only for a usable grid.
    juce::int64 beatFrame (const Grid& grid, double sampleRate, double k);
    // A dragged crop point on the grid. frame at or outside a file end (<= 0, >= length)
    // stays: the file ends are always reached. Else the nearest beat; when that beat is
    // closer than minLen to other (the crop point that is not dragged), the next beat
    // away from it (earlier for a start, later for an end, movedEnd says which); a beat
    // outside the file gives the file end on that side. A grid that is not usable gives
    // frame back. The caller clamps after it (clampCrop), which then changes nothing
    // unless no beat was left between other and the file end.
    juce::int64 snapCropPoint (juce::int64 frame, juce::int64 other, bool movedEnd, const Grid& grid, double sampleRate,
                               juce::int64 length, juce::int64 minLen);
    // A loop on the grid, the way the render snaps one: the start on a beat inside the
    // file, the length a whole number of beats (at least one), the end inside the file.
    // movedEnd false: the start was dragged, the end is the fixed one (it goes to its
    // nearest beat, the start to its own, at least one beat before). true: the end was
    // dragged. A loop at the file end is cut down to the beats that fit; when not one
    // whole beat fits there the start moves back. Returns false and changes nothing when
    // the grid is not usable or no whole beat fits in the file at all.
    bool snapLoop (juce::int64& start, juce::int64& end, bool movedEnd, const Grid& grid, double sampleRate, juce::int64 length);
    // True when a render with snapToGrid on this grid would leave the points where they
    // are: every crop point that is set sits on a beat (cropOnGrid; an unset point is
    // never moved), the loop is one snapLoop gives back unchanged (loopOnGrid). The panel
    // asks for the grid snap of the render only then, so the render never moves a point
    // a second time.
    bool cropOnGrid (const TakeEdit::EditData& edit, const Grid& grid, double sampleRate, juce::int64 length);
    bool loopOnGrid (juce::int64 start, juce::int64 end, const Grid& grid, double sampleRate, juce::int64 length);
    // The beat lines of a view, visible beats only: at most one line per kMinBeatPx
    // pixels, whatever the length of the take. Every beat when beats are that far apart,
    // else every bar (4 beats), else every 2, 4, 8... bars. A bar line carries a number
    // (bar 1 starts at the downbeat) every 1, 2, 4... bars so that labels are at least
    // kMinBarLabelPx apart; beats before the downbeat have no number. out is cleared and
    // filled in rising x (x in pixels from the left of the view); nothing for a grid
    // that is not usable.
    struct BeatLine
    {
        double x = 0.0;
        juce::int64 beat = 0; // 0 = the downbeat
        bool bar = false;     // the first beat of a bar
        int label = 0;        // the bar number to print, 0 = none
    };
    constexpr double kMinBeatPx = 7.0, kMinBarLabelPx = 34.0;
    constexpr int kBeatsPerBar = 4;
    // resolution (manual stepping, phase 2): 0 = the automatic density above; each step
    // up doubles the beats from line to line, each step down halves them, never finer
    // than one line per beat. Clamped to -kMaxResolutionStep..kMaxResolutionStep.
    constexpr int kMaxResolutionStep = 6;
    void beatLines (const View& v, const Grid& grid, double sampleRate, std::vector<BeatLine>& out, int resolution = 0);
    // Loop length jump (phase 2): the loop start stays, the length halves (doubleIt false)
    // or doubles. false and the points unchanged when there is no loop, the half would be
    // shorter than minLen, or the double would run past length.
    bool scaleLoop (juce::int64& start, juce::int64& end, juce::int64 length, juce::int64 minLen, bool doubleIt);
    // The note of the snap row. tone 0 = plain, 1 = amber (grid snap asked for, no usable grid).
    juce::String snapNote (bool snapGrid, bool zeroCrossing, bool gridIsUsable);
    int snapNoteTone (bool snapGrid, bool gridIsUsable);
    inline const char* noTempoNote() { return "No valid tempo for this take. Grid snap is off; points are used as set."; }

    // Fit to bars (slice 4). The tempo of the fit row: what was typed, inside
    // TakeEdit::kMinBpm..kMaxBpm (30 to 300); a value that is not a positive number gives 0.
    double fitTempo (double typed);
    // The readout of the fit row from the pure TakeEdit::fitBars (regionFrames, rate,
    // bpm, kBeatsPerBar): "8 bars, +12.4 ms", "1 bar, -3.0 ms". "-" when no bar count
    // can be given (no region, no rate, a tempo outside the limits).
    juce::String fitReadout (juce::int64 regionFrames, double sampleRate, double bpm);
    // The playhead readout: "3.120 s 2.3.1" (seconds, then bar.beat.sixteenth from the
    // take's grid, kBeatsPerBar beats a bar, all 1-based); seconds only for a grid that
    // is not usable or a frame before the downbeat. Empty for frame < 0 or no rate.
    juce::String playheadText (juce::int64 frame, double sampleRate, const Grid& grid);
    // No host tempo (round 15): the fit row never acts as if the tempo were 120. All pure.
    // fitTempoFromHost: the tempo the field starts from, the value of onHostBpm inside
    // 30 to 300; 0 = not set (no callback value, 0, below 0, NaN, infinite).
    // fitFieldText: the text of the tempo field, "97.00"; empty while the tempo is not
    // set (the field then shows noHostTempoText() as its placeholder).
    // fitBarsText: the bars readout, fitReadout with a set tempo, else noHostTempoText().
    // fitApplyAllowed: Apply crop with Fit to bars on needs a set tempo; with the toggle
    // off Apply sends no fit options and is always allowed.
    inline const char* noHostTempoText() { return "no host tempo"; }
    double fitTempoFromHost (double hostBpm);
    juce::String fitFieldText (double bpm);
    juce::String fitBarsText (juce::int64 regionFrames, double sampleRate, double bpm);
    bool fitApplyAllowed (bool fitOn, double bpm);
    // Stretch to fit: the tempo the made take counts as when its region is taken as the
    // whole bars the render reported, fitBpm * (regionLen - leftover) / regionLen with
    // the leftover in frames. A stretch from it to fitBpm makes the take exactly that
    // many bars long. 0 = no stretch: nothing to go on, a leftover under half a frame,
    // or a tempo outside 30 to 300.
    double stretchFromBpm (juce::int64 regionFrames, double sampleRate, double leftoverMs, double fitBpm);
    // Hard stretch warning of the fit row (o138). A stretch from fromBpm to toBpm is hard
    // when the tempo changes by strictly more than kHardStretch (10 %) either way: the
    // ratio toBpm / fromBpm is over 1.10 or under 0.90; the two limits themselves are not
    // hard. No stretch (a tempo that is not a positive number) is not hard.
    // stretchPercent is the size of that change in whole percent, 25 for 96 to 120.
    constexpr double kHardStretch = 0.10;
    bool hardStretch (double fromBpm, double toBpm);
    int stretchPercent (double fromBpm, double toBpm);
    // What the fit readout carries after its bars when Stretch to fit of that region
    // would be hard: a second line "far off: stretch 25 %" (with the line break in
    // front). Empty when the stretch is not hard or there is none.
    juce::String fitWarning (juce::int64 regionFrames, double sampleRate, double bpm);

    // Seam warning of a loop render, from the report alone. 0 = none, 1 = the loop end
    // and start do not join cleanly, 2 = they join badly. There is a warning only when a
    // crossfade was made (seamFadeSamples > 0); the signed seamCorrelation under
    // kSeamWarn gives 1, under kSeamStrong 2. A value that is not a number gives 0.
    constexpr double kSeamWarn = -0.30, kSeamStrong = -0.60;
    int seamLevel (const TakeEdit::EditResult& result);

    // The result line after an edit job. tone: 0 = made, 1 = nothing made, 2 = failed.
    // A loop render (asked.loopRepeats >= 1) reads "Loop made: <name>, 4 x 1.000 s", and
    // with a seam warning "Loop made: <name>. Ends do not join cleanly (-0.52); try moving
    // a flag." (level 1) or "... Ends join badly, the level dips (-0.75); try moving a
    // flag." (level 2).
    juce::String resultText (const HotStepLinkProcessor::TakeEditReport& report, const TakeEdit::EditOptions& asked);
    int resultTone (const HotStepLinkProcessor::TakeEditReport& report);
    // As resultTone, and for a loop that was made with a seam warning: 4 = level 1
    // (amber), 5 = level 2 (the error colour).
    int resultToneFor (const HotStepLinkProcessor::TakeEditReport& report, const TakeEdit::EditOptions& asked);
    // A take name of more than 24 characters as its first 10, "..." and its last 10.
    juce::String shortName (const juce::String& name);
    // "0.500 s", and "1:02.500" from one minute on.
    juce::String timeText (juce::int64 frames, double sampleRate);
    // The readout row under the waveform.
    juce::String cropReadout (juce::int64 start, juce::int64 end, juce::int64 length, double sampleRate);
    // The readout row: cropReadout as it is while no loop is set, else the short form
    // "Crop 1.000 s to 2.000 s (1.000 s)   Loop 0.500 s to 1.500 s (1.000 s)".
    juce::String readout (juce::int64 cropStart, juce::int64 cropEnd, juce::int64 loopStart, juce::int64 loopEnd, juce::int64 length, double sampleRate);

    // Test hook (teeth), 0 in the plugin. One deliberate fault of the take editor at a
    // time, so the takeeditor test group can be shown failing: mutPixelOff = frameAtX
    // reads one pixel to the right; mutClampPass = a moved start may pass the end;
    // mutCancelKeeps = Cancel does not store the points of open again; mutNoThrottle =
    // every drag step writes; mutNoUpWrite = the mouse up does not write;
    // mutNoSelfClose = the editor keeps the panel open when its take left the list.
    // Slice 2 (takeloop group): mutLoopNoClamp = a dragged loop flag is not clamped;
    // mutFlagAnywhere = flags and crop handles are picked outside their own area too;
    // mutExportLoose = Export loop skips its guards (no loop, a job waiting) and sends
    // the default Repeats; mutSeamLoose = seamLevel ignores the fade length and takes
    // the two limits themselves (and a value that is not a number) as a warning;
    // mutButtonsOverlap = Export loop is laid out 28 px to the right, over Apply crop.
    // Slice 3 (o134): mutSnapOff = a dragged point is not snapped; mutSnapSendLoose =
    // a request sends snapToGrid as the toggle stands, usable grid or not, points on
    // the grid or not; mutZeroNotSent = Apply crop does not send snapToZeroCrossing;
    // mutBeatsNoThin = beatLines gives every beat and numbers every bar; mutSnapNoEnds =
    // a crop point dragged to a file end is snapped too.
    // o133 (takeloop group, item 8): mutNoButtonCheck = any mouse button takes and
    // drags a flag or a crop handle, as before o133; mutOldAnchor = a loop made by a
    // drag is fixed at the loop start that loopAtClick left, not at the click frame.
    // o135 (takeloop group, items 9 and 10): mutStrictRight = lanePress as before o135
    // (only the right button alone clears, Ctrl + left on macOS makes a loop, a right
    // press that ends a drag clears); mutClearMidDrag = lanePress ignores dragOn;
    // mutOpenNoEndDrag = open() does not end a drag of the take shown before.
    // o135 round B (takesnap group, item 8): mutGridStale = setLiveGrid does nothing,
    // so the panel keeps the grid it was opened with.
    // o136 (takefit group): mutFitReadoutLoose = the bars readout is shown with the
    // toggle off too, counts the whole take instead of the crop, has no "+", no
    // singular and no "-"; mutFitTempoLoose = the tempo field takes the typed number
    // as it is (no limits, no comma, no revert) and does not count as typed;
    // mutFitNoHost = open() does not ask the host tempo; mutFitStretchLoose = Stretch
    // to fit is lit whenever a take is open; mutFitSendAlways = Apply crop and Export
    // loop send the fit options whatever the toggle says; mutFitStretchWrong = the
    // stretch is asked with from and to swapped and always reads as started;
    // o138: mutFitWarnWrong = hardStretch says the opposite, the warning is one long
    // line and the fit readout is shown with the toggle off too.
    // mutFitRowTight = Stretch to fit is laid out 100 px to the left, over a readout
    // that is 82 px narrower.
    enum Mutant { mutNone = 0, mutPixelOff, mutClampPass, mutCancelKeeps, mutNoThrottle, mutNoUpWrite, mutNoSelfClose,
                  mutLoopNoClamp, mutFlagAnywhere, mutExportLoose, mutSeamLoose, mutButtonsOverlap,
                  mutNoButtonCheck, mutOldAnchor,
                  mutSnapOff, mutSnapSendLoose, mutZeroNotSent, mutBeatsNoThin, mutSnapNoEnds,
                  mutStrictRight, mutClearMidDrag, mutOpenNoEndDrag, mutGridStale,
                  mutFitReadoutLoose, mutFitTempoLoose, mutFitNoHost, mutFitStretchLoose, mutFitSendAlways,
                  mutFitStretchWrong, mutFitRowTight, mutFitWarnWrong };
    extern std::atomic<int> testMutant;
}

// The take editor screen: an opaque panel over the Generate plate. Slice 1: waveform
// and the two crop handles. Slice 2: two loop flags in the loop lane, the Repeats box
// and Export loop. Slice 3: the Snap to grid and Zero crossing toggles with their note
// in the snap row, and beats on the ruler. Slice 4: the fit row (Fit to bars, tempo
// field, bars readout, Stretch to fit). It holds no processor: the editor wires the callbacks and drives
// tick() from its VBlank tick (no timer of its own).
class TakeEditorPanel : public juce::Component,
                        public juce::TooltipClient
{
public:
    struct Take
    {
        juce::File temp;          // the take's temp file: the key of every call
        juce::String name;        // shown in the title only; a report is matched by its ticket, never by name
        juce::String tempoText;   // the cell's tempo tag text, may be empty
        TakeEdit::EditData edit;  // the points the take has now
        TakeEditorMath::Grid grid; // the take's measured tempo and downbeat; measured false = no grid
    };

    TakeEditorPanel();
    ~TakeEditorPanel() override;

    // Starts the peak build of take.temp on the panel's own pool thread and shows the
    // take. The caller makes the panel visible. A drag still on for the take shown
    // before is ended first, with its write to that take.
    void open (const Take& take);
    // restore: the points the take had at open are stored again first. Ends with onClosed.
    void close (bool restore);
    bool isOpen() const { return opened; }
    const juce::File& getTemp() const { return take.temp; }
    // Once per VBlank from the editor: picks up the peaks, flushes a throttled point
    // write, polls the edit report of the Apply ticket while an Apply is running and
    // gives up on it after kApplyTimeoutMs.
    void tick();
    // The take's tempo as it is now. The editor calls it from its refresh (its VBlank
    // tick) while the panel is open: a tempo measured or changed after open() reaches
    // the snap, the note, the ruler and the info line, so the panel draws the grid the
    // render uses. Nothing happens when nothing changed. The points are not moved, and
    // a drag that is on goes on: its next step snaps on the new grid.
    void setLiveGrid (const TakeEditorMath::Grid& grid, const juce::String& tempoText);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override; // warp lane: resets one marker
    juce::String getTooltip() override; // the loop lane says how it is used

    // setTakeEdit, requestTakeEdit and getTakeEditReport of the processor. onApply
    // returns the job's ticket (0 = not started, whyNot says why). onReport fills the
    // report of that ticket and returns true once the job has finished.
    std::function<bool (const juce::File&, const TakeEdit::EditData&)> onSetEdit;
    std::function<juce::uint64 (const juce::File&, const TakeEdit::EditData&, const TakeEdit::EditOptions&, juce::String& whyNot)> onApply;
    std::function<bool (juce::uint64 ticket, HotStepLinkProcessor::TakeEditReport&)> onReport;
    // getTakeEditState of the processor. The 30 s of kApplyTimeoutMs start when the job
    // is first seen running, not at the click: while it is queued behind another take
    // job the result line reads queuedText() and nothing times out. unknown without a
    // report = the job is gone (dropped, or the plugin is closing): droppedText(), Apply
    // on again. Not set = every job counts as running from the click.
    std::function<HotStepLinkProcessor::TakeEditState (juce::uint64 ticket)> onJobState;
    std::function<void()> onClosed;
    // Slice 4. onHostBpm: the tempo the tempo field starts from (the host tempo; the
    // manual tempo in the standalone); asked at open() until a tempo was typed. Not set
    // or not a usable number (0 = the host gives no tempo) = the tempo is not set: the
    // field is empty and shows "no host tempo", the bars readout says the same, and
    // Apply crop with Fit to bars on is off until a valid tempo is typed. onStretch: requestTakeStretch of the processor for
    // the listed take with this name (the take an Apply just made); false = not started.
    std::function<double()> onHostBpm;
    std::function<bool (const juce::String& takeName, double fromBpm, double toBpm)> onStretch;
    // Audition (Play, Stop, Loop in the zoom row): play, stop, setLoop and getState of
    // the processor's EditorAudition. Play sends the loop region in frames of the file
    // (0 and length while no loop is set). onAuditionState: 0 idle, 1 loading, 2 playing;
    // the panel reads it in tick(), so a clip that ends by itself puts Play back to idle.
    // The audition ends at Stop, at close() and when another take is opened.
    std::function<void (const juce::File&, juce::int64 start, juce::int64 end, juce::int64 length, bool loop)> onAuditionPlay;
    // Round 2b: called in Play before onAuditionPlay with the warp markers (frames of
    // the file) when any marker has moved, else with none (the plain take).
    std::function<void (const std::vector<WarpMarkers::Marker>&)> onAuditionWarp;
    std::function<void()> onAuditionStop;
    std::function<void (bool)> onAuditionLoop;
    std::function<int()> onAuditionState;
    // The playhead: getPlayFraction of the EditorAudition (0..1 of the take, -1 = none),
    // read in tick() while the audition plays. Not set = no playhead.
    std::function<double()> onAuditionPosition;

    static constexpr int kWriteEveryMs = 100; // point writes while a handle is dragged
    // An Apply with no report for its ticket after this long is given up: Apply is
    // enabled again and the result line says so. The ticket is dropped, so a report
    // that comes later is never shown.
    static constexpr int kApplyTimeoutMs = 30000;
    static const char* applyTimeoutText() { return "Edit did not finish. Try Apply again."; }
    static constexpr int kQueuedNoteAfterMs = 400; // queuedText() is shown after this long in the queue
    static const char* queuedText() { return "Waiting for another job to finish..."; }
    static const char* droppedText() { return "The edit was dropped before it finished. Try again."; }
    // Tests: the clock of the Apply timeout and of the drag write throttle
    // (milliseconds, wraps like juce::Time::getMillisecondCounter, which it is when
    // this is empty).
    std::function<juce::uint32()> clockForTest;
    // Test hooks (teeth), all false in the plugin. testMatchByName: a report is taken
    // only when its sourceName equals the take's name, as before the ticket.
    // testNoTimeout: an Apply waits for ever. testKeepExpiredTicket: the ticket is kept
    // after the timeout and a late report is shown.
    // testClockAtClick: the timeout counts from the click whatever onJobState says, as
    // before the job state.
    static std::atomic<bool> testMatchByName, testNoTimeout, testKeepExpiredTicket, testClockAtClick;

    // Test access (no display): handle frames, a drag given in frames, the peak state.
    juce::int64 getCropStartForTest() const { return cropStart; }
    juce::int64 getCropEndForTest() const { return cropEnd; }
    juce::int64 getLengthForTest() const { return length; }
    bool headerReadyForTest() const { return length > 0; }
    bool peaksReadyForTest() const { return peaks != nullptr && peaks->ok; }
    bool peaksFailedForTest() const { return peakError.isNotEmpty(); }
    juce::Thread::ThreadID peaksThreadForTest() const { return peaks != nullptr ? peaks->buildThread : juce::Thread::ThreadID(); }
    // The same path as the mouse: begin takes the handle, drag moves it to a frame
    // (clamped, throttled write), end is the mouse up (always writes).
    void beginDragForTest (TakeEditorMath::Handle h) { dragging = h; dragLoop = false; grabOffset = 0.0; }
    void dragToFrameForTest (juce::int64 frame) { moveHandleTo (frame); }
    void endDragForTest() { endDrag(); }
    TakeEditorMath::Handle handleAtForTest (juce::Point<int> p) const { return handleAt (p); }
    void clickApplyForTest() { applyCrop(); }
    void clickResetForTest() { resetCrop(); }
    bool applyEnabledForTest() const { return applyButton.isEnabled(); }
    // Slice 2, the loop. Flag frames (-1 = no loop); a flag drag by frame: begin takes
    // the start or end flag (no flag while no loop is set), dragToFrameForTest and
    // endDragForTest are the ones of the crop handles. createLoopForTest is the click
    // in the empty loop lane at a frame followed by the mouse up; clearLoopForTest the
    // right click in the lane. flagAtForTest is the pick of a pointer position.
    juce::int64 getLoopStartForTest() const { return loopStart; }
    juce::int64 getLoopEndForTest() const { return loopEnd; }
    bool hasLoopForTest() const { return hasLoop(); }
    void beginLoopDragForTest (TakeEditorMath::Handle h) { dragging = hasLoop() ? h : TakeEditorMath::handleNone; dragLoop = true; loopJustMade = false; loopAnchor = -1; grabOffset = 0.0; }
    void createLoopForTest (juce::int64 frame) { createLoopAt (frame); endDrag(); }
    void clearLoopForTest() { clearLoop(); }
    TakeEditorMath::Handle flagAtForTest (juce::Point<int> p) const { return flagAt (p); }
    void setRepeatsForTest (int n) { repeatsBox.setValue ((double) n, juce::dontSendNotification); }
    int getRepeatsForTest() const { return repeats(); }
    void clickExportLoopForTest() { exportLoop(); }
    bool exportLoopEnabledForTest() const { return exportButton.isEnabled(); }
    bool resetEnabledForTest() const { return resetButton.isEnabled(); }
    int seamLevelShownForTest() const { return shownSeamLevel; } // of the result line now: 0, 1 or 2
    bool resultIsWarningForTest() const; // the result line is amber
    bool resultFitsForTest() const;      // the text of the result line is not wider than its row
    bool readoutFitsForTest() const;
    // Shows this report as the report of an edit asked with these options: the display
    // path of tick() without a job.
    void showReportForTest (const HotStepLinkProcessor::TakeEditReport& r, const TakeEdit::EditOptions& asked) { askedOptions = asked; showReport (r); }
    const TakeEdit::EditOptions& askedOptionsForTest() const { return askedOptions; }
    bool waitingForTest() const { return waiting; }
    juce::uint64 waitTicketForTest() const { return waitTicket; } // 0 = no Apply is waited for
    juce::String resultTextForTest() const { return resultLabel.getText(); }
    bool resultIsErrorForTest() const; // the result line is in the error colour
    juce::String readoutTextForTest() const { return readoutLabel.getText(); }
    // The playhead (Mock-up B): frame -1 = hidden. set = what tick() does with a position.
    void setPlayheadForTest (juce::int64 frame) { setPlayhead (frame); }
    juce::int64 playheadFrameForTest() const { return playheadFrame; }
    juce::String playheadTextForTest() const { return playheadValue.getText(); }
    // The playhead readout is a right-aligned overlay inside the ruler strip, shown, and its
    // text fits unclipped. boundsOut = the overlay rect. The width override (0 = default) lets
    // a test show the check fails when the box is too small for the text.
    bool playheadFitsForTest (juce::Rectangle<int>* boundsOut = nullptr) const;
    juce::Rectangle<int> rulerAreaForTest() const { return rulerArea; }
    void setPlayheadOverlayWidthForTest (int w) { playheadOverlayW = w; resized(); }
    // Right-hand x limit for ruler ticks and labels: they stop short of the playhead overlay.
    static int rulerClipRight (int rulerRight, int rulerWidth, int overlayW) noexcept
    {
        return rulerRight - juce::jmax (0, juce::jmin (rulerWidth, overlayW)) - 4; // 4 px gap before the overlay
    }
    // Every control and reserved row with its bounds in panel coordinates. hit = it
    // takes the mouse. button = a button or box (at least 48 x 24); the waveform and the
    // loop lane are drag areas with handles of their own.
    // Slice 3, snap. The toggles are per panel (not saved, both off in a new editor;
    // they stay as they are when another take is opened). set = the state without a
    // click; click = what a click does (Button::setToggleState with a notification, the
    // onClick path). The two button references are for real mouse events.
    void setSnapGridForTest (bool on) { snapGridButton.setToggleState (on, juce::dontSendNotification); snapChanged(); }
    bool getSnapGridForTest() const { return snapGridButton.getToggleState(); }
    void setZeroCrossingForTest (bool on) { zeroButton.setToggleState (on, juce::dontSendNotification); snapChanged(); }
    bool getZeroCrossingForTest() const { return zeroButton.getToggleState(); }
    void clickSnapGridForTest() { snapGridButton.setToggleState (! snapGridButton.getToggleState(), juce::sendNotificationSync); }
    void clickZeroCrossingForTest() { zeroButton.setToggleState (! zeroButton.getToggleState(), juce::sendNotificationSync); }
    juce::Button& snapGridButtonForTest() { return snapGridButton; }
    juce::Button& zeroCrossingButtonForTest() { return zeroButton; }
    bool gridUsableForTest() const { return gridIsUsable(); }
    bool snapActiveForTest() const { return snapActive(); } // the toggle is on and the grid usable: drags snap
    const TakeEditorMath::Grid& gridForTest() const { return take.grid; }
    double sampleRateForTest() const { return sampleRate; }
    juce::String noteTextForTest() const { return noteLabel.getText(); }
    int noteToneForTest() const { return noteTone; } // 0 plain, 1 amber
    bool noteIsAmberForTest() const;
    bool noteFitsForTest() const; // the note, wrapped to the width of its label, is not higher than the label
    // The beat lines the ruler paints now; x in panel coordinates. Empty = the ruler shows seconds.
    std::vector<TakeEditorMath::BeatLine> beatLinesForTest() const;
    int getResolutionForTest() const { return gridResolution; }
    void stepResolution (int delta);   // the grid resolution box steps (phase 2), per panel, not saved
    void scaleLoopLength (bool doubleIt); // /2 and x2 (phase 2), GUI and edit state only
    bool rulerShowsBeatsForTest() const { return gridIsUsable() && length > 0; }
    // Slice 4, fit to bars. The toggle and the tempo are per panel (not saved; the
    // toggle is off in a new editor, both stay when another take is opened).
    // setFitBpmForTest is the text typed into the tempo field and the return key.
    void setFitBarsForTest (bool on) { fitButton.setToggleState (on, juce::dontSendNotification); fitChanged(); }
    bool getFitBarsForTest() const { return fitButton.getToggleState(); }
    void setFitBpmForTest (const juce::String& typed) { tempoField.setText (typed, juce::dontSendNotification); tempoTyped(); }
    double getFitBpmForTest() const { return fitBpm; }
    juce::String tempoFieldTextForTest() const { return tempoField.getText(); }
    juce::String fitTextForTest() const { return barsLabel.getText(); }
    bool fitTextFitsForTest() const { return textFits (barsLabel); }
    bool fitIsAmberForTest() const;   // the fit readout is in the warning colour
    // Every line of the fit readout fits between the borders of its label and the
    // lines are not higher than it. widest and lines are given back for the printout.
    bool fitInsideForTest (int& widest, int& lines, int& labelW) const;
    bool stretchEnabledForTest() const { return stretchButton.isEnabled(); }
    void clickStretchForTest() { stretchToFit(); }
    juce::Button& fitButtonForTest() { return fitButton; }
    juce::Button& stretchButtonForTest() { return stretchButton; }
    // Round 2a, warp markers (model and lane, no audio stretch yet). The markers are
    // auto-placed on the bar starts of the take's grid once the length is known.
    const std::vector<WarpMarkers::Marker>& warpMarkersForTest() const { ensureWarp(); return warpMarkers; }
    void dragWarpForTest (int index, juce::int64 dstFrame) { ensureWarp(); WarpMarkers::moveTo (warpMarkers, (std::size_t) index, dstFrame, length); repaint(); }
    void auditionPlayForTest() { auditionPlay(); }
    void resetWarpForTest (int index) { WarpMarkers::resetOne (warpMarkers, (std::size_t) index); repaint(); }
    int warpMarkerAtForTest (juce::Point<int> p) const { return warpMarkerAt (p); }
    juce::Rectangle<int> warpLaneForTest() const { return warpLane; }
    // Round 3: the warp buttons as a click does them.
    void clickWarpToHostForTest() { warpToHost(); }
    void clickResetMarkersForTest() { resetMarkers(); }
    void clickApplyWarpForTest() { applyWarp(); }
    bool warpToHostEnabledForTest() const { return warpHostButton.isEnabled(); }
    bool resetMarkersEnabledForTest() const { return warpResetButton.isEnabled(); }
    bool applyWarpEnabledForTest() const { return applyWarpButton.isEnabled(); }
    void setWarpForTest (bool on) { warpButton.setToggleState (on, juce::dontSendNotification); refreshButtons(); }
    struct Area { juce::String name; juce::Rectangle<int> bounds; bool hit = false; bool button = false; };
    std::vector<Area> areasForTest() const;

private:
    // What the pool job and the panel share. The job holds this block and nothing else.
    struct Shared
    {
        std::atomic<bool> cancel { false };
        std::atomic<int> percent { 0 };
        std::atomic<int> generation { 0 }; // 0 nothing, 1 header, 2 finished (ok or not)
        std::mutex lock;
        TakePeaks::Data header;                       // lock
        std::shared_ptr<const TakePeaks::Data> done;  // lock
    };
    std::shared_ptr<Shared> shared;
    int seenGeneration = 0, seenPercent = -1;
    std::shared_ptr<const TakePeaks::Data> peaks;
    juce::String peakError;
    void cancelBuild();

    Take take;
    bool opened = false;
    TakeEdit::EditData openedEdit;
    juce::int64 length = 0, cropStart = 0, cropEnd = 0;
    juce::int64 loopStart = -1, loopEnd = -1; // -1 and -1 = no loop
    bool hasLoop() const { return loopStart >= 0 && loopEnd > loopStart; }
    double sampleRate = 0.0;
    int channels = 0;

    TakeEditorMath::View view() const;
    TakeEditorMath::Handle handleAt (juce::Point<int> p) const;
    TakeEditorMath::Handle flagAt (juce::Point<int> p) const; // loop flags: the loop lane only
    TakeEditorMath::Handle dragging = TakeEditorMath::handleNone;
    bool dragLoop = false;      // dragging is a loop flag, not a crop handle
    juce::int64 loopAnchor = -1; // the click frame of the loop this mouse down made: its fixed end for the whole drag
    bool loopJustMade = false;  // the mouse down made the loop: the end follows only after 3 px of drag
    void createLoopAt (juce::int64 frame); // leaves the end flag in the drag
    void clearLoop();
    double grabOffset = 0.0; // pointer x minus the line x at mouse down
    bool writePending = false;
    juce::uint32 lastWriteMs = 0;
    void moveHandleTo (juce::int64 frame);
    void pointsMoved(); // texts, and the throttled write of a drag step
    void endDrag();
    void writeEdit();
    TakeEdit::EditData currentEdit() const;
    juce::Rectangle<int> handleStrip (juce::int64 frame) const;

    bool waiting = false;
    juce::uint64 waitTicket = 0;   // the Apply whose report is waited for
    juce::uint32 waitStartMs = 0;  // the click, then the tick the job was first seen running
    bool runSeen = false;          // the job of waitTicket has been seen running: the timeout counts
    juce::String makingText;       // the result line while the job runs
    int shownSeamLevel = 0;
    int repeats() const;
    void startJob (const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options, const juce::String& making);
    void showReport (const HotStepLinkProcessor::TakeEditReport& r);
    void exportLoop();
    bool textFits (const juce::Label&) const;
    juce::uint32 nowMs() const { return clockForTest ? clockForTest() : juce::Time::getMillisecondCounter(); }
    TakeEdit::EditOptions askedOptions;
    void applyCrop();
    void resetCrop();
    // Slice 3
    bool gridIsUsable() const { return TakeEditorMath::gridUsable (take.grid, sampleRate); }
    bool snapActive() const { return snapGridButton.getToggleState() && gridIsUsable(); }
    void snapChanged(); // a toggle changed: the note
    void refreshNote();
    void gridOptions (TakeEdit::EditOptions&, bool onGrid) const;
    int noteTone = 0;
    // Slice 4
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
    double fitBpm = 120.0; // teeth: before round 15
#else
    double fitBpm = 0.0;        // 0 = not set (no host tempo and nothing typed), never 120
#endif
    bool tempoByUser = false;   // a tempo was typed: open() no longer asks the host tempo
    void fitChanged();          // the toggle
    void tempoTyped();          // the tempo field lost its editor
    void stretchToFit();
    juce::String stretchName;   // the take the last Apply made with a bar count; empty = nothing to stretch
    double stretchFrom = 0.0, stretchTo = 0.0;
    int stretchBars = 0;
    int gridResolution = 0; // manual grid resolution step, 0 = automatic
    mutable std::vector<TakeEditorMath::BeatLine> beatCache; // reused by paint: no growth after the first frame
    void setResult (const juce::String& text, int tone);
    bool resultHeld = false; // a result of this take is shown; the "move a handle" hint does not replace it
    void refreshTexts();
    void refreshButtons();
    // Audition
    void auditionPlay();
    void auditionStop();
    void refreshAudition(); // the Play and Stop buttons from onAuditionState
    int auditionShown = -1;
    int playheadOverlayW = 0;       // test override of the overlay width, 0 = kPlayheadOverlayW
    juce::int64 playheadFrame = -1; // frame of the file the playhead is drawn at, -1 = hidden
    void setPlayhead (juce::int64 frame);
    // Warp markers (round 2a). mutable: built lazily from paint and the test reads.
    mutable std::vector<WarpMarkers::Marker> warpMarkers;
    mutable bool warpBuilt = false;
    int warpDrag = -1;              // marker index in the drag, -1 = none
    void ensureWarp() const;
    int warpMarkerAt (juce::Point<int> p) const;
    void paintWarp (juce::Graphics&) const;
    // Round 3: the warp buttons. warpTagBpm: the tempo Warp to host warped to (the acid
    // tempo of Apply warp); 0 after a hand move or a reset (then the markers tell it).
    double warpTagBpm = 0.0;
    double warpTargetBpm() const;   // host tempo, else the Fit to bars tempo; 0 = none
    void warpToHost();
    void resetMarkers();
    void applyWarp();
    int playheadX() const;          // x in panel coordinates, -1 when hidden or out of view

    // One source for bounds: set in resized(), used by paint and the mouse code.
    juce::Rectangle<int> rulerArea, warpLane, loopLane, waveArea, scrollArea, snapRow, fitRow, zoomRow;
    juce::Image waveImage;
    bool waveDirty = true;
    void renderWave();
    void paintRuler (juce::Graphics&) const;

    juce::Label titleLabel, infoLabel, readoutLabel, resultLabel;
    // The tempo field: a label that, while its text is empty, shows a placeholder (when
    // idle and in its text editor).
    struct TempoLabel : juce::Label
    {
        juce::String placeholder;
        void paint (juce::Graphics&) override;
        void editorShown (juce::TextEditor*) override;
    };
    juce::Label repeatsLabel, noteLabel, barsLabel, playheadValue;
    TempoLabel tempoField;
    juce::ToggleButton snapGridButton { "Snap to grid" }, zeroButton { "Zero crossing" }, fitButton { "Fit to bars" },
                       loopButton { "Loop" };
    juce::TextButton playButton { "Play" }, stopButton { "Stop" }, loopHalfButton { "/2" }, loopDoubleButton { "x2" };
    juce::Slider repeatsBox;
    juce::Slider gridResBox; // manual grid resolution (phase 2): -6..6, 0 = automatic
    juce::TextButton closeButton { "Close" }, resetButton { "Reset" }, cancelButton { "Cancel" }, applyButton { "Apply crop" },
                     exportButton { "Export loop" }, stretchButton { "Stretch to fit" };
    juce::ToggleButton warpButton { "Warp" };
    juce::TextButton warpHostButton { "Warp to host" }, warpResetButton { "Reset markers" }, applyWarpButton { "Apply warp" };
    juce::ThreadPool pool { 1 }; // peak builds only; declared last, stopped first

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TakeEditorPanel)
};
