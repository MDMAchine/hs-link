// HOT-STEP LINK -- MidiDrumTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// A synthetic drum loop (kick, snare, closed hat, open hat at known times and levels)
// through MidiExport::hitsFromOnsets + buildHitsFile, read back: each hit type on its GM
// note, at least 4 distinct pitches, velocity spread at least 40.
#include <cstdlib>
#include "MidiExport.h"
#include "TempoMeasure.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <tuple>
#include <map>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

struct Rng
{
    juce::uint32 s = 0x12345678u;
    float next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float) ((double) s / 4294967295.0 * 2.0 - 1.0); }
};

enum Kind { kick, snare, chat, ohat };

static void addHit (juce::AudioBuffer<float>& buf, double sr, double sec, Kind kind, float level, Rng& rng)
{
    const int a = (int) (sec * sr);
    const int len = (int) ((kind == ohat ? 0.4 : 0.25) * sr);
    double ph = 0.0, hpS = 0.0, hpS2 = 0.0, lp = 0.0;
    const double aHp = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 7000.0 / sr);
    const double aLp = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 5000.0 / sr);
    for (int i = 0; i < len && a + i < buf.getNumSamples(); ++i)
    {
        const double t = (double) i / sr;
        double v = 0.0;
        if (kind == kick)
        {
            const double f = 50.0 + 100.0 * std::exp (-t / 0.03); // 150 Hz -> 50 Hz drop
            ph += 2.0 * juce::MathConstants<double>::pi * f / sr;
            v = std::sin (ph) * std::exp (-t / 0.12);
        }
        else if (kind == snare)
        {
            lp += aLp * ((double) rng.next() - lp);
            v = (0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * 200.0 * t) + 1.2 * lp) * std::exp (-t / 0.06);
        }
        else
        {
            const double n = (double) rng.next();
            hpS += aHp * (n - hpS);
            const double h1 = n - hpS;
            hpS2 += aHp * (h1 - hpS2);
            v = (h1 - hpS2) * std::exp (-t / (kind == chat ? 0.02 : 0.15));
        }
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.addSample (ch, a + i, (float) v * level);
    }
}

