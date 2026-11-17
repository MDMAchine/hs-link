// HOT-STEP LINK -- WarpMarkers.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
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
        bool pinned = false; // F3: AUTO-WARP and Quantize leave a pinned marker where it is
        bool moved() const { return srcFrame != dstFrame; }
    };

    // Smallest distance between two neighbouring markers after a drag.
    constexpr std::int64_t kMinGapFrames = 64;

    // Test mutants (teeth), 0 in the plugin. 1: placement drops the last bar start.
    // 2: a drag is not clamped by its neighbours. 3: the percent is dst over src.
    // 4: scaleToTempo leaves every dst at its src (no scaling). 5: insertAt ignores the
    // minimum gap. 6: removeAt also removes the fixed anchors. 7: insertAt sets dst = src.
    // 8: autoPlaceFromBeats sets dst = src (no beat seeding).
    enum Mutant { mutNone = 0, mutDropLast = 1, mutNoClamp = 2, mutInverse = 3, mutNoScale = 4,
                  mutNoInsertGap = 5, mutRemoveAnchor = 6, mutInsertNoMap = 7, mutNoBeatSeed = 8 };
#if defined (HSLINK_TEST_HOOKS)
    inline std::atomic<int> testMutant { mutNone };
#endif

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
#if defined (HSLINK_TEST_HOOKS)
        if (testMutant.load() == mutDropLast && ! out.empty())
            out.pop_back();
