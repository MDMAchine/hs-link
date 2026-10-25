// HOT-STEP LINK -- TakeStretch.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include "WarpMarkers.h"

// Offline change of a take's tempo: reads one WAV, writes another. No GUI, no
// processor, no globals; never the audio or the message thread. The call owns
// everything it touches (own reader, own buffers) and gives a plain result back.
namespace TakeStretch
{
    constexpr double kMinBpm = 30.0, kMaxBpm = 300.0;
    constexpr double kMinRatio = 0.5, kMaxRatio = 2.0; // output length / input length
    constexpr double kMaxSeconds = 600.0;              // as the tempo job: 10 minutes
    constexpr int kMaxChannels = 8;

    struct StretchRequest
    {
        juce::File source, dest;
        double sourceBpm = 0.0, targetBpm = 0.0;
        // stretch: time stretch, the pitch stays (Signalsmith Stretch).
        // repitch: plain resample, tempo and pitch move together (the A/B reference).
        enum class Mode { stretch, repitch };
        Mode mode = Mode::stretch;
    };

    struct StretchResult
    {
        bool ok = false;
        juce::String error;       // plain text when ok is false
        double ratio = 0.0;       // sourceBpm / targetBpm = output length / input length
        juce::int64 inSamples = 0, outSamples = 0;
        double sampleRate = 0.0;
        int channels = 0;
    };

    // Output length = round (input length x sourceBpm / targetBpm), in both modes. The
    // output starts at the source's first sample (no leading gap, no cut start) and the
    // tail is flushed. Sample rate, channel count and sample format of the source are
    // kept (16 and 24 bit stay so, 32 bit float stays float).
    // The source is opened read only and never modified. Refused: dest equal to source,
    // a dest that exists (nothing is ever overwritten), a source or dest that is not a
    // .wav, a tempo outside kMinBpm..kMaxBpm, a ratio outside kMinRatio..kMaxRatio, a
    // header that is not sane, a take longer than kMaxSeconds, and in mode stretch a
    // take shorter than the library needs to start (about 0.2 s).
    // The audio is written to a temporary name beside dest and renamed (no replace) when
    // complete; on failure or abort the partial file is removed and dest is not created.
    // shouldAbort is polled per block. Never throws: every failure is ok = false.
    StretchResult stretchTakeFile (const StretchRequest& request, const std::function<bool()>& shouldAbort = {});

    // Piecewise warp in memory (Mock-up B, round 2b), for the editor preview. markers
    // are in frames of in (src -> dst, ascending, first at 0, last at the end); each
    // region src [a, b) is time stretched to its dst length (Signalsmith, pitch kept)
    // and the regions are concatenated, so the output length is the sum of the dst
    // lengths. A region of equal length is copied; one too short for the stretcher
    // or outside kMinRatio..kMaxRatio is resampled linearly. bypassStretch (tests,
    // the teeth): the input comes back unwarped. Off the audio thread only;
    // allocates. Empty buffer when aborted or markers are not usable.
    juce::AudioBuffer<float> renderWarped (const juce::AudioBuffer<float>& in, double sampleRate,
                                           const std::vector<WarpMarkers::Marker>& markers,
                                           const std::function<bool()>& shouldAbort = {}, bool bypassStretch = false);

    // Tests only, null in the plugin: when not null it replaces the random token of the
    // temporary name ("<dest name>.<token>.stretching"); the pointer must stay valid
    // while set.
    extern std::atomic<const char*> testPartToken;
}
