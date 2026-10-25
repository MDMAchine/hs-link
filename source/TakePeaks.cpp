// HOT-STEP LINK -- TakePeaks.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#include "TakePeaks.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cmath>

namespace TakePeaks
{
    static Data failed (Data d, const juce::String& why)
    {
        d.ok = false;
        d.error = why;
        d.mins.clear();
        d.maxs.clear();
        return d;
    }

    Data build (const juce::File& file, const std::function<bool()>& shouldAbort,
                const std::function<void (double)>& progress, const std::function<void (const Data&)>& onHeader)
    {
        Data d;
        d.buildThread = juce::Thread::getCurrentThreadId();
        try
        {
            if (! file.existsAsFile())
                return failed (d, "the file is missing");
            juce::WavAudioFormat wav;
            auto in = file.createInputStream();
            if (in == nullptr)
                return failed (d, "the file cannot be opened");
            std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (in.release(), true));
            if (reader == nullptr)
                return failed (d, "not a readable WAV file");
            d.length = reader->lengthInSamples;
            d.sampleRate = reader->sampleRate;
            d.channels = (int) reader->numChannels;
            if (d.channels < 1 || d.channels > kMaxChannels || ! (d.sampleRate >= 1000.0 && d.sampleRate <= 768000.0) || d.length < 1)
                return failed (d, "the WAV header is not usable");
            if ((double) d.length / d.sampleRate > kMaxSeconds)
                return failed (d, "the take is longer than 10 minutes");
            d.lanes = d.channels == 2 ? 2 : 1;
            if (onHeader)
                onHeader (d);

            const auto buckets = (size_t) d.numBuckets();
            d.mins.assign ((size_t) d.lanes, std::vector<float> (buckets, 0.0f));
            d.maxs.assign ((size_t) d.lanes, std::vector<float> (buckets, 0.0f));
            juce::AudioBuffer<float> block (d.channels, kBlockFrames);
            for (juce::int64 pos = 0; pos < d.length; pos += kBlockFrames)
            {
                if (shouldAbort && shouldAbort())
                    return failed (d, "aborted");
                const int n = (int) juce::jmin ((juce::int64) kBlockFrames, d.length - pos);
                if (! reader->read (&block, 0, n, pos, true, true))
                    return failed (d, "the file could not be read");
                // kBlockFrames is a multiple of kBucketFrames, so a bucket never spans two blocks.
                for (int at = 0; at < n; at += kBucketFrames)
                {
                    const int len = juce::jmin (kBucketFrames, n - at);
                    const auto b = (size_t) ((pos + at) / kBucketFrames);
                    if (d.lanes == 2)
                    {
                        for (int c = 0; c < 2; ++c)
                        {
                            const auto r = juce::FloatVectorOperations::findMinAndMax (block.getReadPointer (c, at), len);
                            d.mins[(size_t) c][b] = r.getStart();
                            d.maxs[(size_t) c][b] = r.getEnd();
                        }
                    }
                    else
                    {
                        float lo = 0.0f, hi = 0.0f;
                        for (int c = 0; c < d.channels; ++c)
                        {
                            const auto r = juce::FloatVectorOperations::findMinAndMax (block.getReadPointer (c, at), len);
                            lo = c == 0 ? r.getStart() : juce::jmin (lo, r.getStart());
                            hi = c == 0 ? r.getEnd() : juce::jmax (hi, r.getEnd());
                        }
                        d.mins[0][b] = lo;
                        d.maxs[0][b] = hi;
                    }
                }
                if (progress)
                    progress ((double) (pos + n) / (double) d.length);
            }
            d.ok = true;
            return d;
        }
        catch (...)
        {
            return failed (d, "reading the take failed");
        }
    }

    bool columnMinMax (const Data& data, int lane, juce::int64 firstFrame, double framesPerPixel, float& lo, float& hi)
    {
        lo = hi = 0.0f;
        if (lane < 0 || lane >= (int) data.mins.size() || lane >= (int) data.maxs.size() || ! (framesPerPixel > 0.0))
            return false;
        const auto& mn = data.mins[(size_t) lane];
        const auto& mx = data.maxs[(size_t) lane];
        const auto buckets = (juce::int64) juce::jmin (mn.size(), mx.size());
        const auto span = juce::jmax ((juce::int64) 1, (juce::int64) std::ceil (framesPerPixel));
        const auto endFrame = juce::jmin (data.length, firstFrame + span); // exclusive
        if (firstFrame < 0 || firstFrame >= endFrame)
            return false;
        const auto b0 = firstFrame / kBucketFrames;
        const auto b1 = juce::jmin (buckets, (endFrame + kBucketFrames - 1) / kBucketFrames); // exclusive
        if (b0 >= b1)
            return false;
        lo = mn[(size_t) b0];
        hi = mx[(size_t) b0];
        for (auto b = b0 + 1; b < b1; ++b)
        {
            lo = juce::jmin (lo, mn[(size_t) b]);
            hi = juce::jmax (hi, mx[(size_t) b]);
        }
        return true;
    }
}