int main()
{
    const double sr = 44100.0;
    juce::AudioBuffer<float> buf (2, (int) (sr * 8.5));
    buf.clear();
    Rng rng;
    // 16 hits, 0.5 s apart, levels from 1.0 down to 0.06 (about 24 dB).
    const Kind pattern[16] = { kick, chat, snare, ohat, kick, chat, snare, chat,
                               kick, ohat, snare, chat, kick, chat, snare, ohat };
    const float levels[16] = { 1.0f, 0.5f, 0.9f, 0.4f, 0.7f, 0.2f, 0.6f, 0.3f,
                               0.45f, 0.25f, 0.35f, 0.1f, 0.25f, 0.06f, 0.15f, 0.12f };
    const int want[4] = { 36, 38, 42, 46 };
    std::vector<double> secs;
    for (int k = 0; k < 16; ++k)
    {
        secs.push_back (0.25 + 0.5 * k);
        addHit (buf, sr, secs.back(), pattern[k], levels[k], rng);
    }
    const auto hits = MidiExport::hitsFromOnsets (buf, sr, secs);
    for (size_t k = 0; k < hits.size(); ++k)
    {
        // Features for the printout (same window rule as hitsFromOnsets).
        const int a = (int) (secs[k] * sr), b = std::min (buf.getNumSamples(), (int) ((secs[k] + 0.4) * sr));
        const auto f = MidiExport::drumFeatures (buf.getReadPointer (0) + a, std::min (b - a, (int) (0.5 * sr) - 1), sr);
        std::printf ("  hit %2d kind %d level %.2f -> note %d  (low %.2f mid %.2f high %.2f decay %.3f s)\n",
                     (int) k, (int) pattern[k], levels[k], hits[k].note, f.low, f.mid, f.high, f.decaySec);
    }

    auto file = MidiExport::buildHitsFile (hits, 120.0, 4);
    juce::MemoryOutputStream mo;
    file.writeTo (mo, 1);
    juce::MemoryInputStream mi (mo.getData(), mo.getDataSize(), false);
    juce::MidiFile back;
    check (back.readFrom (mi), "hits file reads back");

    std::map<int, int> hist;
    std::vector<int> notes, vels;
    for (int t = 0; t < back.getNumTracks(); ++t)
        for (const auto* e : *back.getTrack (t))
            if (e->message.isNoteOn())
            {
                ++hist[e->message.getNoteNumber()];
                notes.push_back (e->message.getNoteNumber());
                vels.push_back (e->message.getVelocity());
            }
    std::printf ("  note histogram:");
    for (const auto& [p, c] : hist)
        std::printf (" %d->%d", p, c);
    const int vmin = vels.empty() ? 0 : *std::min_element (vels.begin(), vels.end());
    const int vmax = vels.empty() ? 0 : *std::max_element (vels.begin(), vels.end());
    std::printf ("\n  velocity min %d max %d spread %d\n", vmin, vmax, vmax - vmin);

    int misses = 0;
    for (size_t k = 0; k < notes.size() && k < 16; ++k)
        if (notes[k] != want[(int) pattern[k]])
            ++misses;
    misses += 16 - (int) std::min<size_t> (16, notes.size());
    std::printf ("  GM misses %d of 16\n", misses);
    {
        // Per class (row 6): every kick 36, every snare 38, hats 42 or 46 (at most 1 off).
        bool kOk = true, sOk = true; int hatOff = 0;
        std::printf ("  per hit note:");
        for (size_t k = 0; k < 16; ++k)
        {
            const int n = k < notes.size() ? notes[k] : -1;
            std::printf (" %s=%d", pattern[k] == kick ? "K" : pattern[k] == snare ? "S" : pattern[k] == chat ? "ch" : "oh", n);
            if (pattern[k] == kick)  kOk = kOk && n == 36;
            if (pattern[k] == snare) sOk = sOk && n == 38;
            if (pattern[k] == chat || pattern[k] == ohat) hatOff += (n == 42 || n == 46) ? 0 : 1;
        }
        std::printf ("\n");
        check (kOk, "every kick hit on note 36");
        check (sOk, "every snare hit on note 38");
        std::printf ("  hats off 42/46: %d of 8 (quiet last open hat reads as crash 49)\n", hatOff);
        check (hatOff <= 1, "hat hits on note 42 or 46 (at most 1 of 8 off)");
    }
    check (notes.size() == 16, "16 notes exported");
    check (hist.size() >= 4, "at least 4 distinct pitches");
    check (misses <= 1, "each hit type on its GM note (at most 1 miss in 16)");
    check (vmax - vmin >= 40, "velocity spread at least 40");
    check (vmax >= 125, "loudest hit near 127");

    {
        // Region + note length: 120 bpm at 48 kHz, 8 hits at known frames, region = bars
        // 2-3 (frames 96000..288000, 2.0 s .. 6.0 s), 960 ppq.
        const double rsr = 48000.0;
        const juce::int64 hitFrames[8] = { 24000, 72000, 96000, 120000, 123000, 192000, 264000, 312000 };
        const int hitNotes[8] = { 36, 38, 36, 38, 38, 36, 42, 36 };
        std::vector<MidiExport::Hit> all;
        for (int k = 0; k < 8; ++k)
            all.push_back ({ (double) hitFrames[k] / rsr, 0.8f, hitNotes[k] });
        const double r0 = 96000.0 / rsr, r1 = 288000.0 / rsr;
        const auto in = MidiExport::hitsInRegion (all, r0, r1);
        auto rf = MidiExport::buildHitsFile (in, 120.0, 4);
        const int ppq = rf.getTimeFormat();
        std::vector<int> on, len, nn;
        for (int t = 0; t < rf.getNumTracks(); ++t)
        {
            juce::MidiMessageSequence s (*rf.getTrack (t));
            s.updateMatchedPairs();
            for (int i = 0; i < s.getNumEvents(); ++i)
                if (s.getEventPointer (i)->message.isNoteOn())
                {
                    const auto* e = s.getEventPointer (i);
                    on.push_back ((int) e->message.getTimeStamp());
                    nn.push_back (e->message.getNoteNumber());
                    len.push_back (e->noteOffObject != nullptr ? (int) (e->noteOffObject->message.getTimeStamp() - e->message.getTimeStamp()) : -1);
                }
        }
        // Expected: the 5 hits in [2 s, 6 s), from the region start; 1/16 = ppq/4 ticks,
        // the 2.5 s hit clipped by the same-pitch hit 1/32 later.
        const int sixteenth = ppq / 4;
        const int wantOn[5] = { 0, ppq * 1, ppq + ppq / 8, ppq * 4, ppq * 7 };
        const int wantLen[5] = { sixteenth, ppq / 8, sixteenth, sixteenth, sixteenth };
        std::printf ("  region ppq %d, notes %d (want 5)\n", ppq, (int) on.size());
        bool onOk = on.size() == 5, lenOk = on.size() == 5;
        for (size_t k = 0; k < on.size(); ++k)
        {
            const int wo = k < 5 ? wantOn[k] : -1, wl = k < 5 ? wantLen[k] : -1;
            std::printf ("    note %d on %d (want %d) len %d (want %d)\n", nn[k], on[k], wo, len[k], wl);
            if (k >= 5 || std::abs (on[k] - wo) > 1)
                onOk = false;
            if (k >= 5 || len[k] != wl)
                lenOk = false;
        }
        check (onOk, "region: in-region hits only, timed from region start (within 1 tick)");
        check (lenOk, "note length 1/16, clipped before the next same-pitch hit");
        const auto sm = MidiExport::shiftTempoMap ({ { 0.0, 100.0 }, { 3.0, 140.0 } }, 2.0);
        std::printf ("  shifted map:");
        for (const auto& g : sm)
            std::printf (" %.3f@%.1f", g.startSec, g.bpm);
        std::printf ("\n");
        check (sm.size() == 2 && sm[0].startSec == 0.0 && sm[0].bpm == 100.0 && std::abs (sm[1].startSec - 1.0) < 1e-9 && sm[1].bpm == 140.0,
               "tempo map shifted to the region start");
    }

    {
        // Pitched notes: a bass line A2 C3 E3 G3 (sine + 2 harmonics), 0.5 s each from
        // 0.25 s at 120 bpm -> starts at ticks 480, 1440, 2400, 3360, 960 long.
        const double bsr = 44100.0;
        const int bassNotes[4] = { 45, 48, 52, 55 };
        juce::AudioBuffer<float> bass (1, (int) (bsr * 2.75));
        bass.clear();
        float* bp = bass.getWritePointer (0);
        double ph = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            const double hz = 440.0 * std::pow (2.0, (bassNotes[k] - 69) / 12.0);
            const int a = (int) std::lround ((0.25 + 0.5 * k) * bsr), len = (int) std::lround (0.5 * bsr);
            for (int i = 0; i < len; ++i)
            {
                const double env = std::min (1.0, std::min (i, len - i) / (0.005 * bsr));
                bp[a + i] = (float) (0.5 * env * (std::sin (ph) + 0.5 * std::sin (2.0 * ph) + 0.3 * std::sin (3.0 * ph)));
                ph += 2.0 * juce::MathConstants<double>::pi * hz / bsr;
            }
        }
        const auto pt = MidiExport::trackNotes (bp, bass.getNumSamples(), bsr);
        std::printf ("  bass pitched fraction %.2f, tonal %d\n", pt.pitchedFraction, (int) MidiExport::isTonal (pt));
        std::vector<int> pn, ps, pl;
        if (MidiExport::isTonal (pt))
        {
            auto nf = MidiExport::buildNotesFile (pt.notes, "bass notes", 120.0, 4);
            juce::MemoryOutputStream o2;
            nf.writeTo (o2, 1);
            juce::MemoryInputStream i2 (o2.getData(), o2.getDataSize(), false);
            juce::MidiFile nb;
            nb.readFrom (i2);
            if (nb.getNumTracks() >= 2)
                for (auto* ev : *nb.getTrack (1))
                    if (ev->message.isNoteOn())
                    {
                        pn.push_back (ev->message.getNoteNumber());
                        ps.push_back ((int) ev->message.getTimeStamp());
                        pl.push_back (ev->noteOffObject != nullptr ? (int) (ev->noteOffObject->message.getTimeStamp() - ev->message.getTimeStamp()) : -1);
                    }
        }
        bool pitchesOk = pn.size() == 4, startsOk = pn.size() == 4;
        for (size_t k = 0; k < pn.size(); ++k)
        {
            std::printf ("    note %d start %d len %d\n", pn[k], ps[k], pl[k]);
            if (k < 4)
            {
                pitchesOk = pitchesOk && pn[k] == bassNotes[k];
                startsOk = startsOk && std::abs (ps[k] - (480 + 960 * (int) k)) <= 30;
            }
        }
        check (MidiExport::isTonal (pt), "bass line is tonal (notes track written)");
        check (pitchesOk, "bass notes exactly A2 C3 E3 G3 (45 48 52 55)");
        check (startsOk, "bass note starts within 30 ticks");

        // The drum loop above: no notes track.
        std::vector<float> dm ((size_t) buf.getNumSamples(), 0.0f);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                dm[(size_t) i] += 0.5f * buf.getSample (ch, i);
        const auto dt = MidiExport::trackNotes (dm.data(), (int) dm.size(), sr);
        std::printf ("  drums pitched fraction %.2f, notes %d, tonal %d\n", dt.pitchedFraction, (int) dt.notes.size(), (int) MidiExport::isTonal (dt));
        check (! MidiExport::isTonal (dt), "drum loop is not tonal (no notes track)");
    }

    // Onset gate: a sustained horn-like line (band-limited sawtooth, 80 ms attack, 5.5 Hz
    // vibrato, a new note every 0.9 s) under the drum loop adds at most 1 hit.
    {
        auto countHits = [] (const juce::AudioBuffer<float>& b, double rate)
        {
            std::vector<float> m ((size_t) b.getNumSamples(), 0.0f);
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                for (int i = 0; i < b.getNumSamples(); ++i)
                    m[(size_t) i] += b.getSample (ch, i) / (float) b.getNumChannels();
            std::vector<double> on;
            for (const auto& o : TempoMeasure::onsets (m.data(), m.size(), rate))
                on.push_back (o.sec);
            const auto kept = MidiExport::percussiveOnsets (b, rate, on);
            if (std::getenv ("HSLINK_GATE_DEBUG") != nullptr)
            {
                std::printf ("    onsets:");
                for (double x : on)
                    std::printf (" %.3f%s", x, std::find (kept.begin(), kept.end(), x) != kept.end() ? "" : "x");
                std::printf ("\n");
            }
            int offGrid = 0; // kept hits more than 40 ms from every drum hit (0.25 + 0.5 k)
            for (double x : kept)
                if (std::abs (x - 0.25 - 0.5 * std::round ((x - 0.25) / 0.5)) > 0.04)
                    ++offGrid;
            return std::make_tuple ((int) on.size(), (int) MidiExport::hitsFromOnsets (b, rate, kept).size(), offGrid);
        };
        const auto drumsOnly = countHits (buf, sr);
        juce::AudioBuffer<float> mix (buf);
        const double hornHz[9] = { 233.1, 261.6, 311.1, 233.1, 349.2, 311.1, 261.6, 233.1, 196.0 };
        double ph = 0.0, lp1 = 0.0, lp2 = 0.0;
        const double aLp = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 2500.0 / sr);
        for (int i = 0; i < mix.getNumSamples(); ++i)
        {
            const double t = (double) i / sr;
            const int n = std::min (8, (int) (t / 0.9));
            const double tn = t - 0.9 * n;
            const double env = std::min (1.0, tn / 0.08) * (tn > 0.8 ? std::max (0.0, 1.0 - (tn - 0.8) / 0.1) : 1.0);
            const double f = hornHz[n] * (1.0 + 0.006 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t));
            ph += f / sr;
            ph -= std::floor (ph);
            lp1 += aLp * ((2.0 * ph - 1.0) - lp1);
            lp2 += aLp * (lp1 - lp2);
            for (int ch = 0; ch < mix.getNumChannels(); ++ch)
                mix.addSample (ch, i, (float) (0.35 * env * lp2));
        }
        const auto withHorn = countHits (mix, sr);
        std::printf ("  gate: drums only %d onsets -> %d hits; drums + horn %d onsets -> %d hits, %d off the drum times\n",
                     std::get<0> (drumsOnly), std::get<1> (drumsOnly), std::get<0> (withHorn), std::get<1> (withHorn), std::get<2> (withHorn));
        check (std::get<1> (drumsOnly) == std::get<0> (drumsOnly), "gate keeps every drum-only onset");
        check (std::get<2> (withHorn) <= 1, "horn line adds at most 1 hit off the drum times");

        // The real take, when HSLINK_REALTAKE_WAV names one; unset means skip.
        const char* realEnv = std::getenv ("HSLINK_REALTAKE_WAV");
        const juce::File real = (realEnv != nullptr && realEnv[0] != 0) ? juce::File (juce::String::fromUTF8 (realEnv)) : juce::File();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> rd (real.existsAsFile() ? wav.createReaderFor (new juce::FileInputStream (real), true) : nullptr);
        if (rd == nullptr)
            std::printf ("  real take: SKIP (set HSLINK_REALTAKE_WAV=<take wav>)\n");
        else
        {
            juce::AudioBuffer<float> rb ((int) rd->numChannels, (int) rd->lengthInSamples);
            rd->read (&rb, 0, rb.getNumSamples(), 0, true, true);
            const auto r = countHits (rb, rd->sampleRate);
            const double bars = (double) rb.getNumSamples() / rd->sampleRate / (4.0 * 60.0 / 88.0);
            std::printf ("  real take: %.2f s, %.2f bars at 88 bpm; onsets %d (%.1f per bar) -> hits %d (%.1f per bar)\n",
                         (double) rb.getNumSamples() / rd->sampleRate, bars, std::get<0> (r), std::get<0> (r) / bars, std::get<1> (r), std::get<1> (r) / bars);
        }
    }

    {
        // Export quantize: 120 bpm, 960 ppq, 1/16 grid = 240 ticks, downbeat at 0.
        // Hits at ticks 250, 470, 735, 1100 (off grid by +10, -10, +15, -100... nearest 240, 480, 720, 1200).
        const int tks[4] = { 250, 470, 735, 1100 };
        const int want16[4] = { 240, 480, 720, 1200 };
        std::vector<MidiExport::Hit> qh;
        for (int k = 0; k < 4; ++k)
            qh.push_back ({ tks[k] / 1920.0, 0.8f, 36 + 2 * k });
        auto onTicks = [] (const juce::MidiFile& f)
        {
            std::vector<int> v;
            for (int t = 0; t < f.getNumTracks(); ++t)
                for (const auto* e : *f.getTrack (t))
                    if (e->message.isNoteOn())
                        v.push_back ((int) std::lround (e->message.getTimeStamp()));
            return v;
        };
        const auto off = onTicks (MidiExport::buildHitsFile (qh, 120.0, 4, 4, std::nullopt, {}, 0, 1.0, 0.0));
        const auto full = onTicks (MidiExport::buildHitsFile (qh, 120.0, 4, 4, std::nullopt, {}, 16, 1.0, 0.0));
        const auto half = onTicks (MidiExport::buildHitsFile (qh, 120.0, 4, 4, std::nullopt, {}, 16, 0.5, 0.0));
        const auto shifted = onTicks (MidiExport::buildHitsFile (qh, 120.0, 4, 4, std::nullopt, {}, 16, 1.0, 100.0 / 1920.0));
        bool okOff = off.size() == 4, okFull = full.size() == 4, okHalf = half.size() == 4, okShift = shifted.size() == 4;
        for (int k = 0; k < 4; ++k)
        {
            const int a = k < (int) off.size() ? off[k] : -1, b = k < (int) full.size() ? full[k] : -1;
            const int c = k < (int) half.size() ? half[k] : -1, d = k < (int) shifted.size() ? shifted[k] : -1;
            std::printf ("  quantize hit %d: in %d  off %d  1/16@100 %d (want %d)  1/16@50 %d (want %d)  down+100 %d\n",
                         k, tks[k], a, b, want16[k], c, (tks[k] + want16[k]) / 2, d);
            okOff = okOff && a == tks[k];
            okFull = okFull && b == want16[k];
            okHalf = okHalf && std::abs (c - (tks[k] + want16[k]) / 2) <= 1;
            okShift = okShift && d % 240 == 0;
        }
        check (okOff, "quantize Off leaves hit ticks unchanged");
        check (okFull, "1/16 at 100 percent lands exactly on grid");
        check (okHalf, "1/16 at 50 percent lands halfway to grid");
        check (okShift, "down+100: hits snap to multiples of 240 (origin at the downbeat)");
        // Downbeat just under one bar (3835 of 3840 ticks), no tempo map, 120 bpm: the
        // origin rounds to the next bar. It must stay 0 or more, tick 0 carries a tempo,
        // and no event lands before tick 0.
        {
            const MidiExport::TempoMap m {};
            std::vector<MidiExport::Hit> nh { { 3835.0 / 1920.0 + 0.05, 0.8f, 36 }, { 2.6, 0.7f, 38 } };
            const double org = MidiExport::exportOriginTicks (nh, 120.0, 4, 4, m, 3835.0 / 1920.0);
            const auto f = MidiExport::buildHitsFile (nh, 120.0, 4, 4, std::nullopt, m, 0, 0.0, 3835.0 / 1920.0);
            bool tempoAt0 = false, allPos = true;
            double minTs = 1.0e9;
            for (int tr = 0; tr < f.getNumTracks(); ++tr)
                for (auto* ev : *f.getTrack (tr))
                {
                    minTs = std::min (minTs, ev->message.getTimeStamp());
                    allPos = allPos && ev->message.getTimeStamp() >= 0.0;
                    tempoAt0 = tempoAt0 || (ev->message.isTempoMetaEvent() && ev->message.getTimeStamp() == 0.0);
                }
            std::printf ("  near-bar downbeat: origin %.1f (want >= 0) tempo at tick 0=%d min tick %.1f%c", org, (int) tempoAt0, minTs, 10);
            check (org >= 0.0, "near-bar downbeat: export origin is 0 or more");
            check (tempoAt0 && allPos, "near-bar downbeat: tick 0 carries a tempo and every tick is 0 or more");
        }
        // Downbeat at 100 ticks, hits at 100 + 240k + jitter: hit k lands on exactly 240k.
        {
            const int jit[4] = { 7, -9, 12, -5 };
            std::vector<MidiExport::Hit> dh;
            for (int k = 0; k < 4; ++k)
                dh.push_back ({ (100 + 240 * k + jit[k]) / 1920.0, 0.8f, 36 + 2 * k });
            const auto dq = onTicks (MidiExport::buildHitsFile (dh, 120.0, 4, 4, std::nullopt, {}, 16, 1.0, 100.0 / 1920.0));
            bool ok = dq.size() == 4;
            for (int k = 0; k < 4 && k < (int) dq.size(); ++k)
            {
                std::printf ("  down=100 hit %d: in %d  out %d (want %d)  out%%240 %d\n", k, 100 + 240 * k + jit[k], dq[k], 240 * k, dq[k] % 240);
                ok = ok && dq[k] == 240 * k;
            }
            check (ok, "downbeat=100 ticks: hit k lands on 240k");
        }
        // Two kicks 20 ms apart (ticks 960 and 998 at 120 bpm), 1/16 at 100 percent: both
        // snap to 960. Must export one note at the louder velocity. Same with quantize off
        // when two onsets round to the same tick (0.1 ms apart).
        {
            auto notesOf = [] (const juce::MidiFile& f)
            {
                std::vector<std::pair<int, int>> v;
                for (int t = 0; t < f.getNumTracks(); ++t)
                    for (const auto* e : *f.getTrack (t))
                        if (e->message.isNoteOn())
                            v.push_back ({ (int) std::lround (e->message.getTimeStamp()), (int) e->message.getVelocity() });
                return v;
            };
            const std::vector<MidiExport::Hit> two { { 0.50, 0.4f, 36 }, { 0.52, 0.9f, 36 } };
            const std::vector<MidiExport::Hit> solo { { 0.52, 0.9f, 36 } };
            const int wantVel = notesOf (MidiExport::buildHitsFile (solo, 120.0, 4, 4, std::nullopt, {}, 16, 1.0, 0.0)).at (0).second;
            const auto q = notesOf (MidiExport::buildHitsFile (two, 120.0, 4, 4, std::nullopt, {}, 16, 1.0, 0.0));
            std::printf ("  dup kick q16: notes %d (want 1)", (int) q.size());
            for (const auto& n : q)
                std::printf ("  [tick %d vel %d]", n.first, n.second);
            std::printf ("  want tick 960 vel %d%c", wantVel, 10);
            check (q.size() == 1 && q[0].first == 960 && q[0].second == wantVel, "same tick+pitch after 1/16 quantize merges to one note, louder velocity");
            const std::vector<MidiExport::Hit> near { { 0.5, 0.9f, 36 }, { 0.5001, 0.3f, 36 } };
            const auto o = notesOf (MidiExport::buildHitsFile (near, 120.0, 4, 4, std::nullopt, {}, 0, 0.0, 0.0));
            std::printf ("  dup kick q off: notes %d (want 1) first vel %d (want %d)%c", (int) o.size(), o.empty() ? -1 : o[0].second, wantVel, 10);
            check (o.size() == 1 && o[0].second == wantVel, "same tick+pitch with quantize off merges to one note, louder velocity");
        }
    }

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
