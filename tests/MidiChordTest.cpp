// HOT-STEP LINK -- MidiChordTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// C2 (plan Section 3): 5 bars at 100 BPM, C Am F G7 Dsus4 (one chord per bar, bass on the
// root, a melody with a non-chord neighbour tone on beat 3 of each bar), then the same
// progression with a kick on every beat. ChordExport::analyseChords + buildChordsFile:
// names in order, changes on bar ticks only, bass note = root, C major, per-bar accuracy
// 5 of 5 clean and at least 4 of 5 with the kick.
// Teeth (-DHSLINK_TEETH=...): CHORD_NO_KEY (no in-key bonus), CHORD_NO_SMOOTH (no change
// cost, no mid-bar margin, no 2-beat rule), CHORD_ROOT_SWAP (bass bonus and bass note on
// the fifth).
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "ChordExport.h"

static int failures = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

static double mtof (double m) { return 440.0 * std::pow (2.0, (m - 69.0) / 12.0); }

struct Bar { const char* name; int root; std::vector<int> tones; int melody[4]; };

// Bars: chord tones (MIDI, octave 4), melody per beat (MIDI, octave 5; beat 3 a neighbour).
static const std::vector<Bar>& bars()
{
    static const std::vector<Bar> b = {
        { "C",     0, { 60, 64, 67 },     { 72, 76, 74, 72 } }, // D on beat 3
        { "Am",    9, { 57, 60, 64 },     { 69, 72, 71, 69 } }, // B on beat 3
        { "F",     5, { 57, 60, 65 },     { 69, 72, 70, 69 } }, // Bb on beat 3
        { "G7",    7, { 55, 59, 62, 65 }, { 71, 74, 72, 71 } }, // C on beat 3
        { "Dsus4", 2, { 57, 62, 67 },     { 74, 79, 76, 74 } }, // E on beat 3
    };
    return b;
}

static juce::AudioBuffer<float> render (double sr, double bpm, bool kick)
{
    const double beat = 60.0 / bpm;
    const auto& bs = bars();
    const int n = (int) std::ceil (sr * beat * 4.0 * (double) bs.size());
    juce::AudioBuffer<float> buf (2, n);
    buf.clear();
    const double tau = juce::MathConstants<double>::twoPi;
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        const int bi = juce::jmin ((int) bs.size() - 1, (int) (t / (4.0 * beat)));
        const int bt = juce::jmin (3, (int) ((t - bi * 4.0 * beat) / beat));
        const double tb = t - (bi * 4.0 + bt) * beat; // time since this beat
        const auto& b = bs[(size_t) bi];
        const double env = 0.35 + 0.65 * std::exp (-tb / 0.4);
        double v = 0.0;
        auto tone = [&] (double m, double a)
        {
            const double f = mtof (m);
            for (int h = 1; h <= 4; ++h)
                v += a / h * std::sin (tau * f * h * t);
        };
        tone (36.0 + b.root, 0.30 * env);               // bass on the root, octave 2
        for (int m : b.tones) tone ((double) m, 0.12 * env);
        if (std::getenv ("HSLINK_CHORD_NO_MELODY") == nullptr)
            tone ((double) b.melody[bt], 0.10 * env);
        if (kick && tb < 0.25)
        {
            const double ph = tau * (45.0 * tb + (120.0 - 45.0) * 0.03 * (1.0 - std::exp (-tb / 0.03)));
            v += 0.9 * std::sin (ph) * std::exp (-tb / 0.12);
        }
        buf.setSample (0, i, (float) (0.4 * v));
        buf.setSample (1, i, (float) (0.4 * v));
    }
    return buf;
}

