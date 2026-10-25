// HOT-STEP LINK -- TempoMeasure.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#include "TempoMeasure.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    const double kPi = 3.14159265358979323846;
    const double kTwoPi = 2.0 * kPi;
    const int kHarmonics = 4;
    // Beat kernel: a peak at every grid beat, as a short cosine series. Weights sum to 1,
    // so a take whose every onset sits on the grid scores 1.
    const double kHarm[kHarmonics] = { 0.48, 0.24, 0.16, 0.12 };
    const double kKernelRms = 0.4050; // sqrt (sum of kHarm squared / 2)
    // The envelope smoother delays every onset by about this much (measured on clicks).
    const double kLatencySec = 0.0045;
    const std::size_t kMaxOnsets = 6000;

    struct Onset
    {
        double t = 0.0; // seconds
        double w = 0.0; // strength
    };

    struct Curves
    {
        double frameRate = 0.0;
        std::vector<float> all, low; // onset strength per frame: all bands, low band
    };

    bool aborted (const TempoMeasure::AbortFn& f) { return f != nullptr && f(); }

    // Three bands (below 150 Hz, 150 Hz to 3 kHz, above), each: squared, smoothed at
    // 30 Hz (so the cycles of a held bass note are not read as onsets), log, rise over
    // 10 ms, positive part. false = aborted or no signal.
    bool buildCurves (const float* x, std::size_t n, double sr, Curves& c, const TempoMeasure::AbortFn& shouldAbort)
    {
        const int hop = std::max (1, (int) std::lround (sr / 400.0));
        c.frameRate = sr / (double) hop;
        const std::size_t frames = n / (std::size_t) hop;
        if (frames < 16)
            return false;
        auto coef = [sr] (double hz) { return 1.0 - std::exp (-kTwoPi * hz / sr); };
        const double aLow = coef (150.0), aMid = coef (3000.0), aEnv = coef (30.0);
        std::vector<float> env[3];
        for (auto& e : env)
            e.assign (frames, 0.0f);
        double l1 = 0.0, l2 = 0.0, m1 = 0.0;
        double e0[3] = { 0.0, 0.0, 0.0 }, e1[3] = { 0.0, 0.0, 0.0 };
        for (std::size_t f = 0; f < frames; ++f)
        {
            if ((f & 2047) == 0 && aborted (shouldAbort))
                return false;
            const float* p = x + f * (std::size_t) hop;
            for (int i = 0; i < hop; ++i)
            {
                double s = (double) p[i];
                if (! (std::abs (s) < 1.0e6))
                    s = 0.0; // NaN, inf or garbage
                l1 += aLow * (s - l1);
                l2 += aLow * (l1 - l2);
                m1 += aMid * (s - m1);
                const double b[3] = { l2, m1 - l2, s - m1 };
                for (int k = 0; k < 3; ++k)
                {
                    e0[k] += aEnv * (b[k] * b[k] + 1.0e-30 - e0[k]);
                    e1[k] += aEnv * (e0[k] - e1[k]);
                }
            }
            for (int k = 0; k < 3; ++k)
                env[k][f] = (float) e1[k];
        }
        double mean[3] = { 0.0, 0.0, 0.0 }, total = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            for (std::size_t f = 0; f < frames; ++f)
                mean[k] += (double) env[k][f];
            mean[k] /= (double) frames;
            total += mean[k];
        }
        if (! (total > 1.0e-10))
            return false; // silence
        c.all.assign (frames, 0.0f);
        c.low.assign (frames, 0.0f);
        const std::size_t span = 4; // frames, 10 ms
        std::vector<float> lg (frames);
        for (int k = 0; k < 3; ++k)
        {
            if (mean[k] < 1.0e-5 * total)
                continue; // an empty band adds only noise
            const double floorE = 0.02 * mean[k];
            for (std::size_t f = 0; f < frames; ++f)
                lg[f] = (float) std::log ((double) env[k][f] + floorE);
            for (std::size_t f = span; f < frames; ++f)
            {
                const float d = lg[f] - lg[f - span];
                if (d > 0.0f)
                {
                    c.all[f] += d;
                    if (k == 0)
                        c.low[f] = d;
                }
            }
        }
        // Light smoothing, so the peak position can be read between two frames.
        float prev = c.all[0];
        for (std::size_t f = 1; f + 1 < frames; ++f)
        {
            const float cur = c.all[f];
            c.all[f] = 0.25f * prev + 0.5f * cur + 0.25f * c.all[f + 1];
            prev = cur;
        }
        return true;
    }

    // Local maxima of the curve, at least 30 ms apart, sorted by time.
    std::vector<Onset> pickOnsets (const Curves& c)
    {
        std::vector<Onset> out;
        const auto& o = c.all;
        const std::ptrdiff_t n = (std::ptrdiff_t) o.size();
        const std::ptrdiff_t reach = 12;
        const float minRise = 0.3f;
        for (std::ptrdiff_t f = 1; f + 1 < n; ++f)
        {
            const float v = o[(std::size_t) f];
            if (v < minRise)
                continue;
            bool top = true;
            for (std::ptrdiff_t k = std::max<std::ptrdiff_t> (0, f - reach); k < f && top; ++k)
                top = o[(std::size_t) k] < v;
            for (std::ptrdiff_t k = f + 1; k <= std::min (n - 1, f + reach) && top; ++k)
                top = o[(std::size_t) k] <= v;
            if (! top)
                continue;
            const double a = o[(std::size_t) (f - 1)], b = v, d = o[(std::size_t) (f + 1)];
            const double den = a - 2.0 * b + d;
            double delta = den < 0.0 ? 0.5 * (a - d) / den : 0.0;
            delta = std::max (-0.5, std::min (0.5, delta));
            Onset on;
            on.t = ((double) f + delta) / c.frameRate - kLatencySec;
            on.w = b;
            out.push_back (on);
        }
        if (out.empty())
            return out;
        // Drop the small ones, and keep one loud hit from outweighing the rest.
        std::vector<double> ws;
        ws.reserve (out.size());
        for (const auto& on : out)
            ws.push_back (on.w);
        std::sort (ws.begin(), ws.end());
        const double p90 = ws[(std::size_t) ((double) (ws.size() - 1) * 0.9)];
        std::vector<Onset> kept;
        kept.reserve (out.size());
        for (const auto& on : out)
            if (on.w >= 0.15 * p90)
                kept.push_back (on);
        if (kept.size() > kMaxOnsets)
        {
            std::vector<double> kw;
            kw.reserve (kept.size());
            for (const auto& on : kept)
                kw.push_back (on.w);
            std::sort (kw.begin(), kw.end());
            const double cut = kw[kw.size() - kMaxOnsets];
            std::vector<Onset> top;
            top.reserve (kMaxOnsets);
            for (const auto& on : kept)
                if (on.w >= cut && top.size() < kMaxOnsets)
                    top.push_back (on);
            kept.swap (top);
        }
        ws.clear();
        for (const auto& on : kept)
            ws.push_back (on.w);
        std::sort (ws.begin(), ws.end());
        const double cap = 4.0 * ws[ws.size() / 2];
        for (auto& on : kept)
            on.w = std::min (on.w, cap);
        return kept;
    }

    // Coarse tempo: comb over the autocorrelation of the onset curve at half its frame
    // rate. Several tempos that are multiples of one another score alike on a steady
    // take; the fastest one within 10 percent of the best is returned (the octave is
    // settled later). 0 = none found or aborted.
    double coarseTempo (const Curves& c, double hintBpm, const TempoMeasure::AbortFn& shouldAbort, double& strength)
    {
        const std::size_t m = c.all.size() / 2;
        const double rate = c.frameRate * 0.5;
        if (m < 64)
            return 0.0;
        std::vector<float> a (m);
        double mean = 0.0;
        for (std::size_t i = 0; i < m; ++i)
        {
            a[i] = c.all[2 * i] + c.all[2 * i + 1];
            mean += (double) a[i];
        }
        mean /= (double) m;
        for (auto& v : a)
            v -= (float) mean;
        const std::size_t maxLag = std::min (m / 2, (std::size_t) std::ceil (4.0 * 60.0 * rate / TempoMeasure::kMinBpm) + 2);
        std::vector<double> acf (maxLag + 1, 0.0);
        for (std::size_t lag = 0; lag <= maxLag; ++lag)
        {
            if ((lag & 31) == 0 && aborted (shouldAbort))
                return 0.0;
            double s = 0.0;
            const float* p = a.data();
            const float* q = a.data() + lag;
            const std::size_t count = m - lag;
            for (std::size_t i = 0; i < count; ++i)
                s += (double) (p[i] * q[i]);
            acf[lag] = s / (double) count;
        }
        if (! (acf[0] > 0.0))
            return 0.0;
        const bool hinted = hintBpm >= 30.0 && hintBpm <= 300.0;
        const double step = 0.25;
        const int count = (int) std::floor ((TempoMeasure::kMaxBpm - TempoMeasure::kMinBpm) / step) + 1;
        std::vector<double> score ((std::size_t) count, 0.0);
        double best = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double bpm = TempoMeasure::kMinBpm + step * (double) i;
            const double lag = 60.0 * rate / bpm;
            double s = 0.0, wsum = 0.0;
            for (int h = 1; h <= 4; ++h)
            {
                const double pos = lag * (double) h;
                if (pos > (double) maxLag - 1.0)
                    break;
                const std::size_t i0 = (std::size_t) pos;
                const double fr = pos - (double) i0;
                s += (acf[i0] * (1.0 - fr) + acf[i0 + 1] * fr) / (double) h;
                wsum += 1.0 / (double) h;
            }
            if (wsum <= 0.0)
                continue;
            s /= wsum * acf[0];
            if (hinted && s > 0.0)
            {
                // A small lead for the octaves of the asked tempo.
                double d = std::log2 (bpm / hintBpm);
                d -= std::round (d);
                s *= 1.0 + 0.15 * std::exp (-d * d / (2.0 * 0.03 * 0.03));
            }
            score[(std::size_t) i] = s;
            best = std::max (best, s);
        }
        if (! (best > 0.0))
            return 0.0;
        strength = best;
        for (int i = count - 1; i >= 0; --i)
        {
            const double s = score[(std::size_t) i];
            if (s < 0.9 * best)
                continue;
            const double left = i > 0 ? score[(std::size_t) (i - 1)] : -1.0e30;
            const double right = i + 1 < count ? score[(std::size_t) (i + 1)] : -1.0e30;
            if (s < left || s < right)
                continue;
            double delta = 0.0;
            if (i > 0 && i + 1 < count)
            {
                const double den = left - 2.0 * s + right;
                if (den < 0.0)
                    delta = std::max (-0.5, std::min (0.5, 0.5 * (left - right) / den));
            }
            return TempoMeasure::kMinBpm + step * ((double) i + delta);
        }
        return 0.0;
    }

    struct GridFit
    {
        double score = 0.0; // 0..1: share of the onset weight that sits on the grid
        double psi = 0.0;   // beat phase in radians at the centre time
    };

    // Onset energy on the beat grid at this tempo, at the best beat phase.
    GridFit fitGrid (const std::vector<Onset>& on, double centre, double weightSum, double bpm)
    {
        const double omega = kTwoPi * bpm / 60.0;
        double fr[kHarmonics] = { 0.0, 0.0, 0.0, 0.0 }, fi[kHarmonics] = { 0.0, 0.0, 0.0, 0.0 };
        for (const auto& o : on)
        {
            const double th = omega * (o.t - centre);
            const double c1 = std::cos (th), s1 = std::sin (th);
            const double c2 = c1 * c1 - s1 * s1, s2 = 2.0 * c1 * s1;
            const double c3 = c2 * c1 - s2 * s1, s3 = c2 * s1 + s2 * c1;
            const double c4 = c2 * c2 - s2 * s2, s4 = 2.0 * c2 * s2;
            fr[0] += o.w * c1; fi[0] += o.w * s1;
            fr[1] += o.w * c2; fi[1] += o.w * s2;
            fr[2] += o.w * c3; fi[2] += o.w * s3;
            fr[3] += o.w * c4; fi[3] += o.w * s4;
        }
        auto value = [&fr, &fi] (double psi)
        {
            double g = 0.0;
            for (int h = 0; h < kHarmonics; ++h)
            {
                const double a = psi * (double) (h + 1);
                g += kHarm[h] * (fr[h] * std::cos (a) + fi[h] * std::sin (a));
            }
            return g;
        };
        const int steps = 64;
        double bestPsi = 0.0, bestG = -1.0e300;
        for (int i = 0; i < steps; ++i)
        {
            const double psi = kTwoPi * (double) i / (double) steps;
            const double g = value (psi);
            if (g > bestG)
            {
                bestG = g;
                bestPsi = psi;
            }
        }
        double h = kTwoPi / (double) steps;
        for (int it = 0; it < 4; ++it)
        {
            const double a = value (bestPsi - h), b = value (bestPsi), d = value (bestPsi + h);
            const double den = a - 2.0 * b + d;
            if (den < 0.0)
            {
                const double cand = bestPsi + h * std::max (-1.0, std::min (1.0, 0.5 * (a - d) / den));
                const double g = value (cand);
                if (g >= b)
                {
                    bestPsi = cand;
                    bestG = g;
                }
            }
            h *= 0.25;
        }
        GridFit r;
        r.score = weightSum > 0.0 ? bestG / weightSum : 0.0;
        r.psi = bestPsi;
        return r;
    }

    // Best tempo on a kFineStepBpm grid from lo to hi, read between two grid points.
    double fineTempo (const std::vector<Onset>& on, double centre, double weightSum, double lo, double hi,
                      const TempoMeasure::AbortFn& shouldAbort)
    {
        const double step = TempoMeasure::kFineStepBpm;
        const int count = std::max (3, (int) std::ceil ((hi - lo) / step) + 1);
        std::vector<double> score ((std::size_t) count);
        int best = 0;
        for (int i = 0; i < count; ++i)
        {
            if ((i & 15) == 0 && aborted (shouldAbort))
                return 0.0;
            score[(std::size_t) i] = fitGrid (on, centre, weightSum, lo + step * (double) i).score;
            if (score[(std::size_t) i] > score[(std::size_t) best])
                best = i;
        }
        double delta = 0.0;
        if (best > 0 && best + 1 < count)
        {
            const double a = score[(std::size_t) (best - 1)], b = score[(std::size_t) best], d = score[(std::size_t) (best + 1)];
            const double den = a - 2.0 * b + d;
            if (den < 0.0)
                delta = std::max (-0.5, std::min (0.5, 0.5 * (a - d) / den));
        }
        return lo + step * ((double) best + delta);
    }

    double firstBeatOf (double centre, double psi, double bpm)
    {
        const double period = 60.0 / bpm;
        const double t0 = centre + psi / (kTwoPi * bpm / 60.0);
        return t0 - std::floor (t0 / period) * period;
    }

    // Sum of the onset weights within tol of the grid points first + (k + frac) * period.
    double energyOnGrid (const std::vector<Onset>& on, double first, double period, double frac, double duration)
    {
        const double tol = std::min (0.035, 0.15 * period);
        double sum = 0.0;
        std::size_t j = 0;
        for (double t = first + frac * period; t < duration; t += period)
        {
            while (j < on.size() && on[j].t < t - tol)
                ++j;
            for (std::size_t k = j; k < on.size() && on[k].t <= t + tol; ++k)
                sum += on[k].w;
        }
        return sum;
    }
}

