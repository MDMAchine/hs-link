// HOT-STEP LINK -- TakeEditorFlowTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// The take editor panel's Apply flow with callback fakes (no processor, no window).
// Checks the EditOptions sent by Apply warp and Apply crop with Fit to bars, that a
// success report shows the made take, that a rejected report or a whyNot shows the
// error and leaves the take unchanged, and that no host tempo (onHostBpm 0) gives an
// empty tempo field with Apply blocked.
// Writes only under <cwd>/takeflow_scratch and removes it at the end.
#include "PluginProcessor.h"
#include "TakeEditorPanel.h"
#include "PluginEditor.h"
#include "TakeEdit.h"
#include "WarpMarkers.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0, passes = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? passes : failures)++;
}

bool writeWav (const juce::File& f, double sr, int frames)
{
    f.deleteFile();
    f.getParentDirectory().createDirectory();
    juce::AudioBuffer<float> b (2, frames);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < 2; ++c)
            b.setSample (c, i, (float) (0.3 * std::sin (6.283185307179586 * 220.0 * (double) i / sr)));
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (os == nullptr)
        return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
    if (w == nullptr)
        return false;
    os.release();
    return w->writeFromAudioSampleBuffer (b, 0, frames);
}

struct Fakes
{
    int applyCalls = 0, setEditCalls = 0;
    TakeEdit::EditOptions sent;
    juce::uint64 nextTicket = 7;
    juce::String why;
    bool reportReady = false;
    HotStepLinkProcessor::TakeEditReport report;
    double hostBpm = 100.0;

    void wire (TakeEditorPanel& p)
    {
        p.onSetEdit = [this] (const juce::File&, const TakeEdit::EditData&) { ++setEditCalls; return true; };
        p.onApply = [this] (const juce::File&, const TakeEdit::EditData&, const TakeEdit::EditOptions& o, juce::String& w)
        {
            ++applyCalls;
            sent = o;
            w = why;
            return nextTicket;
        };
        p.onReport = [this] (juce::uint64 t, HotStepLinkProcessor::TakeEditReport& r)
        {
            if (! reportReady)
                return false;
            r = report;
            r.ticket = t;
            return true;
        };
        p.onJobState = [] (juce::uint64) { return HotStepLinkProcessor::TakeEditState::running; };
        p.onHostBpm = [this] { return hostBpm; };
        p.onStretch = [] (const juce::String&, double, double) { return true; };
    }
};

