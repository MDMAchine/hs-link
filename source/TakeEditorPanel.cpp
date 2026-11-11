// HOT-STEP LINK -- TakeEditorPanel.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#include "TakeEditorPanel.h"
#include <cmath>

namespace
{
    // The HslinkLook set (PluginEditor.cpp).
    const juce::Colour kBg (0xff0b0e16), kPanel (0xff111521), kAccent (0xfff472b6), kText (0xffd6dbe8);
    const juce::Colour kBright (0xfff3f5fa), kDim (0xff7c859d), kField (0xff0a0d16), kEdge (0x38a0aac8);
    const juce::Colour kAmber (0xffffb347), kRed (0xffff6b6b), kGreen (0xff6fd08c);
    constexpr int kPad = 12;
    constexpr int kPlayheadOverlayW = 126; // the playhead readout overlay at the right end of the ruler strip
    constexpr int kAboveWave = 134, kFixedRows = 390; // waveform height = panel height - kFixedRows
    enum Tone { toneMade = 0, toneNothing = 1, toneFailed = 2, toneHint = 3, toneSeam = 4, toneSeamStrong = 5 };
}

//==============================================================================
namespace TakeEditorMath
{
    std::atomic<int> testMutant { mutNone };

    View wholeTake (juce::int64 length, int widthPx)
    {
        View v;
        v.length = juce::jmax ((juce::int64) 0, length);
        v.widthPx = juce::jmax (1, widthPx);
        v.framesPerPixel = v.length > 0 ? (double) v.length / (double) v.widthPx : 1.0;
        return v;
    }

    double maxZoom (juce::int64 length, int widthPx)
    {
        const double whole = length > 0 ? (double) length / (double) juce::jmax (1, widthPx) : 1.0;
        if (testMutant.load (std::memory_order_relaxed) == mutZoomNoCap)
            return juce::jmax (1.0, whole * 4.0);
        return juce::jmax (1.0, whole / kMinFramesPerPixel);
    }

    juce::int64 clampViewStart (juce::int64 length, int widthPx, double zoom, juce::int64 start)
    {
        juce::ignoreUnused (widthPx);
        if (testMutant.load (std::memory_order_relaxed) == mutZoomNoClamp)
            return start;
        const auto visible = (juce::int64) std::llround ((double) juce::jmax ((juce::int64) 0, length) / juce::jmax (1.0, zoom));
        return juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length - visible), start);
    }

    View zoomedView (juce::int64 length, int widthPx, double zoom, juce::int64 start)
    {
        auto v = wholeTake (length, widthPx);
        const double z = juce::jlimit (1.0, maxZoom (length, widthPx), zoom);
        v.framesPerPixel /= z;
        v.firstFrame = clampViewStart (length, widthPx, z, start);
        return v;
    }

    void zoomAround (juce::int64 length, int widthPx, double& zoom, juce::int64& start, double anchorX, double factor)
    {
        const auto before = zoomedView (length, widthPx, zoom, start);
        const double anchorFrame = (double) before.firstFrame + anchorX * before.framesPerPixel;
        zoom = juce::jlimit (1.0, maxZoom (length, widthPx), zoom * factor);
        const auto after = zoomedView (length, widthPx, zoom, 0);
        if (testMutant.load (std::memory_order_relaxed) == mutZoomNoAnchor)
            return; // the start stays: the frame under the pointer moves
        start = clampViewStart (length, widthPx, zoom, (juce::int64) std::llround (anchorFrame - anchorX * after.framesPerPixel));
    }

    juce::int64 frameAtX (const View& v, double x)
    {
        if (testMutant.load (std::memory_order_relaxed) == mutPixelOff)
            x += 1.0;
        return juce::jlimit ((juce::int64) 0, v.length, v.firstFrame + (juce::int64) std::llround (x * v.framesPerPixel));
    }

    double xOfFrame (const View& v, juce::int64 frame)
    {
        return v.framesPerPixel > 0.0 ? (double) (frame - v.firstFrame) / v.framesPerPixel : 0.0;
    }

    bool frameInView (const View& v, juce::int64 frame)
    {
        const double x = xOfFrame (v, frame);
        return x >= -0.5 && x <= (double) v.widthPx + 0.5;
    }

    juce::int64 minCropLength (double sampleRate)
    {
        return juce::jmax ((juce::int64) 2, (juce::int64) std::llround (0.020 * sampleRate));
    }

    void clampCrop (juce::int64& start, juce::int64& end, juce::int64 length, juce::int64 minLen, bool movedEnd)
    {
        if (length <= minLen)
        {
            start = 0;
            end = juce::jmax ((juce::int64) 0, length);
            return;
        }
        if (movedEnd)
        {
            start = juce::jlimit ((juce::int64) 0, length - minLen, start);
            end = juce::jlimit (start + minLen, length, end);
        }
        else
        {
            end = juce::jlimit (minLen, length, end);
            start = juce::jlimit ((juce::int64) 0, testMutant.load (std::memory_order_relaxed) == mutClampPass ? length : end - minLen, start);
        }
    }

    TakeEdit::EditData toEditData (juce::int64 start, juce::int64 end, juce::int64 length, const TakeEdit::EditData& base)
    {
        auto e = base;
        e.cropStart = start <= 0 ? -1 : start;
        e.cropEnd = end >= length ? -1 : end;
        return e;
    }

    void fromEditData (const TakeEdit::EditData& edit, juce::int64 length, juce::int64& start, juce::int64& end)
    {
        start = edit.cropStart < 0 ? 0 : juce::jmin (edit.cropStart, length);
        end = edit.cropEnd < 0 ? length : juce::jmin (edit.cropEnd, length);
        if (end <= start)
        {
            start = 0;
            end = length;
        }
    }

    Handle pickHandle (double xStart, double xEnd, double x)
    {
        const double ds = std::abs (x - xStart), de = std::abs (x - xEnd);
        const bool s = ds <= kGrabPx, e = de <= kGrabPx;
        if (s && e)
        {
            if (ds != de)
                return ds < de ? handleStart : handleEnd;
            return x < 0.5 * (xStart + xEnd) ? handleStart : handleEnd;
        }
        return s ? handleStart : e ? handleEnd : handleNone;
    }

    void loopFromEditData (const TakeEdit::EditData& edit, juce::int64 length, juce::int64& start, juce::int64& end)
    {
        start = end = -1;
        if (! edit.hasLoop() || length <= 0)
            return;
        start = juce::jmin (edit.loopStart, length);
        end = juce::jmin (edit.loopEnd, length);
        if (end <= start)
            start = end = -1;
    }

    TakeEdit::EditData withLoop (TakeEdit::EditData e, juce::int64 start, juce::int64 end)
    {
        const bool ok = start >= 0 && end > start;
        e.loopStart = ok ? start : -1;
        e.loopEnd = ok ? end : -1;
        return e;
    }

    void loopAtClick (juce::int64 frame, juce::int64 length, juce::int64 minLen, juce::int64& start, juce::int64& end)
    {
        const auto len = juce::jmax (minLen, length / 8);
        start = juce::jlimit ((juce::int64) 0, juce::jmax ((juce::int64) 0, length - len), frame);
        end = juce::jmin (length, start + len);
    }

    LanePress lanePress (bool popup, bool left, bool right, bool middle, bool dragOn)
    {
        const int mut = testMutant.load (std::memory_order_relaxed);
        const bool leftAlone = left && ! right && ! middle;
        if (mut == mutStrictRight)
            return right && ! left && ! middle ? laneClear : leftAlone ? laneTake : laneNothing;
        // popup without the right button is Ctrl + left on macOS. Left and right together
        // and anything with the middle button stay "nothing", as on every platform.
        if (popup)
            return (dragOn && mut != mutClearMidDrag) || middle || (left && right) ? laneNothing : laneClear;
        return leftAlone ? laneTake : laneNothing;
    }

    bool gridUsable (const Grid& grid, double sampleRate)
    {
        return (grid.measured || grid.fallback) && TakeEdit::gridUsable (grid.bpm, grid.downbeatSec) && sampleRate > 0.0 && std::isfinite (sampleRate);
    }

    juce::int64 beatFrame (const Grid& grid, double sampleRate, double k)
    {
        // The same terms in the same order as TakeEdit::snapToGrid, so a beat found here
        // is a point that function gives back unchanged.
        const double beat = sampleRate * 60.0 / grid.bpm, origin = grid.downbeatSec * sampleRate;
        return (juce::int64) std::llround (origin + k * beat);
    }

    juce::int64 snapCropPoint (juce::int64 frame, juce::int64 other, bool movedEnd, const Grid& grid, double sampleRate,
                               juce::int64 length, juce::int64 minLen)
    {
        const int mut = testMutant.load (std::memory_order_relaxed);
        if (mut == mutSnapOff || ! gridUsable (grid, sampleRate))
            return frame;
        if ((frame <= 0 || frame >= length) && mut != mutSnapNoEnds)
            return frame; // a file end is a point of its own: it is always reached
        const double beat = sampleRate * 60.0 / grid.bpm, origin = grid.downbeatSec * sampleRate;
        double k = std::round (((double) frame - origin) / beat);
        // The nearest beat would come closer than minLen to the other point: one beat on.
        if (movedEnd && beatFrame (grid, sampleRate, k) < other + minLen)
            k = std::ceil (((double) (other + minLen) - origin) / beat);
        else if (! movedEnd && beatFrame (grid, sampleRate, k) > other - minLen)
            k = std::floor (((double) (other - minLen) - origin) / beat);
        return juce::jlimit ((juce::int64) 0, length, beatFrame (grid, sampleRate, k));
    }

    bool snapLoop (juce::int64& start, juce::int64& end, bool movedEnd, const Grid& grid, double sampleRate, juce::int64 length)
    {
        if (testMutant.load (std::memory_order_relaxed) == mutSnapOff || ! gridUsable (grid, sampleRate) || length <= 0)
            return false;
        const double beat = sampleRate * 60.0 / grid.bpm, origin = grid.downbeatSec * sampleRate;
        auto at = [&grid, sampleRate] (double k) { return beatFrame (grid, sampleRate, k); };
        auto beats = [beat] (double n) { return (juce::int64) std::llround (n * beat); };
        // The first beat inside the file, found as the render finds it.
        double kMin = std::ceil (-origin / beat);
        if (at (kMin) < 0)
            kMin += 1.0;
        double k = 0.0, n = 0.0;
        if (movedEnd)
        {
            k = juce::jmax (kMin, std::round (((double) start - origin) / beat));
            n = std::round ((double) (end - at (k)) / beat);
        }
        else
        {
            const double kEnd = std::round (((double) end - origin) / beat);
            k = juce::jmax (kMin, juce::jmin (std::round (((double) start - origin) / beat), kEnd - 1.0));
            n = kEnd - k;
        }
        n = juce::jmax (1.0, n);
        if (at (k) + beats (n) > length)
        {
            n = std::floor ((double) (length - at (k)) / beat) + 1.0;
            if (n >= 1.0 && at (k) + beats (n) > length)
                n -= 1.0;
            if (n < 1.0)
            {
                // Not one beat fits after this start: the last start that leaves one.
                n = 1.0;
                k = std::floor (((double) length - origin) / beat) - 1.0;
                if (at (k) + beats (1.0) > length)
                    k -= 1.0;
                if (k < kMin)
                    return false;
            }
        }
        start = at (k);
        end = start + beats (n);
        return true;
    }

    bool cropOnGrid (const TakeEdit::EditData& edit, const Grid& grid, double sampleRate, juce::int64 length)
    {
        if (! gridUsable (grid, sampleRate))
            return false;
        auto stays = [&grid, sampleRate, length] (juce::int64 p)
        {
            return p < 0 || juce::jlimit ((juce::int64) 0, length, TakeEdit::snapToGrid (p, sampleRate, grid.bpm, grid.downbeatSec)) == p;
        };
        return stays (edit.cropStart) && stays (edit.cropEnd);
    }

    bool loopOnGrid (juce::int64 start, juce::int64 end, const Grid& grid, double sampleRate, juce::int64 length)
    {
        if (! (start >= 0 && end > start))
            return false;
        auto s = start, e = end;
        return snapLoop (s, e, true, grid, sampleRate, length) && s == start && e == end;
    }

    bool scaleLoop (juce::int64& start, juce::int64& end, juce::int64 length, juce::int64 minLen, bool doubleIt)
    {
        if (! (start >= 0 && end > start && end <= length))
            return false;
        const auto len = end - start;
        const auto newEnd = doubleIt ? start + 2 * len : start + len / 2;
        if (newEnd - start < juce::jmax ((juce::int64) 1, minLen) || newEnd > length)
            return false;
        end = newEnd;
        return true;
    }

    void beatLines (const View& v, const Grid& grid, double sampleRate, std::vector<BeatLine>& out, int resolution)
    {
        out.clear();
        if (! gridUsable (grid, sampleRate) || v.length <= 0 || v.widthPx < 1 || ! (v.framesPerPixel > 0.0))
            return;
        const double beat = sampleRate * 60.0 / grid.bpm, origin = grid.downbeatSec * sampleRate, pxPerBeat = beat / v.framesPerPixel;
        const bool all = testMutant.load (std::memory_order_relaxed) == mutBeatsNoThin;
        double step = 1.0, labelBars = 1.0; // beats from line to line, bars from number to number
        if (! all && pxPerBeat < kMinBeatPx)
            for (step = (double) kBeatsPerBar; step * pxPerBeat < kMinBeatPx && step < 1.0e12; step *= 2.0) {}
        if (! all && resolution != 0)
        {
            step = std::ldexp (step, juce::jlimit (-kMaxResolutionStep, kMaxResolutionStep, resolution));
            step = juce::jmax (1.0, step);
        }
        if (! all)
            while (labelBars * kBeatsPerBar * pxPerBeat < kMinBarLabelPx && labelBars < 1.0e12)
                labelBars *= 2.0;
        // bar.beat labels when one beat is at least kMinTagPx wide; halved while the half
        // is wide enough too (bar.beat.part). The lines follow the labels down.
        double tagStep = 0.0;
        if (! all && pxPerBeat >= kMinTagPx)
        {
            tagStep = 1.0;
            for (int n = 0; n < kMaxTagHalvings && 0.5 * tagStep * pxPerBeat >= (testMutant.load (std::memory_order_relaxed) == mutTagTight ? 0.5 * kMinTagPx : kMinTagPx); ++n)
                tagStep *= 0.5;
            step = juce::jmin (step, tagStep);
        }
        const double first = (double) v.firstFrame;
        const double last = juce::jmin ((double) v.length, first + (double) v.widthPx * v.framesPerPixel);
        const size_t most = all ? (size_t) 100000 : (size_t) juce::jmin ((double) v.widthPx / juce::jmin (kMinBeatPx, step * pxPerBeat), 4000.0) + 4; // by the width, never by the length
        out.reserve (juce::jmin (most, (size_t) 4096));
        for (double k = std::ceil ((first - origin) / beat / step) * step; out.size() < most; k += step)
        {
            const auto f = beatFrame (grid, sampleRate, k);
            if ((double) f > last)
                break;
            if (f < v.firstFrame)
                continue;
            BeatLine b;
            b.x = xOfFrame (v, f);
            b.beat = (juce::int64) k;
            const double barIndex = std::floor (k / (double) kBeatsPerBar);
            b.bar = barIndex * (double) kBeatsPerBar == k;
            if (b.bar && barIndex >= 0.0 && barIndex < 2.0e9 && std::fmod (barIndex, labelBars) == 0.0)
                b.label = (int) barIndex + 1;
            else if (tagStep > 0.0 && ! b.bar && barIndex >= 0.0 && barIndex < 2.0e9 && std::fmod (k, tagStep) == 0.0)
            {
                const double inBar = k - barIndex * (double) kBeatsPerBar, whole = std::floor (inBar);
                b.tagBar = (int) barIndex + 1;
                b.tagBeat = (int) whole + 1;
                b.tagPart = inBar > whole ? (int) std::lround ((inBar - whole) / tagStep) + 1 : 0;
            }
            out.push_back (b);
        }
    }

    juce::String snapNote (bool snapGrid, bool zeroCrossing, bool gridIsUsable)
    {
        // Order when both are on: the beat first (while dragging), then the zero crossing
        // (by the render, crop points only), as in TakeEdit::EditOptions.
        if (snapGrid && ! gridIsUsable)
            return zeroCrossing ? juce::String ("No valid tempo, grid snap is off. Apply crop moves cuts to a zero crossing.")
                                : juce::String (noTempoNote());
        if (snapGrid)
            return zeroCrossing ? "Beats first; Apply crop then moves each cut to a zero crossing (up to 3 ms)."
                                : "Dragged points snap to the beats of the take.";
        return zeroCrossing ? "Apply crop moves each cut to the nearest zero crossing, up to 3 ms away." : "";
    }

    int snapNoteTone (bool snapGrid, bool gridIsUsable)
    {
        return snapGrid && ! gridIsUsable ? 1 : 0;
    }

    int seamLevel (const TakeEdit::EditResult& result)
    {
        if (testMutant.load (std::memory_order_relaxed) == mutSeamLoose)
            return result.seamCorrelation <= kSeamStrong ? 2 : ! (result.seamCorrelation > kSeamWarn) ? 1 : 0;
        if (result.seamFadeSamples <= 0 || std::isnan (result.seamCorrelation))
            return 0; // no crossfade was made: nothing was measured
        return result.seamCorrelation < kSeamStrong ? 2 : result.seamCorrelation < kSeamWarn ? 1 : 0;
    }

    double fitTempo (double typed)
    {
        return std::isfinite (typed) && typed > 0.0 ? juce::jlimit (TakeEdit::kMinBpm, TakeEdit::kMaxBpm, typed) : 0.0;
    }

    juce::String playheadText (juce::int64 frame, double sampleRate, const Grid& grid)
    {
        if (frame < 0 || ! (sampleRate > 0.0))
            return {};
        const double sec = (double) frame / sampleRate;
        juce::String t = juce::String (sec, 3) + " s";
        if (! gridUsable (grid, sampleRate))
            return t;
        const double beats = (sec - grid.downbeatSec) * grid.bpm / 60.0;
        if (! (beats >= 0.0) || ! std::isfinite (beats))
            return t;
        const auto sixteenths = (juce::int64) std::floor (beats * 4.0 + 1.0e-9);
        const auto beat = sixteenths / 4;
        return t + " " + juce::String (beat / kBeatsPerBar + 1) + "." + juce::String (beat % kBeatsPerBar + 1)
                 + "." + juce::String (sixteenths % 4 + 1);
    }

    juce::String fitReadout (juce::int64 regionFrames, double sampleRate, double bpm)
    {
        const auto fit = TakeEdit::fitBars (regionFrames, sampleRate, bpm, kBeatsPerBar);
        if (testMutant.load (std::memory_order_relaxed) == mutFitReadoutLoose)
            return juce::String (fit.bars) + " bars, " + juce::String (fit.leftoverMs, 1) + " ms";
        if (fit.bars <= 0)
            return "-";
        return juce::String (fit.bars) + (fit.bars == 1 ? " bar, " : " bars, ") + (fit.leftoverMs >= 0.0 ? "+" : "") + juce::String (fit.leftoverMs, 1) + " ms";
    }

    double fitTempoFromHost (double hostBpm)
    {
        const double host = fitTempo (hostBpm);
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
        return host > 0.0 ? host : 120.0; // teeth: before round 15
#else
        return host;
#endif
    }

    juce::String fitFieldText (double bpm)
    {
#if ! defined (HSLINK_TEETH_NO_TEMPO_GATE)
        if (! (bpm > 0.0))
            return {};
#endif
        return juce::String (bpm, 2);
    }

    juce::String fitBarsText (juce::int64 regionFrames, double sampleRate, double bpm)
    {
#if ! defined (HSLINK_TEETH_NO_TEMPO_GATE)
        if (! (bpm > 0.0))
            return noHostTempoText();
#endif
        return fitReadout (regionFrames, sampleRate, bpm);
    }

    bool fitApplyAllowed (bool fitOn, double bpm)
    {
#if defined (HSLINK_TEETH_NO_TEMPO_GATE)
        juce::ignoreUnused (fitOn, bpm);
        return true; // teeth: before round 15
#else
        return ! fitOn || bpm > 0.0;
#endif
    }

    double stretchFromBpm (juce::int64 regionFrames, double sampleRate, double leftoverMs, double fitBpm)
    {
        if (regionFrames <= 0 || ! (sampleRate > 0.0) || ! std::isfinite (leftoverMs) || fitTempo (fitBpm) != fitBpm)
            return 0.0;
        const double leftover = leftoverMs * sampleRate / 1000.0;
        if (std::abs (leftover) < 0.5)
            return 0.0; // whole bars already
        const double from = fitBpm * ((double) regionFrames - leftover) / (double) regionFrames;
        return from >= TakeEdit::kMinBpm && from <= TakeEdit::kMaxBpm ? from : 0.0;
    }

    bool hardStretch (double fromBpm, double toBpm)
    {
        if (! (fromBpm > 0.0) || ! (toBpm > 0.0) || ! std::isfinite (fromBpm) || ! std::isfinite (toBpm))
            return false;
        const double ratio = toBpm / fromBpm;
        const bool hard = ratio > 1.0 + kHardStretch || ratio < 1.0 - kHardStretch;
        return testMutant.load (std::memory_order_relaxed) == mutFitWarnWrong ? ! hard : hard;
    }

    int stretchPercent (double fromBpm, double toBpm)
    {
        if (! (fromBpm > 0.0) || ! (toBpm > 0.0) || ! std::isfinite (fromBpm) || ! std::isfinite (toBpm))
            return 0;
        return (int) std::lround (std::abs (toBpm / fromBpm - 1.0) * 100.0);
    }

    juce::String fitWarning (juce::int64 regionFrames, double sampleRate, double bpm)
    {
        const auto fit = TakeEdit::fitBars (regionFrames, sampleRate, bpm, kBeatsPerBar);
        if (fit.bars <= 0)
            return {};
        const double from = stretchFromBpm (regionFrames, sampleRate, fit.leftoverMs, bpm);
        if (! hardStretch (from, bpm))
            return {};
        const auto percent = juce::String (stretchPercent (from, bpm));
        if (testMutant.load (std::memory_order_relaxed) == mutFitWarnWrong)
            return ", far from whole bars (stretch " + percent + " %)";
        return "\nfar off: stretch " + percent + " %";
    }

    juce::String shortName (const juce::String& name)
    {
        return name.length() > 24 ? name.substring (0, 10) + "..." + name.getLastCharacters (10) : name;
    }

    juce::String readout (juce::int64 cropStart, juce::int64 cropEnd, juce::int64 loopStart, juce::int64 loopEnd, juce::int64 length, double sampleRate)
    {
        if (length <= 0 || ! (loopStart >= 0 && loopEnd > loopStart))
            return cropReadout (cropStart, cropEnd, length, sampleRate);
        juce::String t;
        if (cropStart <= 0 && cropEnd >= length)
            t << "Crop: whole take";
        else
            t << "Crop " << timeText (cropStart, sampleRate) << " to " << timeText (cropEnd, sampleRate) << " (" << timeText (cropEnd - cropStart, sampleRate) << ")";
        t << "   Loop " << timeText (loopStart, sampleRate) << " to " << timeText (loopEnd, sampleRate) << " (" << timeText (loopEnd - loopStart, sampleRate) << ")";
        return t;
    }

    juce::String timeText (juce::int64 frames, double sampleRate)
    {
        if (! (sampleRate > 0.0))
            return "-";
        const double s = (double) frames / sampleRate;
        if (s < 60.0)
            return juce::String (s, 3) + " s";
        const int m = (int) (s / 60.0);
        return juce::String (m) + ":" + juce::String (s - 60.0 * m, 3).paddedLeft ('0', 6);
    }

    juce::String cropReadout (juce::int64 start, juce::int64 end, juce::int64 length, double sampleRate)
    {
        if (length <= 0)
            return {};
        if (start <= 0 && end >= length)
            return "Crop: whole take, " + timeText (length, sampleRate);
        return "Crop " + timeText (start, sampleRate) + " to " + timeText (end, sampleRate)
             + ", length " + timeText (end - start, sampleRate) + " (" + juce::String (end - start) + " frames)";
    }

    int resultTone (const HotStepLinkProcessor::TakeEditReport& report)
    {
        return ! report.ok ? toneFailed : report.newName.isEmpty() ? toneNothing : toneMade;
    }

    int resultToneFor (const HotStepLinkProcessor::TakeEditReport& report, const TakeEdit::EditOptions& asked)
    {
        const int tone = resultTone (report);
        if (tone != toneMade || asked.loopRepeats < 1)
            return tone;
        const int level = seamLevel (report.result);
        return level == 1 ? (int) toneSeam : level == 2 ? (int) toneSeamStrong : tone;
    }

    juce::String resultText (const HotStepLinkProcessor::TakeEditReport& report, const TakeEdit::EditOptions& asked)
    {
        if (! report.ok)
            return report.result.error.isNotEmpty() ? report.result.error : juce::String ("The edit failed.");
        if (report.newName.isEmpty())
            return "Nothing was made: a take with this exact edit exists already.";
        if (asked.loopRepeats >= 1)
        {
            juce::String l ("Loop made: " + report.newName);
            const int level = seamLevel (report.result);
            if (level > 0)
                l << (level == 1 ? ". Ends do not join cleanly (" : ". Ends join badly, the level dips (")
                  << juce::String (report.result.seamCorrelation, 2) << "); try moving a flag.";
            else if (report.result.sampleRate > 0.0)
                l << ", " << asked.loopRepeats << " x "
                  << juce::String ((double) report.result.outSamples / (double) asked.loopRepeats / report.result.sampleRate, 3) << " s";
            return l;
        }
        juce::String t ("Made " + report.newName);
        if (report.result.sampleRate > 0.0)
            t << ", " << juce::String ((double) report.result.outSamples / report.result.sampleRate, 3) << " s";
        if (asked.fitBars && report.result.bars > 0)
            t << ", " << report.result.bars << (report.result.bars == 1 ? " bar, " : " bars, ")
              << (report.result.leftoverMs >= 0.0 ? "+" : "") << juce::String (report.result.leftoverMs, 1) << " ms";
        return t;
    }
}

