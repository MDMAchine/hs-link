// HOT-STEP LINK -- MidiExport.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// MIDI export of one take: two type 1 files at kPpq.
//   <name>_tempo.mid: tempo, time signature and a "Bar N" marker at every bar start.
//   <name>_hits.mid:  one GM drum note (channel 10) per detected onset, the note from
//                     classifyHits (kick 36, snare 38, closed hat 42, open hat 46, low
//                     tom 45, high tom 50, crash 49, ride 51), velocity from the onset peak relative to the take's loudest hit.
// Tick 0 is the file start; ticks are seconds through the tempo map (one fixed tempo
// when the map is empty, else one tempo event per warp segment). Message thread or a
// worker thread, never the audio thread.
namespace MidiExport
{
    constexpr int kPpq = 960;
    constexpr int kHitNote = 36;          // GM kick (bass drum 1)
    constexpr int kDrumChannel = 10;      // GM drums, 1-based as juce::MidiMessage
    constexpr int kHitTicks = kPpq / 4;   // a 16th note at kPpq (ticks are tempo-free)
    constexpr double kPeakWindowSec = 0.03; // the hit's peak is read over this window
    constexpr double kFloorDb = -60.0;    // velocity 1 at or below this peak, 127 at 0 dBFS
    constexpr double kDefaultBpm = 120.0; // hits timing when the take has no usable tempo

    constexpr int kKickNote = 36, kSnareNote = 38, kClosedHatNote = 42, kOpenHatNote = 46, kPercNote = 39;
    constexpr int kLowTomNote = 45, kHighTomNote = 50, kCrashNote = 49, kRideNote = 51;
    constexpr int kVelMin = 30;            // a hit kVelRangeDb below the loudest one
    constexpr double kVelRangeDb = 36.0;   // loudest hit 127, this far below -> kVelMin

    struct Hit { double sec = 0.0; float peak = 0.0f; int note = kHitNote; };

    // One tempo segment: from startSec (file time) on, quarter notes at bpm. A tempo map
    // is a list of these sorted by startSec, the first at 0; empty = one fixed tempo.
    struct TempoSeg { double startSec = 0.0; double bpm = 120.0; };
    using TempoMap = std::vector<TempoSeg>;

    // Key signature meta (FF 59): -7..7 flats/sharps, major or minor. Not supplied yet.
    struct KeySig { int sharpsOrFlats = 0; bool minor = false; };

    // Time-signature denominator: a power of two 1..64, else 4.
    inline int validDen (int den)
    {
        return (den >= 1 && den <= 64 && (den & (den - 1)) == 0) ? den : 4;
    }

    // Quarter notes per bar for num/den (7/8 -> 3.5, so 3360 ticks at kPpq).
    inline double quartersPerBar (int beatsPerBar, int den)
    {
        return (double) std::max (1, beatsPerBar) * 4.0 / (double) validDen (den);
    }

    // Peak dBFS kFloorDb..0 linearly to velocity 1..127.
    inline int velocityForPeak (float peak)
    {
        const double db = peak > 0.0f ? 20.0 * std::log10 ((double) peak) : kFloorDb;
        const double x = (std::clamp (db, kFloorDb, 0.0) - kFloorDb) / -kFloorDb;
        return (int) std::lround (1.0 + x * 126.0);
    }

    // Velocity relative to the take: peak dB below loudestPeak, 0..kVelRangeDb -> 127..kVelMin.
    inline int velocityRelative (float peak, float loudestPeak)
    {
        if (! (loudestPeak > 0.0f) || ! (peak > 0.0f))
            return kVelMin;
        const double below = 20.0 * std::log10 ((double) loudestPeak / (double) peak);
        const double x = std::clamp (below / kVelRangeDb, 0.0, 1.0);
        return (int) std::lround (127.0 - x * (127.0 - (double) kVelMin));
    }

    // Spectral features of a hit, from mono samples starting at the onset.
    struct DrumFeatures { double low = 0.0, mid = 0.0, high = 0.0; double decaySec = 0.0; };

