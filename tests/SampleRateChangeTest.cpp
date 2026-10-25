// HOT-STEP LINK -- SampleRateChangeTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// One live processor, prepareToPlay called again at 44.1 -> 96 -> 48 kHz with the
// capture, the take audition and the take editor audition alive. Checks: processBlock
// output finite and of the right length after each change; the take crossfade (10 ms),
// the editor Play hold (12 ms duck) and the editor head fade (5 ms) follow the new rate;
// the editor clip that plays across a re-prepare is dropped to exact silence (the
// documented behaviour, no fade at the change); a capture running across a re-prepare
// keeps its own rate, and a capture started after it uses the new rate.
// Writes its WAVs under <cwd>/srate_scratch and removes that folder at the end.
#include "PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0, passes = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? passes : failures)++;
}

juce::File writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double rate)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (os == nullptr)
        return f;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                          .withSampleRate (rate)
                                          .withNumChannels (b.getNumChannels())
                                          .withBitsPerSample (32)
                                          .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    return f;
}

juce::int64 wavFrames (const juce::File& f, double* rateOut)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> r (wav.createReaderFor (new juce::FileInputStream (f), true));
    if (r == nullptr)
        return -1;
    if (rateOut != nullptr)
        *rateOut = r->sampleRate;
    return r->lengthInSamples;
}

juce::AudioBuffer<float> dc (int ch, int n, float v)
{
    juce::AudioBuffer<float> b (ch, n);
    for (int c = 0; c < ch; ++c)
        juce::FloatVectorOperations::fill (b.getWritePointer (c), v, n);
    return b;
}

std::unique_ptr<AuditionTake> dcTake (int n, float v)
{
    auto t = std::make_unique<AuditionTake>();
    t->audio = dc (2, n, v);
    t->rms = std::abs (v);
    t->targetRms = std::abs (v);
    return t;
}

constexpr int kBlock = 256;

// processBlock on noise: every output sample finite, buffer kept at 2 x kBlock.
void processCheck (HotStepLinkProcessor& p, double r, juce::uint32& seed, bool takeActive)
{
    const int ch = juce::jmax (2, p.getTotalNumOutputChannels());
    int bad = 0, blocks = 64;
    bool sizeOk = true;
    float peak = 0.0f;
    juce::MidiBuffer midi;
    for (int b = 0; b < blocks; ++b)
    {
        juce::AudioBuffer<float> buf (ch, kBlock);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                buf.setSample (c, i, ((float) (seed & 0xFFFF) / 32768.0f - 1.0f) * 0.5f);
            }
        p.processBlock (buf, midi);
        sizeOk = sizeOk && buf.getNumSamples() == kBlock && buf.getNumChannels() == ch;
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < kBlock; ++i)
            {
                const float v = buf.getSample (c, i);
                if (! std::isfinite (v)) ++bad;
                else peak = juce::jmax (peak, std::abs (v));
            }
    }
    std::printf ("  %.0f Hz processBlock (take active %d): %d blocks x %d, non-finite %d, peak %.4f\n",
                 r, (int) takeActive, blocks, kBlock, bad, peak);
    char msg[200];
    // A take selected with the host transport stopped replaces the input with the
    // silence of the take; no take = the input passes (noise peak about 0.5).
    if (takeActive)
        std::snprintf (msg, sizeof (msg), "%.0f Hz: processBlock finite, right length, take active = exact silence", r);
    else
        std::snprintf (msg, sizeof (msg), "%.0f Hz: processBlock finite, right length, input passes (peak > 0.4)", r);
    check (bad == 0 && sizeOk && (takeActive ? peak == 0.0f : peak > 0.4f), msg);
}

// Take crossfade: DC 1 take, then a DC 0 take; returns the index of the first exact 0.
int measureCrossfade (HotStepLinkProcessor& p)
{
    p.audition.publish (dcTake (96000, 1.0f));
    juce::AudioBuffer<float> buf (2, kBlock);
    for (int k = 0; k < 8; ++k) { buf.clear(); p.audition.render (buf, kBlock, 0, true, false); }
    p.audition.publish (dcTake (96000, 0.0f));
    int first0 = -1, idx = 0;
    for (int k = 0; k < 16 && first0 < 0; ++k)
    {
        buf.clear();
        p.audition.render (buf, kBlock, 0, true, false);
        for (int i = 0; i < kBlock; ++i, ++idx)
            if (first0 < 0 && buf.getSample (0, i) == 0.0f) first0 = idx;
    }
    p.audition.publish (nullptr);
    for (int k = 0; k < 16; ++k) { buf.clear(); p.audition.render (buf, kBlock, 0, true, false); }
    p.audition.collectGarbage();
    return first0;
}