//==============================================================================
TakeEditorPanel::TakeEditorPanel()
{
    setOpaque (true);
    titleLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, kBright);
    titleLabel.setMinimumHorizontalScale (1.0f);
    for (auto* l : { &infoLabel, &readoutLabel, &resultLabel })
    {
        l->setFont (juce::FontOptions (11.0f));
        l->setMinimumHorizontalScale (1.0f);
    }
    infoLabel.setColour (juce::Label::textColourId, kDim);
    repeatsLabel.setText ("Repeats", juce::dontSendNotification);
    repeatsLabel.setFont (juce::FontOptions (11.0f));
    repeatsLabel.setMinimumHorizontalScale (1.0f);
    repeatsLabel.setColour (juce::Label::textColourId, kDim);
    noteLabel.setFont (juce::FontOptions (10.0f));
    noteLabel.setMinimumHorizontalScale (1.0f); // two rows of words, never squeezed
    noteLabel.setColour (juce::Label::textColourId, kDim);
    snapGridButton.setTooltip ("A dragged crop handle or loop flag jumps from beat to beat of the take's measured tempo. Needs a take with a valid tempo.");
    zeroButton.setTooltip ("Apply crop moves each cut to the nearest zero crossing within 3 ms, so the cut does not click. Done when the take is made; loop points are not moved.");
    snapGridButton.onClick = [this] { snapChanged(); };
    zeroButton.onClick = [this] { snapChanged(); };
    addAndMakeVisible (snapGridButton);
    addAndMakeVisible (zeroButton);
    // Slice 4: the fit row.
    fitButton.setTooltip ("Shows how many whole bars the crop is at the tempo in the field, and what is left over. Apply crop reports the same for the take it makes. Nothing is stretched by this.");
    fitButton.onClick = [this] { fitChanged(); };
    addAndMakeVisible (fitButton);
    tempoField.setFont (juce::FontOptions (12.0f));
    tempoField.setJustificationType (juce::Justification::centred);
    tempoField.setEditable (true, false, false); // one click edits; return or a click elsewhere takes the value
    tempoField.setColour (juce::Label::backgroundColourId, kField);
    tempoField.setColour (juce::Label::outlineColourId, kEdge);
    tempoField.setColour (juce::Label::textColourId, kText);
    tempoField.setColour (juce::Label::backgroundWhenEditingColourId, kField);
    tempoField.setColour (juce::Label::textWhenEditingColourId, kBright);
    tempoField.setColour (juce::Label::outlineWhenEditingColourId, kAccent);
    tempoField.setTooltip ("Tempo for Fit to bars in bpm, 30 to 300. Starts at the host tempo.");
    tempoField.placeholder = TakeEditorMath::noHostTempoText();
    tempoField.setText (TakeEditorMath::fitFieldText (fitBpm), juce::dontSendNotification);
    tempoField.onTextChange = [this] { tempoTyped(); };
    addAndMakeVisible (tempoField);
    barsLabel.setFont (juce::FontOptions (11.0f));
    barsLabel.setMinimumHorizontalScale (1.0f);
    barsLabel.setColour (juce::Label::textColourId, kText);
    stretchButton.setTooltip ("After an Apply crop with Fit to bars on: makes one more take, the new one stretched (pitch kept) to exactly the whole bars it was reported as, at the tempo in the field.");
    stretchButton.onClick = [this] { stretchToFit(); };
    addAndMakeVisible (stretchButton);
    // The playhead readout (Mock-up B): a small right-aligned overlay inside the ruler strip,
    // dim text on a kBg box. It takes no row of the layout.
    playheadValue.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    playheadValue.setColour (juce::Label::backgroundColourId, kBg);
    playheadValue.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    playheadValue.setColour (juce::Label::textColourId, kDim);
    playheadValue.setMinimumHorizontalScale (1.0f);
    playheadValue.setInterceptsMouseClicks (false, false);
    playheadValue.setBorderSize (juce::BorderSize<int> (0, 4, 0, 4));
    playheadValue.setJustificationType (juce::Justification::centredRight);
    addChildComponent (playheadValue);
    for (auto* l : { &titleLabel, &infoLabel, &readoutLabel, &resultLabel, &repeatsLabel, &noteLabel, &barsLabel })
    {
        l->setJustificationType (juce::Justification::centredLeft);
        l->setBorderSize (juce::BorderSize<int>());
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (*l);
    }
    closeButton.setTooltip ("Close the take editor. The crop points stay with the take.");
    resetButton.setTooltip ("Put both crop handles back at the ends of the take and clear the loop");
    exportButton.setTooltip ("Make a new take that plays the part between the two loop flags as many times as Repeats says. This take is not changed.");
    exportButton.onClick = [this] { exportLoop(); };
    repeatsBox.setSliderStyle (juce::Slider::IncDecButtons);
    repeatsBox.setIncDecButtonsMode (juce::Slider::incDecButtonsNotDraggable);
    repeatsBox.setRange ((double) TakeEditorMath::kMinRepeats, (double) TakeEditorMath::kMaxRepeats, 1.0);
    repeatsBox.setValue ((double) TakeEditorMath::kDefaultRepeats, juce::dontSendNotification);
    repeatsBox.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 28);
    repeatsBox.setTooltip ("How many times the loop is played in the exported take (1 to 64)");
    addAndMakeVisible (repeatsBox);
    cancelButton.setTooltip ("Close and put the crop points back as they were when the editor was opened");
    applyButton.setTooltip ("Make a new take from the part between the two handles. This take is not changed.");
    applyButton.setComponentID ("pink");
    closeButton.onClick = [this] { close (false); };
    cancelButton.onClick = [this] { close (true); };
    resetButton.onClick = [this] { resetCrop(); };
    applyButton.onClick = [this] { applyCrop(); };
    for (auto* b : { &closeButton, &resetButton, &cancelButton, &applyButton, &exportButton })
        addAndMakeVisible (*b);
    // Audition: the left end of the zoom row.
    playButton.setTooltip ("Play the part between the two loop flags (the whole take while no loop is set). The host transport does not have to run.");
    stopButton.setTooltip ("Stop the audition");
    loopButton.setTooltip ("On: the audition repeats until Stop. Off: it plays once.");
    loopButton.setToggleState (true, juce::dontSendNotification);
    playButton.onClick = [this] { auditionPlay(); };
    stopButton.onClick = [this] { auditionStop(); };
    loopButton.onClick = [this] { if (onAuditionLoop) onAuditionLoop (loopButton.getToggleState()); };
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (loopButton);
    // Phase 2: loop length jump and manual grid resolution, in the middle of the zoom row.
    loopHalfButton.setTooltip ("Halve the loop length; the loop start stays.");
    loopDoubleButton.setTooltip ("Double the loop length; the loop start stays. Off when it would run past the end of the take.");
    gridResBox.setSliderStyle (juce::Slider::IncDecButtons);
    gridResBox.setIncDecButtonsMode (juce::Slider::incDecButtonsNotDraggable);
    gridResBox.setRange ((double) -TakeEditorMath::kMaxResolutionStep, (double) TakeEditorMath::kMaxResolutionStep, 1.0);
    gridResBox.setValue (0.0, juce::dontSendNotification);
    gridResBox.setTextBoxStyle (juce::Slider::TextBoxLeft, true, 30, 28);
    gridResBox.textFromValueFunction = [] (double v)
    {
        const int n = juce::roundToInt (v);
        return n == 0 ? juce::String ("A") : (n > 0 ? "x" : "/") + juce::String (1 << std::abs (n));
    };
    gridResBox.setTooltip ("Grid resolution by hand: A = automatic, x2 x4... = fewer grid lines, /2 /4... = more (never finer than one line per beat)");
    gridResBox.onValueChange = [this] { stepResolution (juce::roundToInt (gridResBox.getValue()) - gridResolution); };
    loopHalfButton.onClick = [this] { scaleLoopLength (false); };
    loopDoubleButton.onClick = [this] { scaleLoopLength (true); };
    for (auto* b : { (juce::Component*) &loopHalfButton, (juce::Component*) &loopDoubleButton, (juce::Component*) &gridResBox })
        addAndMakeVisible (*b);
    // Warp (Mock-up B, round 3): the middle of the zoom row, Apply warp at the right end of the bottom row.
    warpButton.setTooltip ("On: Play previews the take with the warp markers applied. Off: the plain take.");
    warpButton.setToggleState (true, juce::dontSendNotification);
    warpButton.onClick = [this] { refreshButtons(); };
    warpHostButton.setTooltip ("Move the warp markers so the whole take plays at the host tempo (the Fit to bars tempo when there is no host).");
    warpHostButton.onClick = [this] { warpToHost(); };
    warpResetButton.setTooltip ("Put every warp marker back where it was placed automatically.");
    warpResetButton.onClick = [this] { resetMarkers(); };
    applyWarpButton.setTooltip ("Make a new take with the warp rendered in (pitch kept), tagged with the tempo it now plays at. This take is not changed.");
    applyWarpButton.setComponentID ("pink");
    applyWarpButton.onClick = [this] { applyWarp(); };
    for (auto* b : { (juce::Button*) &warpButton, (juce::Button*) &warpHostButton, (juce::Button*) &warpResetButton, (juce::Button*) &applyWarpButton })
        addAndMakeVisible (*b);
    // Horizontal zoom: -, +, Fit at the right of the scroll bar row. Not persisted.
    zoomOutButton.setTooltip ("Zoom out (the mouse wheel over the waveform zooms about the pointer; Shift + wheel scrolls)");
    zoomInButton.setTooltip ("Zoom in (the mouse wheel over the waveform zooms about the pointer; Shift + wheel scrolls)");
    zoomFitButton.setTooltip ("Show the whole take");
    zoomOutButton.onClick = [this] { zoomBy (1.0 / (1.25 * 1.25), 0.5 * waveArea.getWidth()); };
    zoomInButton.onClick = [this] { zoomBy (1.25 * 1.25, 0.5 * waveArea.getWidth()); };
    zoomFitButton.onClick = [this] { zoom = 1.0; viewStart = 0; zoomChanged(); };
    zoomLoopButton.setTooltip ("Zoom to the loop (or the crop when there is no loop)");
    zoomLoopButton.onClick = [this] { zoomToLoop(); };
    for (auto* b : { &zoomOutButton, &zoomInButton, &zoomFitButton, &zoomLoopButton })
        addAndMakeVisible (*b);
    scrollBar.setAutoHide (false);
    scrollBar.addListener (this);
    addChildComponent (scrollBar);
    zoomChanged();
    refreshButtons();
    refreshAudition();
}