    // Band energy split over the first 50 ms (low: lowpass 150 Hz, high: highpass
    // 5 kHz, mid: the rest) and the decay time of the
    // high band (10 ms frames until 20 dB under its loudest frame, up to n samples).
    inline DrumFeatures drumFeatures (const float* x, int n, double sr)
    {
        DrumFeatures f;
        if (x == nullptr || n <= 0 || ! (sr > 0.0))
            return f;
        // RBJ biquads, Q 0.707: lowpass 150 Hz, highpass 5 kHz (direct form 1).
        struct Bq
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            Bq (bool high, double fc, double rate)
            {
                const double w = 2.0 * juce::MathConstants<double>::pi * std::min (fc, 0.45 * rate) / rate;
                const double c = std::cos (w), al = std::sin (w) / (2.0 * 0.7071), a0 = 1.0 + al;
                b1 = (high ? -(1.0 + c) : (1.0 - c)) / a0;
                b0 = b2 = (high ? (1.0 + c) : (1.0 - c)) * 0.5 / a0;
                a1 = -2.0 * c / a0;
                a2 = (1.0 - al) / a0;
            }
            double run (double x)
            {
                const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                return y;
            }
        };
        Bq lpf (false, 150.0, sr), hpf (true, 5000.0, sr);
        const int nBand = std::min (n, (int) std::ceil (0.05 * sr));
        const int frame = std::max (1, (int) std::lround (0.01 * sr));
        double tot = 0.0, lo = 0.0, hi = 0.0;
        double frameE = 0.0, bestE = 0.0, lastLoud = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double s = (double) x[i];
            const double l2 = lpf.run (s);
            const double hp2 = hpf.run (s);
            if (i < nBand)
            {
                tot += s * s;
                lo += l2 * l2;
                hi += hp2 * hp2;
            }
            frameE += hp2 * hp2;
            if ((i + 1) % frame == 0)
            {
                bestE = std::max (bestE, frameE);
                if (frameE > 0.0 && frameE >= bestE * 0.01) // within 20 dB of the loudest frame
                    lastLoud = (double) (i + 1) / sr;
                frameE = 0.0;
            }
        }
        if (tot > 0.0)
        {
            f.low = std::min (1.0, lo / tot);
            f.high = std::min (1.0 - f.low, hi / tot);
            f.mid = std::max (0.0, 1.0 - f.low - f.high);
        }
        f.decaySec = lastLoud;
        return f;
    }

    // GM drum note for a hit's features (absolute band split of the first 50 ms):
    // mostly low = kick; mostly high = hat or cymbal by how long the highs ring (closed
    // hat, open hat past 120 ms, ride past 250 ms with some mid bell, crash past 350 ms);
    // mid-heavy with real lows and few highs = tom (low tom when the lows are strong);
    // mid with some highs = snare; anything else falls back to snare (mid-heavy) or
    // closed hat, never a catch-all one note.
    // Teeth (HSLINK_TEETH=MIDI_ONE_NOTE): every hit on kHitNote, absolute velocity.
    inline int classifyDrum (const DrumFeatures& f)
    {
#if defined (HSLINK_TEETH_MIDI_ONE_NOTE)
        juce::ignoreUnused (f);
        return kHitNote;
#else
        if (f.low >= 0.5)
            return kKickNote;
        if (f.high >= 0.45)
        {
            if (f.decaySec > 0.35) return kCrashNote;
            if (f.decaySec > 0.25 && f.mid >= 0.2) return kRideNote;
            return f.decaySec > 0.12 ? kOpenHatNote : kClosedHatNote;
        }
        if (f.mid >= 0.6 && f.low >= 0.15 && f.high < 0.03)
            return f.low >= 0.3 ? kLowTomNote : kHighTomNote;
        if (f.high >= 0.03 && f.low < 0.35)
            return kSnareNote;
        return f.mid >= 0.5 ? kSnareNote : kClosedHatNote;
#endif
    }

    // Notes for all hits of a take. A full mix carries bass under every onset, so the
    // absolute split can put every hit in one class (Alex 2026-11-10: "just c note").
    // When 4 or more hits all land on one note and their features actually vary, the
    // hits are re-classed relative to the take's median band split: clearly more low
    // than typical = kick, clearly more high = hat (open when the highs ring past
    // 120 ms), clearly more mid with fewer highs = snare, the rest closed hat.
    // Deterministic: depends only on the features, in order.
    inline std::vector<int> classifyHits (const std::vector<DrumFeatures>& fs)
    {
        std::vector<int> notes;
        notes.reserve (fs.size());
        for (const auto& f : fs)
            notes.push_back (classifyDrum (f));
#if ! defined (HSLINK_TEETH_MIDI_ONE_NOTE)
        if (fs.size() < 4)
            return notes;
        bool allSame = true;
        for (int n : notes)
            allSame = allSame && n == notes.front();
        if (! allSame)
            return notes;
        auto median = [&fs] (double DrumFeatures::* m)
        {
            std::vector<double> v;
            for (const auto& f : fs) v.push_back (f.*m);
            std::sort (v.begin(), v.end());
            const size_t h = v.size() / 2;
            return v.size() % 2 ? v[h] : 0.5 * (v[h - 1] + v[h]);
        };
        const double mLow = median (&DrumFeatures::low), mMid = median (&DrumFeatures::mid), mHigh = median (&DrumFeatures::high);
        double spread = 0.0;
        for (const auto& f : fs)
            spread = std::max ({ spread, std::abs (f.low - mLow), std::abs (f.high - mHigh) });
        if (spread < 0.02) // the same sound every time: one note is right
            return notes;
        const double t = std::max (0.02, 0.25 * spread);
        for (size_t i = 0; i < fs.size(); ++i)
        {
            const auto& f = fs[i];
            const double dl = f.low - mLow, dm = f.mid - mMid, dh = f.high - mHigh;
            if (dl > t && dl >= dh)
                notes[i] = kKickNote;
            else if (dh > t)
                notes[i] = f.decaySec > 0.12 ? kOpenHatNote : kClosedHatNote;
            else if (dm > t || (dl < -t && dh >= 0.0))
                notes[i] = kSnareNote;
            else
                notes[i] = kClosedHatNote;
        }
#endif
        return notes;
    }

    // Hits from onset times in a take: per onset the peak over kPeakWindowSec and a GM
    // note from the channel mix over up to 400 ms after it (cut at the next onset).
    inline std::vector<Hit> hitsFromOnsets (const juce::AudioBuffer<float>& buf, double sr, const std::vector<double>& secs)
    {
        std::vector<Hit> out;
        std::vector<DrumFeatures> feats;
        std::vector<float> mono;
        const int nCh = buf.getNumChannels(), nS = buf.getNumSamples();
        const float g = 1.0f / (float) std::max (1, nCh);
        for (size_t k = 0; k < secs.size(); ++k)
        {
            const int a = juce::jlimit (0, nS, (int) std::floor (secs[k] * sr));
            double endSec = secs[k] + 0.4;
            if (k + 1 < secs.size() && secs[k + 1] > secs[k])
                endSec = std::min (endSec, secs[k + 1]);
            const int b = juce::jlimit (a, nS, (int) std::floor (endSec * sr));
            mono.assign ((size_t) (b - a), 0.0f);
            for (int ch = 0; ch < nCh; ++ch)
            {
                const float* p = buf.getReadPointer (ch);
                for (int i = a; i < b; ++i)
                    mono[(size_t) (i - a)] += p[i] * g;
            }
            Hit h;
            h.sec = secs[k];
            const int pb = juce::jlimit (a, nS, a + (int) std::ceil (kPeakWindowSec * sr));
            for (int ch = 0; ch < nCh; ++ch)
                if (pb > a)
                    h.peak = std::max (h.peak, buf.getMagnitude (ch, a, pb - a));
            feats.push_back (drumFeatures (mono.data(), (int) mono.size(), sr));
            out.push_back (h);
        }
        const auto notes = classifyHits (feats);
        for (size_t k = 0; k < out.size(); ++k)
            out[k].note = notes[k];
        return out;
    }

    // MIDI-only onset gate: keeps an onset when at least one of three bands (under 150 Hz,
    // 150 Hz..3 kHz, over 3 kHz) shows a drum-like transient: the energy over [t-5, t+25] ms
    // is at least kGateRise times the energy over [t-45, t-10] ms, and the energy 60..120 ms
    // later (cut before the next onset) has fallen to at most kGateSustain of it. Sustained
    // tonal material (horns, pads, vibrato, slow attacks) rises slowly or holds, so it fails.
    // The tempo detector keeps its own onsets; this only thins what becomes MIDI hits.
    // Teeth (HSLINK_TEETH=MIDI_NO_GATE): every onset passes.
    constexpr double kGateRise = 2.0, kGateSustain = 0.7;
    inline std::vector<double> percussiveOnsets (const juce::AudioBuffer<float>& buf, double sr, const std::vector<double>& secs)
    {
#if defined (HSLINK_TEETH_MIDI_NO_GATE)
        juce::ignoreUnused (buf, sr);
        return secs;
#else
        std::vector<double> out;
        const int nCh = buf.getNumChannels(), nS = buf.getNumSamples();
        if (nCh <= 0 || nS <= 0 || ! (sr > 0.0))
            return out;
        // Squared band signals, one pass over the channel mix.
        std::vector<float> e[3];
        for (auto& v : e)
            v.assign ((size_t) nS, 0.0f);
        const double aL = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 150.0 / sr);
        const double aH = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 3000.0 / sr);
        const double g = 1.0 / (double) nCh;
        double l = 0.0, m = 0.0;
        double mean[3] = { 0.0, 0.0, 0.0 };
        for (int i = 0; i < nS; ++i)
        {
            double x = 0.0;
            for (int ch = 0; ch < nCh; ++ch)
                x += (double) buf.getSample (ch, i) * g;
            l += aL * (x - l);
            m += aH * (x - m);
            const double b[3] = { l, m - l, x - m };
            for (int k = 0; k < 3; ++k)
            {
                e[k][(size_t) i] = (float) (b[k] * b[k]);
                mean[k] += b[k] * b[k];
            }
        }
        for (auto& v : mean)
            v /= (double) nS;
        auto avg = [&] (int k, double s0, double s1)
        {
            const int a = juce::jlimit (0, nS, (int) std::floor (s0 * sr));
            const int b = juce::jlimit (0, nS, (int) std::floor (s1 * sr));
            if (b <= a)
                return 0.0;
            double acc = 0.0;
            for (int i = a; i < b; ++i)
                acc += (double) e[k][(size_t) i];
            return acc / (double) (b - a);
        };
        for (size_t j = 0; j < secs.size(); ++j)
        {
            const double t = secs[j];
            const double nextT = j + 1 < secs.size() ? secs[j + 1] : 1.0e9;
            bool keep = false;
            for (int k = 0; k < 3 && ! keep; ++k)
            {
                const double floorE = 1.0e-4 * mean[k] + 1.0e-12;
                const double pre = avg (k, t - 0.045, t - 0.010) + floorE;
                const double post = avg (k, t - 0.005, t + 0.025) + floorE;
                if (post < kGateRise * pre)
                    continue;
                const double lateEnd = std::min (t + 0.120, nextT - 0.005);
                if (lateEnd - (t + 0.060) < 0.020)
                {
                    keep = true; // the next hit comes too soon to read the sustain
                    continue;
                }
                const double late = avg (k, t + 0.060, lateEnd) + floorE;
                keep = late <= kGateSustain * post;
            }
            if (keep)
                out.push_back (t);
        }
        return out;