static void runCase (const char* label, bool kick, int minCorrect)
{
    const double sr = 44100.0, bpm = 100.0;
    const int barTicks = 4 * ChordExport::kPpq;
    const auto buf = render (sr, bpm, kick);
    const auto res = ChordExport::analyseChords (buf, sr, bpm, 0.0, 4);
    std::printf ("  %s tonality=%.3f threshold=%.3f\n", label, res.tonality, ChordExport::kMinTonality);
    check (res.tonality > 1.25 * ChordExport::kMinTonality, (juce::String (label) + ": tonality at least 25% over the gate").toRawUTF8());
    if (std::getenv ("HSLINK_CHORD_DEBUG") != nullptr)
    {
        const auto beats = ChordExport::beatChroma (buf, sr, bpm, 0.0, 4);
        for (size_t b = 0; b < 4 && b < beats.size(); ++b)
        {
            std::printf ("  beat %d chroma:", (int) b);
            for (double c : beats[b].chroma) std::printf (" %.3f", c);
            std::printf ("\n    C %.3f  Csus2 %.3f\n", ChordExport::stateScore (beats[b], 0 * ChordExport::kNumQualities + ChordExport::Maj, res.key),
                         ChordExport::stateScore (beats[b], 0 * ChordExport::kNumQualities + ChordExport::Sus2, res.key));
        }
    }
    std::printf ("[%s] key tonic %d minor %d sig %d, %d chords:\n", label, res.key.tonic, (int) res.key.minor,
                 res.key.sig.sharpsOrFlats, (int) res.chords.size());
    int midBar = 0;
    for (const auto& c : res.chords)
    {
        const bool onBar = std::fmod (c.startTicks, (double) barTicks) == 0.0;
        midBar += onBar ? 0 : 1;
        std::printf ("  %-6s root %2d q %d  ticks %7.0f..%7.0f%s\n", c.name.toRawUTF8(), c.root, c.quality,
                     c.startTicks, c.endTicks, onBar ? "" : "  (mid-bar)");
    }
    const auto& bs = bars();
    int correct = 0;
    for (size_t k = 0; k < bs.size(); ++k)
    {
        const double mid = (double) k * barTicks + barTicks / 2.0;
        juce::String got = "N";
        for (const auto& c : res.chords)
            if (c.startTicks <= mid && mid < c.endTicks) got = c.name;
        correct += got == bs[k].name ? 1 : 0;
        std::printf ("  bar %d: want %-6s got %s\n", (int) k + 1, bs[k].name, got.toRawUTF8());
    }
    const double pct = 100.0 * correct / (double) bs.size();
    std::printf ("  per-bar accuracy %d of %d = %.0f%%, mid-bar changes %d\n", correct, (int) bs.size(), pct, midBar);

    // Bass channel read back from the written file: note k = 36 + root of chord k.
    const auto f = ChordExport::buildChordsFile (res.chords, "chords", bpm, 4, 4, res.key.sig);
    juce::MemoryOutputStream o;
    f.writeTo (o, 1);
    juce::MemoryInputStream in (o.getData(), o.getDataSize(), false);
    juce::MidiFile back;
    back.readFrom (in);
    std::vector<int> bass;
    if (back.getNumTracks() >= 2)
        for (auto* e : *back.getTrack (1))
            if (e->message.isNoteOn() && e->message.getChannel() == ChordExport::kBassChannel)
                bass.push_back (e->message.getNoteNumber());
    int bassOk = 0;
    std::printf ("  bass notes:");
    for (size_t k = 0; k < bass.size(); ++k)
    {
        const int want = k < res.chords.size() ? 36 + res.chords[k].root : -1;
        std::printf (" %d(want %d)", bass[k], want);
        bassOk += bass[k] == want ? 1 : 0;
    }
    std::printf ("\n");

    const juce::String L (label);
    check (! res.key.minor && res.key.tonic == 0 && res.key.sig.sharpsOrFlats == 0, (L + ": key C major, 0 sharps").toRawUTF8());
    check (correct >= minCorrect, (L + ": per-bar accuracy at least " + juce::String (minCorrect) + " of 5").toRawUTF8());
    check (midBar == 0, (L + ": every chord change on a bar tick (0 mid-bar changes)").toRawUTF8());
    check (bass.size() == res.chords.size() && bassOk == (int) bass.size() && ! bass.empty(),
           (L + ": every bass note on channel 2 = 36 + chord root").toRawUTF8());
    if (! kick)
    {
        bool order = res.chords.size() == bs.size();
        for (size_t k = 0; order && k < bs.size(); ++k)
            order = res.chords[k].name == bs[k].name && res.chords[k].root == bs[k].root
                    && res.chords[k].startTicks == (double) k * barTicks;
        check (order, "clean: exactly C Am F G7 Dsus4, roots 0 9 5 7 2, each starting on its bar tick");
        bool q = res.chords.size() == bs.size()
                 && res.chords[0].quality == ChordExport::Maj && res.chords[1].quality == ChordExport::Min
                 && res.chords[2].quality == ChordExport::Maj && res.chords[3].quality == ChordExport::Dom7
                 && res.chords[4].quality == ChordExport::Sus4;
        check (q, "clean: qualities maj min maj 7 sus4");
    }
}

int main()
{
    runCase ("clean", false, 5);
    runCase ("kick", true, 5);
    std::printf ("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