const char* TempoMeasure::levelName (Level level)
{
    switch (level)
    {
        case Level::high:   return "high";
        case Level::medium: return "medium";
        case Level::low:    return "low";
        case Level::none:   break;
    }
    return "none";
}

TempoMeasure::Result TempoMeasure::analyse (const float* mono, std::size_t numSamples, double sampleRate,
                                            double hintBpm, const AbortFn& shouldAbort)
{
    Result none;
    if (mono == nullptr || ! (sampleRate >= 4000.0) || ! (sampleRate <= 768000.0))
        return none;
    const double duration = (double) numSamples / sampleRate;
    none.why.rule = "shorter than kMinSeconds";
    if (duration < kMinSeconds)
        return none;

    Curves curves;
    none.why.rule = "no signal or aborted";
    if (! buildCurves (mono, numSamples, sampleRate, curves, shouldAbort))
        return none;
    const auto onsets = pickOnsets (curves);
    none.why.onsets = (int) onsets.size();
    none.why.rule = "fewer than 8 onsets";
    if (onsets.size() < 8)
        return none; // a pad, a drone, one hit: nothing to count

    const double coarse = coarseTempo (curves, hintBpm, shouldAbort, none.why.coarseStrength);
    none.why.coarseBpm = coarse;
    none.why.rule = "no coarse tempo";
    if (! (coarse > 0.0))
        return none;

    double weightSum = 0.0, weightSq = 0.0;
    for (const auto& o : onsets)
    {
        weightSum += o.w;
        weightSq += o.w * o.w;
    }
    if (! (weightSum > 0.0))
        return none;
    const double centre = 0.5 * duration;

    double bpm = fineTempo (onsets, centre, weightSum, coarse * 0.98 - 0.2, coarse * 1.02 + 0.2, shouldAbort);
    none.why.rule = "aborted";
    if (! (bpm > 0.0) || aborted (shouldAbort))
        return none;

    // Half or double. First the octave the take itself points to: the one inside 70 to
    // 180; where two fit (70 to 90 and its double), the faster one when the half beats
    // of the slower carry onsets too.
    const bool hinted = hintBpm >= 30.0 && hintBpm <= 300.0;
    double hintFit = 0.0;
    {
        // Half a BPM of room at both ends, so a take at 70 or at 180 stays what it is.
        while (bpm < 69.5)
            bpm *= 2.0;
        while (bpm > 180.5)
            bpm *= 0.5;
        const double slow = bpm >= 139.0 ? bpm * 0.5 : bpm;
        if (slow >= 69.5 && slow <= 90.25)
        {
            const auto fit = fitGrid (onsets, centre, weightSum, slow);
            const double first = firstBeatOf (centre, fit.psi, slow);
            const double onBeat = energyOnGrid (onsets, first, 60.0 / slow, 0.0, duration);
            const double offBeat = energyOnGrid (onsets, first, 60.0 / slow, 0.5, duration);
            bpm = offBeat >= 0.5 * onBeat ? slow * 2.0 : slow;
        }
    }
    // With a hint: the octave nearest the asked tempo, but only inside the search range
    // and only when the onsets fit its grid about as well. A take that plays a 140 feel
    // is not called 70 because 70 was asked.
    if (hinted)
    {
        const double asked = bpm * std::pow (2.0, std::round (std::log2 (hintBpm / bpm)));
        if (asked != bpm && asked >= kMinBpm && asked <= kMaxBpm)
        {
            const double own = fitGrid (onsets, centre, weightSum, bpm).score;
            hintFit = own > 0.0 ? fitGrid (onsets, centre, weightSum, asked).score / own : 0.0;
            if (hintFit >= kHintOctaveFit)
                bpm = asked;
        }
    }
    // The octave step scales the error with it: settle on the fine grid once more.
    bpm = fineTempo (onsets, centre, weightSum, bpm - 6.0 * kFineStepBpm, bpm + 6.0 * kFineStepBpm, shouldAbort);
    if (! (bpm > 0.0))
        return none;
    const auto fit = fitGrid (onsets, centre, weightSum, bpm);
    const double period = 60.0 / bpm;
    const double first = firstBeatOf (centre, fit.psi, bpm);
    const int beats = first < duration ? (int) std::floor ((duration - first) / period) + 1 : 0;
    none.why.bpm = bpm;
    none.why.gridShare = fit.score;
    none.why.rule = "fewer than 8 beats";
    if (beats < 8)
        return none;

    // Confidence: how much of the onset weight sits on the grid, and how far that is
    // above what onsets at random times would reach.
    const double chance = std::sqrt (weightSq) / weightSum * kKernelRms;
    const double z = chance > 0.0 ? fit.score / chance : 0.0;
    auto clamp01 = [] (double v) { return std::max (0.0, std::min (1.0, v)); };
    const double confidence = std::sqrt (clamp01 ((z - 5.0) / 7.0) * clamp01 ((fit.score - 0.05) / 0.30));

    // Drift: per beat the strongest onset near it, then the mean distance per window.
    const double tol = std::min (0.08, 0.3 * period);
    const int window = beats < 32 ? 4 : 8;
    double driftMax = 0.0, driftEnd = 0.0;
    {
        std::size_t j = 0;
        double sumW = 0.0, sumD = 0.0;
        int matched = 0;
        for (int k = 0; k < beats; ++k)
        {
            const double t = first + period * (double) k;
            while (j < onsets.size() && onsets[j].t < t - tol)
                ++j;
            double bestW = 0.0, bestD = 0.0;
            for (std::size_t i = j; i < onsets.size() && onsets[i].t <= t + tol; ++i)
                if (onsets[i].w > bestW)
                {
                    bestW = onsets[i].w;
                    bestD = onsets[i].t - t;
                }
            if (bestW > 0.0)
            {
                sumW += bestW;
                sumD += bestW * bestD;
                ++matched;
            }
            if ((k + 1) % window == 0 || k + 1 == beats)
            {
                if (matched >= std::max (2, window / 4) && sumW > 0.0)
                {
                    const double mean = 1000.0 * sumD / sumW;
                    driftMax = std::max (driftMax, std::abs (mean));
                    driftEnd = mean;
                }
                sumW = sumD = 0.0;
                matched = 0;
            }
        }
    }

    // Downbeat guess: of the 4 beat positions, the one with the most low-band onset
    // energy (all bands when the low band is empty). A later position must lead by
    // 5 percent, so even beats give the first beat.
    double downbeat = first;
    {
        double acc[2][4] = { { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 } };
        const std::ptrdiff_t frames = (std::ptrdiff_t) curves.all.size();
        for (int k = 0; k < beats; ++k)
        {
            const auto mid = (std::ptrdiff_t) std::llround ((first + period * (double) k + kLatencySec) * curves.frameRate);
            float lowMax = 0.0f, allMax = 0.0f;
            for (std::ptrdiff_t f = std::max<std::ptrdiff_t> (0, mid - 10); f <= std::min (frames - 1, mid + 10); ++f)
            {
                lowMax = std::max (lowMax, curves.low[(std::size_t) f]);
                allMax = std::max (allMax, curves.all[(std::size_t) f]);
            }
            acc[0][k & 3] += (double) lowMax;
            acc[1][k & 3] += (double) allMax;
        }
        const double lowSum = acc[0][0] + acc[0][1] + acc[0][2] + acc[0][3];
        const double* use = lowSum > 1.0e-6 ? acc[0] : acc[1];
        int bestClass = 0;
        for (int i = 1; i < 4; ++i)
            if (use[i] > use[bestClass] * 1.05)
                bestClass = i;
        downbeat = first + period * (double) bestClass;
    }

    Result r;
    r.why = none.why;
    r.why.significance = z;
    r.why.driftMs = driftMax;
    r.why.confidenceRaw = confidence;
    r.why.rule = confidence < kLowConfidence ? "confidence under kLowConfidence"
                 : driftMax > kWanderMs && confidence >= kMediumConfidence ? "drift over kWanderMs"
                 : confidence < kMediumConfidence ? "confidence under kMediumConfidence"
                 : driftMax > kSteadyMs && confidence >= kHighConfidence ? "drift over kSteadyMs"
                 : confidence < kHighConfidence ? "confidence under kHighConfidence" : "high";
    r.confidence = confidence;
    r.level = confidence >= kHighConfidence ? Level::high
              : confidence >= kMediumConfidence ? Level::medium
              : confidence >= kLowConfidence ? Level::low : Level::none;
    if (driftMax > kWanderMs && r.level > Level::low)
        r.level = Level::low;
    else if (driftMax > kSteadyMs && r.level > Level::medium)
        r.level = Level::medium;
    if (r.level == Level::none)
        return r; // no number the detector does not have
    r.valid = r.level >= Level::medium;
    r.bpm = bpm;
    r.downbeatSec = downbeat;
    r.driftMs = driftMax;
    r.driftEndMs = driftEnd;
    r.firstBeatSec = first;
    r.beats = beats;
    r.hintFit = hintFit;
    if (hinted)
    {
        const double oct = std::round (std::log2 (bpm / hintBpm));
        if (std::abs (bpm / (hintBpm * std::pow (2.0, oct)) - 1.0) <= kHintOctaveTol)
            r.hintOctave = (int) oct;
    }
    return r;
}
