#ifndef HSLINK_STUB_DEFAULT_PATH
 #define HSLINK_STUB_DEFAULT_PATH "../HOT-Step-CPP/server/scripts/daw-contract-stub.mjs"
#endif
// HOT-STEP LINK -- BypassUploadTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Host bypass arriving while a job is still sending (repaint with a source upload, the
// stub answers POST /generate after 5 s). Checks: the bypassed blocks pass the input
// through bit-exact; the job keeps going and reaches done (bypass does not touch jobs),
// state sequence printed; un-bypass gives the same output as before the bypass; the
// take lands once; destroy is quick and leaves no partial file or open handle (the
// takes folder can be deleted right after).
// Needs node and the contract stub (HSLINK_STUB, default HSLINK_STUB_DEFAULT_PATH from CMake).
// Scratch under <cwd>/bypass_upload_scratch, removed at the end.
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

juce::AudioBuffer<float> noise (int ch, int n, juce::uint32 seed)
{
    juce::AudioBuffer<float> b (ch, n);
    juce::uint32 s = seed;
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            b.setSample (c, i, 0.5f * ((float) (s & 0xffff) / 32768.0f - 1.0f));
        }
    return b;
}

float maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    float m = 0.0f;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            m = juce::jmax (m, std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return m;
}

// Runs one block through the processor, normal or bypassed; returns the output.
juce::AudioBuffer<float> runBlock (HotStepLinkProcessor& p, const juce::AudioBuffer<float>& in, bool bypassed)
{
    juce::AudioBuffer<float> b (in);
    juce::MidiBuffer m;
    if (bypassed)
        p.processBlockBypassed (b, m);
    else
        p.processBlock (b, m);
    return b;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile ("bypass_upload_scratch");
    scratch.deleteRecursively();
    scratch.createDirectory();
    std::printf ("Bypass during source upload\n");

    auto stub = juce::File (juce::SystemStats::getEnvironmentVariable (
        "HSLINK_STUB", HSLINK_STUB_DEFAULT_PATH));
    if (! stub.existsAsFile())
    {
        std::printf ("  [INFO] stub not found, skipped\n");
        return 1;
    }
    juce::ChildProcess server;
    check (server.start ("node \"" + stub.getFullPathName() + "\" --ok --port 3921", 0), "stub started");
    juce::Thread::sleep (500);

    // Source clip for the multipart upload: 0.5 s stereo float 48 kHz.
    auto src = scratch.getChildFile ("src.wav");
    {
        juce::AudioBuffer<float> b (2, 24000);
        for (int i = 0; i < 24000; ++i)
        {
            const float v = 0.3f * (float) std::sin (0.05 * i);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        std::unique_ptr<juce::OutputStream> os (src.createOutputStream());
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0)
                                              .withNumChannels (2).withBitsPerSample (32)
                                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, 24000);
    }

    auto takes = scratch.getChildFile ("takes");
    {
        auto p = std::make_unique<HotStepLinkProcessor>();
        const int bs = 512;
        p->setPlayConfigDetails (2, 2, 48000.0, bs);
        p->prepareToPlay (48000.0, bs);
        p->daw.setEndpoint ("http://127.0.0.1:3921", "test-key");
        p->daw.setTakesFolder (takes);
        p->daw.start();

        bool online = false;
        for (int i = 0; i < 600 && ! online; ++i) { online = p->daw.getSnapshot().online; if (! online) juce::Thread::sleep (20); }
        check (online, "worker online before the submit");

        const auto in = noise (2, bs, 0x1234567u);
        const auto refOut = runBlock (*p, in, false); // normal output before any bypass

        DawJobRequest r;
        r.mode = "repaint";
        r.caption = "slowsubmit"; // stub holds the generate answer 5 s
        r.takes = 1;
        r.sourceWav = src;
        r.hasRegion = true;
        r.regionStartS = 0.1;
        r.regionEndS = 0.4;
        const int id = p->daw.submitGenerate (r);
        check (id != 0, "job accepted");

        // Wait for the sending state (submitting flag up).
        juce::StringArray seq;
        auto note = [&] (const DawJob& j)
        {
            const auto s = DawJob::label (j);
            if (seq.isEmpty() || seq[seq.size() - 1] != s) seq.add (s);
        };
        bool sending = false;
        for (int i = 0; i < 300 && ! sending; ++i)
        {
            DawJob j;
            if (p->daw.getJob (id, j)) { note (j); sending = j.submitting; }
            if (! sending) juce::Thread::sleep (10);
        }
        check (sending, "job reached sending (upload in flight)");

        // Bypass for ~1 s of blocks while the upload is held (HSLINK_BYP_CONTROL=1: no bypass, control run).
        const bool noBypass = juce::SystemStats::getEnvironmentVariable ("HSLINK_BYP_CONTROL", {}) == "1";
        float bypDiff = 0.0f;
        int bypBlocks = 0, bypWhileSending = 0;
        for (int k = 0; k < 100; ++k)
        {
            const auto bin = noise (2, bs, 0x9000u + (juce::uint32) k);
            const auto out = noBypass ? bin : runBlock (*p, bin, true);
            bypDiff = juce::jmax (bypDiff, maxAbsDiff (bin, out));
            ++bypBlocks;
            DawJob j;
            if (p->daw.getJob (id, j)) { note (j); if (j.submitting) ++bypWhileSending; }
            juce::Thread::sleep (10);
        }
        std::printf ("  bypassed blocks=%d (while sending=%d), max abs diff in->out=%.9g\n", bypBlocks, bypWhileSending, (double) bypDiff);
        check (bypWhileSending > 0 && bypDiff == 0.0f, "bypass during upload passes audio through bit-exact");

        // Un-bypass: the same input gives the same output as before the bypass.
        const auto afterOut = runBlock (*p, in, false);
        const float unDiff = maxAbsDiff (refOut, afterOut);
        bool finite = true;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < bs; ++i)
                finite = finite && std::isfinite (afterOut.getSample (c, i));
        std::printf ("  un-bypass: max abs diff vs pre-bypass output=%.9g, finite=%d\n", (double) unDiff, (int) finite);
        check (finite && unDiff == 0.0f, "un-bypass resumes normal output");

        // The job keeps going: bypass does not cancel it.
        DawJob j;
        bool fin = false;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < 30000.0)
        {
            if (p->daw.getJob (id, j)) { note (j); if (j.isFinished()) { fin = true; break; } }
            runBlock (*p, in, false);
            juce::Thread::sleep (20);
        }
        std::printf ("  state sequence: %s; final=%s takes=%d err='%s'\n", seq.joinIntoString (" > ").toRawUTF8(),
                     DawJob::stateName (j.state), j.takeFiles.size(), j.error.toRawUTF8());
        check (fin && j.state == DawJob::State::done, "job sent during bypass completes (done)");
        juce::Thread::sleep (500); // a duplicate landing would show up here
        auto wavs = takes.findChildFiles (juce::File::findFiles, false, "*.wav*");
        int jobsWithId = 0;
        for (auto& x : p->daw.getJobs()) if (x.localId == id) ++jobsWithId;
        std::printf ("  takeFiles=%d, files in takes folder=%d, job rows=%d\n", j.takeFiles.size(), wavs.size(), jobsWithId);
        check (j.takeFiles.size() == 1 && wavs.size() == 1 && jobsWithId == 1, "take lands once, not twice");

        const auto d0 = juce::Time::getMillisecondCounterHiRes();
        p.reset();
        const double ms = juce::Time::getMillisecondCounterHiRes() - d0;
        auto left = takes.findChildFiles (juce::File::findFiles, false, "*.part");
        std::printf ("  processor destroyed in %.1f ms, partial files=%d\n", ms, left.size());
        check (ms < 1000.0 && left.isEmpty(), "destroy is quick, no partial file");
    }
    const bool gone = takes.deleteRecursively() && ! takes.exists();
    check (gone, "takes folder deletable after destroy (no open file handle)");

    server.kill();
    scratch.deleteRecursively();
    std::printf ("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