#endif
    }

    // Export quantize: tick moved toward the nearest grid line (gridTicks apart, lines at
    // offsetTicks + n * gridTicks) by strength 0..1. gridTicks <= 0 or strength 0: unchanged.
    inline double quantizeTick (double tick, double gridTicks, double offsetTicks, double strength)
    {
        if (! (gridTicks > 0.0) || ! (strength > 0.0))
            return tick;
        const double q = offsetTicks + std::round ((tick - offsetTicks) / gridTicks) * gridTicks;
        return tick + std::min (1.0, strength) * (q - tick);
    }

    // Grid in ticks for a quantize choice: 0 off, 16 = 1/16, 8 = 1/8.
    inline double quantizeGridTicks (int div) { return div > 0 ? 4.0 * (double) kPpq / (double) div : 0.0; }

    inline double ticksPerSecond (double bpm) { return bpm / 60.0 * (double) kPpq; }

    // Tempo map of a warped file: one segment per marker pair, starting at the pair's
    // dst time, at srcBpm * src span / dst span (the source's fixed tempo sped up or
    // slowed by that region's warp). Bad pairs are skipped. Fewer than 2 markers: empty.
    // Teeth (HSLINK_TEETH=MIDI_SINGLE_TEMPO): always empty, so one tempo is written.
    template <typename MarkerT>
    inline TempoMap tempoMapFromWarp (const std::vector<MarkerT>& m, double srcBpm, double sampleRate)
    {
        TempoMap out;
#if defined (HSLINK_TEETH_MIDI_SINGLE_TEMPO)
        juce::ignoreUnused (m, srcBpm, sampleRate);
        return out;
#else
        if (m.size() < 2 || ! (srcBpm > 0.0) || ! (sampleRate > 0.0))
            return out;
        // A pair whose ratio matches the running segment's within what kMergeFrames of
        // rounding on either span explains is the same tempo (dst frames are rounded,
        // so |d * accS - s * accD| <= accS + s for one exact ratio): it extends
        // that segment, whose bpm is then taken over the summed spans.
        constexpr double kMergeFrames = 1.5;
        double accS = 0.0, accD = 0.0;
        for (size_t i = 0; i + 1 < m.size(); ++i)
        {
            const double s = (double) (m[i + 1].srcFrame - m[i].srcFrame);
            const double d = (double) (m[i + 1].dstFrame - m[i].dstFrame);
            if (! (s > 0.0 && d > 0.0))
                continue;
            if (! out.empty() && std::abs (d * accS - s * accD) <= kMergeFrames * (accS + s))
            {
                accS += s;
                accD += d;
                out.back().bpm = srcBpm * accS / accD;
                continue;
            }
            const double start = out.empty() ? 0.0 : std::max (0.0, (double) m[i].dstFrame / sampleRate);
            const double b = srcBpm * s / d;
            if (std::isfinite (b) && b > 0.0 && (out.empty() || start > out.back().startSec))
            {
                out.push_back ({ start, b });
                accS = s;
                accD = d;
            }
        }
        return out;
#endif
    }

    // Start tick of every segment (integrated across the map at kPpq).
    inline std::vector<double> segTicks (const TempoMap& map)
    {
        std::vector<double> t (map.size(), 0.0);
        for (size_t i = 1; i < map.size(); ++i)
            t[i] = t[i - 1] + (map[i].startSec - map[i - 1].startSec) * ticksPerSecond (map[i - 1].bpm);
        return t;
    }

    // File seconds to ticks through the map (unrounded). Before 0: 0.
    inline double secToTicks (const TempoMap& map, const std::vector<double>& st, double sec)
    {
        sec = std::max (0.0, sec);
        size_t i = 0;
        while (i + 1 < map.size() && map[i + 1].startSec <= sec)
            ++i;
        return st[i] + (sec - map[i].startSec) * ticksPerSecond (map[i].bpm);
    }

    // The hits inside [startSec, endSec), their times made relative to startSec.
    inline std::vector<Hit> hitsInRegion (const std::vector<Hit>& hits, double startSec, double endSec)
    {
        std::vector<Hit> out;
        for (auto h : hits)
            if (h.sec >= startSec && h.sec < endSec)
            {
                h.sec -= startSec;
                out.push_back (h);
            }
        return out;
    }

    // The map seen from startSec on, with startSec as the new 0 (empty stays empty).
    inline TempoMap shiftTempoMap (const TempoMap& map, double startSec)
    {
        TempoMap out;
        for (size_t i = 0; i < map.size(); ++i)
        {
            if (i + 1 < map.size() && map[i + 1].startSec <= startSec)
                continue;
            out.push_back ({ std::max (0.0, map[i].startSec - startSec), map[i].bpm });
        }
        return out;
    }

    // Peak of any channel in [sec, sec + kPeakWindowSec).
    inline float peakAt (const juce::AudioBuffer<float>& buf, double sampleRate, double sec)
    {
        const int a = juce::jlimit (0, buf.getNumSamples(), (int) std::floor (sec * sampleRate));
        const int b = juce::jlimit (a, buf.getNumSamples(), a + (int) std::ceil (kPeakWindowSec * sampleRate));
        float p = 0.0f;
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            if (b > a)
                p = std::max (p, buf.getMagnitude (ch, a, b - a));
        return p;
    }

    inline juce::MidiMessageSequence tempoTrack (double bpm, int beatsPerBar, int den = 4,
                                                 std::optional<KeySig> key = std::nullopt,
                                                 const TempoMap& map = {}, double originTicks = 0.0)
    {
        juce::MidiMessageSequence t;
        auto name = juce::MidiMessage::textMetaEvent (3, "Tempo");
        name.setTimeStamp (0.0);
        t.addEvent (name);
        auto tempo = juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / bpm));
        tempo.setTimeStamp (0.0);
        if (map.size() < 2)
            t.addEvent (tempo);
        else
        {
            // Tick 0 carries the tempo in effect at the origin; segments that start at or
            // before the origin do not get their own (collapsed) events.
            const auto st = segTicks (map);
            size_t first = 0;
            while (first + 1 < map.size() && std::round (st[first + 1] - originTicks) <= 0.0)
                ++first;
            for (size_t i = first; i < map.size(); ++i)
            {
                auto e = juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / map[i].bpm));
                e.setTimeStamp (i == first ? 0.0 : std::round (st[i] - originTicks));
                t.addEvent (e);
            }
        }
        auto sig = juce::MidiMessage::timeSignatureMetaEvent (beatsPerBar, validDen (den));
        sig.setTimeStamp (0.0);
        t.addEvent (sig);
        if (key.has_value())
        {
            auto ks = juce::MidiMessage::keySignatureMetaEvent (juce::jlimit (-7, 7, key->sharpsOrFlats), key->minor);
            ks.setTimeStamp (0.0);
            t.addEvent (ks);
        }
        return t;
    }

    // Tempo file: bar markers from downbeatSec every bar (num/den, bpm in quarter notes)
    // up to durationSec.
    inline juce::MidiFile buildTempoFile (double bpm, double downbeatSec, int beatsPerBar, double durationSec,
                                          int den = 4, std::optional<KeySig> key = std::nullopt,
                                          const TempoMap& map = {}, double originTicks = 0.0)
    {
        auto t = tempoTrack (bpm, beatsPerBar, den, key, map, originTicks);
        if (map.size() >= 2)
        {
            // Bars every quartersPerBar in tick space from the downbeat's tick, so bar
            // lines follow the tempo map.
            const auto st = segTicks (map);
            const double barTicks = quartersPerBar (beatsPerBar, den) * (double) kPpq;
            const double durTicks = secToTicks (map, st, durationSec);
            double first = secToTicks (map, st, downbeatSec) - originTicks;
            while (first - barTicks >= -1.0e-6)
                first -= barTicks;
            int n = 1;
            double last = 0.0;
            for (double k = first; k < durTicks; k += barTicks, ++n)
            {
                auto m = juce::MidiMessage::textMetaEvent (6, "Bar " + juce::String (n));
                m.setTimeStamp (std::round (std::max (0.0, k)));
                t.addEvent (m);
                last = m.getTimeStamp();
            }
            auto end = juce::MidiMessage::endOfTrack();
            end.setTimeStamp (std::max (last, std::round (durTicks - originTicks)));
            t.addEvent (end);
            t.updateMatchedPairs();
            juce::MidiFile f;
            f.setTicksPerQuarterNote (kPpq);
            f.addTrack (t);
            return f;
        }
        const double tps = ticksPerSecond (bpm);
        const double barSec = 60.0 / bpm * quartersPerBar (beatsPerBar, den);
        double first = downbeatSec - originTicks / tps;
        while (first - barSec >= -1.0e-9)
            first -= barSec; // a full bar before the downbeat is a bar too
        int n = 1;
        double last = 0.0;
        for (double s = first; s < durationSec; s += barSec, ++n)
        {
            auto m = juce::MidiMessage::textMetaEvent (6, "Bar " + juce::String (n));
            m.setTimeStamp (std::round (std::max (0.0, s) * tps));
            t.addEvent (m);
            last = m.getTimeStamp();
        }
        auto end = juce::MidiMessage::endOfTrack();
        end.setTimeStamp (std::max (last, std::round (durationSec * tps - originTicks)));
        t.addEvent (end);
        t.updateMatchedPairs();
        juce::MidiFile f;
        f.setTicksPerQuarterNote (kPpq);
        f.addTrack (t);
        return f;
    }

    // Export origin in ticks: subtracted from every event so the downbeat lands on 0 or a
    // whole bar. A hit before that origin (a pickup) moves the origin one bar earlier.
    inline double exportOriginTicks (const std::vector<Hit>& hits, double bpm, int beatsPerBar, int den,
                                     const TempoMap& map, double downbeatSec)
    {
        const bool useMap = map.size() >= 2;
        const auto st = useMap ? segTicks (map) : std::vector<double>();
        const double tps = ticksPerSecond (bpm);
        const double barTicks = quartersPerBar (beatsPerBar, den) * (double) kPpq;
        const double downTicks = useMap ? secToTicks (map, st, downbeatSec) : downbeatSec * tps;
        if (! (barTicks > 0.0) || ! std::isfinite (downTicks))
            return 0.0;
        double shift = downTicks - std::round (downTicks / barTicks) * barTicks;
        double minTick = 1.0e300;
        for (const auto& hit : hits)
            minTick = std::min (minTick, useMap ? std::round (secToTicks (map, st, hit.sec))
                                                : std::round (std::max (0.0, hit.sec) * tps));
        while (! hits.empty() && minTick - shift < -0.5)
            shift -= barTicks;
        // Never negative: a downbeat that rounds up to the next bar would leave a gap before
        // the first tempo event. Hits before the origin clamp to tick 0.
        while (shift < 0.0)
            shift += barTicks;
        return shift;
    }

    // Hits file: a tempo track (so the hits line up when opened alone) and the hits track.
    inline juce::MidiFile buildHitsFile (const std::vector<Hit>& hits, double bpm, int beatsPerBar,
                                         int den = 4, std::optional<KeySig> key = std::nullopt,
                                         const TempoMap& map = {}, int quantDiv = 0,
                                         double quantStrength = 0.0, double downbeatSec = 0.0)
    {
        const double tps = ticksPerSecond (bpm);
        const bool useMap = map.size() >= 2;
        const auto st = useMap ? segTicks (map) : std::vector<double>();
        juce::MidiMessageSequence h;
        auto name = juce::MidiMessage::textMetaEvent (3, "Hits");
        name.setTimeStamp (0.0);
        h.addEvent (name);
        double last = 0.0;
        float loudest = 0.0f;
        for (const auto& hit : hits)
            loudest = std::max (loudest, hit.peak);
        std::vector<double> ons;
        // One origin for the whole export (same value buildTempoFile takes): the downbeat
        // lands on tick 0 or a whole bar, so the quantize grid is plain multiples of grid.
        const double origin = exportOriginTicks (hits, bpm, beatsPerBar, den, map, downbeatSec);
        for (const auto& hit : hits)
            ons.push_back (std::max (0.0, (useMap ? std::round (secToTicks (map, st, hit.sec))
                                                  : std::round (std::max (0.0, hit.sec) * tps)) - origin));
        if (const double grid = quantizeGridTicks (quantDiv); grid > 0.0 && quantStrength > 0.0)
        {
            for (auto& o : ons)
                o = std::max (0.0, std::round (quantizeTick (o, grid, 0.0, quantStrength)));
        }
        for (size_t i = 0; i < hits.size(); ++i)
        {
            const auto& hit = hits[i];
            const double on = ons[i];
            // Same tick and pitch (after quantize or tick rounding): keep one note, the
            // louder hit (first one on a tie).
            bool dup = false;
            for (size_t j = 0; j < hits.size() && ! dup; ++j)
                if (j != i && hits[j].note == hit.note && ons[j] == on
                    && (hits[j].peak > hit.peak || (hits[j].peak == hit.peak && j < i)))
                    dup = true;
            if (dup)
                continue;
            // A 16th note, ended before the next hit on the same pitch.
            double len = (double) kHitTicks;
            for (size_t j = 0; j < hits.size(); ++j)
                if (j != i && hits[j].note == hit.note && ons[j] > on)
                    len = std::min (len, ons[j] - on);
#if defined (HSLINK_TEETH_MIDI_ONE_NOTE)
            const auto vel = (juce::uint8) velocityForPeak (hit.peak);
#else
            const auto vel = (juce::uint8) velocityRelative (hit.peak, loudest);
#endif
            const int note = juce::jlimit (0, 127, hit.note);
            auto a = juce::MidiMessage::noteOn (kDrumChannel, note, vel);
            a.setTimeStamp (on);
            auto b = juce::MidiMessage::noteOff (kDrumChannel, note);
            b.setTimeStamp (on + len);
            h.addEvent (a);
            h.addEvent (b);
            last = std::max (last, on + len);
        }
        h.updateMatchedPairs();
        auto end = juce::MidiMessage::endOfTrack();
        end.setTimeStamp (last);
        h.addEvent (end);
        juce::MidiFile f;
        f.setTicksPerQuarterNote (kPpq);
#if ! defined (HSLINK_TEETH_MIDI_TRACKS)
        f.addTrack (tempoTrack (bpm, beatsPerBar, den, key, map, origin));
#endif
        f.addTrack (h);
        return f;
    }

    // ---- Pitched notes (tonal takes) ----
    constexpr double kPitchWinSec = 2048.0 / 44100.0; // analysis window (2048 at 44.1 kHz)
    constexpr double kPitchHopSec = 256.0 / 44100.0;  // hop (256 at 44.1 kHz)
    constexpr double kPitchMinHz = 40.0, kPitchMaxHz = 1500.0;
    constexpr double kYinThresh = 0.15;      // CMNDF dip under this = confident pitch
    constexpr double kOctaveSlack = 0.1;     // prefer tau/2 when its dip is this close to the best
    constexpr double kPitchGateDb = -50.0;   // frames quieter than this (RMS dBFS) are unpitched
    constexpr double kMinNoteSec = 0.06;     // a note holds its pitch at least this long
    constexpr double kTonalFraction = 0.4;   // over this share of frames pitched = tonal take
    constexpr int kNoteChannel = 1;

    // Note velocity from the level: peak RMS in the first 50 ms, -30 dB -> 20, 0 dB -> 127.
    inline int velocityForLevel (double rmsPeak)
    {
        if (! (rmsPeak > 0.0))
            return 1;
        const double db = 20.0 * std::log10 (rmsPeak);
        return juce::jlimit (1, 127, (int) std::lround (20.0 + (db + 30.0) / 30.0 * 107.0));
    }

    // vel 0 = velocity relative to the loudest note (from rms); 1..127 = absolute.
    struct PitchNote { double sec = 0.0, durSec = 0.0; int note = 60; float rms = 0.0f; int vel = 0; };
    struct PitchTrack { std::vector<PitchNote> notes; double pitchedFraction = 0.0; };

    // Melody tracker settings (defaults follow the export plan).
    struct MelodyParams
    {
        int smoothFrames = 5;          // median window in frames (odd), 1 = off
        double hystCents = 70.0;       // a new note needs at least this far from the held one
        double holdSec = 0.04;         // ... for at least this long
        double gapSec = 0.06;          // pitch lost longer than this ends the note
        double decayDb = 20.0;         // RMS this far under the note's peak ends the note
        double mergeGapSec = 0.05;     // same-pitch notes closer than this merge (no onset between)
        double onsetSnapSec = 0.03;    // an onset this far before a pitch run moves its start
        double velocitySec = 0.05;     // velocity from the peak RMS in this span
    };

    inline double hzToMidi (double hz) { return 69.0 + 12.0 * std::log2 (hz / 440.0); }

    // Monophonic YIN melody tracking on mono x (decimated to about 11 kHz first, so the
    // cost stays low on long takes). Per frame: pitch with an octave check, then a median
    // over smoothFrames, then hysteresis (a new note needs hystCents for holdSec).
    // A note ends when pitch is lost for over gapSec, the RMS falls decayDb under its
    // peak, or at an onset (onsetSecs, may be empty). Short gaps are bridged, same-pitch
    // neighbours closer than mergeGapSec with no onset between are merged, and notes
    // shorter than kMinNoteSec are dropped. Pitch = median of the note's frames.
    // pitchedFraction = confident frames / all frames (silence counts as unpitched).
    inline PitchTrack trackMelody (const float* x, int n, double sr, const std::vector<double>& onsetSecs = {},
                                   const MelodyParams& prm = {})
    {
        PitchTrack out;
        if (x == nullptr || n <= 0 || ! (sr > 0.0))
            return out;
        // Decimate: two passes of a D-sample moving average, then every Dth sample.
        const int D = std::max (1, (int) std::floor (sr / 11025.0));
        const double dsr = sr / (double) D;
        std::vector<float> y;
        {
            std::vector<double> a ((size_t) n), b ((size_t) n);
            for (int pass = 0; pass < 2; ++pass)
            {
                double acc = 0.0;
                for (int i = 0; i < n; ++i)
                {
                    acc += pass == 0 ? (double) x[i] : a[(size_t) i];
                    if (i >= D)
                        acc -= pass == 0 ? (double) x[i - D] : a[(size_t) (i - D)];
                    (pass == 0 ? a : b)[(size_t) i] = acc / (double) D;
                }
            }
            for (int i = 0; i < n; i += D)
                y.push_back ((float) b[(size_t) i]);
        }
        const int m = (int) y.size();
        const int W = std::max (16, (int) std::lround (kPitchWinSec * dsr));
        const int hop = std::max (1, (int) std::lround (kPitchHopSec * dsr));
        const int maxTau = std::min (W - 2, (int) std::ceil (dsr / kPitchMinHz));
        const int minTau = std::max (2, (int) std::floor (dsr / kPitchMaxHz));
        const int I = W - maxTau; // integration length
        if (m < W || I < 8 || maxTau <= minTau)
            return out;
        const int nF = (m - W) / hop + 1;
        std::vector<double> midi ((size_t) nF, -1.0), rms ((size_t) nF, 0.0);
        std::vector<double> d ((size_t) maxTau + 1);
        const double gate = std::pow (10.0, kPitchGateDb / 20.0);
        int pitched = 0;
        for (int f = 0; f < nF; ++f)
        {
            const float* p = y.data() + (size_t) f * (size_t) hop;
            double e = 0.0;
            for (int i = 0; i < W; ++i)
                e += (double) p[i] * p[i];
            rms[(size_t) f] = std::sqrt (e / W);
            if (rms[(size_t) f] < gate)
                continue;
            d[0] = 1.0;
            double run = 0.0;
            for (int tau = 1; tau <= maxTau; ++tau)
            {
                double s = 0.0;
                for (int i = 0; i < I; ++i)
                {
                    const double q = (double) p[i] - (double) p[i + tau];
                    s += q * q;
                }
                run += s;
                d[(size_t) tau] = run > 0.0 ? s * tau / run : 1.0;
            }
            int best = -1;
            for (int tau = minTau; tau < maxTau; ++tau)
                if (d[(size_t) tau] < kYinThresh)
                {
                    while (tau + 1 < maxTau && d[(size_t) tau + 1] < d[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            if (best < 0)
                continue;
            // Octave check: a dip near best/2 close to the best one = the higher octave.
            {
                const int hh = (int) std::lround (best * 0.5);
                int hb = -1;
                for (int tau = std::max (minTau, hh - 1); tau <= std::min (maxTau - 1, hh + 1); ++tau)
                    if (hb < 0 || d[(size_t) tau] < d[(size_t) hb])
                        hb = tau;
                if (hb > minTau && hb < best && d[(size_t) hb] <= d[(size_t) best] + kOctaveSlack)
                    best = hb;
            }
            double t = (double) best;
            {
                const double a0 = d[(size_t) best - 1], a1 = d[(size_t) best], a2 = d[(size_t) best + 1];
                const double den = a0 - 2.0 * a1 + a2;
                if (den > 0.0)
                    t += 0.5 * (a0 - a2) / den;
            }
            const double hz = dsr / t;
            if (hz < kPitchMinHz || hz > kPitchMaxHz)
                continue;
            midi[(size_t) f] = hzToMidi (hz);
            ++pitched;
        }
        out.pitchedFraction = (double) pitched / (double) nF;
        // Median smoothing over pitched frames only (unpitched frames stay unpitched).
        std::vector<double> sm (midi);
        if (prm.smoothFrames > 1)
        {
            const int hw = prm.smoothFrames / 2;
            std::vector<double> win;
            for (int f = 0; f < nF; ++f)
            {
                if (midi[(size_t) f] < 0.0)
                    continue;
                win.clear();
                for (int g = std::max (0, f - hw); g <= std::min (nF - 1, f + hw); ++g)
                    if (midi[(size_t) g] >= 0.0)
                        win.push_back (midi[(size_t) g]);
                std::nth_element (win.begin(), win.begin() + (long) (win.size() / 2), win.end());
                sm[(size_t) f] = win[win.size() / 2];
            }
        }
        // Frame f stands for its window start: YIN only turns confident once the window's
        // integration span lies inside the new note (window centre measured ~22 ms late).
        const double frameDt = (double) hop / dsr;
        auto frameSec = [&] (int f) { return (double) f * frameDt; };
        auto framesFor = [&] (double s) { return std::max (1, (int) std::ceil (s / frameDt - 1.0e-9)); };
        const int holdF = framesFor (prm.holdSec);
        const int gapF = std::max (0, (int) std::floor (prm.gapSec / frameDt + 1.0e-9));
        const double hyst = std::max (0.0, prm.hystCents) / 100.0;
        const double decay = std::pow (10.0, -std::max (0.0, prm.decayDb) / 20.0);
        const int velF = framesFor (prm.velocitySec);
        std::vector<char> split ((size_t) nF, 0);
        std::vector<double> ons (onsetSecs);
        std::sort (ons.begin(), ons.end());
        for (double s : ons)
        {
            const int f = (int) std::lround (s / frameDt);
            if (f > 0 && f < nF)
                split[(size_t) f] = 1;
        }
        std::vector<PitchNote> raw;
        int start = -1, cur = -1, pend = 0, pendStart = -1, pendNote = -1;
        double peak = 0.0, quietBelow = 0.0;
        auto close = [&] (int endF)
        {
            if (start >= 0 && endF > start)
            {
                std::vector<double> ms;
                double e = 0.0, vpk = 0.0;
                for (int f = start; f < endF; ++f)
                {
                    if (midi[(size_t) f] >= 0.0)
                        ms.push_back (midi[(size_t) f]);
                    e += rms[(size_t) f] * rms[(size_t) f];
                    if (f < start + velF)
                        vpk = std::max (vpk, rms[(size_t) f]);
                }
                if (! ms.empty())
                {
                    std::nth_element (ms.begin(), ms.begin() + (long) (ms.size() / 2), ms.end());
                    PitchNote pn;
                    pn.sec = frameSec (start);
                    pn.durSec = frameSec (endF) - pn.sec;
                    pn.note = juce::jlimit (0, 127, (int) std::lround (ms[ms.size() / 2]));
                    pn.rms = (float) std::sqrt (e / (endF - start));
                    pn.vel = velocityForLevel (vpk);
                    raw.push_back (pn);
                }
            }
            start = -1;
            cur = -1;
            pend = 0;
            peak = 0.0;
        };
        auto open = [&] (int f, int note)
        {
            start = f;
            cur = note;
            pend = 0;
            peak = rms[(size_t) f];
        };
        int gap = 0;
        for (int f = 0; f < nF; ++f)
        {
            const double mf = sm[(size_t) f];
            if (mf < 0.0)
            {
                ++gap;
                if (start >= 0 && gap > gapF)
                    close (f - gap + 1);
                if (start < 0 && gap > gapF)
                    quietBelow = 0.0;
                continue;
            }
            const double lv = rms[(size_t) f];
            if (start < 0 && lv < quietBelow)
                continue; // still the decayed tail of the last note
            quietBelow = 0.0;
            gap = 0;
            const int r = (int) std::lround (mf);
            if (start >= 0 && split[(size_t) f])
                close (f);
            if (start < 0)
            {
                open (f, r);
                continue;
            }
            peak = std::max (peak, lv);
            if (lv < peak * decay)
            {
                quietBelow = peak * decay;
                close (f);
                continue;
            }
            if (r == cur || std::abs (mf - (double) cur) < hyst)
            {
                pend = 0;
                continue;
            }
            if (pend == 0 || r != pendNote)
            {
                pendStart = f;
                pendNote = r;
                pend = 0;
            }
            if (++pend >= holdF)
            {
                close (pendStart);
                open (pendStart, r);
                for (int g = pendStart; g <= f; ++g)
                    peak = std::max (peak, rms[(size_t) g]);
            }
        }
        if (start >= 0)
            close (nF - gap);
        // Merge same-pitch neighbours across short gaps with no onset between.
        auto onsetIn = [&] (double a, double b)
        {
            auto it = std::upper_bound (ons.begin(), ons.end(), a);
            return it != ons.end() && *it <= b;
        };
        std::vector<PitchNote> merged;
        for (const auto& nt : raw)
        {
            if (! merged.empty())
            {
                auto& pv = merged.back();
                const double pEnd = pv.sec + pv.durSec;
                if (pv.note == nt.note && nt.sec - pEnd < prm.mergeGapSec && ! onsetIn (pEnd - frameDt, nt.sec))
                {
                    const double total = pv.durSec + nt.durSec;
                    if (total > 0.0)
                        pv.rms = (float) std::sqrt ((pv.rms * pv.rms * pv.durSec + nt.rms * nt.rms * nt.durSec) / total);
                    pv.durSec = nt.sec + nt.durSec - pv.sec;
                    continue;
                }
            }
            merged.push_back (nt);
        }
        // Onset snap (an onset shortly before a pitch run starts the note), then the minimum length.
        double prevEnd = -1.0e300;
        for (auto nt : merged)
        {
            auto it = std::upper_bound (ons.begin(), ons.end(), nt.sec);
            if (it != ons.begin())
            {
                const double o = *(it - 1);
                if (nt.sec - o <= prm.onsetSnapSec && o >= prevEnd)
                {
                    nt.durSec += nt.sec - o;
                    nt.sec = o;
                }
            }
            if (nt.durSec >= kMinNoteSec - 1.0e-9)
            {
                out.notes.push_back (nt);
                prevEnd = nt.sec + nt.durSec;
            }
        }
        return out;
    }

    // Older entry point, kept for callers and tests: the melody tracker with default settings.
    inline PitchTrack trackNotes (const float* x, int n, double sr, const std::vector<double>& onsetSecs = {})
    {
        return trackMelody (x, n, sr, onsetSecs);
    }

    // Simple drums-vs-tonal rule: tonal when over kTonalFraction of all frames hold a
    // confident pitch. Teeth (HSLINK_TEETH=MIDI_NO_NOTES): never tonal.
    inline bool isTonal (const PitchTrack& t)
    {
#if defined (HSLINK_TEETH_MIDI_NO_NOTES)
        juce::ignoreUnused (t);
        return false;
#else
        return t.pitchedFraction > kTonalFraction && ! t.notes.empty();
#endif
    }

    // Notes starting inside [startSec, endSec), relative to startSec, cut at endSec.
    inline std::vector<PitchNote> notesInRegion (const std::vector<PitchNote>& notes, double startSec, double endSec)
    {
        std::vector<PitchNote> out;
        for (auto nt : notes)
            if (nt.sec >= startSec && nt.sec < endSec)
            {
                nt.durSec = std::min (nt.durSec, endSec - nt.sec);
                nt.sec -= startSec;
                out.push_back (nt);
            }
        return out;
    }

    // Notes file: a tempo track and one note track named trackName (channel 1).
    // Velocity: the note's own vel when set, else segment RMS relative to the loudest note.
    // originTicks: the export origin (pass the same exportOriginTicks the hits file uses so
    // both files line up). Starts quantize like the hits (quantDiv, quantStrength); ends
    // snap to the grid when within a quarter step, and a note never overlaps the next one.
    inline juce::MidiFile buildNotesFile (const std::vector<PitchNote>& notes, const juce::String& trackName,
                                          double bpm, int beatsPerBar, int den = 4,
                                          std::optional<KeySig> key = std::nullopt, const TempoMap& map = {},
                                          int quantDiv = 0, double quantStrength = 0.0, double originTicks = 0.0)
    {
        const double tps = ticksPerSecond (bpm);
        const bool useMap = map.size() >= 2;
        const auto st = useMap ? segTicks (map) : std::vector<double>();
        const double origin = std::isfinite (originTicks) ? std::max (0.0, originTicks) : 0.0;
        auto toT = [&] (double s)
        {
            return std::max (0.0, (useMap ? std::round (secToTicks (map, st, s)) : std::round (std::max (0.0, s) * tps)) - origin);
        };
        juce::MidiMessageSequence h;
        auto name = juce::MidiMessage::textMetaEvent (3, trackName);
        name.setTimeStamp (0.0);
        h.addEvent (name);
        float loudest = 0.0f;
        for (const auto& nt : notes)
            loudest = std::max (loudest, nt.rms);
        struct Ev { double on, off; int note, vel; };
        std::vector<Ev> evs;
        const double grid = quantizeGridTicks (quantDiv);
        const bool q = grid > 0.0 && quantStrength > 0.0;
        for (const auto& nt : notes)
        {
            double on = toT (nt.sec), off = toT (nt.sec + nt.durSec);
            if (q)
            {
                on = std::max (0.0, std::round (quantizeTick (on, grid, 0.0, quantStrength)));
                const double g = std::round (off / grid) * grid;
                if (std::abs (g - off) <= 0.25 * grid)
                    off = g;
            }
            const int vel = nt.vel > 0 ? nt.vel : velocityRelative (nt.rms, loudest);
            evs.push_back ({ on, off, juce::jlimit (0, 127, nt.note), juce::jlimit (1, 127, vel) });
        }
        std::stable_sort (evs.begin(), evs.end(), [] (const Ev& a, const Ev& b) { return a.on < b.on; });
        double last = 0.0;
        for (size_t i = 0; i < evs.size(); ++i)
        {
            auto& e = evs[i];
            if (i + 1 < evs.size())
                e.off = std::min (e.off, evs[i + 1].on);
            if (e.off < e.on + 1.0)
            {
                if (i + 1 < evs.size() && evs[i + 1].on <= e.on)
                    continue; // quantized onto the same tick as the next note: keep the later one
                e.off = e.on + 1.0;
            }
            auto a = juce::MidiMessage::noteOn (kNoteChannel, e.note, (juce::uint8) e.vel);
            a.setTimeStamp (e.on);
            auto b = juce::MidiMessage::noteOff (kNoteChannel, e.note);
            b.setTimeStamp (e.off);
            h.addEvent (a);
            h.addEvent (b);
            last = std::max (last, e.off);
        }
        h.updateMatchedPairs();
        auto end = juce::MidiMessage::endOfTrack();
        end.setTimeStamp (last);
        h.addEvent (end);
        juce::MidiFile f;
        f.setTicksPerQuarterNote (kPpq);
        f.addTrack (tempoTrack (bpm, beatsPerBar, den, key, map, origin));
        f.addTrack (h);
        return f;
    }

    inline bool writeFile (const juce::MidiFile& f, const juce::File& dest)
    {
        const auto part = dest.withFileExtension ("midpart");
        part.deleteFile();
        {
            juce::FileOutputStream os (part);
            if (! os.openedOk() || ! f.writeTo (os, 1))
            {
                part.deleteFile();
                return false;
            }
        }
        dest.deleteFile();
        return part.moveFileTo (dest);
    }
}
