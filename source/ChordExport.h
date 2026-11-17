// HOT-STEP LINK -- ChordExport.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include "MidiExport.h"
#include <array>
#include <complex>
#include <limits>

// Chord export (<name>_chords.mid), plan Section 3 (round C1).
//   Chroma: 4096 FFT, 1024 hop, 130-2000 Hz, harmonic weighting (sqrt magnitude, tilted
//   down 1/sqrt(f/55) so upper partials count less than fundamentals), L1 normalised,
//   averaged per beat on the bar grid. Bass chroma (55-260 Hz) picks the root bonus.
//   Templates: 12 roots x maj, min, 7, maj7, m7, sus2, sus4, dim, aug, plus N.
//   Cosine similarity; sevenths -0.03; in-key chords +0.05; bass = root +0.05.
//   Key: whole-region chroma against Krumhansl-Kessler major / minor profiles.
//   Viterbi smoothing: per-bar (default; a mid-bar change pays a margin and must last
//   2 beats) or per-beat. Voicing: root 36-47, close voicing 48-64 with minimal movement,
//   velocity 90, legato, one chord-name text event per chord. N writes nothing.
// Message thread or worker thread, never the audio thread.
namespace ChordExport
{
    using MidiExport::kPpq;
    using MidiExport::KeySig;
    using MidiExport::TempoMap;

    constexpr int kFftOrder = 12;
    constexpr int kFftSize = 1 << kFftOrder;  // 4096
    constexpr int kHop = 1024;
    constexpr double kLoHz = 55.0, kHiHz = 2000.0, kBassHiHz = 260.0;
    // Chroma floor: at 44.1k a 4096 bin is 10.8 Hz, narrower than a semitone only above
    // about 190 Hz; below 130 Hz a C2 fundamental leaked into the D bin (C read as Csus2).
   #if HSLINK_TEETH_CHORD_LOW_FLOOR
    constexpr double kChromaLoHz = 55.0; // teeth: old floor
   #else
    constexpr double kChromaLoHz = 130.0;
   #endif
    constexpr double kSeventhPenalty = 0.03, kKeyBonus = 0.05, kBassBonus = 0.05;
    constexpr double kChangeCost = 0.15;       // any chord change
    constexpr double kMidBarMargin = 0.30;     // extra cost of a mid-bar change (per-bar mode)
    constexpr double kNScore = 0.35;           // N emission on a sounding beat
    constexpr double kSilentRel = 1.0e-3;      // beat energy below this x loudest beat = silent
    constexpr int kChordVel = 90, kBassChannel = 2, kChordChannel = 3;

    enum Quality { Maj = 0, Min, Dom7, Maj7, Min7, Sus2, Sus4, Dim, Aug, kNumQualities };
    constexpr int kNumStates = 12 * kNumQualities + 1; // last = N
    constexpr int kNState = kNumStates - 1;

    enum class Mode { PerBar, PerBeat };

    inline const std::vector<int>& qualityIntervals (int q)
    {
        static const std::vector<int> iv[kNumQualities] = {
            { 0, 4, 7 }, { 0, 3, 7 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10 },
            { 0, 2, 7 }, { 0, 5, 7 }, { 0, 3, 6 }, { 0, 4, 8 } };
        return iv[juce::jlimit (0, kNumQualities - 1, q)];
    }

    inline const char* qualitySuffix (int q)
    {
        static const char* s[kNumQualities] = { "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim", "aug" };
        return s[juce::jlimit (0, kNumQualities - 1, q)];
    }

    inline bool isSeventh (int q) { return q == Dom7 || q == Maj7 || q == Min7; }

