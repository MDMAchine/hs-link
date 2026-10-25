// HOT-STEP LINK -- TakeStretch.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#include "TakeStretch.h"
#include "ExclusiveFile.h"
#include "WavTempoTag.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <exception>
#include <memory>
#include <vector>

// Vendored, MIT (third_party/signalsmith-stretch, third_party/signalsmith-linear). The
// headers are kept byte-identical to upstream, so their warnings are switched off here.
#if defined (_MSC_VER)
 #pragma warning (push, 0)
#endif
#include "signalsmith-stretch/signalsmith-stretch.h"
#if defined (_MSC_VER)
 #pragma warning (pop)
#endif

namespace TakeStretch
{
std::atomic<const char*> testPartToken { nullptr };

namespace
{
    using AbortFn = std::function<bool()>;

    // Time stretch. The shape of the library's own exact(): seek so that the first
    // output sample is the source's first sample, the middle in blocks (so an abort is
    // seen), then the tail flushed. false = aborted; tooShort = nothing was done.
    bool runStretch (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out, double sampleRate,
                     const AbortFn& stopNow, bool& tooShort)
    {
        const int channels = in.getNumChannels();
        const int inN = in.getNumSamples(), outN = out.getNumSamples();
        const float rate = (float) ((double) inN / (double) outN); // input samples per output sample
        signalsmith::stretch::SignalsmithStretch<float> st (20261008L); // fixed seed: the same file every run
        st.presetDefault (channels, (float) sampleRate);
        const int seekLen = st.outputSeekLength (rate);
        const int mainOut = outN - (int) ((float) seekLen / rate);
        tooShort = inN < seekLen || mainOut <= 0;
        if (tooShort)
            return false;
        std::vector<const float*> src ((size_t) channels);
        std::vector<float*> dst ((size_t) channels);
        for (int c = 0; c < channels; ++c)
            src[(size_t) c] = in.getReadPointer (c);
        st.outputSeek (src, seekLen);
        const int restIn = inN - seekLen;
        const int block = 1 << 14;
        for (int outPos = 0, inPos = 0; outPos < mainOut;)
        {
            if (stopNow())
                return false;
            const int outEnd = juce::jmin (mainOut, outPos + block);
            // Cumulative rounded positions: the blocks add up to exactly restIn.
            const int inEnd = outEnd == mainOut ? restIn
                                                : (int) std::llround ((double) outEnd * (double) restIn / (double) mainOut);
            for (int c = 0; c < channels; ++c)
            {
                src[(size_t) c] = in.getReadPointer (c) + seekLen + inPos;
                dst[(size_t) c] = out.getWritePointer (c) + outPos;
            }
            st.process (src, inEnd - inPos, dst, outEnd - outPos);
            outPos = outEnd;
            inPos = inEnd;
        }
        if (stopNow())
            return false;
        for (int c = 0; c < channels; ++c)
            dst[(size_t) c] = out.getWritePointer (c) + mainOut;
        st.flush (dst, outN - mainOut, rate);
        return true;
    }

    // Plain resample. The interpolator's own delay is taken off the front, so the
    // output starts at the source's first sample. false = aborted.
    bool runRepitch (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out, const AbortFn& stopNow)
    {
        const int inN = in.getNumSamples(), outN = out.getNumSamples();
        const double speed = (double) inN / (double) outN;
        const int skip = (int) std::llround ((double) juce::WindowedSincInterpolator::getBaseLatency() / speed);
        const int total = outN + skip;
        // Zeros behind the take: the interpolator never reads past the buffer.
        std::vector<float> padded ((size_t) inN + (size_t) std::ceil ((double) skip * speed) + 512, 0.0f);
        std::vector<float> made ((size_t) total);
        const int block = 1 << 15;
        for (int c = 0; c < in.getNumChannels(); ++c)
        {
            std::copy (in.getReadPointer (c), in.getReadPointer (c) + inN, padded.begin());
            juce::WindowedSincInterpolator sinc;
            int inPos = 0;
            for (int outPos = 0; outPos < total;)
            {
                if (stopNow())
                    return false;
                const int n = juce::jmin (block, total - outPos);
                const int avail = (int) padded.size() - inPos;
                if (avail <= 0)
                {
                    std::fill (made.begin() + outPos, made.end(), 0.0f);
                    break;
                }
                inPos += sinc.process (speed, padded.data() + inPos, made.data() + outPos, n, avail, 0);
                outPos += n;
            }
            std::copy (made.begin() + skip, made.begin() + skip + outN, out.getWritePointer (c));
        }
        return true;
    }