double TakeEditorPanel::warpTargetBpm() const
{
    const double host = onHostBpm ? TakeEditorMath::fitTempoFromHost (onHostBpm()) : 0.0;
    return host > 0.0 ? host : TakeEditorMath::fitTempo (fitBpm);
}

void TakeEditorPanel::warpToHost()
{
    ensureWarp();
    const double to = warpTargetBpm();
    if (! opened || waiting || warpMarkers.size() < 2 || ! (to > 0.0) || ! gridIsUsable())
        return;
    warpMarkers = WarpMarkers::scaleToTempo (warpMarkers, take.grid.bpm, to);
    warpTagBpm = to;
    gridResBox.setValue ((double) gridResolution, juce::dontSendNotification);
    refreshButtons();
    repaint();
}

void TakeEditorPanel::resetMarkers()
{
    if (! opened)
        return;
    warpBuilt = false;
    markersEdited = false;
    warpTagBpm = 0.0;
    ensureWarp();
    refreshButtons();
    repaint();
}

void TakeEditorPanel::applyWarp()
{
    ensureWarp();
    if (! opened || waiting || length <= 0 || peakError.isNotEmpty() || WarpMarkers::movedCount (warpMarkers) == 0)
        return;
    TakeEdit::EditOptions options;
    options.loopRepeats = 0;
    options.warp = warpMarkers;
    options.warpBpm = warpTagBpm > 0.0 ? warpTagBpm : WarpMarkers::resultTempo (warpMarkers, take.grid.bpm);
    startJob (currentEdit(), options, "Making the warped take...");
}

void TakeEditorPanel::auditionPlay()
{
    if (! opened || length <= 0 || ! onAuditionPlay)
        return;
    const bool loop = hasLoop();
    if (onAuditionWarp)
    {
        ensureWarp();
        onAuditionWarp (warpButton.getToggleState() && WarpMarkers::movedCount (warpMarkers) > 0 ? warpMarkers : std::vector<WarpMarkers::Marker> {});
    }
    sentRegionStart = loop ? loopStart : 0;
    sentRegionEnd = loop ? loopEnd : length;
    onAuditionPlay (take.temp, sentRegionStart, sentRegionEnd, length, loopButton.getToggleState());
    refreshAudition();
}

void TakeEditorPanel::auditionStop()
{
    if (onAuditionStop)
        onAuditionStop();
    refreshAudition();
}

int TakeEditorPanel::playheadX() const
{
    if (playheadFrame < 0 || length <= 0)
        return -1;
    const double x = TakeEditorMath::xOfFrame (view(), playheadFrame);
    if (! (x >= 0.0) || x > (double) waveArea.getWidth())
        return -1;
    return juce::jmin (waveArea.getRight() - 1, waveArea.getX() + (int) std::floor (x));
}

void TakeEditorPanel::setPlayhead (juce::int64 frame)
{
    if (frame == playheadFrame)
        return;
    const int oldX = playheadX();
    playheadFrame = frame;
    const int newX = playheadX();
    playheadValue.setText (TakeEditorMath::playheadText (frame, sampleRate, take.grid), juce::dontSendNotification);
    playheadValue.setVisible (playheadValue.getText().isNotEmpty());
    const int top = rulerArea.getY(), h = waveArea.getBottom() - top;
    if (oldX >= 0)
        repaint (oldX - 6, top, 13, h);
    if (newX >= 0)
        repaint (newX - 6, top, 13, h);
}

void TakeEditorPanel::refreshAudition()
{
    const int state = onAuditionState ? onAuditionState() : 0;
    if (state == auditionShown)
        return;
    auditionShown = state;
    playButton.setButtonText (state == 1 ? "Loading" : state == 2 ? "Playing" : "Play");
    playButton.setToggleState (state != 0, juce::dontSendNotification);
    stopButton.setEnabled (state != 0);
}

TakeEditorPanel::~TakeEditorPanel()
{
    if (onAuditionStop)
        onAuditionStop();
    cancelBuild();
    pool.removeAllJobs (true, 2000);
}

std::atomic<bool> TakeEditorPanel::testMatchByName { false };
std::atomic<bool> TakeEditorPanel::testNoTimeout { false };
std::atomic<bool> TakeEditorPanel::testKeepExpiredTicket { false };
std::atomic<bool> TakeEditorPanel::testClockAtClick { false };

bool TakeEditorPanel::resultIsWarningForTest() const
{
    return resultLabel.findColour (juce::Label::textColourId) == kAmber;
}

bool TakeEditorPanel::textFits (const juce::Label& l) const
{
    juce::AttributedString as;
    as.append (l.getText(), l.getFont());
    juce::TextLayout tl;
    tl.createLayout (as, 100000.0f);
    return tl.getWidth() <= (float) l.getWidth();
}

bool TakeEditorPanel::fitIsAmberForTest() const
{
    return barsLabel.findColour (juce::Label::textColourId) == kAmber;
}

bool TakeEditorPanel::fitInsideForTest (int& widest, int& lines, int& labelW) const
{
    const auto text = barsLabel.getText();
    const auto parts = juce::StringArray::fromLines (text);
    float w = 0.0f;
    for (const auto& part : parts)
    {
        juce::AttributedString as;
        as.append (part, barsLabel.getFont());
        juce::TextLayout tl;
        tl.createLayout (as, 100000.0f);
        w = juce::jmax (w, tl.getWidth());
    }
    const auto inside = barsLabel.getBorderSize().subtractedFrom (barsLabel.getLocalBounds());
    widest = (int) std::ceil (w);
    lines = parts.size();
    labelW = inside.getWidth();
    return w <= (float) inside.getWidth() && (float) lines * barsLabel.getFont().getHeight() <= (float) inside.getHeight();
}

bool TakeEditorPanel::noteIsAmberForTest() const
{
    return noteLabel.findColour (juce::Label::textColourId) == kAmber;
}

bool TakeEditorPanel::noteFitsForTest() const
{
    if (noteLabel.getText().isEmpty())
        return true;
    juce::AttributedString as;
    as.append (noteLabel.getText(), noteLabel.getFont());
    juce::TextLayout tl;
    tl.createLayout (as, (float) noteLabel.getWidth());
    return tl.getHeight() <= (float) noteLabel.getHeight() && tl.getWidth() <= (float) noteLabel.getWidth();
}

std::vector<TakeEditorMath::BeatLine> TakeEditorPanel::beatLinesForTest() const
{
    std::vector<TakeEditorMath::BeatLine> lines;
    if (length > 0)
        TakeEditorMath::beatLines (view(), take.grid, sampleRate, lines, gridResolution);
    for (auto& l : lines)
        l.x += (double) waveArea.getX();
    return lines;
}

void TakeEditorPanel::snapChanged()
{
    refreshNote();
}

