// HOT-STEP LINK -- WarpMarkers.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

// Warp markers of the take editor (Mock-up B, round 2a): model only, no audio. Pure, no
// JUCE, no statics in any audio path (the editor is message thread only).
// A marker pins a frame of the source (srcFrame) to a frame of the warped take
// (dstFrame). Auto-placed markers sit on the bar starts of the take's beat grid (the
// tempo and downbeat TempoMeasure detected, or the fallback tempo), src == dst.
// Percent convention: the label of a region is its playback speed,
// srcLength / dstLength * 100. A region of 2.0 s source squeezed to 1.9 s reads
// 105.3 %: over 100 = faster (blue), under 100 = slower (amber).
namespace WarpMarkers
{
    struct Marker
    {
        std::int64_t srcFrame = 0, dstFrame = 0;
        bool moved() const { return srcFrame != dstFrame; }
    };

    // Smallest distance between two neighbouring markers after a drag.
    constexpr std::int64_t kMinGapFrames = 64;

    // Test mutants (teeth), 0 in the plugin. 1: placement drops the last bar start.
    // 2: a drag is not clamped by its neighbours. 3: the percent is dst over src.
    // 4: scaleToTempo leaves every dst at its src (no scaling). 5: insertAt ignores the
    // minimum gap. 6: removeAt also removes the fixed anchors. 7: insertAt sets dst = src.
    enum Mutant { mutNone = 0, mutDropLast = 1, mutNoClamp = 2, mutInverse = 3, mutNoScale = 4,
                  mutNoInsertGap = 5, mutRemoveAnchor = 6, mutInsertNoMap = 7 };
    inline std::atomic<int> testMutant { mutNone };

    // One marker on every bar start inside 0..length, plus the fixed anchors at 0 and
    // at length (both always present). bpm <= 0, rate <= 0 or length <= 0: none.
    inline std::vector<Marker> autoPlace (std::int64_t length, double sampleRate, double bpm, double downbeatSec, int beatsPerBar)
    {
        std::vector<Marker> out;
        if (length <= 0 || ! (sampleRate > 0.0) || ! (bpm > 0.0) || beatsPerBar <= 0)
            return out;
        const double barFrames = 60.0 / bpm * (double) beatsPerBar * sampleRate;
        if (! (barFrames >= 1.0))
            return out;
        double first = downbeatSec * sampleRate;
        first -= std::floor (first / barFrames) * barFrames; // earliest bar start at or after 0
        // Fixed anchors at frame 0 and at the take end (round 2b): every region of the
        // take is inside a marker pair, so a warp never changes the take length. A bar
        // start closer than kMinGapFrames to an anchor merges into it (8 s at 120.01
        // bpm: the 5th bar start is 32 frames before the end and becomes the end).
        out.push_back ({ 0, 0 });
        for (int k = 0;; ++k)
        {
            const double f = first + (double) k * barFrames;
            if (f > (double) length + 1.0)
                break;
            const auto at = (std::int64_t) std::llround (f);
            if (at < kMinGapFrames || at > length - kMinGapFrames)
                continue;
            out.push_back ({ at, at });
        }
        out.push_back ({ length, length });
        if (testMutant.load() == mutDropLast && ! out.empty())
            out.pop_back();
        return out;
    }

    // dst clamped between the neighbours of marker i (kMinGapFrames from each), and
    // inside 0..length at the ends. Returns the clamped value; markers are not changed.
    inline std::int64_t clampDst (const std::vector<Marker>& m, std::size_t i, std::int64_t dst, std::int64_t length)
    {
        if (i >= m.size() || testMutant.load() == mutNoClamp)
            return dst;
        std::int64_t lo = i > 0 ? m[i - 1].dstFrame + kMinGapFrames : 0;
        std::int64_t hi = i + 1 < m.size() ? m[i + 1].dstFrame - kMinGapFrames : length;
        if (hi < lo)
            hi = lo = m[i].dstFrame; // no room: stays
        return dst < lo ? lo : (dst > hi ? hi : dst);
    }

    inline void moveTo (std::vector<Marker>& m, std::size_t i, std::int64_t dst, std::int64_t length)
    {
        if (i < m.size())
            m[i].dstFrame = clampDst (m, i, dst, length);
    }

    inline void resetOne (std::vector<Marker>& m, std::size_t i)
    {
        if (i < m.size())
            m[i].dstFrame = m[i].srcFrame;
    }

    inline int movedCount (const std::vector<Marker>& m)
    {
        int n = 0;
        for (const auto& k : m)
            n += k.moved() ? 1 : 0;
        return n;
    }