    StretchResult run (const StretchRequest& rq, const AbortFn& shouldAbort)
    {
        StretchResult res;
        auto fail = [&res] (const juce::String& why)
        {
            res.ok = false;
            res.error = why;
            return res;
        };
        const AbortFn stopNow = [&shouldAbort] { return shouldAbort != nullptr && shouldAbort(); };

        if (! (rq.sourceBpm >= kMinBpm && rq.sourceBpm <= kMaxBpm))
            return fail ("source tempo must be " + juce::String (kMinBpm, 0) + " to " + juce::String (kMaxBpm, 0) + " BPM");
        if (! (rq.targetBpm >= kMinBpm && rq.targetBpm <= kMaxBpm))
            return fail ("target tempo must be " + juce::String (kMinBpm, 0) + " to " + juce::String (kMaxBpm, 0) + " BPM");
        res.ratio = rq.sourceBpm / rq.targetBpm;
        if (! (res.ratio >= kMinRatio && res.ratio <= kMaxRatio))
            return fail ("the length would change by more than a factor of 2");
        if (rq.source == juce::File() || rq.dest == juce::File())
            return fail ("no source or destination file given");
        if (rq.dest == rq.source)
            return fail ("the destination is the source file");
        if (! rq.source.hasFileExtension ("wav") || ! rq.dest.hasFileExtension ("wav"))
            return fail ("source and destination must be .wav files");
        if (! rq.source.existsAsFile())
            return fail ("the source file is not there");
        if (rq.dest.exists())
            return fail ("the destination exists already; nothing is overwritten");
        if (! rq.dest.getParentDirectory().isDirectory())
            return fail ("the destination folder is not there");

        // Read: read only, whole, into this call's own buffer.
        juce::AudioBuffer<float> in;
        int bits = 0;
        bool isFloat = false;
        {
            const auto fileBytes = rq.source.getSize();
            auto stream = std::make_unique<juce::FileInputStream> (rq.source);
            if (stream->failedToOpen())
                return fail ("the source file could not be opened");
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (stream.release(), true));
            if (rd == nullptr)
                return fail ("the source is not a readable WAV file");
            bits = (int) rd->bitsPerSample;
            isFloat = rd->usesFloatingPointData;
            if (! (rd->sampleRate >= 8000.0 && rd->sampleRate <= 768000.0) || rd->numChannels == 0
                || (int) rd->numChannels > kMaxChannels || rd->lengthInSamples <= 0
                || rd->lengthInSamples > fileBytes)
                return fail ("the source header is not sane (rate, channels or length)");
            if (! ((isFloat && bits == 32) || (! isFloat && (bits == 16 || bits == 24))))
                return fail ("only 16 bit, 24 bit and 32 bit float WAV files are handled");
            if ((double) rd->lengthInSamples > rd->sampleRate * kMaxSeconds)
                return fail ("the take is longer than " + juce::String (kMaxSeconds / 60.0, 0) + " minutes");
            res.sampleRate = rd->sampleRate;
            res.channels = (int) rd->numChannels;
            res.inSamples = rd->lengthInSamples;
            in.setSize (res.channels, (int) res.inSamples);
            const int chunk = 1 << 16;
            for (int pos = 0; pos < in.getNumSamples(); pos += chunk)
            {
                if (stopNow())
                    return fail ("aborted");
                if (! rd->read (&in, pos, juce::jmin (chunk, in.getNumSamples() - pos), (juce::int64) pos, true, true))
                    return fail ("the source file could not be read to its end");
            }
        }

        res.outSamples = (juce::int64) std::llround ((double) res.inSamples * res.ratio);
        if (res.outSamples <= 0)
            return fail ("the take is too short");
        juce::AudioBuffer<float> out (res.channels, (int) res.outSamples);
        out.clear();
        if (rq.mode == StretchRequest::Mode::stretch)
        {
            bool tooShort = false;
            if (! runStretch (in, out, res.sampleRate, stopNow, tooShort))
                return fail (tooShort ? "the take is too short to stretch" : "aborted");
        }
        else if (! runRepitch (in, out, stopNow))
            return fail ("aborted");
        in.setSize (0, 0);