void TakeEditorPanel::setLiveGrid (const TakeEditorMath::Grid& g, const juce::String& tempoText)
{
    if (! opened || TakeEditorMath::testMutant.load() == TakeEditorMath::mutGridStale)
        return;
    auto eq = [] (double a, double b) { return a == b || (std::isnan (a) && std::isnan (b)); };
    const bool sameGrid = g.measured == take.grid.measured && g.fallback == take.grid.fallback && eq (g.bpm, take.grid.bpm) && eq (g.downbeatSec, take.grid.downbeatSec);
    if (sameGrid && tempoText == take.tempoText)
        return;
    take.grid = g;
    take.tempoText = tempoText;
    refreshTexts(); // the info line and the snap note
    if (! sameGrid)
    {
        refreshButtons(); // a grid that became usable enables the Warp row and grid resolution
        repaint();        // ruler and beat lines; the points stay where they are
    }
}

void TakeEditorPanel::refreshNote()
{
    // Before the header is read the rate is not known, so nothing is said about the grid yet.
    const bool known = opened && length > 0, usable = gridIsUsable();
    const bool grid = snapGridButton.getToggleState(), zero = zeroButton.getToggleState();
    noteTone = known ? TakeEditorMath::snapNoteTone (grid, usable) : 0;
    noteLabel.setColour (juce::Label::textColourId, noteTone == 1 ? kAmber : kDim);
    noteLabel.setText (known ? TakeEditorMath::snapNote (grid, zero, usable) : juce::String(), juce::dontSendNotification);
}

void TakeEditorPanel::fitChanged()
{
    refreshTexts();
    refreshButtons(); // Apply is off while Fit to bars is on and the tempo is not set
}

// The placeholder of the empty tempo field ("no host tempo"), dim, in the field and in its editor.
void TakeEditorPanel::TempoLabel::paint (juce::Graphics& g)
{
    juce::Label::paint (g);
    if (getText().isEmpty() && ! isBeingEdited() && placeholder.isNotEmpty())
    {
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawFittedText (placeholder, getLocalBounds().reduced (2, 0), juce::Justification::centred, 1, 0.7f);
    }
}

void TakeEditorPanel::TempoLabel::editorShown (juce::TextEditor* ed)
{
    if (ed != nullptr)
        ed->setTextToShowWhenEmpty (placeholder, kDim);
    juce::Label::editorShown (ed);
}

void TakeEditorPanel::tempoTyped()
{
    const auto text = tempoField.getText().trim().replaceCharacter (',', '.');
    const double v = TakeEditorMath::fitTempo (text.containsAnyOf ("0123456789") ? text.getDoubleValue() : 0.0);
    if (TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitTempoLoose)
        fitBpm = tempoField.getText().getDoubleValue();
    else if (v > 0.0)
    {
        fitBpm = v;
        tempoByUser = true;
    }
    // What is shown is what is used: the limited value, or the one before when the text was no tempo
    // (empty while no tempo is set: never 120 and never 0.00).
    tempoField.setText (TakeEditorMath::fitFieldText (fitBpm), juce::dontSendNotification);
    refreshTexts();
    refreshButtons();
}

void TakeEditorPanel::stretchToFit()
{
    if (! opened || waiting || stretchName.isEmpty() || ! (stretchFrom > 0.0))
        return;
    const bool wrong = TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitStretchWrong;
    const bool started = (onStretch && (wrong ? onStretch (stretchName, stretchTo, stretchFrom) : onStretch (stretchName, stretchFrom, stretchTo))) || wrong;
    if (started)
        setResult ("Stretching to " + juce::String (stretchBars) + (stretchBars == 1 ? " bar at " : " bars at ") + juce::String (wrong ? stretchFrom : stretchTo, 2)
                       + " bpm. The new take is listed when it is done.", toneHint);
    else
        setResult ("Could not start the stretch: the new take is no longer listed.", toneFailed);
    stretchName.clear(); // one stretch per Apply
    refreshButtons();
}

void TakeEditorPanel::gridOptions (TakeEdit::EditOptions& o, bool onGrid) const
{
    // The render is asked to snap only when it would move nothing: the toggle is on, the
    // grid usable and the points already on beats (a drag put them there). Points set
    // with the toggle off stay where they were set.
    o.snapToGrid = TakeEditorMath::testMutant.load() == TakeEditorMath::mutSnapSendLoose ? snapGridButton.getToggleState()
                                                                                         : snapActive() && onGrid && ! take.grid.fallback;
    if (o.snapToGrid)
    {
        o.gridBpm = take.grid.bpm;
        o.gridDownbeatSec = take.grid.downbeatSec;
    }
}

bool TakeEditorPanel::resultFitsForTest() const { return textFits (resultLabel); }
bool TakeEditorPanel::readoutFitsForTest() const { return textFits (readoutLabel); }
bool TakeEditorPanel::playheadFitsForTest (juce::Rectangle<int>* boundsOut) const
{
    const auto v = playheadValue.getBounds();
    if (boundsOut != nullptr)
        *boundsOut = v;
    const auto b = playheadValue.getBorderSize();
    juce::AttributedString as;
    as.append (playheadValue.getText(), playheadValue.getFont());
    juce::TextLayout tl;
    tl.createLayout (as, 100000.0f);
    const float phTextW = tl.getWidth();
    return playheadValue.isVisible() && rulerArea.contains (v) && v.getRight() == rulerArea.getRight()
        && getLocalBounds().contains (v) && phTextW <= (float) (v.getWidth() - b.getLeftAndRight());
}

int TakeEditorPanel::repeats() const
{
    return juce::jlimit (TakeEditorMath::kMinRepeats, TakeEditorMath::kMaxRepeats, (int) std::lround (repeatsBox.getValue()));
}

bool TakeEditorPanel::resultIsErrorForTest() const
{
    return resultLabel.findColour (juce::Label::textColourId) == kRed;
}

void TakeEditorPanel::cancelBuild()
{
    if (shared != nullptr)
        shared->cancel.store (true); // a late result lands in a block nobody reads
    shared.reset();
    seenGeneration = 0;
    seenPercent = -1;
}

void TakeEditorPanel::open (const Take& t)
{
    // A drag still on belongs to the take shown until now: it ends here, so its last
    // step (held back by the throttle) is written to that take, not dropped.
    if (opened && TakeEditorMath::testMutant.load() != TakeEditorMath::mutOpenNoEndDrag)
        endDrag();
    auditionStop(); // the audition belongs to the take shown until now
    cancelBuild();
    take = t;
    opened = true;
    openedEdit = t.edit;
    length = cropStart = cropEnd = 0;
    loopStart = loopEnd = -1;
    warpMarkers.clear();
    warpBuilt = false;
    markersEdited = false;
    warpDrag = -1;
    dragLoop = loopJustMade = runSeen = false;
    loopAnchor = -1;
    shownSeamLevel = 0;
    sampleRate = 0.0;
    channels = 0;
    peaks.reset();
    peakError.clear();
    dragging = TakeEditorMath::handleNone;
    writePending = waiting = resultHeld = false;
    waitTicket = 0; // an Apply of the take shown before is no longer waited for
    stretchName.clear();
    if (! tempoByUser && TakeEditorMath::testMutant.load() != TakeEditorMath::mutFitNoHost)
    {
        // The tempo field starts from the host tempo until a tempo was typed.
        // No usable host tempo = not set (0): the field is empty, never 120.
        fitBpm = TakeEditorMath::fitTempoFromHost (onHostBpm ? onHostBpm() : 0.0);
        tempoField.setText (TakeEditorMath::fitFieldText (fitBpm), juce::dontSendNotification);
    }
    zoom = 1.0; // a new take opens whole
    viewStart = 0;
    zoomChanged();

    shared = std::make_shared<Shared>();
    // The job holds the shared block and the path, never the panel (it may be gone first).
    pool.addJob ([s = shared, file = take.temp]
    {
        auto d = TakePeaks::build (file, [s] { return s->cancel.load(); },
                                   [s] (double p) { s->percent.store ((int) (100.0 * p)); },
                                   [s] (const TakePeaks::Data& h)
                                   {
                                       {
                                           const std::lock_guard<std::mutex> g (s->lock);
                                           s->header = h;
                                       }
                                       s->generation.store (1);
                                   });
        auto done = std::make_shared<const TakePeaks::Data> (std::move (d));
        {
            const std::lock_guard<std::mutex> g (s->lock);
            s->done = done;
        }
        s->generation.store (2);
    });
    titleLabel.setText (take.name, juce::dontSendNotification);
    refreshTexts();
    refreshButtons();
    repaint();
}

void TakeEditorPanel::close (bool restore)
{
    if (! opened)
        return;
    endDrag();
    if (restore && onSetEdit && TakeEditorMath::testMutant.load() != TakeEditorMath::mutCancelKeeps)
        onSetEdit (take.temp, openedEdit);
    auditionStop();
    cancelBuild();
    opened = waiting = false;
    waitTicket = 0;
    peaks.reset();
    waveImage = {};
    if (onClosed)
        onClosed();
}

void TakeEditorPanel::tick()
{
    if (! opened)
        return;
    refreshAudition();
    if (onAuditionRegion && length > 0)
    {
        // A moved loop brace reaches the audition at the next tick, playing or not.
        const bool loop = hasLoop();
        const juce::int64 rs = loop ? loopStart : 0, re = loop ? loopEnd : length;
        if (rs != sentRegionStart || re != sentRegionEnd)
        {
            sentRegionStart = rs;
            sentRegionEnd = re;
            if (auditionShown != 0)
                onAuditionRegion (rs, re, length);
        }
    }
    {
        // The playhead follows the audition's own position (an atomic the audio thread
        // writes), only while it plays; stopped or loading = hidden.
        juce::int64 f = -1;
        if (auditionShown == 2 && onAuditionPosition && length > 0)
        {
            const double frac = onAuditionPosition();
            if (frac >= 0.0 && frac <= 1.0)
                f = juce::jlimit ((juce::int64) 0, length, (juce::int64) std::llround (frac * (double) length));
        }
        setPlayhead (f);
    }
    if (shared != nullptr)
    {
        const int g = shared->generation.load();
        if (g != seenGeneration)
        {
            seenGeneration = g;
            TakePeaks::Data h;
            std::shared_ptr<const TakePeaks::Data> done;
            {
                const std::lock_guard<std::mutex> lk (shared->lock);
                h = shared->header;
                done = shared->done;
            }
            if (length == 0 && h.length > 0)
            {
                // The header comes first: ruler and handles are right before the waveform is.
                length = h.length;
                sampleRate = h.sampleRate;
                channels = h.channels;
                TakeEditorMath::fromEditData (take.edit, length, cropStart, cropEnd);
                TakeEditorMath::clampCrop (cropStart, cropEnd, length, TakeEditorMath::minCropLength (sampleRate), true);
                TakeEditorMath::loopFromEditData (take.edit, length, loopStart, loopEnd);
                if (hasLoop())
                    TakeEditorMath::clampCrop (loopStart, loopEnd, length, TakeEditorMath::minCropLength (sampleRate), true);
                // open() ran zoomChanged() with length 0, which disabled -, + and Fit; the
                // real length is known only now, so the zoom state is refreshed here.
                zoomChanged();
            }
            if (g == 2 && done != nullptr)
            {
                if (done->ok)
                    peaks = done;
                else
                    peakError = "Cannot read the take: " + done->error;
            }
            waveDirty = true;
            refreshTexts();
            refreshButtons();
            repaint();
        }
        else if (g < 2)
        {
            const int p = shared->percent.load();
            if (p != seenPercent)
            {
                seenPercent = p;
                repaint (waveArea);
            }
        }
    }
    if (dragging != TakeEditorMath::handleNone && writePending
        && nowMs() - lastWriteMs >= (juce::uint32) kWriteEveryMs)
        writeEdit();
    if (waitTicket != 0 && (waiting || testKeepExpiredTicket.load()))
    {
        // Only the report of this Apply's ticket ends the wait: another job's report
        // is never asked for, whatever take it was made from.
        HotStepLinkProcessor::TakeEditReport r;
        if (onReport && onReport (waitTicket, r) && r.ticket == waitTicket
            && (! testMatchByName.load() || r.sourceName == take.name))
        {
            showReport (r);
        }
        else if (waiting)
        {
            // No report yet. The 30 s count from the tick the job is first seen running:
            // a job that waits behind another one on the pool thread is not late.
            using State = HotStepLinkProcessor::TakeEditState;
            const auto state = onJobState && ! testClockAtClick.load() ? onJobState (waitTicket) : State::running;
            if (state == State::unknown)
            {
                // No report and no job: it was dropped, or the plugin is closing.
                waiting = false;
                waitTicket = 0;
                setResult (droppedText(), toneFailed);
                refreshButtons();
            }
            else
            {
                if (! runSeen && state != State::queued)
                {
                    runSeen = true;
                    waitStartMs = nowMs();
                    setResult (makingText, toneHint);
                }
                else if (! runSeen && nowMs() - waitStartMs >= (juce::uint32) kQueuedNoteAfterMs && resultLabel.getText() != queuedText())
                    setResult (queuedText(), toneHint); // not at once: every job is queued for a moment
                if (runSeen && ! testNoTimeout.load() && nowMs() - waitStartMs >= (juce::uint32) kApplyTimeoutMs)
                {
                    waiting = false;
                    if (! testKeepExpiredTicket.load())
                        waitTicket = 0; // a report that comes after this is not this panel's result
                    setResult (applyTimeoutText(), toneFailed);
                    refreshButtons();
                }
            }
        }
    }
}

//==============================================================================
TakeEditorMath::View TakeEditorPanel::view() const
{
    if (TakeEditorMath::testMutant.load() == TakeEditorMath::mutZoomWhole)
        return TakeEditorMath::wholeTake (length, waveArea.getWidth());
    return TakeEditorMath::zoomedView (length, waveArea.getWidth(), zoom, viewStart);
}

bool TakeEditorPanel::gripShown (juce::int64 frame) const
{
    return length > 0 && (TakeEditorMath::testMutant.load() == TakeEditorMath::mutHandleEdgeClamp || TakeEditorMath::frameInView (view(), frame));
}

void TakeEditorPanel::zoomBy (double factor, double anchorX)
{
    if (length <= 0)
        return;
    TakeEditorMath::zoomAround (length, waveArea.getWidth(), zoom, viewStart, anchorX, factor);
    zoomChanged();
}

void TakeEditorPanel::setViewStart (juce::int64 start)
{
    const auto s = TakeEditorMath::clampViewStart (length, waveArea.getWidth(), zoom, start);
    if (s == viewStart)
        return;
    viewStart = s;
    zoomChanged();
}

void TakeEditorPanel::zoomChanged()
{
    const bool zoomed = zoom > 1.0 + 1.0e-9 && length > 0;
    const auto v = view();
    scrollBar.setRangeLimits (0.0, (double) juce::jmax ((juce::int64) 1, length), juce::dontSendNotification);
    scrollBar.setCurrentRange ((double) v.firstFrame, v.framesPerPixel * (double) v.widthPx, juce::dontSendNotification);
    scrollBar.setVisible (zoomed);
    zoomOutButton.setEnabled (zoomed);
    zoomFitButton.setEnabled (zoomed);
    zoomInButton.setEnabled (length > 0 && zoom < TakeEditorMath::maxZoom (length, waveArea.getWidth()) - 1.0e-9);
    zoomLoopButton.setEnabled (length > 0);
    waveDirty = true;
    repaint();
}