bool waitPlaying (EditorAudition& ea)
{
    for (int i = 0; i < 400; ++i)
    {
        if (ea.getState() == EditorAudition::playing) return true;
        juce::Thread::sleep (5);
    }
    return false;
}

// Editor Play of a DC 0.5 loop on silent input: hold = first non-zero index, fade =
// samples from there until exact 0.5.
void measurePlay (HotStepLinkProcessor& p, const juce::File& take, int& hold, int& fade, bool& playing)
{
    p.editorAudition.play (take, 0, 0, 0, true);
    playing = waitPlaying (p.editorAudition);
    hold = -1; fade = -1;
    juce::AudioBuffer<float> buf (2, kBlock);
    int idx = 0;
    for (int k = 0; k < 32 && fade < 0; ++k)
    {
        buf.clear();
        p.editorAudition.render (buf, kBlock);
        for (int i = 0; i < kBlock; ++i, ++idx)
        {
            const float v = buf.getSample (0, i);
            if (hold < 0 && v != 0.0f) hold = idx;
            if (hold >= 0 && fade < 0 && v >= 0.5f) fade = idx - hold;
        }
    }
}

// Silent input through the editor audition: largest sample over n samples.
float editorPeak (HotStepLinkProcessor& p, int n)
{
    juce::AudioBuffer<float> buf (2, kBlock);
    float pk = 0.0f;
    for (int done = 0; done < n; done += kBlock)
    {
        buf.clear();
        p.editorAudition.render (buf, kBlock);
        pk = juce::jmax (pk, buf.getMagnitude (0, kBlock));
    }
    return pk;
}