#endif
        return out;
    }

    // Card D slice 2: a simple beat track over onset times (seconds, time order). Starts at
    // the onset nearest startSec (pass TempoMeasure::Result::downbeatSnappedSec), then
    // walks forward: the next beat is the onset nearest last + period inside
    // +-kBeatTolerance * period; the period follows each found interval (half weight), so
    // a slow tempo drift is followed. No onset in the window: that beat is skipped (NaN)
    // and the walk goes on from the prediction. Returns one time per beat index from the
    // start (NaN for a missed beat). Empty when there are no onsets or bpm <= 0.
    constexpr double kBeatTolerance = 0.15;
    // Strength path only (Job 19): after kRelockMisses missed beats in a row the window
    // widens to +-kRelockTolerance of a period until a hit lands, then the period is
    // re-measured from the next onset and the 2% clamp re-centres on it. Covers tempo
    // steps up to about 20%; ramps or steps past 20% are out of scope.
    constexpr int kRelockMisses = 4;
    constexpr double kRelockTolerance = 0.35;
    // Job 21: the quiet fallback is vetoed when any onset within +-kStrongVetoTolerance of
    // the predicted beat clears the strength floor (a strong hit just outside the window
    // means the weak one near the beat is not the beat). Same width as the relock window.
    constexpr double kStrongVetoTolerance = kRelockTolerance;
    namespace detail
    {
    inline std::vector<double> trackBeatsImpl (const std::vector<double>& onsetSecs, const std::vector<double>* strengths,
                                               double bpm, double startSec, double endSec)
    {
        std::vector<double> out;
        if (onsetSecs.empty() || ! (bpm > 0.0))
            return out;
        double period = 60.0 / bpm;
        // First onset on the downbeat's beat lattice (start + j * period, within a
        // kBeatTolerance of a period): the onset at the downbeat itself may be missing (an onset
        // right at the file start has no pre-roll to rise from).
        std::size_t best = onsetSecs.size();
        for (std::size_t i = 0; i < onsetSecs.size(); ++i)
        {
            const double d = onsetSecs[i] - startSec;
            if (d < -0.5 * period)
                continue;
            if (std::abs (d - std::round (d / period) * period) <= kBeatTolerance * period)
            {
                best = i;
                break;
            }
        }
        if (best == onsetSecs.size())
            return out; // no onset on the downbeat lattice: no track
        if (strengths != nullptr && strengths->size() == onsetSecs.size())
        {
            // Strengths are linear and non-negative (onset energies, not dB). The only
            // callers today are tests passing positive literals; no source path feeds this.
           #ifdef jassert
            jassert (std::all_of (strengths->begin(), strengths->end(), [] (double v) { return std::isfinite (v) && v >= 0.0; }));
           #else
            assert (std::all_of (strengths->begin(), strengths->end(), [] (double v) { return std::isfinite (v) && v >= 0.0; }));
           #endif
            // Strength overload: on that lattice beat, take the strongest onset within
            // kBeatTolerance of it; ties go to the onset nearest the lattice beat.
            const double d0 = onsetSecs[best] - startSec;
            const double lattice = startSec + std::round (d0 / period) * period;
            double bestS = -1.0e300, bestD = 1.0e300;
            for (std::size_t i = 0; i < onsetSecs.size(); ++i)
            {
                const double dd = std::abs (onsetSecs[i] - lattice);
                if (dd > kBeatTolerance * period)
                    continue;
                const double st = (*strengths)[i];
                if (st > bestS || (st == bestS && dd < bestD))
                {
                    bestS = st;
                    bestD = dd;
                    best = i;
                }
            }
        }
        double nominal = 60.0 / bpm;
        const bool useStrength = strengths != nullptr && strengths->size() == onsetSecs.size();
        double minStrength = 0.0;
        if (useStrength)
        {
            auto sorted = *strengths;
            std::sort (sorted.begin(), sorted.end());
            minStrength = 0.5 * sorted[sorted.size() / 2];
        }
        double last = onsetSecs[best];
        out.push_back (last);
        double predicted = last;
        int missRun = 0; // strength path: beats missed in a row (relock widens after 4)
        for (int guard = 0; guard < 100000; ++guard)
        {
            predicted += period;
            if (predicted > endSec)
                break;
            // Job 17: with strengths, take the strongest onset in the window that is at
            // least half the median strength (ties to the nearest); the period is clamped
            // to +-2% of 60/bpm so one stray pick cannot drag the walk off the beat.
            // The time-only path (no strengths) is unchanged: nearest pick, no clamp.
            // Job 19: after kRelockMisses misses in a row the strength path widens the
            // window to kRelockTolerance until a hit lands (tempo step relock). The 4th
            // window is already wide: at +20% four windows span five real beats.
            const bool relocking = useStrength && missRun >= kRelockMisses - 1;
            const double tol = relocking ? kRelockTolerance : kBeatTolerance;
            double hit = std::nan (""), hitD = tol * period, hitS = -1.0e300;
            for (std::size_t i = 0; i < onsetSecs.size(); ++i)
            {
                const double s = onsetSecs[i], d = std::abs (s - predicted);
                const double st = useStrength ? (*strengths)[i] : 0.0;
                if (d > tol * period || (useStrength && st < minStrength))
                    continue;
                if (useStrength ? (st > hitS || (st == hitS && d < hitD)) : d <= hitD)
                {
                    hitD = d;
                    hitS = st;
                    hit = s;
                }
            }
            // Job 18: a nearer eligible onset wins over the strongest unless the
            // strongest is at least 2x its strength (nearest such one).
            if (useStrength && ! std::isnan (hit))
            {
                const double strongS = hitS;
                double nearD = hitD;
                for (std::size_t i = 0; i < onsetSecs.size(); ++i)
                {
                    const double s = onsetSecs[i], d = std::abs (s - predicted), st = (*strengths)[i];
                    if (d > tol * period || st < minStrength || d >= nearD || strongS >= 2.0 * st)
                        continue;
                    nearD = d;
                    hit = s;
                }
            }
            // Job 19 (quiet passage): the window has onsets but none clears the strength
            // floor; drop the floor for this window and take the nearest onset, but only
            // within half the window (a lone weak ghost well off the beat stays skipped).
            bool strongNear = false;
            if (useStrength && std::isnan (hit))
                for (std::size_t i = 0; i < onsetSecs.size() && ! strongNear; ++i)
                    strongNear = std::abs (onsetSecs[i] - predicted) <= kStrongVetoTolerance * period
                                 && (*strengths)[i] >= minStrength;
            if (useStrength && std::isnan (hit) && ! strongNear)
            {
                double nearD = 0.5 * tol * period;
                for (std::size_t i = 0; i < onsetSecs.size(); ++i)
                {
                    const double d = std::abs (onsetSecs[i] - predicted);
                    if (d <= nearD)
                    {
                        nearD = d;
                        hit = onsetSecs[i];
                    }
                }
            }
            out.push_back (hit);
            if (useStrength && ! std::isnan (hit) && relocking)
            {
                // Relock hit: re-estimate the period from the next onset after it (the
                // strongest at or above the floor in 0.75..1.25 periods, else the
                // nearest one) and re-centre the 2% clamp on it.
                double nextS = -1.0e300, nextD = 1.0e300, next = std::nan ("");
                bool nextEligible = false;
                for (std::size_t i = 0; i < onsetSecs.size(); ++i)
                {
                    const double dt = onsetSecs[i] - hit;
                    if (dt < 0.75 * period || dt > 1.25 * period)
                        continue;
                    const double st = (*strengths)[i], d = std::abs (dt - period);
                    const bool el = st >= minStrength;
                    if ((el && ! nextEligible) || (el == nextEligible && (el ? (st > nextS || (st == nextS && d < nextD)) : d < nextD)))
                    {
                        nextEligible = el;
                        nextS = st;
                        nextD = d;
                        next = onsetSecs[i];
                    }
                }
                if (! std::isnan (next))
                {
                    nominal = next - hit;
                    period = nominal;
                }
                predicted = hit;
                last = hit;
                missRun = 0;
                continue;
            }
            missRun = std::isnan (hit) ? missRun + 1 : 0;
            if (! std::isnan (hit))
            {
                period = 0.5 * period + 0.5 * (hit - last) / std::max (1.0, std::round ((hit - last) / period));
                if (useStrength)
                    period = std::clamp (period, 0.98 * nominal, 1.02 * nominal);
                predicted = hit;
                last = hit;
            }
        }
        return out;
    }
    } // namespace detail

    inline std::vector<double> trackBeats (const std::vector<double>& onsetSecs, double bpm, double startSec, double endSec)
    {
        return detail::trackBeatsImpl (onsetSecs, nullptr, bpm, startSec, endSec);
    }

    // Job 11 (gap 4): overload with a parallel strength list (same order as onsetSecs).
    // The first beat is the strongest onset within kBeatTolerance of the lattice beat
    // (ties to the one nearest it); later beats take the strongest onset in the window
    // at or above half the median strength (Job 17). A strength
    // list of the wrong size falls back to the time-only pick.
    inline std::vector<double> trackBeats (const std::vector<double>& onsetSecs, const std::vector<double>& strengths,
                                           double bpm, double startSec, double endSec)
    {
        return detail::trackBeatsImpl (onsetSecs, &strengths, bpm, startSec, endSec);
    }

    // Card D slice 2: warp markers seeded from tracked beats. src = the detected beat
    // frame, dst = the matching line of the host grid (hostBpm), counted from the first
    // tracked beat (the downbeat, kept where it is). Plus the fixed anchors at 0 and at
    // length. Confidence gate: confidence < threshold (the downbeatOffsetToApply gate)
    // gives no markers at all (Raw). Beats whose src or dst would come closer than
    // kMinGapFrames to a neighbour or the anchors are dropped, so the list stays sorted.
    // Teeth: testMutant mutNoBeatSeed makes every dst = src (the old identity warp).
    inline std::vector<Marker> autoPlaceFromBeats (std::int64_t length, double sampleRate, const std::vector<double>& beatSecs,
                                                   double hostBpm, double confidence, double threshold)
    {
        std::vector<Marker> out;
        if (length <= 0 || ! (sampleRate > 0.0) || ! (hostBpm > 0.0) || beatSecs.empty() || ! (confidence >= threshold))
            return out;
        const double hostBeat = 60.0 / hostBpm * sampleRate;
        const double origin = beatSecs.front() * sampleRate;
        out.push_back ({ 0, 0 });
        for (std::size_t k = 0; k < beatSecs.size(); ++k)
        {
            if (std::isnan (beatSecs[k]))
                continue;
            const auto src = (std::int64_t) std::llround (beatSecs[k] * sampleRate);
            auto dst = (std::int64_t) std::llround (origin + (double) k * hostBeat);
#if defined (HSLINK_TEST_HOOKS)
            if (testMutant.load() == mutNoBeatSeed)
                dst = src;
#endif
            const auto& prev = out.back();
            if (src - prev.srcFrame < kMinGapFrames || dst - prev.dstFrame < kMinGapFrames
                || src > length - kMinGapFrames || dst > length - kMinGapFrames)
                continue;
            out.push_back ({ src, dst });
        }
        out.push_back ({ length, length });
        return out;
    }

    // dst clamped between the neighbours of marker i (kMinGapFrames from each), and
    // inside 0..length at the ends. Returns the clamped value; markers are not changed.
    inline std::int64_t clampDst (const std::vector<Marker>& m, std::size_t i, std::int64_t dst, std::int64_t length)
    {
#if defined (HSLINK_TEST_HOOKS)
        const int warpMutant = testMutant.load();
#else
        const int warpMutant = mutNone;
#endif
        if (i >= m.size() || warpMutant == mutNoClamp)
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
#if defined (HSLINK_TEST_HOOKS)
        const int warpMutant = testMutant.load();
#else
        const int warpMutant = mutNone;
#endif
        return warpMutant == mutInverse ? dstLen / srcLen * 100.0 : srcLen / dstLen * 100.0;
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
#if defined (HSLINK_TEST_HOOKS)
        const int warpMutant = testMutant.load();
#else
        const int warpMutant = mutNone;
#endif
        const double k = warpMutant == mutNoScale ? 1.0 : fromBpm / toBpm;
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

    // Apply warp renders only the S/E section: the source crop [cropStart, cropEnd) mapped
    // through the markers to the warped output [dstStart, dstEnd), clamped to [0, outLength].
    // Returns false when the mapped section is empty.
    inline bool warpedCropRange (const std::vector<Marker>& m, std::int64_t cropStart, std::int64_t cropEnd,
                                 std::int64_t outLength, std::int64_t& dstStart, std::int64_t& dstEnd)
    {
        auto clampOut = [outLength] (std::int64_t v) { return v < 0 ? (std::int64_t) 0 : v > outLength ? outLength : v; };
        dstStart = clampOut (mapFrame (m, cropStart, true));
        dstEnd = clampOut (mapFrame (m, cropEnd, true));
        return dstEnd > dstStart;
    }

    // Adds a marker at source frame src with dst = the warped position there now, so the
    // audio does not move. Refused (-1) outside the first..last marker or closer than
    // kMinGapFrames (src or dst) to a neighbour. Returns the new index; the list stays
    // sorted, so the neighbour clamps of moveTo hold.
    inline int insertAt (std::vector<Marker>& m, std::int64_t src)
    {
        if (m.size() < 2)
            return -1;
#if defined (HSLINK_TEST_HOOKS)
        const int warpMutant = testMutant.load();
#else
        const int warpMutant = mutNone;
#endif
        const bool noGap = warpMutant == mutNoInsertGap;
        for (std::size_t i = 0; i + 1 < m.size(); ++i)
        {
            if (! (src >= m[i].srcFrame && src <= m[i + 1].srcFrame))
                continue;
            const auto dst = warpMutant == mutInsertNoMap ? src : mapFrame (m, src, true);
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
#if defined (HSLINK_TEST_HOOKS)
        const int warpMutant = testMutant.load();
#else
        const int warpMutant = mutNone;
#endif
        if (i >= m.size() || (anchor && warpMutant != mutRemoveAnchor))
            return false;
        m.erase (m.begin() + (std::ptrdiff_t) i);
        return true;
    }

    // F3: fresh (a new placement) with the pinned markers of old kept as they are. A fresh
    // marker within kMinGapFrames of a pin (src or dst), or on the other side of a pin in
    // src than in dst, is dropped, so the list stays sorted and the neighbour clamps hold.
    inline std::vector<Marker> keepPinned (const std::vector<Marker>& old, const std::vector<Marker>& fresh)
    {
        std::vector<Marker> out;
        for (const auto& q : old)
            if (q.pinned)
                out.push_back (q);
        if (out.empty())
            return fresh;
        const std::size_t pins = out.size();
        for (const auto& f : fresh)
        {
            bool clash = false;
            for (std::size_t k = 0; k < pins && ! clash; ++k)
            {
                const auto& q = out[k];
                const auto ds = f.srcFrame - q.srcFrame, dd = f.dstFrame - q.dstFrame;
                clash = (ds < kMinGapFrames && ds > -kMinGapFrames) || (dd < kMinGapFrames && dd > -kMinGapFrames)
                        || ((ds < 0) != (dd < 0));
            }
            if (! clash)
                out.push_back (f);
        }
        std::sort (out.begin(), out.end(), [] (const Marker& a, const Marker& b) { return a.srcFrame < b.srcFrame; });
        return out;
    }

    inline int pinnedCount (const std::vector<Marker>& m)
    {
        int n = 0;
        for (const auto& x : m)
            n += x.pinned ? 1 : 0;
        return n;
    }
}
