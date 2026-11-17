// HOT-STEP LINK -- MidiMelodyTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// M2: a synthetic monophonic tone sequence with known pitches and onsets (one note has a
// one-hop octave-up glitch, one a 30 ms dropout, one a +-50 cent vibrato) through
// MidiExport::trackNotes + buildNotesFile, read back: note count, pitches, onsets within
// 30 ms, no spurious notes, loud note velocity well over the quiet one.
// G1: downbeat not at 0. The melody file built with the hits origin must put each note
// on the same tick as the hit at the same time in the hits file, and both tempo tracks
// must match. HSLINK_MELODY_OLD_API: build against the pre-origin buildNotesFile (teeth).
#include <cstdlib>
#include <cstring>
#include "MidiExport.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>
#include <cmath>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

struct Tone { double sec, dur; int note; float amp; bool octaveGlitch, dropout, vibrato; };

static void addTone (std::vector<float>& x, double sr, const Tone& t)
{
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    const double f0 = 440.0 * std::pow (2.0, (t.note - 69) / 12.0);
    const int a = (int) (t.sec * sr), len = (int) (t.dur * sr);
    double ph = 0.0;
    for (int i = 0; i < len && a + i < (int) x.size(); ++i)
    {
        const double s = (double) i / sr;
        double f = f0;
        if (t.vibrato)
            f *= std::pow (2.0, 0.5 / 12.0 * std::sin (twoPi * 5.5 * s)); // +-50 cents
        if (t.octaveGlitch && s >= 0.2 && s < 0.206)
            f *= 2.0; // about one analysis hop an octave up
        ph += twoPi * f / sr;
        double env = std::min (1.0, s / 0.005) * std::min (1.0, (t.dur - s) / 0.01);
        if (t.dropout && s >= 0.2 && s < 0.23)
            env = 0.0; // 30 ms dropout, under the 60 ms gap bridge
        const double v = std::sin (ph) + 0.3 * std::sin (2.0 * ph) + 0.15 * std::sin (3.0 * ph);
        x[(size_t) (a + i)] += (float) (v * env * t.amp);
    }
}

static juce::MidiFile roundTrip (const juce::MidiFile& f)
{
    juce::MemoryOutputStream mo;
    f.writeTo (mo, 1);
    juce::MemoryInputStream mi (mo.getData(), mo.getDataSize(), false);
    juce::MidiFile back;
    if (! back.readFrom (mi))
        std::printf ("  (read back failed)\n");
    return back;
}

struct On { double tick; int note, vel; };
static std::vector<On> noteOns (const juce::MidiFile& f, int track)
{
    std::vector<On> out;
    if (track >= f.getNumTracks())
        return out;
    const auto* s = f.getTrack (track);
    for (int i = 0; i < s->getNumEvents(); ++i)
    {
        const auto& m = s->getEventPointer (i)->message;
        if (m.isNoteOn())
            out.push_back ({ m.getTimeStamp(), m.getNoteNumber(), (int) m.getVelocity() });
    }
    return out;
}

static juce::MidiFile notesFile (const std::vector<MidiExport::PitchNote>& notes, double origin)
{
#if HSLINK_MELODY_OLD_API
    juce::ignoreUnused (origin);
    return MidiExport::buildNotesFile (notes, "melody", 120.0, 4);
#else
    return MidiExport::buildNotesFile (notes, "melody", 120.0, 4, 4, std::nullopt, {}, 0, 0.0, origin);
#endif
}