void TakeEditorPanel::zoomToFrames (juce::int64 first, juce::int64 end)
{
    if (length <= 0 || end <= first)
        return;
    const auto whole = TakeEditorMath::wholeTake (length, waveArea.getWidth());
    const double span = (double) (end - first);
    zoom = juce::jlimit (1.0, TakeEditorMath::maxZoom (length, waveArea.getWidth()), (whole.framesPerPixel * (double) whole.widthPx) / span);
    viewStart = TakeEditorMath::clampViewStart (length, waveArea.getWidth(), zoom, first);
    zoomChanged();
}

void TakeEditorPanel::zoomToLoop()
{
    const auto s = hasLoop() ? loopStart : cropStart, e = hasLoop() ? loopEnd : cropEnd;
    const auto pad = (e - s) / 20; // a little room either side for the handles
    zoomToFrames (juce::jmax ((juce::int64) 0, s - pad), juce::jmin (length, e + pad));
}

void TakeEditorPanel::overviewSeek (double xInOverview)
{
    if (length <= 0 || overviewArea.getWidth() < 1)
        return;
    const auto v = view();
    const double centre = juce::jlimit (0.0, 1.0, xInOverview / (double) overviewArea.getWidth()) * (double) length;
    setViewStart ((juce::int64) std::llround (centre - 0.5 * v.framesPerPixel * (double) v.widthPx));
}

void TakeEditorPanel::scrollBarMoved (juce::ScrollBar*, double newRangeStart)
{
    setViewStart ((juce::int64) std::llround (newRangeStart));
}

void TakeEditorPanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (length <= 0)
        return;
    const auto top = juce::Rectangle<int> (waveArea.getX(), rulerArea.getY(), waveArea.getWidth(), waveArea.getBottom() - rulerArea.getY());
    if (! top.contains (e.getPosition()))
        return;
    const float dy = std::abs (w.deltaY) >= std::abs (w.deltaX) ? w.deltaY : -w.deltaX;
    if (dy == 0.0f)
        return;
    // The wheel zooms 1.25x per notch about the pointer (with or without Ctrl / Cmd);
    // Shift + wheel or a horizontal swipe scrolls, as in most DAWs.
    const bool pan = e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
    if (! pan)
    {
        const double notches = (double) dy / 0.25; // about 0.25 per wheel notch
        zoomBy (std::pow (1.25, juce::jlimit (-4.0, 4.0, notches)), (double) e.position.x - waveArea.getX());
        return;
    }
    if (zoom <= 1.0 + 1.0e-9)
        return;
    const auto v = view();
    // A notch pans an eighth of the view.
    setViewStart (v.firstFrame - (juce::int64) std::llround ((double) dy / 0.25 * 0.125 * v.framesPerPixel * v.widthPx));
}

TakeEdit::EditData TakeEditorPanel::currentEdit() const
{
    return length > 0 ? TakeEditorMath::withLoop (TakeEditorMath::toEditData (cropStart, cropEnd, length, take.edit), loopStart, loopEnd) : take.edit;
}

TakeEditorMath::Handle TakeEditorPanel::handleAt (juce::Point<int> p) const
{
    using namespace TakeEditorMath;
    if (length <= 0 || (! waveArea.contains (p) && testMutant.load() != mutFlagAnywhere))
        return handleNone; // crop handles live in the waveform only
    const auto v = view();
    double xs = waveArea.getX() + xOfFrame (v, cropStart), xe = waveArea.getX() + xOfFrame (v, cropEnd);
    if (testMutant.load() == mutHandleEdgeClamp)
        xe = juce::jmin ((double) waveArea.getRight(), xe); // the old clamp: picked at the edge
    else if (! gripShown (cropStart) || ! gripShown (cropEnd))
    {
        // A handle off the view is not picked; the one in it is, by its own grab rule.
        const bool sIn = gripShown (cropStart), eIn = gripShown (cropEnd);
        const bool tabS = sIn && p.y < waveArea.getY() + kTabPx && p.x >= xs && p.x < xs + kTabPx;
        const bool tabE = eIn && p.y >= waveArea.getBottom() - kTabPx && p.x > xe - kTabPx && p.x <= xe;
        if (tabS || (sIn && std::abs (p.x - xs) <= kGrabPx))
            return handleStart;
        if (tabE || (eIn && std::abs (p.x - xe) <= kGrabPx))
            return handleEnd;
        return handleNone;
    }
    const bool inStartTab = p.y < waveArea.getY() + kTabPx && p.x >= xs && p.x < xs + kTabPx;
    const bool inEndTab = p.y >= waveArea.getBottom() - kTabPx && p.x > xe - kTabPx && p.x <= xe;
    if (inStartTab != inEndTab)
        return inStartTab ? handleStart : handleEnd;
    return pickHandle (xs, xe, (double) p.x);
}

TakeEditorMath::Handle TakeEditorPanel::flagAt (juce::Point<int> p) const
{
    using namespace TakeEditorMath;
    if (length <= 0 || ! hasLoop() || (! loopLane.contains (p) && testMutant.load() != mutFlagAnywhere))
        return handleNone; // loop flags live in the loop lane only
    const auto v = view();
    double xs = loopLane.getX() + xOfFrame (v, loopStart), xe = loopLane.getX() + xOfFrame (v, loopEnd);
    if (testMutant.load() == mutHandleEdgeClamp)
        xe = juce::jmin ((double) loopLane.getRight(), xe);
    else if (! gripShown (loopStart) || ! gripShown (loopEnd))
    {
        const bool sIn = gripShown (loopStart), eIn = gripShown (loopEnd);
        if (sIn && ((p.x >= xs && p.x < xs + kTabPx) || std::abs (p.x - xs) <= kGrabPx))
            return handleStart;
        if (eIn && ((p.x > xe - kTabPx && p.x <= xe) || std::abs (p.x - xe) <= kGrabPx))
            return handleEnd;
        return handleNone;
    }
    const bool inStartTab = p.x >= xs && p.x < xs + kTabPx, inEndTab = p.x > xe - kTabPx && p.x <= xe;
    if (inStartTab != inEndTab)
        return inStartTab ? handleStart : handleEnd;
    return pickHandle (xs, xe, (double) p.x);
}

void TakeEditorPanel::createLoopAt (juce::int64 frame)
{
    if (length <= 0)
        return;
    TakeEditorMath::loopAtClick (frame, length, TakeEditorMath::minCropLength (sampleRate), loopStart, loopEnd);
    if (snapActive())
        TakeEditorMath::snapLoop (loopStart, loopEnd, true, take.grid, sampleRate, length); // start on a beat, whole beats long
    loopAnchor = juce::jlimit ((juce::int64) 0, length, frame); // the click itself, not the start loopAtClick may move back
    dragging = TakeEditorMath::handleEnd; // a drag on from here moves the end
    dragLoop = loopJustMade = true;
    grabOffset = 0.0;
    if (! waiting)
        resultHeld = false;
    writePending = true; // written on the mouse up, or by the throttle when the drag goes on
    refreshTexts();
    refreshButtons();
    repaint (loopLane.getUnion (waveArea));
}

void TakeEditorPanel::scaleLoopLength (bool doubleIt)
{
    if (! opened || length <= 0 || dragging != TakeEditorMath::handleNone)
        return;
    if (! TakeEditorMath::scaleLoop (loopStart, loopEnd, length, TakeEditorMath::minCropLength (sampleRate), doubleIt))
        return;
    if (! waiting)
        resultHeld = false;
    writeEdit();
    refreshTexts();
    refreshButtons();
    repaint (loopLane.getUnion (waveArea));
}

void TakeEditorPanel::stepResolution (int delta)
{
    gridResolution = juce::jlimit (-TakeEditorMath::kMaxResolutionStep, TakeEditorMath::kMaxResolutionStep, gridResolution + delta);
    refreshButtons();
    repaint();
}

void TakeEditorPanel::clearLoop()
{
    if (! opened || ! hasLoop())
        return;
    loopStart = loopEnd = -1;
    if (dragLoop)
        dragging = TakeEditorMath::handleNone;
    dragLoop = loopJustMade = false;
    loopAnchor = -1;
    if (! waiting)
        resultHeld = false;
    writeEdit();
    refreshTexts();
    repaint (loopLane.getUnion (waveArea));
}

juce::Rectangle<int> TakeEditorPanel::handleStrip (juce::int64 frame) const
{
    const int x = waveArea.getX() + (int) std::floor (TakeEditorMath::xOfFrame (view(), frame));
    return juce::Rectangle<int> (x - TakeEditorMath::kTabPx - 2, waveArea.getY(), 2 * TakeEditorMath::kTabPx + 4, waveArea.getHeight());
}

void TakeEditorPanel::moveHandleTo (juce::int64 frame)
{
    using namespace TakeEditorMath;
    if (dragging == handleNone || length <= 0)
        return;
    if (dragLoop && ! hasLoop())
        return;
    // A loop flag moves by the rules of a crop handle: the same clamp, the same minimum.
    auto& a = dragLoop ? loopStart : cropStart;
    auto& b = dragLoop ? loopEnd : cropEnd;
    const bool end = dragging == handleEnd;
    const auto oldA = a, oldB = b;
    // Snap first, clamp after: the handle jumps from beat to beat, and the file ends
    // and the minimum length still have the last word.
    const bool snap = snapActive();
    if (snap && ! dragLoop)
        frame = snapCropPoint (frame, end ? a : b, end, take.grid, sampleRate, length, minCropLength (sampleRate));
    (end ? b : a) = frame;
    if (snap && dragLoop)
        snapLoop (a, b, end, take.grid, sampleRate, length);
    if (! (dragLoop && testMutant.load() == mutLoopNoClamp))
        clampCrop (a, b, length, minCropLength (sampleRate), end);
    if (a == oldA && b == oldB)
        return;
    const auto before = end ? oldB : oldA, after = end ? b : a;
    if ((end ? a : b) != (end ? oldA : oldB))
        repaint (loopLane.getUnion (waveArea).expanded (1)); // a snapped loop: the flag that was not dragged went to its beat too
    else
    {
        // Only the span between the old and the new line (shading and handle) is repainted;
        // for a loop flag the same span of the loop lane too.
        const auto span = handleStrip (before).getUnion (handleStrip (after));
        repaint ((dragLoop ? span.withTop (loopLane.getY()) : span).getIntersection (loopLane.getUnion (waveArea).expanded (1)));
    }
    pointsMoved();
}

void TakeEditorPanel::pointsMoved()
{
    using namespace TakeEditorMath;
    if (! waiting)
        resultHeld = false;
    refreshTexts();
    writePending = true;
    if (nowMs() - lastWriteMs >= (juce::uint32) kWriteEveryMs || testMutant.load() == mutNoThrottle)
        writeEdit();
    else
        refreshButtons();
}

void TakeEditorPanel::writeEdit()
{
    if (onSetEdit && length > 0)
        onSetEdit (take.temp, currentEdit()); // a lock and one write in the processor, no file IO
    writePending = false;
    lastWriteMs = nowMs();
    refreshButtons();
}

void TakeEditorPanel::endDrag()
{
    if (dragging == TakeEditorMath::handleNone)
        return;
    dragging = TakeEditorMath::handleNone;
    dragLoop = loopJustMade = false;
    loopAnchor = -1;
    if (TakeEditorMath::testMutant.load() == TakeEditorMath::mutNoUpWrite)
        return;
    writeEdit(); // always on mouse up, whatever the throttle did
}

namespace
{
    // Only the left button alone makes or drags a flag or a handle.
    bool leftOnly (const juce::ModifierKeys& m)
    {
        return m.isLeftButtonDown() && ! m.isRightButtonDown() && ! m.isMiddleButtonDown();
    }
}

void TakeEditorPanel::mouseDown (const juce::MouseEvent& e)
{
    using namespace TakeEditorMath;
    const bool anyButton = testMutant.load() == mutNoButtonCheck;
    const bool dragOn = dragging != handleNone;
    if (! anyButton)
        endDrag(); // a press while a drag is still on (a second button) ends it with its write
    dragLoop = loopJustMade = false;
    overviewDrag = false;
    if (overviewArea.contains (e.getPosition()) && length > 0)
    {
        overviewDrag = true;
        overviewSeek ((double) e.position.x - overviewArea.getX());
        return;
    }
    loopAnchor = -1;
    dragging = handleNone;
    warpDrag = -1;
    if (warpLane.contains (e.getPosition()))
    {
        // The warp lane: a left press takes the marker under the pointer. Nothing else here.
        if (leftOnly (e.mods) && length > 0)
        {
            // Alt + click (Option on macOS) removes the marker under the pointer.
            if (e.mods.isAltDown())
                removeWarpMarker (warpMarkerAt (e.getPosition()));
            else
                warpDrag = warpMarkerAt (e.getPosition());
        }
        return;
    }
    const bool inLane = loopLane.contains (e.getPosition());
    // A context click in the loop lane (right button, Ctrl + left on macOS) clears the
    // loop. It never starts a drag, and a press that ended a drag does not clear.
    LanePress act = laneTake;
    if (anyButton)
        act = inLane && e.mods.isPopupMenu() ? laneClear : laneTake;
    else if (inLane)
        act = lanePress (e.mods.isPopupMenu(), e.mods.isLeftButtonDown(), e.mods.isRightButtonDown(), e.mods.isMiddleButtonDown(), dragOn);
    else
        act = leftOnly (e.mods) ? laneTake : laneNothing;
    if (act == laneClear)
    {
        if (length > 0)
            clearLoop();
        return;
    }
    if (act == laneNothing)
        return; // middle button, right button outside the lane, two buttons at once: nothing
    if (inLane)
    {
        // The loop lane: a flag is taken, and a click while no loop is set makes one
        // there. Crop handles are never taken here.
        if (length <= 0)
            return;
        if (! hasLoop())
        {
            createLoopAt (frameAtX (view(), (double) e.position.x - loopLane.getX()));
            return;
        }
        dragging = flagAt (e.getPosition());
        if (dragging == handleNone)
            return;
        dragLoop = true;
        const auto lf = dragging == handleEnd ? loopEnd : loopStart;
        grabOffset = e.position.x - (float) (loopLane.getX() + xOfFrame (view(), lf));
        return;
    }
    dragging = handleAt (e.getPosition());
    if (dragging == handleNone)
        return;
    const auto f = dragging == handleEnd ? cropEnd : cropStart;
    grabOffset = e.position.x - (float) (waveArea.getX() + xOfFrame (view(), f));
}