bool openAndWait (TakeEditorPanel& p, const juce::File& f, double bpm)
{
    TakeEditorPanel::Take take;
    take.temp = f;
    take.name = "take one";
    take.grid.bpm = bpm;
    take.grid.downbeatSec = 0.0;
    take.grid.measured = bpm > 0.0;
    p.open (take);
    for (int i = 0; i < 500 && ! p.peaksReadyForTest() && ! p.peaksFailedForTest(); ++i)
    {
        juce::Thread::sleep (10);
        p.tick();
    }
    return p.peaksReadyForTest();
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    {
        // Ruler clip: a label near the right edge must not run under the playhead overlay.
        const int rulerX = 10, rulerW = 600, rulerR = rulerX + rulerW, overlayW = 126;
        const int lim = TakeEditorPanel::rulerClipRight (rulerR, rulerW, overlayW);
        const int x = rulerR - overlayW - 20; // a tick 20 px left of the overlay, its 44 px label reaches past it
        const bool drawn = x + 3 + 44 <= lim;
        std::printf ("ruler clip: right=%d overlayL=%d limit=%d labelX=%d labelEnd=%d drawn=%d%c",
                     rulerR, rulerR - overlayW, lim, x + 3, x + 3 + 44, (int) drawn, 10);
        check (lim <= rulerR - overlayW, "ruler clip: limit at or left of the overlay");
        check (! drawn, "ruler clip: label overlapping the overlay is skipped");
        check (TakeEditorPanel::rulerClipRight (rulerR, 80, overlayW) >= rulerX, "ruler clip: overlay wider than ruler keeps limit >= ruler left");
    }
    {
        using CS = SourceWaveView::CaptureState;
        const juce::Colour dim (0xff7c859d);
        const auto rec = SourceWaveView::headerStyleFor (CS::recording);
        const auto idl = SourceWaveView::headerStyleFor (CS::idle);
        const auto cap = SourceWaveView::headerStyleFor (CS::captured);
        std::printf ("header rec r=%.3f g=%.3f dot=%d | idle %08x dot=%d | captured %08x dot=%d\n",
                     rec.text.getFloatRed(), rec.text.getFloatGreen(), (int) rec.dot,
                     (unsigned) idl.text.getARGB(), (int) idl.dot, (unsigned) cap.text.getARGB(), (int) cap.dot);
        check (rec.dot && rec.text.getFloatRed() > 0.7f && rec.text.getFloatGreen() < 0.4f, "header: recording shows red dot and red text");
        check (! idl.dot && idl.text == dim, "header: idle unchanged (kDim, no dot)");
        check (! cap.dot && cap.text != dim, "header: captured no dot, not kDim");
    }
    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile ("takeflow_scratch");
    root.deleteRecursively();
    const double sr = 48000.0;
    const auto wavFile = root.getChildFile ("take.wav");
    check (writeWav (wavFile, sr, 4 * 48000), "setup: 4 s stereo WAV written");

    // A: Apply warp sends the warp markers, warpBpm and a plain (non loop) render.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 120.0), "A: peaks ready");
        panel.setWarpForTest (true);
        const auto before = panel.warpMarkersForTest();
        std::printf ("  A: markers %d\n", (int) before.size());
        check (before.size() >= 3, "A: auto-placed markers on the 120 BPM grid");
        const juce::int64 movedDst = before.size() >= 3 ? before[1].dstFrame + 2400 : 0;
        if (before.size() >= 3)
            panel.dragWarpForTest (1, movedDst);
        // Pull the last marker in by a quarter beat so the marker span (and so the implied
        // tempo) really changes: a middle-only drag leaves the result tempo at 120.
        const juce::int64 lastDst = before.size() >= 3 ? before.back().dstFrame - 6000 : 0;
        if (before.size() >= 3)
            panel.dragWarpForTest ((int) before.size() - 1, lastDst);
        const auto markers = panel.warpMarkersForTest();
        const double expectBpm = before.size() >= 3
            ? 120.0 * (double) (before.back().srcFrame - before.front().srcFrame) / (double) (lastDst - before.front().dstFrame)
            : 0.0;
        std::printf ("  A: last src %lld dst %lld\n", (long long) (markers.empty() ? -1 : markers.back().srcFrame), (long long) (markers.empty() ? -1 : markers.back().dstFrame));
        check (markers.size() >= 3 && markers.back().dstFrame == lastDst, "A: last marker dragged in");
        check (std::abs (expectBpm - 120.0) > 1.0, "A: dragged markers imply a non 120 tempo");
        panel.clickApplyWarpForTest();
        std::printf ("  A: applyCalls %d warp %d loopRepeats %d warpBpm %.4f expect %.4f fitBars %d dst1 %lld expect %lld\n",
                     fk.applyCalls, (int) fk.sent.warp.size(), fk.sent.loopRepeats, fk.sent.warpBpm, expectBpm,
                     (int) fk.sent.fitBars, fk.sent.warp.size() > 1 ? (long long) fk.sent.warp[1].dstFrame : -1LL, (long long) movedDst);
        check (fk.applyCalls == 1, "A: one onApply call");
        check (fk.sent.warp.size() == markers.size() && ! fk.sent.warp.empty(), "A: warp markers sent (warp on)");
        check (fk.sent.warp.size() > 1 && fk.sent.warp[1].dstFrame == movedDst, "A: moved marker dst sent as dragged");
        check (fk.sent.loopRepeats == 0, "A: loopRepeats 0");
        check (fk.sent.warpBpm > 0.0 && std::abs (fk.sent.warpBpm - expectBpm) < 0.01, "A: warpBpm is the dragged-marker tempo (not 120)");
        check (! fk.sent.fitBars, "A: no fit asked by warp");
        check (panel.waitingForTest() && panel.waitTicketForTest() == 7, "A: waits on ticket 7");
    }

    // A2: a real tempo change. Warp to host 120 -> 90, then Apply warp: warpBpm is the
    // target and the warped span is 120/90 = 1.333x the source span.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.hostBpm = 90.0;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 120.0), "A2: peaks ready");
        panel.setWarpForTest (true);
        panel.clickWarpToHostForTest();
        panel.clickApplyWarpForTest();
        const auto& w = fk.sent.warp;
        const double s = w.size() >= 2 ? (double) (w.back().srcFrame - w.front().srcFrame) : 0.0;
        const double d = w.size() >= 2 ? (double) (w.back().dstFrame - w.front().dstFrame) : 0.0;
        const double ratio = s > 0.0 ? d / s : 0.0;
        std::printf ("  A2: applyCalls %d warp %d warpBpm %.4f target 90.0000 source 120.0000 span ratio %.4f expect 1.3333\n",
                     fk.applyCalls, (int) w.size(), fk.sent.warpBpm, ratio);
        check (fk.applyCalls == 1, "A2: one onApply call");
        check (std::abs (fk.sent.warpBpm - 90.0) < 0.01, "A2: warpBpm is the 90 BPM target, not the 120 source");
        check (std::abs (ratio - 120.0 / 90.0) < 0.02 * (120.0 / 90.0), "A2: warped span is 1.333x the source (within 2%)");
    }

    // B: Apply crop with Fit to bars at host 100 BPM, then a success report.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 0.0), "B: peaks ready");
        std::printf ("  B: tempo field '%s' fitBpm %.3f\n", panel.tempoFieldTextForTest().toRawUTF8(), panel.getFitBpmForTest());
        check (std::abs (panel.getFitBpmForTest() - 100.0) < 1e-9, "B: tempo starts from host 100");
        panel.beginDragForTest (TakeEditorMath::handleEnd);
        panel.dragToFrameForTest (3 * 48000);
        panel.endDragForTest();
        panel.setFitBarsForTest (true);
        check (panel.applyEnabledForTest(), "B: Apply on with fit and a tempo");
        panel.clickApplyForTest();
        std::printf ("  B: applyCalls %d loopRepeats %d fitBars %d fitBpm %.3f beatsPerBar %d warp %d\n",
                     fk.applyCalls, fk.sent.loopRepeats, (int) fk.sent.fitBars, fk.sent.fitBpm, fk.sent.fitBeatsPerBar, (int) fk.sent.warp.size());
        check (fk.applyCalls == 1, "B: one onApply call");
        check (fk.sent.fitBars && std::abs (fk.sent.fitBpm - 100.0) < 1e-9 && fk.sent.fitBeatsPerBar == 4, "B: fitBars on at 100 BPM, 4 beats a bar");
        check (fk.sent.loopRepeats == 0 && fk.sent.warp.empty(), "B: crop only, no warp");
        // 3 s at 100 BPM = 5 beats = 1 bar + 600 ms.
        fk.report.ok = true;
        fk.report.newName = "take one crop";
        fk.report.result.ok = true;
        fk.report.result.sampleRate = sr;
        fk.report.result.outSamples = 3 * 48000;
        fk.report.result.bars = 1;
        fk.report.result.leftoverMs = 600.0;
        fk.reportReady = true;
        for (int i = 0; i < 20 && panel.waitingForTest(); ++i)
            panel.tick();
        const auto text = panel.resultTextForTest();
        std::printf ("  B: result '%s' error %d stretch %d\n", text.toRawUTF8(), (int) panel.resultIsErrorForTest(), (int) panel.stretchEnabledForTest());
        check (! panel.waitingForTest(), "B: wait ended by the report");
        check (text == "Made take one crop, 3.000 s, 1 bar, +600.0 ms", "B: result line shows the made take with its bars");
        check (! panel.resultIsErrorForTest(), "B: not in the error colour");
        check (panel.stretchEnabledForTest(), "B: Stretch to fit on for the made take");
    }

    // C: a rejected report, then a whyNot at start: error shown, take unchanged.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 0.0), "C: peaks ready");
        panel.beginDragForTest (TakeEditorMath::handleEnd);
        panel.dragToFrameForTest (2 * 48000);
        panel.endDragForTest();
        const auto cs = panel.getCropStartForTest(), ce = panel.getCropEndForTest();
        const int writes = fk.setEditCalls;
        fk.report.ok = false;
        fk.report.result.error = "The source take is gone.";
        fk.reportReady = true;
        panel.clickApplyForTest();
        for (int i = 0; i < 20 && panel.waitingForTest(); ++i)
            panel.tick();
        std::printf ("  C1: result '%s' error %d crop %lld..%lld (was %lld..%lld) writes %d->%d stretch %d\n",
                     panel.resultTextForTest().toRawUTF8(), (int) panel.resultIsErrorForTest(),
                     (long long) panel.getCropStartForTest(), (long long) panel.getCropEndForTest(), (long long) cs, (long long) ce,
                     writes, fk.setEditCalls, (int) panel.stretchEnabledForTest());
        check (panel.resultTextForTest() == "The source take is gone.", "C1: rejected report shows its error text");
        check (panel.resultIsErrorForTest(), "C1: in the error colour");
        check (panel.getCropStartForTest() == cs && panel.getCropEndForTest() == ce && ce == 2 * 48000, "C1: crop unchanged");
        check (fk.setEditCalls == writes, "C1: no point write from the failed Apply");
        check (! panel.stretchEnabledForTest(), "C1: no Stretch to fit after a failure");

        fk.nextTicket = 0;
        fk.why = "The take is not final yet.";
        panel.clickApplyForTest();
        std::printf ("  C2: result '%s' error %d waiting %d crop %lld..%lld\n", panel.resultTextForTest().toRawUTF8(),
                     (int) panel.resultIsErrorForTest(), (int) panel.waitingForTest(),
                     (long long) panel.getCropStartForTest(), (long long) panel.getCropEndForTest());
        check (fk.applyCalls == 2, "C2: onApply asked again");
        check (panel.resultTextForTest() == "The take is not final yet." && panel.resultIsErrorForTest(), "C2: whyNot shown as an error");
        check (! panel.waitingForTest() && panel.waitTicketForTest() == 0, "C2: nothing waited for");
        check (panel.getCropStartForTest() == cs && panel.getCropEndForTest() == ce, "C2: crop unchanged");
    }

    // D: no host tempo (onHostBpm 0): empty field, Fit to bars blocks Apply.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.hostBpm = 0.0;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 0.0), "D: peaks ready");
        panel.beginDragForTest (TakeEditorMath::handleEnd);
        panel.dragToFrameForTest (3 * 48000);
        panel.endDragForTest();
        const bool applyNoFit = panel.applyEnabledForTest();
        panel.setFitBarsForTest (true);
        std::printf ("  D: field '%s' fitBpm %.3f apply(no fit) %d apply(fit) %d bars '%s'\n",
                     panel.tempoFieldTextForTest().toRawUTF8(), panel.getFitBpmForTest(), (int) applyNoFit,
                     (int) panel.applyEnabledForTest(), panel.fitTextForTest().toRawUTF8());
        check (panel.tempoFieldTextForTest().isEmpty(), "D: tempo field empty");
        check (panel.getFitBpmForTest() == 0.0, "D: fit tempo not set (never 120)");
        check (applyNoFit, "D: Apply on without Fit to bars");
        check (! panel.applyEnabledForTest(), "D: Apply off with Fit to bars and no tempo");
        panel.clickApplyForTest();
        check (fk.applyCalls == 0, "D: clicking Apply sends nothing");
    }

    // E: backlog GUI fix. Reset markers follows a real mouse drag in the warp lane:
    // on once a marker is moved by hand, off again once the double-click puts it back.
    {
        TakeEditorPanel panel;
        panel.setSize (900, 500);
        Fakes fk;
        fk.wire (panel);
        check (openAndWait (panel, wavFile, 120.0), "E: peaks ready");
        panel.setWarpForTest (true);
        const bool offAtStart = panel.resetMarkersEnabledForTest();
        const auto lane = panel.warpLaneForTest();
        juce::Point<int> from (-1, -1);
        for (int x = lane.getX(); x < lane.getRight() && from.x < 0; ++x)
            if (panel.warpMarkerAtForTest ({ x, lane.getCentreY() }) == 1)
                from = { x, lane.getCentreY() };
        const juce::Point<int> to (from.x + 30, from.y);
        auto mev = [&] (juce::Point<int> pos, juce::Point<int> down, bool dragged, int clicks)
        {
            const auto t = juce::Time::getCurrentTime();
            return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos.toFloat(), juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                                     juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &panel, &panel, t, down.toFloat(), t, clicks, dragged);
        };
        auto& comp = static_cast<juce::Component&> (panel);
        comp.mouseDown (mev (from, from, false, 1));
        comp.mouseDrag (mev (to, from, true, 1));
        comp.mouseUp (mev (to, from, true, 1));
        const int movedAfterDrag = WarpMarkers::movedCount (panel.warpMarkersForTest());
        const bool onAfterDrag = panel.resetMarkersEnabledForTest();
        const bool applyOnAfterDrag = panel.applyWarpEnabledForTest();
        const int at = panel.warpMarkerAtForTest (to);
        comp.mouseDoubleClick (mev (to, to, false, 2));
        const int movedAfterReset = WarpMarkers::movedCount (panel.warpMarkersForTest());
        const bool onAfterReset = panel.resetMarkersEnabledForTest();
        std::printf ("  E: lane marker 1 at x %d; start enabled=%d (want 0); after drag moved=%d enabled=%d applyWarp=%d (want >=1 1 1); marker under drop=%d; after double-click moved=%d enabled=%d (want 0 0)\n",
                     from.x, (int) offAtStart, movedAfterDrag, (int) onAfterDrag, (int) applyOnAfterDrag, at, movedAfterReset, (int) onAfterReset);
        check (from.x >= 0, "E: marker 1 found in the warp lane");
        check (! offAtStart, "E: Reset markers off with no moved marker");
        check (movedAfterDrag >= 1 && onAfterDrag, "E: Reset markers on right after a hand drag");
        check (applyOnAfterDrag, "E: Apply warp on right after a hand drag");
        check (movedAfterReset == 0 && ! onAfterReset, "E: Reset markers off again after the double-click reset");
    }

    root.deleteRecursively();
    std::printf ("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