// ---- M2 spec case (docs/MIDI_EXPORT_PLAN.md round M2) ----
// 48 kHz saw+sine, C3 E3 G3 C4 A2, 0.5 s each from 0.25 s, 120 BPM. E3 has a 6 Hz +-60 cent
// vibrato, G3 a 40 ms dip to -40 dB. Amps rise so velocities must rise. trackMelody is
// called directly. Teeth (argv): --teeth-hyst0 runs hysteresis 0 (median and hold still keep 5 notes here; see
// hystGuardCase, which it splits); --teeth-no-origin builds the notes file with origin 0 (first tick
// moves by 480, so the alignment checks fail).
static void m2SpecCase (bool teethHyst0, bool teethNoOrigin)
{
    std::printf ("-- M2 spec case --\n");
    const double sr = 48000.0, bpm = 120.0, tps = MidiExport::ticksPerSecond (bpm), down = 0.25;
    const int want[] = { 48, 52, 55, 60, 45 };
    const float amp[] = { 0.10f, 0.16f, 0.25f, 0.40f, 0.63f };
    const int nN = 5;
    std::vector<float> x ((size_t) (sr * 3.25), 0.0f);
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    std::vector<double> onsets;
    std::vector<MidiExport::Hit> hits;
    for (int k = 0; k < nN; ++k)
    {
        const double t0 = down + 0.5 * k, f0 = 440.0 * std::pow (2.0, (want[k] - 69) / 12.0);
        onsets.push_back (t0);
        MidiExport::Hit h; h.sec = t0; h.peak = amp[k];
        hits.push_back (h);
        const int a = (int) (t0 * sr), len = (int) (0.5 * sr);
        double ph = 0.0;
        for (int i = 0; i < len; ++i)
        {
            const double s = (double) i / sr;
            double f = f0;
            if (k == 1)
                f *= std::pow (2.0, 0.6 / 12.0 * std::sin (twoPi * 6.0 * s)); // +-60 cents
            ph += twoPi * f / sr;
            double saw = 0.0; // band-limited saw, 8 partials
            for (int h2 = 1; h2 <= 8; ++h2)
                saw += std::sin (h2 * ph) / h2;
            double env = std::min (1.0, s / 0.005) * std::min (1.0, (0.5 - s) / 0.005);
            if (k == 2 && s >= 0.2 && s < 0.24)
                env *= 0.01; // 40 ms dip to -40 dB
            x[(size_t) (a + i)] += (float) ((0.5 * saw + 0.5 * std::sin (ph)) * env * amp[k]);
        }
    }
    MidiExport::MelodyParams prm;
    if (teethHyst0)
        prm.hystCents = 0.0;
    const auto tr = MidiExport::trackMelody (x.data(), (int) x.size(), sr, onsets, prm);
    std::printf ("  tracker notes %d (hystCents %.0f)\n", (int) tr.notes.size(), prm.hystCents);
    bool pitchOk = (int) tr.notes.size() == nN, startOk = pitchOk;
    for (size_t k = 0; k < tr.notes.size(); ++k)
    {
        const double dt = k < (size_t) nN ? (tr.notes[k].sec - onsets[k]) * 1000.0 : 0.0;
        std::printf ("  note %d: pitch %d (want %d) start %.4f s (dt %+.1f ms)\n", (int) k, tr.notes[k].note,
                     k < (size_t) nN ? want[k] : -1, tr.notes[k].sec, dt);
        if (k < (size_t) nN)
        {
            pitchOk = pitchOk && tr.notes[k].note == want[k];
            startOk = startOk && std::abs (dt) <= 15.0;
        }
    }
    check ((int) tr.notes.size() == nN, "M2 spec: exactly 5 notes (vibrato and dip hold)");
    check (pitchOk, "M2 spec: pitches 48 52 55 60 45");
    check (startOk, "M2 spec: every start within 15 ms");

    const double origin = teethNoOrigin ? 0.0 : MidiExport::exportOriginTicks (hits, bpm, 4, 4, {}, down);
    const auto nf = roundTrip (MidiExport::buildNotesFile (tr.notes, "melody", bpm, 4, 4, std::nullopt, {}, 16, 1.0, origin));
    const auto hf = roundTrip (MidiExport::buildHitsFile (hits, bpm, 4, 4, std::nullopt, {}, 16, 1.0, down));
    const auto no = noteOns (nf, 1), ho = noteOns (hf, 1);
    std::printf ("  origin %.0f ticks, first melody tick %.0f\n", origin, no.empty() ? -1.0 : no[0].tick);
    const double grid = MidiExport::quantizeGridTicks (16);
    bool gridOk = (int) no.size() == nN, alignOk = no.size() == ho.size() && ! no.empty(), velOk = (int) no.size() == nN;
    for (size_t k = 0; k < no.size(); ++k)
    {
        const double expect = (double) k * 0.5 * tps;
        std::printf ("  tick %d: melody %.0f (want %.0f) hits %.0f vel %d\n", (int) k, no[k].tick, expect,
                     k < ho.size() ? ho[k].tick : -1.0, no[k].vel);
        gridOk = gridOk && std::fmod (no[k].tick, grid) == 0.0 && no[k].tick == expect;
        alignOk = alignOk && k < ho.size() && no[k].tick == ho[k].tick;
        if (k > 0)
            velOk = velOk && no[k].vel > no[k - 1].vel;
    }
    if (velOk)
        velOk = no.back().vel - no.front().vel >= 30;
    check (gridOk, "M2 spec: Q 1/16 at 100% puts every start on an exact 1/16 tick (beat k -> k*960)");
    check (alignOk, "M2 spec: melody ticks equal the hit ticks with the downbeat at 0.25 s");
    check (velOk, "M2 spec: velocities rise with amp, last over first by 30 or more");
}