    inline juce::String pcName (int pc, bool flats)
    {
        static const char* sh[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const char* fl[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
        pc = ((pc % 12) + 12) % 12;
        return flats ? fl[pc] : sh[pc];
    }

    inline juce::String chordName (int root, int q, bool flats) { return pcName (root, flats) + qualitySuffix (q); }

    // Key signature for a tonic pitch class (major or minor), -7..7.
    inline KeySig keySigFor (int tonicPc, bool minor)
    {
        static const int major[12] = { 0, -5, 2, -3, 4, -1, 6, 1, -4, 3, -2, 5 };
        const int rel = minor ? (tonicPc + 3) % 12 : tonicPc % 12;
        return { major[((rel % 12) + 12) % 12], minor };
    }

    struct Key { int tonic = 0; bool minor = false; KeySig sig {}; };

    // Pearson correlation of chroma against the 24 rotated key profiles.
    inline Key detectKey (const std::array<double, 12>& chroma)
    {
        static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
        static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
        auto corr = [&] (const double* prof, int tonic)
        {
            double mx = 0.0, my = 0.0;
            for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) i]; my += prof[i]; }
            mx /= 12.0; my /= 12.0;
            double sxy = 0.0, sxx = 0.0, syy = 0.0;
            for (int i = 0; i < 12; ++i)
            {
                const double x = chroma[(size_t) ((i + tonic) % 12)] - mx, y = prof[i] - my;
                sxy += x * y; sxx += x * x; syy += y * y;
            }
            return (sxx > 0.0 && syy > 0.0) ? sxy / std::sqrt (sxx * syy) : -1.0;
        };
        Key best; double bestR = -2.0;
        for (int t = 0; t < 12; ++t)
            for (int m = 0; m < 2; ++m)
            {
                const double r = corr (m ? mnr : maj, t);
                if (r > bestR) { bestR = r; best.tonic = t; best.minor = m != 0; }
            }
        best.sig = keySigFor (best.tonic, best.minor);
        return best;
    }

    // Scale pitch classes; minor also allows the raised 7th (harmonic minor V).
    inline bool inKey (int root, int q, const Key& k)
    {
        static const int majS[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static const int minS[8] = { 0, 2, 3, 5, 7, 8, 10, 11 };
        bool s[12] = {};
        if (k.minor) for (int d : minS) s[(k.tonic + d) % 12] = true;
        else         for (int d : majS) s[(k.tonic + d) % 12] = true;
        for (int iv : qualityIntervals (q))
            if (! s[(root + iv) % 12])
                return false;
        return true;
    }

    // In-place radix-2 complex FFT.
    inline void fft (std::vector<std::complex<double>>& a)
    {
        const size_t n = a.size();
        for (size_t i = 1, j = 0; i < n; ++i)
        {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap (a[i], a[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1)
        {
            const double ang = -2.0 * juce::MathConstants<double>::pi / (double) len;
            const std::complex<double> wl (std::cos (ang), std::sin (ang));
            for (size_t i = 0; i < n; i += len)
            {
                std::complex<double> w (1.0, 0.0);
                for (size_t k = 0; k < len / 2; ++k)
                {
                    const auto u = a[i + k], v = a[i + k + len / 2] * w;
                    a[i + k] = u + v;
                    a[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    struct Beat
    {
        double startTicks = 0.0, endTicks = 0.0; // absolute file ticks (before any origin shift)
        bool barStart = false;
        std::array<double, 12> chroma {}, bass {};
        double energy = 0.0;
        int frames = 0;
    };

    // Per-beat chroma on the beat grid of (bpm or map, downbeatSec, beatsPerBar/den).
    inline std::vector<Beat> beatChroma (const juce::AudioBuffer<float>& buf, double sampleRate, double bpm,
                                         double downbeatSec, int beatsPerBar, int den = 4, const TempoMap& map = {})
    {
        std::vector<Beat> beats;
        const int ns = buf.getNumSamples(), nch = buf.getNumChannels();
        if (ns <= 0 || nch <= 0 || sampleRate <= 0.0 || ! (bpm > 0.0))
            return beats;
        const bool useMap = map.size() >= 2;
        const auto st = useMap ? MidiExport::segTicks (map) : std::vector<double>();
        const double tps = MidiExport::ticksPerSecond (bpm);
        auto toT = [&] (double s) { return useMap ? MidiExport::secToTicks (map, st, s) : std::max (0.0, s) * tps; };
        const double beatT = 4.0 / (double) MidiExport::validDen (den) * (double) kPpq;
        const int bpb = std::max (1, beatsPerBar);
        const double durT = toT ((double) ns / sampleRate);
        const double dbT = toT (std::max (0.0, downbeatSec));
        const long long bMin = -(long long) std::floor (dbT / beatT + 1.0e-9);
        for (long long b = bMin; dbT + (double) b * beatT < durT - 1.0e-6; ++b)
        {
            Beat bt;
            bt.startTicks = std::max (0.0, dbT + (double) b * beatT);
            bt.endTicks = std::min (durT, dbT + (double) (b + 1) * beatT);
            bt.barStart = ((b % bpb) + bpb) % bpb == 0;
            beats.push_back (bt);
        }
        if (beats.empty())
            return beats;

        std::vector<float> mono ((size_t) ns, 0.0f);
        for (int ch = 0; ch < nch; ++ch)
        {
            const float* p = buf.getReadPointer (ch);
            for (int i = 0; i < ns; ++i) mono[(size_t) i] += p[i] / (float) nch;
        }
        std::vector<double> win ((size_t) kFftSize);
        for (int i = 0; i < kFftSize; ++i)
            win[(size_t) i] = 0.5 - 0.5 * std::cos (2.0 * juce::MathConstants<double>::pi * i / (kFftSize - 1));
        // Bin -> pitch class and weight, once.
        std::vector<int> binPc ((size_t) kFftSize / 2, -1);
        std::vector<double> binW ((size_t) kFftSize / 2, 0.0);
        std::vector<bool> binBass ((size_t) kFftSize / 2, false);
        for (int k = 1; k < kFftSize / 2; ++k)
        {
            const double f = k * sampleRate / kFftSize;
            if (f < kLoHz || f > kHiHz) continue;
            const double fm = 69.0 + 12.0 * std::log2 (f / 440.0);
            const int midi = (int) std::lround (fm);
            binPc[(size_t) k] = ((midi % 12) + 12) % 12;
            // Full chroma weight only where a bin is narrower than a semitone (kChromaLoHz).
            // Below it a bin counts only as far as it sits near a pitch-class centre
            // (1 at the centre, 0 at a quarter tone), so a C2 fundamental no longer leaks
            // into the D bin. The whole 55-260 Hz band still feeds bass[] for the root bonus.
            const double centre = f >= kChromaLoHz ? 1.0 : std::max (0.0, 1.0 - 2.0 * std::abs (fm - midi));
            binW[(size_t) k] = centre / std::sqrt (f / kLoHz);
            binBass[(size_t) k] = f <= kBassHiHz;
        }
        std::vector<std::complex<double>> a ((size_t) kFftSize);
        size_t bi = 0;
        for (int start = -kFftSize / 2; start < ns; start += kHop)
        {
            const double centreT = toT ((start + kFftSize / 2) / sampleRate);
            while (bi + 1 < beats.size() && centreT >= beats[bi].endTicks) ++bi;
            if (centreT < beats[bi].startTicks || centreT >= beats[bi].endTicks) continue;
            for (int i = 0; i < kFftSize; ++i)
            {
                const int s = start + i;
                a[(size_t) i] = { (s >= 0 && s < ns) ? mono[(size_t) s] * win[(size_t) i] : 0.0, 0.0 };
            }
            fft (a);
            std::array<double, 12> c {}, bs {};
            double e = 0.0;
            for (int k = 1; k < kFftSize / 2; ++k)
            {
                if (binPc[(size_t) k] < 0) continue;
                const double m = std::abs (a[(size_t) k]);
                e += m * m;
                const double v = std::sqrt (m) * binW[(size_t) k];
                c[(size_t) binPc[(size_t) k]] += v;
                if (binBass[(size_t) k]) bs[(size_t) binPc[(size_t) k]] += m;
            }
            double sum = 0.0;
            for (double v : c) sum += v;
            auto& bt = beats[bi];
            if (sum > 0.0)
                for (int i = 0; i < 12; ++i) bt.chroma[(size_t) i] += c[(size_t) i] / sum;
            for (int i = 0; i < 12; ++i) bt.bass[(size_t) i] += bs[(size_t) i];
            bt.energy += e;
            ++bt.frames;
        }
        for (auto& bt : beats)
            if (bt.frames > 0)
            {
                double sum = 0.0;
                for (double v : bt.chroma) sum += v;
                if (sum > 0.0) for (auto& v : bt.chroma) v /= sum;
                bt.energy /= bt.frames;
            }
        return beats;
    }

    struct Chord
    {
        double startTicks = 0.0, endTicks = 0.0; // absolute file ticks, on beat or bar ticks
        int root = 0, quality = Maj;
        juce::String name;
    };

    // Tonality gate: tonality = energy-weighted mean over non-silent beats of the chroma
    // peakiness max(chroma) / mean(chroma) (1 = flat, 12 = one pitch class). Unpitched
    // takes (drums, noise) score low and get no chords.
   #if HSLINK_TEETH_CHORD_GATE_OFF
    static constexpr double kMinTonality = 0.0;   // teeth: gate off
   #elif HSLINK_TEETH_CHORD_GATE_HIGH
    static constexpr double kMinTonality = 100.0; // teeth: gate rejects everything
   #else
    static constexpr double kMinTonality = 1.9; // measured: drums 1.33, noise 1.33; tonal 2.76 to 3.95
   #endif

    struct ChordResult { std::vector<Chord> chords; Key key; double tonality = 0.0; };

    // Emission score of state s for a beat (bonuses included).
    inline double stateScore (const Beat& bt, int s, const Key& key, bool useKeyBonus = true)
    {
        const int root = s / kNumQualities, q = s % kNumQualities;
        const auto& iv = qualityIntervals (q);
        double dot = 0.0, cc = 0.0;
        for (int iv1 : iv) dot += bt.chroma[(size_t) ((root + iv1) % 12)];
        for (double v : bt.chroma) cc += v * v;
        if (cc <= 0.0) return 0.0;
        double sc = dot / (std::sqrt (cc) * std::sqrt ((double) iv.size()));
        if (isSeventh (q)) sc -= kSeventhPenalty;
       #if HSLINK_TEETH_CHORD_NO_KEY
        useKeyBonus = false; // teeth: no in-key bonus
       #endif
        if (useKeyBonus && inKey (root, q, key)) sc += kKeyBonus;
        // Bass-heavy mix: weight the lowest strong pitch class as the root.
        int bassPc = -1; double bmax = 0.0;
        for (int i = 0; i < 12; ++i)
            if (bt.bass[(size_t) i] > bmax) { bmax = bt.bass[(size_t) i]; bassPc = i; }
       #if HSLINK_TEETH_CHORD_ROOT_SWAP
        if (bassPc == (root + 7) % 12) sc += kBassBonus; // teeth: bass bonus on the fifth
       #else
        if (bassPc == root) sc += kBassBonus;
       #endif
        return sc;
    }

    // Full analysis: per-beat chroma, key, template scores, Viterbi, merge into chords.
    inline ChordResult analyseChords (const juce::AudioBuffer<float>& buf, double sampleRate, double bpm,
                                      double downbeatSec, int beatsPerBar, int den = 4, const TempoMap& map = {},
                                      Mode mode = Mode::PerBar)
    {
        ChordResult res;
        const auto beats = beatChroma (buf, sampleRate, bpm, downbeatSec, beatsPerBar, den, map);
        if (beats.empty()) return res;
        std::array<double, 12> total {};
        double loud = 0.0;
        for (const auto& bt : beats)
        {
            for (int i = 0; i < 12; ++i) total[(size_t) i] += bt.chroma[(size_t) i] * bt.energy;
            loud = std::max (loud, bt.energy);
        }
        res.key = detectKey (total);
        {
            double wsum = 0.0, psum = 0.0;
            for (const auto& bt : beats)
            {
                if (! (bt.energy > kSilentRel * loud) || bt.frames == 0) continue;
                double mx = 0.0, sm = 0.0;
                for (double v : bt.chroma) { mx = std::max (mx, v); sm += v; }
                if (sm <= 0.0) continue;
                psum += bt.energy * (mx / (sm / 12.0));
                wsum += bt.energy;
            }
            res.tonality = wsum > 0.0 ? psum / wsum : 0.0;
        }
        if (res.tonality < kMinTonality) return res; // unpitched: no chords
        const size_t nb = beats.size();
        std::vector<std::array<double, kNumStates>> em (nb);
        for (size_t b = 0; b < nb; ++b)
        {
            const bool silent = ! (beats[b].energy > kSilentRel * loud) || beats[b].frames == 0;
            for (int s = 0; s < kNState; ++s)
                em[b][(size_t) s] = silent ? -1.0 : stateScore (beats[b], s, res.key);
            em[b][(size_t) kNState] = silent ? 1.0 : kNScore;
        }
        // Viterbi (maximise score minus change costs).
        std::vector<std::array<int, kNumStates>> bp (nb);
        std::array<double, kNumStates> dp = em[0], nx {};
        for (size_t b = 1; b < nb; ++b)
        {
           #if HSLINK_TEETH_CHORD_NO_SMOOTH
            const double cost = 0.0; // teeth: no change cost, no mid-bar margin
           #else
            const double cost = kChangeCost + ((mode == Mode::PerBar && ! beats[b].barStart) ? kMidBarMargin : 0.0);
           #endif
            int bestPrev = 0;
            for (int s = 1; s < kNumStates; ++s)
                if (dp[(size_t) s] > dp[(size_t) bestPrev]) bestPrev = s;
            for (int s = 0; s < kNumStates; ++s)
            {
                const double stay = dp[(size_t) s], move = dp[(size_t) bestPrev] - cost;
                if (stay >= move || bestPrev == s) { nx[(size_t) s] = stay + em[b][(size_t) s]; bp[b][(size_t) s] = s; }
                else { nx[(size_t) s] = move + em[b][(size_t) s]; bp[b][(size_t) s] = bestPrev; }
            }
            dp = nx;
        }
        std::vector<int> path (nb);
        int s = 0;
        for (int i = 1; i < kNumStates; ++i)
            if (dp[(size_t) i] > dp[(size_t) s]) s = i;
        for (size_t b = nb; b-- > 0;)
        {
            path[b] = s;
            if (b > 0) s = bp[b][(size_t) s];
        }
        // Per-bar mode: a change off the bar line must last at least 2 beats.
       #if HSLINK_TEETH_CHORD_NO_SMOOTH
        if (false)
       #else
        if (mode == Mode::PerBar)
       #endif
            for (size_t b = 1; b < nb; ++b)
                if (path[b] != path[b - 1] && ! beats[b].barStart)
                {
                    size_t e = b;
                    while (e < nb && path[e] == path[b]) ++e;
                    if (e - b < 2)
                        for (size_t k = b; k < e; ++k) path[k] = path[b - 1];
                }
        const bool flats = res.key.sig.sharpsOrFlats < 0;
        for (size_t b = 0; b < nb;)
        {
            size_t e = b;
            while (e < nb && path[e] == path[b]) ++e;
            if (path[b] != kNState)
            {
                Chord c;
                c.startTicks = beats[b].startTicks;
                c.endTicks = beats[e - 1].endTicks;
                c.root = path[b] / kNumQualities;
                c.quality = path[b] % kNumQualities;
                c.name = chordName (c.root, c.quality, flats);
                res.chords.push_back (c);
            }
            b = e;
        }
        return res;
    }

    // Close voicing in 48-64 nearest the previous one (first: centred near 56).
    inline std::vector<int> voiceChord (int root, int q, const std::vector<int>& prev)
    {
        const auto& iv = qualityIntervals (q);
        std::vector<int> best;
        double bestCost = std::numeric_limits<double>::max();
        for (size_t r = 0; r < iv.size(); ++r)
        {
            std::vector<int> v;
            int n = 48 + (((root + iv[r]) % 12) - 48 % 12 + 12) % 12;
            for (size_t k = 0; k < iv.size(); ++k)
            {
                const int pc = (root + iv[(r + k) % iv.size()]) % 12;
                if (k > 0) { n = v.back() + 1; while (((n % 12) + 12) % 12 != pc) ++n; }
                v.push_back (n);
            }
            const double over = (double) std::max (0, v.back() - 64);
            double mean = 0.0;
            for (int x : v) mean += x;
            mean /= (double) v.size();
            double cost = 0.0;
            if (prev.empty()) cost = std::abs (mean - 56.0);
            else if (prev.size() == v.size()) for (size_t k = 0; k < v.size(); ++k) cost += std::abs (v[k] - prev[k]);
            else { double pm = 0.0; for (int x : prev) pm += x; cost = std::abs (mean - pm / (double) prev.size()) * (double) v.size(); }
            cost += over * 100.0;
            if (cost < bestCost) { bestCost = cost; best = v; }
        }
        return best;
    }

    // Chords file: tempo track (with KeySig) plus one chords track: bass root on channel
    // kBassChannel (36-47), close voicing on kChordChannel, velocity 90, legato, a text
    // event per chord name. originTicks: the same export origin the hits and melody use.
    inline juce::MidiFile buildChordsFile (const std::vector<Chord>& chords, const juce::String& trackName,
                                           double bpm, int beatsPerBar, int den = 4,
                                           std::optional<KeySig> key = std::nullopt, const TempoMap& map = {},
                                           double originTicks = 0.0)
    {
        const double origin = std::isfinite (originTicks) ? std::max (0.0, originTicks) : 0.0;
        juce::MidiMessageSequence h;
        auto name = juce::MidiMessage::textMetaEvent (3, trackName);
        name.setTimeStamp (0.0);
        h.addEvent (name);
        std::vector<int> prev;
        double last = 0.0;
        for (size_t i = 0; i < chords.size(); ++i)
        {
            const auto& c = chords[i];
            double on = std::round (c.startTicks - origin), off = std::round (c.endTicks - origin);
            if (i + 1 < chords.size()) off = std::round (chords[i + 1].startTicks - origin); // legato
            if (off <= 0.0) continue;
            on = std::max (0.0, on);
            if (off < on + 1.0) off = on + 1.0;
            auto txt = juce::MidiMessage::textMetaEvent (1, c.name);
            txt.setTimeStamp (on);
            h.addEvent (txt);
           #if HSLINK_TEETH_CHORD_ROOT_SWAP
            std::vector<int> notes { 36 + ((c.root + 7) % 12 + 12) % 12 }; // teeth: bass = fifth
           #else
            std::vector<int> notes { 36 + ((c.root % 12) + 12) % 12 };
           #endif
            const auto v = voiceChord (c.root, c.quality, prev);
            prev = v;
            for (size_t k = 0; k < notes.size() + v.size(); ++k)
            {
                const bool bass = k < notes.size();
                const int nn = bass ? notes[k] : v[k - notes.size()];
                const int ch = bass ? kBassChannel : kChordChannel;
                auto a = juce::MidiMessage::noteOn (ch, nn, (juce::uint8) kChordVel);
                a.setTimeStamp (on);
                auto b = juce::MidiMessage::noteOff (ch, nn);
                b.setTimeStamp (off);
                h.addEvent (a);
                h.addEvent (b);
            }
            last = std::max (last, off);
        }
        h.updateMatchedPairs();
        auto end = juce::MidiMessage::endOfTrack();
        end.setTimeStamp (last);
        h.addEvent (end);
        juce::MidiFile f;
        f.setTicksPerQuarterNote (kPpq);
        f.addTrack (MidiExport::tempoTrack (bpm, beatsPerBar, den, key, map, origin));
        f.addTrack (h);
        return f;
    }
}
