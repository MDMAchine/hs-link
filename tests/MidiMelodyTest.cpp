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

int main()
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

    std::printf ("%s (%d failures)\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