juce::File captureFor (HotStepLinkProcessor& p, const juce::File& out, int samples)
{
    juce::AudioBuffer<float> b = dc (2, kBlock, 0.25f);
    for (int done = 0; done < samples; done += kBlock)
        p.capture.push (b, 0, juce::jmin (kBlock, samples - done));
    return out;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile ("srate_scratch");
    dir.deleteRecursively();
    dir.createDirectory();

    HotStepLinkProcessor p;
    juce::uint32 seed = 0x2468ACEu;
    const double rates[] = { 44100.0, 96000.0, 48000.0 };
    char msg[200];
    bool first = true;
    for (double r : rates)
    {
        const bool editorWasPlaying = p.editorAudition.getState() == EditorAudition::playing;
        if (first)
        {
            // The capture starts at 44.1 kHz and runs across the change to 96 kHz.
            p.setRateAndBufferSizeDetails (r, kBlock);
            p.prepareToPlay (r, kBlock);
            check (p.capture.startCapture (dir.getChildFile ("cap_live.wav")), "capture starts at 44.1 kHz");
            captureFor (p, {}, 44100);
        }
        else
        {
            juce::AudioBuffer<float> buf (2, kBlock);
            buf.clear();
            p.audition.render (buf, kBlock, 0, true, false);
            p.setRateAndBufferSizeDetails (r, kBlock);
            p.prepareToPlay (r, kBlock); // re-prepare on a processor that already ran
            // The editor clip that was playing: exact silence from the first sample.
            const float pkFirst = editorPeak (p, kBlock);
            const float pkNext = editorPeak (p, 8192);
            std::printf ("  %.0f Hz after re-prepare: editor was playing %d, first block peak %.9f, next 8192 peak %.9f, state %d\n",
                         r, (int) editorWasPlaying, pkFirst, pkNext, (int) p.editorAudition.getState());
            std::snprintf (msg, sizeof (msg), "%.0f Hz: clip playing across re-prepare drops to exact silence, idle", r);
            check (editorWasPlaying && pkFirst < 1.0e-9f && pkNext < 1.0e-9f
                       && p.editorAudition.getState() == EditorAudition::idle, msg);
            p.editorAudition.poll();
        }

        if (r == 96000.0)
        {
            // Second half of the live capture at 96 kHz, then stop. Pushed: 44100 + the
            // 44.1 kHz processBlock run (an unarmed capture takes processBlock input too,
            // two runs of 64 x 256) + 44100, all at the 44.1 kHz start rate, out at 48 kHz.
            captureFor (p, {}, 44100);
            const juce::int64 wantCap = std::llround ((44100.0 * 2 + 2 * 64 * kBlock) * 48000.0 / 44100.0);
            const bool alive = p.capture.isCapturing();
            const bool stopped = p.capture.stopCapture();
            double fr = 0.0;
            const auto frames = wavFrames (p.capture.getLastFile(), &fr);
            std::printf ("  live capture across 44.1->96 kHz: alive %d, stop %d, %lld frames at %.0f Hz (want %lld at 48000)\n",
                         (int) alive, (int) stopped, (long long) frames, fr, (long long) wantCap);
            check (alive && stopped && fr == 48000.0 && std::llabs (frames - wantCap) <= 2,
                   "capture alive across re-prepare keeps its 44.1 kHz start rate");
            p.prepareToPlay (r, kBlock); // not capturing now: the ring takes the new rate
        }
        if (! first)
        {
            // A capture started after the change: 1 s at the new rate = 48000 frames out.
            p.capture.startCapture (dir.getChildFile (juce::String ("cap_") + juce::String ((int) r) + ".wav"));
            captureFor (p, {}, (int) r);
            p.capture.stopCapture();
            double fr = 0.0;
            const auto frames = wavFrames (p.capture.getLastFile(), &fr);
            std::printf ("  %.0f Hz new capture of %d samples: %lld frames at %.0f Hz (want 48000)\n", r, (int) r, (long long) frames, fr);
            std::snprintf (msg, sizeof (msg), "%.0f Hz: capture after re-prepare uses the new rate (48000 frames)", r);
            check (fr == 48000.0 && std::llabs (frames - 48000) <= 2, msg);
        }

        processCheck (p, r, seed, ! first); // a take is alive across every re-prepare

        const int xf = measureCrossfade (p); // ends with no take
        processCheck (p, r, seed, false);
        const int wantXf = AuditionMath::crossfadeSamples (r);
        std::printf ("  %.0f Hz take crossfade: first exact 0 at %d (want %d = 10 ms)\n", r, xf, wantXf);
        std::snprintf (msg, sizeof (msg), "%.0f Hz: take crossfade is %d samples", r, wantXf);
        check (xf == wantXf && wantXf == (int) std::lround (r * 0.010), msg);

        // Let any duck return finish (a new duck length is taken at the end of a ramp).
        editorPeak (p, 8192);
        int hold = 0, fade = 0;
        bool playing = false;
        // A take at the host rate: no resampler edge in the hold and fade counts.
        const auto take = writeWav (dir.getChildFile (juce::String ("dc_") + juce::String ((int) r) + ".wav"),
                                    dc (2, (int) r, 0.5f), r);
        measurePlay (p, take, hold, fade, playing);
        const int wantHold = (int) std::lround (r * 0.012), wantFade = (int) std::lround (r * 0.005);
        std::printf ("  %.0f Hz editor Play: playing %d, hold %d (want %d = 12 ms), head fade %d (want %d or %d = 5 ms)\n",
                     r, (int) playing, hold, wantHold, fade, wantFade - 1, wantFade);
        std::snprintf (msg, sizeof (msg), "%.0f Hz: editor Play hold is %d samples", r, wantHold);
        check (playing && hold == wantHold && hold == EditorAudition::duckSamples (r), msg);
        std::snprintf (msg, sizeof (msg), "%.0f Hz: editor head fade is %d samples (+-1)", r, wantFade);
        check (playing && fade >= wantFade - 1 && fade <= wantFade + 1, msg);
        p.audition.publish (dcTake (96000, 0.3f)); // a take alive across the next change
        first = false;
    }

    p.editorAudition.stop();
    editorPeak (p, 4096);
    p.editorAudition.poll();
    p.releaseResources();
    std::printf ("\n%d passed, %d failed\n", passes, failures);
    dir.deleteRecursively();
    return failures == 0 ? 0 : 1;
}