// ---- Hysteresis guard case ----
// m2SpecCase's 6 Hz +-60 cent vibrato only crosses the half-semitone line for about 30 ms
// per peak, so the 5-frame median and the 40 ms hold already stop it splitting even with
// hysteresis 0. Here a 1.5 Hz +-62 cent vibrato sits past the line for about 130 ms per
// peak, longer than median and hold can absorb; only the 70 cent hysteresis keeps one note.
// Teeth: --teeth-hyst0 must split it into 3 or more notes and cut the longest one short.
static void hystGuardCase (bool teethHyst0)
{
    std::printf ("-- hysteresis guard case (1.5 Hz +-62 cent vibrato) --\n");
    const double sr = 48000.0, t0 = 0.1, dur = 1.2;
    const int note = 57;
    std::vector<float> x ((size_t) (sr * 1.5), 0.0f);
    const double twoPi = 2.0 * juce::MathConstants<double>::pi;
    const double f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    const int a = (int) (t0 * sr), len = (int) (dur * sr);
    double ph = 0.0;
    for (int i = 0; i < len; ++i)
    {
        const double s = (double) i / sr;
        ph += twoPi * f0 * std::pow (2.0, 0.62 / 12.0 * std::sin (twoPi * 1.5 * s)) / sr;
        double saw = 0.0;
        for (int h2 = 1; h2 <= 8; ++h2)
            saw += std::sin (h2 * ph) / h2;
        const double env = std::min (1.0, s / 0.005) * std::min (1.0, (dur - s) / 0.005);
        x[(size_t) (a + i)] += (float) ((0.5 * saw + 0.5 * std::sin (ph)) * env * 0.3);
    }
    MidiExport::MelodyParams prm;
    if (teethHyst0)
        prm.hystCents = 0.0;
    const auto tr = MidiExport::trackMelody (x.data(), (int) x.size(), sr, { t0 }, prm);
    double longest = 0.0;
    for (size_t k = 0; k < tr.notes.size(); ++k)
    {
        longest = std::max (longest, tr.notes[k].durSec);
        std::printf ("  note %d: pitch %d start %.4f s dur %.4f s\n", (int) k, tr.notes[k].note, tr.notes[k].sec,
                     tr.notes[k].durSec);
    }
    std::printf ("  notes %d, longest %.3f s (hystCents %.0f)\n", (int) tr.notes.size(), longest, prm.hystCents);
    check (tr.notes.size() == 1 && tr.notes[0].note == note, "hyst guard: slow wide vibrato stays one note (57)");
    check (longest >= 1.0, "hyst guard: the held note lasts at least 1.0 of its 1.2 s");
}