    // Speed of a region in percent (see the convention above). 100 for a zero length.
    inline double speedPercent (double srcLen, double dstLen)
    {
        if (! (srcLen > 0.0) || ! (dstLen > 0.0))
            return 100.0;
        return testMutant.load() == mutInverse ? dstLen / srcLen * 100.0 : srcLen / dstLen * 100.0;
    }

    // Speed of the region between marker i and i + 1.
    inline double regionPercent (const std::vector<Marker>& m, std::size_t i)
    {
        if (i + 1 >= m.size())
            return 100.0;
        return speedPercent ((double) (m[i + 1].srcFrame - m[i].srcFrame), (double) (m[i + 1].dstFrame - m[i].dstFrame));
    }

    // Warp to a tempo (round 3): every dst = round (src * fromBpm / toBpm), so each
    // region (and the whole take) plays at toBpm. The end anchor moves too: the warped
    // take is round (length * fromBpm / toBpm) frames long. Either tempo <= 0 or not
    // finite: the markers come back unchanged.
    inline std::vector<Marker> scaleToTempo (std::vector<Marker> m, double fromBpm, double toBpm)
    {
        if (! (fromBpm > 0.0) || ! (toBpm > 0.0) || ! std::isfinite (fromBpm) || ! std::isfinite (toBpm))
            return m;
        const double k = testMutant.load() == mutNoScale ? 1.0 : fromBpm / toBpm;
        for (auto& x : m)
            x.dstFrame = (std::int64_t) std::llround ((double) x.srcFrame * k);
        return m;
    }

    // The tempo a warped take plays at: fromBpm * src span / dst span of the whole
    // marker list. 0 when it cannot be told.
    inline double resultTempo (const std::vector<Marker>& m, double fromBpm)
    {
        if (m.size() < 2 || ! (fromBpm > 0.0))
            return 0.0;
        const double s = (double) (m.back().srcFrame - m.front().srcFrame), d = (double) (m.back().dstFrame - m.front().dstFrame);
        return s > 0.0 && d > 0.0 ? fromBpm * s / d : 0.0;
    }

    // Piecewise-linear map between source and warped frames through the markers
    // (round 4). toDst: src -> dst, else dst -> src. Outside the markers: identity.
    inline std::int64_t mapFrame (const std::vector<Marker>& m, std::int64_t f, bool toDst)
    {
        for (std::size_t i = 0; i + 1 < m.size(); ++i)
        {
            const auto a0 = toDst ? m[i].srcFrame : m[i].dstFrame, a1 = toDst ? m[i + 1].srcFrame : m[i + 1].dstFrame;
            const auto b0 = toDst ? m[i].dstFrame : m[i].srcFrame, b1 = toDst ? m[i + 1].dstFrame : m[i + 1].srcFrame;
            if (f >= a0 && f <= a1 && a1 > a0)
                return b0 + (std::int64_t) std::llround ((double) (f - a0) * (double) (b1 - b0) / (double) (a1 - a0));
        }
        return f;
    }

    // Adds a marker at source frame src with dst = the warped position there now, so the
    // audio does not move. Refused (-1) outside the first..last marker or closer than
    // kMinGapFrames (src or dst) to a neighbour. Returns the new index; the list stays
    // sorted, so the neighbour clamps of moveTo hold.
    inline int insertAt (std::vector<Marker>& m, std::int64_t src)
    {
        if (m.size() < 2)
            return -1;
        const bool noGap = testMutant.load() == mutNoInsertGap;
        for (std::size_t i = 0; i + 1 < m.size(); ++i)
        {
            if (! (src >= m[i].srcFrame && src <= m[i + 1].srcFrame))
                continue;
            const auto dst = testMutant.load() == mutInsertNoMap ? src : mapFrame (m, src, true);
            const std::int64_t g = noGap ? 1 : kMinGapFrames;
            if (src - m[i].srcFrame < g || m[i + 1].srcFrame - src < g || dst - m[i].dstFrame < g || m[i + 1].dstFrame - dst < g)
                return -1;
            m.insert (m.begin() + (std::ptrdiff_t) (i + 1), Marker { src, dst });
            return (int) i + 1;
        }
        return -1;
    }

    // Removes marker i. The first and last markers are the fixed anchors (frame 0 and the
    // take end) and are never removed: every region must sit inside a marker pair.
    inline bool removeAt (std::vector<Marker>& m, std::size_t i)
    {
        const bool anchor = i == 0 || i + 1 == m.size();
        if (i >= m.size() || (anchor && testMutant.load() != mutRemoveAnchor))
            return false;
        m.erase (m.begin() + (std::ptrdiff_t) i);
        return true;
    }
}
