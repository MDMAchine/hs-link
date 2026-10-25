// HOT-STEP LINK -- TakePeaks.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_core/juce_core.h>
#include <functional>
#include <vector>

// Min and max peaks of a take's WAV for the take editor's waveform. No GUI, no
// processor, no globals; never the audio or the message thread. The call owns
// everything it touches (own reader, own buffers) and gives plain data back.
namespace TakePeaks
{
    constexpr int kBucketFrames = 64;    // one min and one max per this many frames
    constexpr int kBlockFrames = 32768;  // read block; shouldAbort is polled once per block
    constexpr double kMaxSeconds = 600.0;
    constexpr int kMaxChannels = 8;

    struct Data
    {
        bool ok = false;
        juce::String error;        // plain text when ok is false
        juce::int64 length = 0;    // frames of the file
        double sampleRate = 0.0;
        int channels = 0;
        // Lanes: mono = 1; stereo = 2 (left, right); more than 2 channels = 1 lane with
        // the min and max over all channels.
        int lanes = 0;
        // mins[lane][b] and maxs[lane][b] cover frames b * kBucketFrames up to
        // (b + 1) * kBucketFrames (the last bucket ends at length).
        std::vector<std::vector<float>> mins, maxs;
        // Tests: the thread build ran on (juce::Thread::getCurrentThreadId).
        juce::Thread::ThreadID buildThread = {};
        juce::int64 numBuckets() const { return (length + kBucketFrames - 1) / kBucketFrames; }
    };

    // Reads file with its own reader. onHeader, when set, is called once with length,
    // sampleRate, channels and lanes filled (no peaks yet) before the first block is
    // read. progress gets 0..1 after each block. shouldAbort is polled per block
    // ("aborted"). Never throws: every failure is ok = false with an error text.
    Data build (const juce::File& file, const std::function<bool()>& shouldAbort = {},
                const std::function<void (double)>& progress = {},
                const std::function<void (const Data&)>& onHeader = {});

    // One pixel column: the min and max of lane over the frames firstFrame up to
    // firstFrame + framesPerPixel (at least one frame), widened to whole buckets and
    // cut to the file. Pure. false (lo = hi = 0) when the range is outside the data.
    bool columnMinMax (const Data& data, int lane, juce::int64 firstFrame, double framesPerPixel, float& lo, float& hi);
}