int main (int argc, char** argv)
{
    const double sr = 44100.0, tps = MidiExport::ticksPerSecond (120.0);
    std::vector<float> x ((size_t) (sr * 4.0), 0.0f);
    const Tone tones[] = {
        { 0.25, 0.40, 57, 0.50f, false, false, false }, // A3 loud
        { 0.85, 0.45, 60, 0.03f, false, true,  false }, // C4 quiet, 30 ms dropout
        { 1.50, 0.50, 64, 0.30f, false, false, true  }, // E4, +-50 cent vibrato
        { 2.20, 0.40, 67, 0.40f, true,  false, false }, // G4, one-hop octave glitch
        { 2.85, 0.40, 62, 0.20f, false, false, false }, // D4
    };
    const int nT = (int) (sizeof (tones) / sizeof (tones[0]));
    std::vector<double> onsets;
    for (const auto& t : tones)
    {
        addTone (x, sr, t);
        onsets.push_back (t.sec);
    }

    // ---- M2: melody notes ----
    const auto track = MidiExport::trackNotes (x.data(), (int) x.size(), sr, onsets);
    const auto ons = noteOns (roundTrip (notesFile (track.notes, 0.0)), 1);
    const double tolTicks = 0.03 * tps;
    std::printf ("  tracker notes %d, file notes %d, expected %d (pitched fraction %.2f)\n",
                 (int) track.notes.size(), (int) ons.size(), nT, track.pitchedFraction);
    bool pitchOk = true, onsetOk = true;
    for (int k = 0; k < std::max (nT, (int) ons.size()); ++k)
    {
        if (k < nT && k < (int) ons.size())
        {
            const double want = tones[k].sec * tps;
            std::printf ("  note %d: expected pitch %d tick %.0f | got pitch %d tick %.0f vel %d (dt %+.0f ticks)\n",
                         k, tones[k].note, want, ons[(size_t) k].note, ons[(size_t) k].tick, ons[(size_t) k].vel,
                         ons[(size_t) k].tick - want);
            pitchOk = pitchOk && ons[(size_t) k].note == tones[k].note;
            onsetOk = onsetOk && std::abs (ons[(size_t) k].tick - want) <= tolTicks;
        }
        else if (k < nT)
            std::printf ("  note %d: expected pitch %d tick %.0f | got nothing\n", k, tones[k].note, tones[k].sec * tps);
        else
            std::printf ("  note %d: expected nothing | got spurious pitch %d tick %.0f\n",
                         k, ons[(size_t) k].note, ons[(size_t) k].tick);
    }
    check ((int) ons.size() == nT, "note count equals tone count (no spurious notes, dropout bridged, glitch ignored)");
    check (pitchOk && (int) ons.size() >= nT, "every pitch exact (vibrato under 70 cents holds, no octave note)");
    check (onsetOk && (int) ons.size() >= nT, "every onset within 30 ms (58 ticks)");
    if ((int) ons.size() >= 2)
    {
        const int loud = ons[0].vel, quiet = ons[1].vel;
        std::printf ("  velocity loud (A3, amp 0.50) %d vs quiet (C4, amp 0.03) %d\n", loud, quiet);
        check (loud - quiet >= 30, "loud note velocity at least 30 over the quiet one (24 dB apart)");
    }
    else
        check (false, "velocity ordering (too few notes)");

    // ---- G1: downbeat at 0.6 s, melody and hits share one origin ----
    {
        const double down = 0.6;
        std::vector<MidiExport::Hit> hits;
        std::vector<MidiExport::PitchNote> pn;
        for (const auto& t : tones)
        {
            MidiExport::Hit h; h.sec = t.sec; h.peak = t.amp;
            hits.push_back (h);
            MidiExport::PitchNote p; p.sec = t.sec; p.durSec = t.dur; p.note = t.note; p.rms = t.amp;
            pn.push_back (p);
        }
        const double origin = MidiExport::exportOriginTicks (hits, 120.0, 4, 4, {}, down);
        const auto hf = roundTrip (MidiExport::buildHitsFile (hits, 120.0, 4, 4, std::nullopt, {}, 0, 0.0, down));
        const auto nf = roundTrip (notesFile (pn, origin));
        const auto ho = noteOns (hf, 1), no = noteOns (nf, 1);
        std::printf ("  origin %.0f ticks (downbeat %.2f s)\n", origin, down);
        check (origin >= 500.0, "origin is a real shift (over 500 ticks)");
        double worst = 0.0;
        for (size_t k = 0; k < std::min (ho.size(), no.size()); ++k)
        {
            std::printf ("  event %d: hits tick %.0f | melody tick %.0f (diff %+.0f)\n",
                         (int) k, ho[k].tick, no[k].tick, no[k].tick - ho[k].tick);
            worst = std::max (worst, std::abs (no[k].tick - ho[k].tick));
        }
        check (ho.size() == no.size() && ! ho.empty() && worst <= 1.0,
               "melody notes on the same ticks as the hits (within 1 tick)");
        auto tempoEvents = [] (const juce::MidiFile& f)
        {
            std::vector<std::pair<double, double>> v;
            if (f.getNumTracks() > 0)
                for (int i = 0; i < f.getTrack (0)->getNumEvents(); ++i)
                {
                    const auto& m = f.getTrack (0)->getEventPointer (i)->message;
                    if (m.isTempoMetaEvent())
                        v.push_back ({ m.getTimeStamp(), m.getTempoSecondsPerQuarterNote() });
                }
            return v;
        };
        const auto th = tempoEvents (hf), tn = tempoEvents (nf);
        bool tempoSame = th.size() == tn.size() && ! th.empty();
        for (size_t k = 0; tempoSame && k < th.size(); ++k)
            tempoSame = std::abs (th[k].first - tn[k].first) < 0.5 && std::abs (th[k].second - tn[k].second) < 1.0e-9;
        std::printf ("  tempo events hits %d, melody %d\n", (int) th.size(), (int) tn.size());
        check (tempoSame, "tempo tracks match (same origin)");
    }

    bool teethHyst0 = false, teethNoOrigin = false;
    for (int i = 1; i < argc; ++i)
    {
        teethHyst0 = teethHyst0 || std::strcmp (argv[i], "--teeth-hyst0") == 0;
        teethNoOrigin = teethNoOrigin || std::strcmp (argv[i], "--teeth-no-origin") == 0;
    }
    m2SpecCase (teethHyst0, teethNoOrigin);
    hystGuardCase (teethHyst0);

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