void TakeEditorPanel::mouseDrag (const juce::MouseEvent& e)
{
    using namespace TakeEditorMath;
    if (overviewDrag)
    {
        overviewSeek ((double) e.position.x - overviewArea.getX());
        return;
    }
    if (warpDrag >= 0)
    {
        warpDragMove (frameAtX (view(), (double) e.position.x - waveArea.getX()), e.mods.isShiftDown());
        return;
    }
    if (dragging == handleNone)
        return;
    if (! leftOnly (e.mods) && testMutant.load() != mutNoButtonCheck)
    {
        endDrag(); // the buttons changed under the drag: it ends here, nothing stays taken
        return;
    }
    const auto frame = frameAtX (view(), (double) e.position.x - grabOffset - waveArea.getX());
    if (dragLoop && loopAnchor >= 0)
    {
        // The loop made by this mouse down stays as it is until the pointer has moved
        // 3 px. From then on the click frame is one end and the pointer the other,
        // either way round, for the whole drag; minimum length and file end still hold.
        if (loopJustMade && std::abs (e.getDistanceFromDragStartX()) < 3)
            return;
        loopJustMade = false;
        const auto minLen = minCropLength (sampleRate);
        if (testMutant.load() == mutOldAnchor)
        {
            loopAnchor = -1; // as before o133: fixed at the start loopAtClick left, on the first step only
            if (frame < loopStart)
            {
                loopEnd = juce::jmax (loopStart, minLen);
                dragging = handleStart;
                repaint (loopLane.getUnion (waveArea));
            }
        }
        else
        {
            const bool leftward = frame < loopAnchor;
            const auto oldStart = loopStart, oldEnd = loopEnd;
            loopStart = leftward ? frame : loopAnchor;
            loopEnd = leftward ? loopAnchor : frame;
            dragging = leftward ? handleStart : handleEnd;
            if (snapActive())
                snapLoop (loopStart, loopEnd, ! leftward, take.grid, sampleRate, length);
            clampCrop (loopStart, loopEnd, length, minLen, ! leftward);
            if (loopStart != oldStart || loopEnd != oldEnd)
            {
                repaint (loopLane.getUnion (waveArea));
                pointsMoved();
            }
            return;
        }
    }
    moveHandleTo (frame);
}

void TakeEditorPanel::mouseUp (const juce::MouseEvent&)
{
    releaseDrag();
}

void TakeEditorPanel::releaseDrag()
{
    overviewDrag = false;
    const bool warpMoved = warpDrag >= 0;
    warpDrag = -1;
    if (warpMoved)
        refreshButtons(); // a hand move turns Reset markers and Apply warp on at once
    // Round 2b: a marker let go while the preview plays renders the warp again (on
    // the audition's job) and plays it from the start of the region.
    if (warpMoved && onAuditionState && onAuditionState() != 0)
        auditionPlay();
    endDrag();
}

bool TakeEditorPanel::removeWarpMarker (int i)
{
    if (i < 0 || ! WarpMarkers::removeAt (warpMarkers, (std::size_t) i))
        return false;
    markersEdited = true;
    warpTagBpm = 0.0;
    refreshButtons();
    repaint (warpLane.getUnion (waveArea));
    return true;
}

int TakeEditorPanel::addWarpMarkerAtX (double xInWave)
{
    ensureWarp();
    if (! warpButton.getToggleState() || length <= 0 || warpMarkers.size() < 2)
        return -1;
    // x is a warped position (the markers are drawn at dst): back to the source frame.
    auto src = WarpMarkers::mapFrame (warpMarkers, TakeEditorMath::frameAtX (view(), xInWave), false);
    if (snapGridButton.getToggleState() && gridIsUsable())
        src = juce::jlimit ((juce::int64) 0, length, TakeEdit::snapToGrid (src, sampleRate, take.grid.bpm, take.grid.downbeatSec));
    const int at = WarpMarkers::insertAt (warpMarkers, src);
    if (at >= 0)
    {
        markersEdited = true;
        refreshButtons();
        repaint (warpLane.getUnion (waveArea));
    }
    return at;
}

void TakeEditorPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    const bool inLane = warpLane.contains (e.getPosition());
    if (! inLane && ! waveArea.contains (e.getPosition()))
        return;
    const int i = inLane ? warpMarkerAt (e.getPosition()) : -1;
    if (i < 0)
    {
        // Empty space with Warp on: a new marker there.
        addWarpMarkerAtX ((double) e.position.x - waveArea.getX());
        return;
    }
    warpMarkerDoubleClicked (i);
}

void TakeEditorPanel::warpMarkerDoubleClicked (int i)
{
    if (i < 0 || (std::size_t) i >= warpMarkers.size())
        return;
    if (removeWarpMarker (i)) // double-click removes a marker; the end anchors reset instead
        return;
    WarpMarkers::resetOne (warpMarkers, (std::size_t) i);
    warpTagBpm = 0.0;
    refreshButtons(); // Reset markers / Apply warp follow the moved count
    repaint (warpLane.getUnion (waveArea));
}

void TakeEditorPanel::warpDragMove (juce::int64 frame, bool shiftDown)
{
    // Horizontal only: dstFrame follows the pointer, clamped between the neighbours.
    const auto before = warpMarkers[(std::size_t) warpDrag].dstFrame;
    WarpMarkers::moveTo (warpMarkers, (std::size_t) warpDrag, warpDragTarget (frame, shiftDown), length);
    warpTagBpm = 0.0; // a hand move: the tag tempo comes from the markers
    if (warpMarkers[(std::size_t) warpDrag].dstFrame != before)
        repaint (warpLane.getUnion (waveArea));
}

bool TakeEditorPanel::shotDragWarp (int marker, double dstSec, bool snap)
{
    // Dev shot hook: press on the marker, one drag step to dstSec, release, as the mouse does.
    ensureWarp();
    if (length <= 0 || marker < 0 || (std::size_t) marker >= warpMarkers.size())
        return false;
    warpDrag = marker;
    warpDragMove ((juce::int64) std::llround (dstSec * sampleRate), ! snap);
    releaseDrag();
    return true;
}

void TakeEditorPanel::shotZoom (double startSec, double endSec)
{
    zoomToFrames ((juce::int64) std::llround (startSec * sampleRate), (juce::int64) std::llround (endSec * sampleRate));
}

juce::int64 TakeEditorPanel::warpDragTarget (juce::int64 dst, bool shiftDown) const
{
    // Snap to the take's beat grid (fixed, from the measured tempo); Shift is free.
    if (! shiftDown && gridIsUsable())
        dst = juce::jlimit ((juce::int64) 0, length, TakeEdit::snapToGrid (dst, sampleRate, take.grid.bpm, take.grid.downbeatSec));
    return dst;
}

juce::int64 TakeEditorPanel::columnSource (juce::int64 d) const
{
    // A warped frame back to the source frame under it while a warp is in effect.
    const bool on = warpButton.getToggleState() && WarpMarkers::movedCount (warpMarkers) > 0; // = effectiveWarp(), no copy
    return on ? WarpMarkers::mapFrame (warpMarkers, d, false) : d;
}

void TakeEditorPanel::ensureWarp() const
{
    if (warpBuilt || length <= 0 || ! gridIsUsable())
        return;
    warpMarkers = WarpMarkers::autoPlace (length, sampleRate, take.grid.bpm, take.grid.downbeatSec, TakeEditorMath::kBeatsPerBar);
    warpBuilt = true;
}

int TakeEditorPanel::warpMarkerAt (juce::Point<int> p) const
{
    ensureWarp();
    if (! warpLane.contains (p) || length <= 0)
        return -1;
    const auto v = view();
    int best = -1, bestD = 7; // within 6 px of the handle centre
    for (std::size_t i = 0; i < warpMarkers.size(); ++i)
    {
        const int x = warpLane.getX() + (int) std::floor (TakeEditorMath::xOfFrame (v, warpMarkers[i].dstFrame));
        const int d = std::abs (p.x - x);
        if (d < bestD)
        {
            bestD = d;
            best = (int) i;
        }
    }
    return best;
}

void TakeEditorPanel::paintWarp (juce::Graphics& g) const
{
    // Regions between markers, shaded by speed (blue faster, amber slower), with the
    // percent at the top left; then a line per marker through the waveform.
    ensureWarp();
    if (length <= 0 || warpMarkers.empty())
        return;
    const auto v = view();
    const juce::Colour blue (0xff60a5fa);
    auto xAt = [&] (juce::int64 f) { return waveArea.getX() + (int) std::floor (TakeEditorMath::xOfFrame (v, f)); };
    g.setFont (juce::FontOptions (10.0f));
    for (std::size_t i = 0; i + 1 < warpMarkers.size(); ++i)
    {
        const double pct = WarpMarkers::regionPercent (warpMarkers, i);
        if (std::abs (pct - 100.0) < 0.05)
            continue;
        const int x0 = xAt (warpMarkers[i].dstFrame), x1 = juce::jmin (waveArea.getRight(), xAt (warpMarkers[i + 1].dstFrame));
        const auto c = pct > 100.0 ? blue : kAmber;
        g.setColour (c.withAlpha (0.16f));
        g.fillRect (x0, waveArea.getY(), x1 - x0, waveArea.getHeight());
        g.setColour (c);
        g.drawText (juce::String (pct, 1) + " %", x0 + 4, waveArea.getY() + 2, juce::jmax (0, x1 - x0 - 6), 14, juce::Justification::centredLeft, false);
    }
    for (const auto& m : warpMarkers)
    {
        const int x = xAt (m.dstFrame);
        if (x < waveArea.getX() || x > waveArea.getRight())
            continue; // out of the zoomed view
        g.setColour ((m.moved() ? kAmber : kAccent).withAlpha (0.45f));
        g.fillRect (juce::jmin (waveArea.getRight() - 1, x), waveArea.getY(), 1, waveArea.getHeight());
    }
}