        // Write beside dest under a temporary name, then rename without replacing.
        const auto* fixedToken = testPartToken.load();
        const auto part = rq.dest.getSiblingFile (rq.dest.getFileName() + "." + (fixedToken != nullptr ? juce::String (fixedToken) : ExclusiveFile::uniqueToken (6))
                                                 + ".stretching");
        // Created here, exclusively: a file that is already at that name is not ours and is
        // neither written nor removed. The guard is armed only after our own create.
        if (! ExclusiveFile::createNew (part))
            return fail (part.exists() ? "the temporary file exists already" : "the destination folder cannot be written");
        struct PartGuard
        {
            juce::File f;
            bool keep = false;
            ~PartGuard()
            {
                if (! keep)
                    f.deleteFile();
            }
        } guard { part };
        {
            std::unique_ptr<juce::OutputStream> os (part.createOutputStream());
            if (os == nullptr)
                return fail ("the destination folder cannot be written");
            juce::WavAudioFormat wav;
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (res.sampleRate)
                                                  .withNumChannels (res.channels)
                                                  .withBitsPerSample (bits)
                                                  .withSampleFormat (isFloat ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                             : juce::AudioFormatWriterOptions::SampleFormat::integral));
            if (w == nullptr)
                return fail ("the WAV writer could not be made");
            const int chunk = 1 << 16;
            for (int pos = 0; pos < out.getNumSamples(); pos += chunk)
            {
                if (stopNow())
                    return fail ("aborted");
                if (! w->writeFromAudioSampleBuffer (out, pos, juce::jmin (chunk, out.getNumSamples() - pos)))
                    return fail ("the destination could not be written (disk full?)");
            }
            if (! w->flush())
                return fail ("the destination could not be written (disk full?)");
        }
        if (stopNow())
            return fail ("aborted");
        WavTempoTag::inject (part, rq.targetBpm); // tempo tags for DAW warping; skipped when the tempo is unknown
        if (! ExclusiveFile::moveNoReplace (part, rq.dest))
            return fail ("the finished file could not be given its name (the destination appeared meanwhile?)");
        guard.keep = true;
        res.ok = true;
        return res;
    }
}

juce::AudioBuffer<float> renderWarped (const juce::AudioBuffer<float>& in, double sampleRate,
                                       const std::vector<WarpMarkers::Marker>& markers,
                                       const std::function<bool()>& shouldAbort, bool bypassStretch)
{
    const int ch = in.getNumChannels(), n = in.getNumSamples();
    juce::AudioBuffer<float> empty;
    if (ch <= 0 || n <= 0 || markers.size() < 2)
        return empty;
    if (bypassStretch)
        return in;
    const AbortFn stopNow = shouldAbort ? shouldAbort : AbortFn ([] { return false; });
    std::int64_t total = 0;
    for (std::size_t i = 0; i + 1 < markers.size(); ++i)
    {
        const auto a = markers[i].srcFrame, b = markers[i + 1].srcFrame;
        const auto d = markers[i + 1].dstFrame - markers[i].dstFrame;
        if (a < 0 || b <= a || b > n || d <= 0)
            return empty;
        total += d;
    }
    if (total <= 0 || total > (std::int64_t) n * 3)
        return empty;
    juce::AudioBuffer<float> out (ch, (int) total);
    out.clear();
    int at = 0;
    for (std::size_t i = 0; i + 1 < markers.size(); ++i)
    {
        if (shouldAbort && shouldAbort())
            return empty;
        const int a = (int) markers[i].srcFrame, srcLen = (int) markers[i + 1].srcFrame - a;
        const int dstLen = (int) (markers[i + 1].dstFrame - markers[i].dstFrame);
        if (dstLen == srcLen)
        {
            for (int c = 0; c < ch; ++c)
                out.copyFrom (c, at, in, c, a, srcLen);
            at += dstLen;
            continue;
        }
        juce::AudioBuffer<float> seg (ch, srcLen), res (ch, dstLen);
        for (int c = 0; c < ch; ++c)
            seg.copyFrom (c, 0, in, c, a, srcLen);
        const double ratio = (double) dstLen / (double) srcLen;
        bool tooShort = false, done = false;
        if (ratio >= kMinRatio && ratio <= kMaxRatio)
            done = runStretch (seg, res, sampleRate, stopNow, tooShort);
        if (! done)
        {
            if (stopNow())
                return empty;
            for (int c = 0; c < ch; ++c)
            {
                const float* s = seg.getReadPointer (c);
                float* d = res.getWritePointer (c);
                for (int k = 0; k < dstLen; ++k)
                {
                    const double x = (double) k / ratio;
                    const int x0 = juce::jmin (srcLen - 1, (int) x);
                    const int x1 = juce::jmin (srcLen - 1, x0 + 1);
                    const float t = (float) (x - (double) x0);
                    d[k] = s[x0] + (s[x1] - s[x0]) * t;
                }
            }
        }
        for (int c = 0; c < ch; ++c)
            out.copyFrom (c, at, res, c, 0, dstLen);
        at += dstLen;
    }
    return out;
}

StretchResult stretchTakeFile (const StretchRequest& request, const std::function<bool()>& shouldAbort)
{
    try
    {
        return run (request, shouldAbort);
    }
    catch (const std::exception& e)
    {
        StretchResult r;
        r.error = juce::String ("failed: ") + e.what();
        return r;
    }
    catch (...)
    {
    }
    StretchResult r;
    r.error = "failed";
    return r;
}
}