void TakeEditorPanel::mouseMove (const juce::MouseEvent& e)
{
    const bool onHandle = handleAt (e.getPosition()) != TakeEditorMath::handleNone || flagAt (e.getPosition()) != TakeEditorMath::handleNone
                          || warpMarkerAt (e.getPosition()) >= 0;
    setMouseCursor (onHandle ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
}

juce::String TakeEditorPanel::getTooltip()
{
    if (length <= 0 || ! loopLane.contains (getMouseXYRelative()))
        return {};
    return hasLoop() ? "Drag a flag to move the loop start or end. Right click here to clear the loop."
                     : "Click or drag here to set a loop";
}

//==============================================================================
void TakeEditorPanel::applyCrop()
{
    if (! opened || waiting || length <= 0 || peakError.isNotEmpty())
        return;
    const auto edit = currentEdit();
    if (! edit.hasCrop())
        return;
    // Fit to bars with no tempo set (no host tempo, nothing typed): no Apply, never a fit at 120.
    if (! TakeEditorMath::fitApplyAllowed (fitButton.getToggleState(), fitBpm))
        return;
    TakeEdit::EditOptions options;
    options.loopRepeats = 0; // crop only
    gridOptions (options, TakeEditorMath::cropOnGrid (edit, take.grid, sampleRate, length));
    // The render moves the crop points (never the loop) to a zero crossing, after the grid.
    options.snapToZeroCrossing = zeroButton.getToggleState() && TakeEditorMath::testMutant.load() != TakeEditorMath::mutZeroNotSent;
    if (fitButton.getToggleState() || TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitSendAlways)
    {
        // The render reports the whole bars of the crop it made at this tempo; nothing is stretched by it.
        options.fitBars = true;
        options.fitBpm = fitBpm;
        options.fitBeatsPerBar = TakeEditorMath::kBeatsPerBar;
    }
    startJob (edit, options, "Making the cropped take...");
}

void TakeEditorPanel::exportLoop()
{
    const bool loose = TakeEditorMath::testMutant.load() == TakeEditorMath::mutExportLoose;
    if (! opened || length <= 0 || peakError.isNotEmpty() || ((waiting || ! hasLoop()) && ! loose))
        return;
    TakeEdit::EditOptions options;
    options.loopRepeats = loose ? TakeEditorMath::kDefaultRepeats : repeats(); // a loop render: the loop region this many times
    gridOptions (options, TakeEditorMath::loopOnGrid (loopStart, loopEnd, take.grid, sampleRate, length));
    if (TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitSendAlways)
    {
        options.fitBars = true;
        options.fitBpm = fitBpm;
    }
    startJob (currentEdit(), options, "Making the loop...");
}

// Apply crop and Export loop: one request by ticket, one wait, one result path.
void TakeEditorPanel::startJob (const TakeEdit::EditData& edit, const TakeEdit::EditOptions& options, const juce::String& making)
{
    askedOptions = options;
    makingText = making;
    stretchName.clear(); // Stretch to fit belongs to the Apply before this one
    juce::String whyNot;
    waitTicket = onApply ? onApply (take.temp, edit, askedOptions, whyNot) : 0;
    waiting = waitTicket != 0;
    waitStartMs = nowMs();
    // Without a job state every job counts as running from the click.
    runSeen = ! onJobState || testClockAtClick.load();
    if (waiting)
        setResult (making, toneHint);
    else
        setResult (whyNot.isNotEmpty() ? whyNot : juce::String ("Could not start: the take is no longer listed or not final."), toneFailed);
    refreshButtons();
}

void TakeEditorPanel::showReport (const HotStepLinkProcessor::TakeEditReport& r)
{
    waiting = false;
    waitTicket = 0;
    const int tone = TakeEditorMath::resultToneFor (r, askedOptions);
    setResult (TakeEditorMath::resultText (r, askedOptions), tone);
    if (! textFits (resultLabel) && r.newName.isNotEmpty())
    {
        // One row: a long take name gives way before the words do.
        auto shorter = r;
        shorter.newName = TakeEditorMath::shortName (r.newName);
        setResult (TakeEditorMath::resultText (shorter, askedOptions), tone);
    }
    shownSeamLevel = tone == toneSeam ? 1 : tone == toneSeamStrong ? 2 : 0;
    // Stretch to fit: on after an Apply crop that reported a bar count and listed a new take.
    stretchName.clear();
    if (r.ok && r.newName.isNotEmpty() && askedOptions.loopRepeats == 0 && askedOptions.fitBars && r.result.bars > 0)
    {
        stretchFrom = TakeEditorMath::stretchFromBpm (r.result.outSamples, r.result.sampleRate, r.result.leftoverMs, askedOptions.fitBpm);
        stretchTo = askedOptions.fitBpm;
        stretchBars = r.result.bars;
        if (stretchFrom > 0.0)
            stretchName = r.newName;
    }
    refreshButtons();
}

void TakeEditorPanel::resetCrop()
{
    if (! opened || length <= 0)
        return;
    cropStart = 0;
    cropEnd = length;
    loopStart = loopEnd = -1; // Reset clears the loop too
    if (! waiting)
        resultHeld = false;
    writeEdit();
    refreshTexts();
    repaint (loopLane.getUnion (waveArea).expanded (1));
}

void TakeEditorPanel::setResult (const juce::String& text, int tone)
{
    resultHeld = true;
    shownSeamLevel = 0;
    resultLabel.setColour (juce::Label::textColourId, tone == toneMade ? kGreen : tone == toneNothing || tone == toneSeam ? kAmber
                                                          : tone == toneFailed || tone == toneSeamStrong ? kRed : kDim);
    resultLabel.setText (text, juce::dontSendNotification);
    resultLabel.repaint();
}

void TakeEditorPanel::refreshTexts()
{
    juce::String info (take.tempoText);
    if (length > 0)
    {
        if (info.isNotEmpty())
            info << "  -  ";
        info << juce::String ((int) std::llround (sampleRate)) << " Hz  -  " << channels << " ch  -  " << TakeEditorMath::timeText (length, sampleRate);
    }
    infoLabel.setText (info, juce::dontSendNotification);
    readoutLabel.setText (TakeEditorMath::readout (cropStart, cropEnd, loopStart, loopEnd, length, sampleRate), juce::dontSendNotification);
    // The fit row: the whole bars of the crop region (what Apply crop renders) at the tempo of the field.
    // When Stretch to fit of that region would be a hard stretch the readout says so on a second line, in amber.
    const int fitMutant = TakeEditorMath::testMutant.load();
    juce::String bars, fitWarn;
    if (fitMutant == TakeEditorMath::mutFitReadoutLoose)
        bars = TakeEditorMath::fitReadout (length, sampleRate, fitBpm);
    else if ((fitButton.getToggleState() || fitMutant == TakeEditorMath::mutFitWarnWrong) && length > 0)
    {
        bars = TakeEditorMath::fitBarsText (cropEnd - cropStart, sampleRate, fitBpm);
        fitWarn = TakeEditorMath::fitWarning (cropEnd - cropStart, sampleRate, fitBpm);
    }
    barsLabel.setColour (juce::Label::textColourId, fitWarn.isNotEmpty() ? kAmber : kText);
    barsLabel.setText (bars + fitWarn, juce::dontSendNotification);
    refreshNote();
}

void TakeEditorPanel::refreshButtons()
{
    const bool crop = length > 0 && currentEdit().hasCrop();
    const bool loop = length > 0 && hasLoop();
    applyButton.setEnabled (opened && ! waiting && crop && peakError.isEmpty() && TakeEditorMath::fitApplyAllowed (fitButton.getToggleState(), fitBpm));
    exportButton.setEnabled (opened && ! waiting && loop && peakError.isEmpty()); // one job at a time: off while Apply waits too
    resetButton.setEnabled (opened && (crop || loop));
    playButton.setEnabled (opened && length > 0 && peakError.isEmpty());
    {
        const auto minLen = TakeEditorMath::minCropLength (sampleRate);
        auto hs = loopStart, he = loopEnd, ds = loopStart, de = loopEnd;
        loopHalfButton.setEnabled (opened && loop && TakeEditorMath::scaleLoop (hs, he, length, minLen, false));
        loopDoubleButton.setEnabled (opened && loop && TakeEditorMath::scaleLoop (ds, de, length, minLen, true));
        gridResBox.setEnabled (opened && length > 0 && gridIsUsable());
    }
    {
        ensureWarp();
        const bool markers = opened && length > 0 && warpMarkers.size() >= 2 && peakError.isEmpty();
        const bool moved = markers && WarpMarkers::movedCount (warpMarkers) > 0;
        warpButton.setEnabled (markers);
        warpHostButton.setEnabled (markers && ! waiting && gridIsUsable() && warpTargetBpm() > 0.0);
        warpResetButton.setEnabled ((moved || (markers && markersEdited && TakeEditorMath::testMutant.load() != TakeEditorMath::mutResetMovedOnly)) && ! waiting); // an added or removed marker counts too
        applyWarpButton.setEnabled (moved && ! waiting);
    }
    stretchButton.setEnabled (opened && ((! waiting && stretchName.isNotEmpty() && stretchFrom > 0.0) || TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitStretchLoose));
    if (! resultHeld)
    {
        resultLabel.setColour (juce::Label::textColourId, kDim);
        resultLabel.setText (length > 0 && ! crop && ! loop && peakError.isEmpty() ? "Move a handle first" : "", juce::dontSendNotification);
    }
}

std::vector<TakeEditorPanel::Area> TakeEditorPanel::areasForTest() const
{
    return { { "Title", titleLabel.getBounds(), false, false },      { "Close", closeButton.getBounds(), true, true },
             { "Info line", infoLabel.getBounds(), false, false },   { "Time ruler", rulerArea, false, false },
             { "Warp lane + markers", warpLane, true, false },        { "Loop lane + loop flags", loopLane, true, false },
             { "Waveform + crop handles", waveArea, true, false },   { "Scroll bar", scrollArea, true, false },
             { "Zoom out", zoomOutButton.getBounds(), true, true },  { "Zoom in", zoomInButton.getBounds(), true, true },
             { "Zoom fit", zoomFitButton.getBounds(), true, true },     { "Zoom loop", zoomLoopButton.getBounds(), true, true },
             { "Readout", readoutLabel.getBounds(), false, false },  { "Snap to grid", snapGridButton.getBounds(), true, true },
             { "Zero crossing", zeroButton.getBounds(), true, true }, { "Snap note", noteLabel.getBounds(), false, false },
             { "Fit to bars", fitButton.getBounds(), true, true },   { "Tempo field", tempoField.getBounds(), true, true },
             { "Bars readout", barsLabel.getBounds(), false, false }, { "Stretch to fit", stretchButton.getBounds(), true, true },
             { "Play", playButton.getBounds(), true, true },         { "Stop", stopButton.getBounds(), true, true },
             { "Loop", loopButton.getBounds(), true, true },
             { "Loop /2", loopHalfButton.getBounds(), true, true }, { "Loop x2", loopDoubleButton.getBounds(), true, true },
             { "Grid resolution", gridResBox.getBounds(), true, true },
             { "Zoom row (empty)", zoomRow, false, false },
             { "Repeats label", repeatsLabel.getBounds(), false, false }, { "Repeats box", repeatsBox.getBounds(), true, true },
             { "Result line", resultLabel.getBounds(), false, false }, { "Reset", resetButton.getBounds(), true, true },
             { "Cancel", cancelButton.getBounds(), true, true },     { "Export loop", exportButton.getBounds(), true, true },
             { "Apply crop", applyButton.getBounds(), true, true },
             { "Warp", warpButton.getBounds(), true, true },          { "Warp to host", warpHostButton.getBounds(), true, true },
             { "Reset markers", warpResetButton.getBounds(), true, true }, { "Apply warp", applyWarpButton.getBounds(), true, true } };
}

//==============================================================================
void TakeEditorPanel::resized()
{
    // Every bound derives from the panel size; the waveform takes the slack. Nothing
    // here touches a drag or the peak build (the editor calls resized() on refreshes).
    const auto c = getLocalBounds().reduced (kPad, 0);
    const int x = c.getX(), w = c.getWidth();
    auto row = [x, w] (int y, int h) { return juce::Rectangle<int> (x, y, w, h); };
    titleLabel.setBounds (x, 12, w - 96, 28);
    closeButton.setBounds (c.getRight() - 88, 12, 88, 28);
    infoLabel.setBounds (row (44, 18));
    rulerArea = row (66, 18);
    warpLane = row (86, 22);   // slice 6
    loopLane = row (112, 20);  // slice 2
    const auto oldWave = waveArea;
    waveArea = row (kAboveWave, juce::jmax (40, getHeight() - kFixedRows));
    const int by = waveArea.getBottom();
    {
        // Scroll bar row: -, +, Fit at the right (24 px tall, ending above the readout),
        // the scroll bar in the rest, 14 px tall.
        auto zr = row (by + 1, 24);
        zoomFitButton.setBounds (zr.removeFromRight (48));
        zr.removeFromRight (4);
        zoomInButton.setBounds (zr.removeFromRight (48));
        zr.removeFromRight (4);
        zoomOutButton.setBounds (zr.removeFromRight (48));
        zr.removeFromRight (8);
        zoomLoopButton.setBounds (zr.removeFromRight (48));
        zr.removeFromRight (8);
        // The overview strip of the whole take (top) and the scroll bar under it.
        overviewArea = zr.removeFromTop (13);
        zr.removeFromTop (1);
        scrollArea = zr;
        scrollBar.setBounds (scrollArea);
    }
    readoutLabel.setBounds (row (by + 26, 18));
    snapRow = row (by + 50, 28);     // slice 3: two toggles, the note takes the rest
    {
        auto sn = snapRow;
        snapGridButton.setBounds (sn.removeFromLeft (116));
        sn.removeFromLeft (4);
        zeroButton.setBounds (sn.removeFromLeft (118));
        sn.removeFromLeft (8);
        noteLabel.setBounds (sn);
    }
    // The playhead readout: an overlay at the right end of the ruler strip, outside the rows.
    {
        const int pw = juce::jmin (rulerArea.getWidth(), playheadOverlayW > 0 ? playheadOverlayW : kPlayheadOverlayW);
        playheadValue.setBounds (rulerArea.withLeft (rulerArea.getRight() - pw));
    }
    fitRow = row (by + 84, 28);      // slice 4: toggle, tempo field, Stretch to fit at the right end, the readout takes the rest
    auto fr = fitRow;
    fitButton.setBounds (fr.removeFromLeft (110));
    fr.removeFromLeft (4);
    tempoField.setBounds (fr.removeFromLeft (72));
    fr.removeFromLeft (8);
    const bool tight = TakeEditorMath::testMutant.load() == TakeEditorMath::mutFitRowTight;
    stretchButton.setBounds (fr.removeFromRight (120).translated (tight ? -100 : 0, 0));
    fr.removeFromRight (tight ? 90 : 8);
    barsLabel.setBounds (fr);
    zoomRow = row (by + 118, 28);    // slice 5; the right end is the Repeats label and box (slice 2)
    repeatsBox.setBounds (zoomRow.removeFromRight (64));
    zoomRow.removeFromRight (4);
    repeatsLabel.setBounds (zoomRow.removeFromRight (56));
    zoomRow.removeFromRight (8);
    // Audition at the left end: Play, Stop, Loop. The zoom row keeps the middle.
    playButton.setBounds (zoomRow.removeFromLeft (68));
    zoomRow.removeFromLeft (4);
    stopButton.setBounds (zoomRow.removeFromLeft (56));
    zoomRow.removeFromLeft (8);
    loopButton.setBounds (zoomRow.removeFromLeft (64));
    zoomRow.removeFromLeft (8);
    loopHalfButton.setBounds (zoomRow.removeFromLeft (48));
    zoomRow.removeFromLeft (4);
    loopDoubleButton.setBounds (zoomRow.removeFromLeft (48));
    // Warp controls (round 3): their own row under the zoom row, so the zoom row keeps
    // its width and every button stays at least 48x24 at the small sizes.
    {
        auto wr = row (by + 152, 28);
        warpButton.setBounds (wr.removeFromLeft (64));
        wr.removeFromLeft (4);
        warpHostButton.setBounds (wr.removeFromLeft (100));
        wr.removeFromLeft (8);
        warpResetButton.setBounds (wr.removeFromLeft (110));
        wr.removeFromLeft (6);
        gridResBox.setBounds (wr.removeFromLeft (60)); // phase 2: grid resolution, in the gap before Apply warp
        applyWarpButton.setBounds (wr.removeFromRight (110)); // at the right end of the warp row: the bottom row is full at 490 wide
    }
    resultLabel.setBounds (row (by + 186, 20));
    auto b = row (by + 212, 32);
    resetButton.setBounds (b.removeFromLeft (80));
    applyButton.setBounds (b.removeFromRight (120));
    b.removeFromRight (8);
    exportButton.setBounds (b.removeFromRight (120).translated (TakeEditorMath::testMutant.load() == TakeEditorMath::mutButtonsOverlap ? 28 : 0, 0));
    b.removeFromRight (8);
    cancelButton.setBounds (b.removeFromRight (90));
    if (waveArea.getWidth() != oldWave.getWidth() || waveArea.getHeight() != oldWave.getHeight())
    {
        waveDirty = true;
        zoomChanged(); // the zoom cap and scroll range follow the width
    }
}

void TakeEditorPanel::renderWave()
{
    waveDirty = false;
    const int w = waveArea.getWidth(), h = waveArea.getHeight();
    if (peaks == nullptr || w < 1 || h < 1)
    {
        waveImage = {};
        return;
    }
    waveImage = juce::Image (juce::Image::ARGB, w, h, true);
    juce::Graphics g (waveImage);
    const auto v = view();
    const bool warpOn = ! effectiveWarp().empty();
    const int lanes = juce::jmax (1, peaks->lanes), gapPx = 2;
    const float laneH = (float) (h - gapPx * (lanes - 1)) / (float) lanes;
    for (int lane = 0; lane < lanes; ++lane)
    {
        const float top = (float) lane * (laneH + (float) gapPx), mid = top + 0.5f * laneH, half = 0.5f * laneH - 2.0f;
        g.setColour (kEdge);
        g.fillRect (0.0f, mid, (float) w, 1.0f);
        g.setColour (kText.withAlpha (0.9f));
        for (int px = 0; px < w; ++px)
        {
            float lo = 0.0f, hi = 0.0f;
            // The column covers warped frames [d0, d1); with a warp in effect the source
            // frames under it come back through the marker map, so the picture is what Play hears.
            const auto d0 = v.firstFrame + (juce::int64) std::llround (px * v.framesPerPixel);
            const auto s0 = columnSource (d0);
            double span = v.framesPerPixel;
            if (warpOn)
            {
                const auto d1 = v.firstFrame + (juce::int64) std::llround ((px + 1) * v.framesPerPixel);
                span = juce::jmax (1.0, (double) (columnSource (d1) - s0));
            }
            if (! TakePeaks::columnMinMax (*peaks, lane, s0, span, lo, hi))
                continue;
            const float y1 = mid - juce::jlimit (-1.0f, 1.0f, hi) * half, y2 = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
            g.fillRect ((float) px, y1, 1.0f, juce::jmax (1.0f, y2 - y1));
        }
    }
}

int TakeEditorPanel::rulerClipNow() const
{
    // Ticks and labels stop short of the playhead readout only while it is shown; with no
    // readout they run the full ruler width.
    const bool overlay = playheadValue.isVisible() || TakeEditorMath::testMutant.load() == TakeEditorMath::mutRulerAlwaysClip;
    return overlay ? rulerClipRight (rulerArea.getRight(), rulerArea.getWidth(), playheadOverlayW > 0 ? playheadOverlayW : kPlayheadOverlayW)
                   : rulerArea.getRight();
}

void TakeEditorPanel::paintRuler (juce::Graphics& g) const
{
    g.setColour (kEdge);
    g.fillRect (rulerArea.getX(), rulerArea.getBottom() - 1, rulerArea.getWidth(), 1);
    if (length <= 0 || ! (sampleRate > 0.0))
        return;
    const int clipR = rulerClipNow();
    if (! beatCache.empty())
    {
        // Beats: a short tick per beat, a longer one per bar, bar numbers thinned by beatLines.
        g.setFont (juce::FontOptions (10.0f));
        for (const auto& b : beatCache)
        {
            const int x = rulerArea.getX() + (int) std::floor (b.x), th = b.bar ? 8 : 4;
            if (x >= clipR)
                continue;
            g.setColour (kDim.withAlpha (b.bar ? 0.9f : 0.55f));
            g.fillRect (x, rulerArea.getBottom() - th, 1, th);
            if (b.label > 0 && x + 3 + 30 <= clipR)
            {
                g.setColour (kDim);
                g.drawText (juce::String (b.label), x + 3, rulerArea.getY(), 30, rulerArea.getHeight() - 2, juce::Justification::centredLeft, false);
            }
            else if (b.tagBar > 0 && x + 3 + 40 <= clipR)
            {
                auto t = juce::String (b.tagBar) + "." + juce::String (b.tagBeat);
                if (b.tagPart > 0)
                    t << "." << b.tagPart;
                g.setColour (kDim.withAlpha (0.8f));
                g.drawText (t, x + 3, rulerArea.getY(), 40, rulerArea.getHeight() - 2, juce::Justification::centredLeft, false);
            }
        }
        return;
    }
    const auto rv = view();
    const double seconds = (double) length / sampleRate, pxPerSec = sampleRate / rv.framesPerPixel;
    const double t0 = (double) rv.firstFrame / sampleRate, t1 = t0 + (double) rulerArea.getWidth() / pxPerSec;
    double step = 600.0;
    for (double s : { 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
        if (s * pxPerSec >= 56.0)
        {
            step = s;
            break;
        }
    g.setFont (juce::FontOptions (10.0f));
    for (int k = (int) std::ceil (t0 / step - 1.0e-9); k * step <= juce::jmin (seconds, t1) + 1.0e-9; ++k)
    {
        const double t = k * step;
        const int x = rulerArea.getX() + (int) std::lround ((t - t0) * pxPerSec);
        g.setColour (kDim.withAlpha (0.7f));
        if (x >= clipR)
            break;
        g.fillRect (x, rulerArea.getBottom() - 6, 1, 6);
        juce::String label;
        if (seconds >= 60.0)
            label = juce::String ((int) (t / 60.0)) + ":" + juce::String ((int) std::lround (t) % 60).paddedLeft ('0', 2);
        else
            label = juce::String (t, step < 0.1 ? 2 : step < 1.0 ? 1 : 0) + " s";
        if (x + 3 + 44 <= clipR) // a label that would run under the playhead overlay is left out
        {
            g.setColour (kDim);
            g.drawText (label, x + 3, rulerArea.getY(), 44, rulerArea.getHeight() - 2, juce::Justification::centredLeft, false);
        }
    }
}

void TakeEditorPanel::paint (juce::Graphics& g)
{
    g.fillAll (kPanel);
    g.setColour (kEdge);
    g.drawRect (getLocalBounds().expanded (1, 0), 1);
    // The beats in view (none without a usable grid: the ruler shows seconds then).
    if (length > 0)
        TakeEditorMath::beatLines (view(), take.grid, sampleRate, beatCache, gridResolution);
    else
        beatCache.clear();
    paintRuler (g);
    if (scrollBar.isVisible())
    {
        // The scroll track: JUCE draws only the thumb, so the track is shown here.
        g.setColour (kEdge.withAlpha (0.6f));
        g.fillRoundedRectangle (scrollArea.toFloat().reduced (0.5f), 4.0f);
    }
    if (! overviewArea.isEmpty())
    {
        // The overview: the whole take, the loop tinted, the view as a box to drag.
        g.setColour (kField);
        g.fillRect (overviewArea);
        if (peaks != nullptr && length > 0)
        {
            const auto ov = TakeEditorMath::wholeTake (length, overviewArea.getWidth());
            const float mid = (float) overviewArea.getCentreY(), half = 0.5f * (float) overviewArea.getHeight() - 1.0f;
            g.setColour (kDim);
            for (int px = 0; px < overviewArea.getWidth(); ++px)
            {
                float lo = 0.0f, hi = 0.0f;
                if (! TakePeaks::columnMinMax (*peaks, 0, (juce::int64) std::llround (px * ov.framesPerPixel), ov.framesPerPixel, lo, hi))
                    continue;
                const float y1 = mid - juce::jlimit (-1.0f, 1.0f, hi) * half, y2 = mid - juce::jlimit (-1.0f, 1.0f, lo) * half;
                g.fillRect ((float) (overviewArea.getX() + px), y1, 1.0f, juce::jmax (1.0f, y2 - y1));
            }
            if (hasLoop())
            {
                const float xs = (float) (overviewArea.getX() + TakeEditorMath::xOfFrame (ov, loopStart));
                const float xe = (float) (overviewArea.getX() + TakeEditorMath::xOfFrame (ov, loopEnd));
                g.setColour (kAccent.withAlpha (0.25f));
                g.fillRect (xs, (float) overviewArea.getY(), juce::jmax (1.0f, xe - xs), (float) overviewArea.getHeight());
            }
            const auto v = view();
            const float bx = (float) (overviewArea.getX() + TakeEditorMath::xOfFrame (ov, v.firstFrame));
            const float bw = juce::jmax (3.0f, (float) (v.framesPerPixel * (double) v.widthPx / ov.framesPerPixel));
            g.setColour (kText.withAlpha (0.12f));
            g.fillRect (bx, (float) overviewArea.getY(), bw, (float) overviewArea.getHeight());
            g.setColour (kText.withAlpha (0.8f));
            g.drawRect (juce::Rectangle<float> (bx, (float) overviewArea.getY(), bw, (float) overviewArea.getHeight()), 1.0f);
        }
    }

    // Loop lane: the two flags and the bar between them, or how to make a loop.
    g.setColour (kField.withAlpha (0.55f));
    g.fillRect (loopLane);
    int loopXs = -1, loopXe = -1;
    bool loopShowS = false, loopShowE = false;
    if (length > 0 && hasLoop())
    {
        using namespace TakeEditorMath;
        const auto v = view();
        juce::Graphics::ScopedSaveState clipLoop (g);
        g.reduceClipRegion (loopLane);
        const int y = loopLane.getY(), h = loopLane.getHeight();
        // A flag off the view is not drawn (the old clamp pinned it to the edge); the
        // band runs to the view edge.
        loopShowS = gripShown (loopStart);
        loopShowE = gripShown (loopEnd);
        loopXs = loopLane.getX() + (int) std::floor (juce::jlimit (-1.0, (double) loopLane.getWidth() + 1.0, xOfFrame (v, loopStart)));
        loopXe = loopLane.getX() + (int) std::floor (juce::jlimit (-1.0, (double) loopLane.getWidth() + 1.0, xOfFrame (v, loopEnd)));
        if (testMutant.load() == mutHandleEdgeClamp)
            loopXe = juce::jmin (loopLane.getRight(), loopXe);
        g.setColour (kGreen.withAlpha (0.30f));
        g.fillRect (loopXs, y + 6, loopXe - loopXs, h - 12);
        g.setColour (kGreen);
        juce::Path pennants; // each flag points into the loop
        if (loopShowS)
        {
            g.fillRect (loopXs, y, 2, h);
            pennants.addTriangle ((float) loopXs, (float) y, (float) (loopXs + kTabPx), (float) y + 0.5f * (float) h, (float) loopXs, (float) (y + h));
        }
        if (loopShowE)
        {
            g.fillRect (loopXe - 2, y, 2, h);
            pennants.addTriangle ((float) loopXe, (float) y, (float) (loopXe - kTabPx), (float) y + 0.5f * (float) h, (float) loopXe, (float) (y + h));
        }
        g.fillPath (pennants);
    }
    else if (length > 0)
    {
        g.setColour (kDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("Click or drag here to set a loop", loopLane.reduced (6, 0), juce::Justification::centredLeft, false);
    }
    g.setColour (kEdge);
    g.drawRect (loopLane, 1);

    // Warp lane: a pennant per marker, pink, amber once moved.
    ensureWarp();
    if (length > 0 && ! warpMarkers.empty())
    {
        const auto v = view();
        const float y = (float) warpLane.getY() + 3.0f, hh = (float) warpLane.getHeight() - 6.0f;
        for (const auto& m : warpMarkers)
        {
            const double mx = TakeEditorMath::xOfFrame (v, m.dstFrame);
            if (mx < -5.0 || mx > (double) warpLane.getWidth() + 5.0)
                continue; // out of the zoomed view
            const float x = (float) juce::jlimit (warpLane.getX() + 5, warpLane.getRight() - 5,
                                                  warpLane.getX() + (int) std::floor (TakeEditorMath::xOfFrame (v, m.dstFrame)));
            juce::Path tag;
            tag.startNewSubPath (x - 5.0f, y);
            tag.lineTo (x + 5.0f, y);
            tag.lineTo (x + 5.0f, y + hh * 0.6f);
            tag.lineTo (x, y + hh);
            tag.lineTo (x - 5.0f, y + hh * 0.6f);
            tag.closeSubPath();
            g.setColour (m.moved() ? kAmber : kAccent);
            g.fillPath (tag);
        }
    }

    // Waveform
    g.setColour (kField);
    g.fillRect (waveArea);
    {
        // A marker move, add, remove, reset or the Warp toggle redraws the wave in warped time.
        std::uint64_t sig = warpButton.getToggleState() ? 1u : 0u;
        if (sig != 0)
            for (const auto& m : warpMarkers)
                sig = sig * 1000003u + (std::uint64_t) m.srcFrame * 31u + (std::uint64_t) m.dstFrame;
        if (sig != waveWarpSig)
        {
            waveWarpSig = sig;
            waveDirty = true;
        }
    }
    if (waveDirty)
        renderWave();
    if (waveImage.isValid())
        g.drawImageAt (waveImage, waveArea.getX(), waveArea.getY());
    else
    {
        g.setColour (kEdge);
        g.fillRect (waveArea.getX(), waveArea.getCentreY(), waveArea.getWidth(), 1);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (peakError.isNotEmpty() ? kRed : kDim);
        const auto text = peakError.isNotEmpty() ? peakError
                                                 : "Reading take... " + juce::String (shared != nullptr ? shared->percent.load() : 0) + "%";
        g.drawFittedText (text, waveArea.reduced (10, 0).withTrimmedBottom (waveArea.getHeight() / 2 + 4), juce::Justification::centredBottom, 2);
    }
    g.saveState(); // a zoomed view draws handles and lines off the waveform: clip to it
    g.reduceClipRegion (waveArea);
    for (const auto& b : beatCache) // faint beat lines, bars a little stronger
    {
        g.setColour (kDim.withAlpha (b.bar ? 0.30f : 0.13f));
        g.fillRect (juce::jmin (waveArea.getX() + (int) std::floor (b.x), waveArea.getRight() - 1), waveArea.getY(), 1, waveArea.getHeight());
    }
    paintWarp (g);
    if (length > 0)
    {
        using namespace TakeEditorMath;
        const auto v = view();
        auto at = [&] (juce::int64 f) { return waveArea.getX() + (int) std::floor (juce::jlimit (-1.0, (double) waveArea.getWidth() + 1.0, xOfFrame (v, f))); };
        const int xs = at (cropStart);
        const int xe = testMutant.load() == mutHandleEdgeClamp ? juce::jmin (waveArea.getRight(), at (cropEnd)) : at (cropEnd);
        // Outside the crop: dimmed. A handle off the view is not drawn (the dimming runs to the edge).
        g.setColour (kBg.withAlpha (0.72f));
        g.fillRect (waveArea.withRight (juce::jlimit (waveArea.getX(), waveArea.getRight(), xs)));
        g.fillRect (waveArea.withLeft (juce::jlimit (waveArea.getX(), waveArea.getRight(), xe)));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        if (gripShown (cropStart))
        {
            g.setColour (kAccent);
            g.fillRect (xs, waveArea.getY(), 2, waveArea.getHeight());
            g.fillRect (xs, waveArea.getY(), kTabPx, kTabPx);                        // start grip, top
            g.setColour (kBg);
            g.drawText ("S", xs, waveArea.getY(), kTabPx, kTabPx, juce::Justification::centred, false);
        }
        if (gripShown (cropEnd))
        {
            g.setColour (kAccent);
            g.fillRect (xe - 2, waveArea.getY(), 2, waveArea.getHeight());
            g.fillRect (xe - kTabPx, waveArea.getBottom() - kTabPx, kTabPx, kTabPx); // end grip, bottom
            g.setColour (kBg);
            g.drawText ("E", xe - kTabPx, waveArea.getBottom() - kTabPx, kTabPx, kTabPx, juce::Justification::centred, false);
        }
    }
    if (loopShowS || loopShowE)
    {
        // The loop points run down through the waveform as thin lines; they are grabbed in the lane only.
        g.setColour (kGreen.withAlpha (0.55f));
        if (loopShowS)
            g.fillRect (loopXs, waveArea.getY(), 1, waveArea.getHeight());
        if (loopShowE)
            g.fillRect (juce::jmax (loopXs, loopXe - 1), waveArea.getY(), 1, waveArea.getHeight());
    }
    g.restoreState();
    g.setColour (kEdge);
    g.drawRect (waveArea, 1);
    if (const int px = playheadX(); px >= 0)
    {
        // The playhead: a thin white line through the waveform, a small marker on the ruler.
        g.setColour (juce::Colours::white);
        g.fillRect (px, waveArea.getY(), 2, waveArea.getHeight());
        juce::Path head;
        head.addTriangle ((float) px - 4.0f, (float) rulerArea.getY(), (float) px + 6.0f, (float) rulerArea.getY(), (float) px + 1.0f, (float) rulerArea.getY() + 8.0f);
        g.fillPath (head);
    }
}
