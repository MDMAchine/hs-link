// HOT-STEP LINK -- TakeEdit.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#include "TakeEdit.h"
#include "ExclusiveFile.h"
#include "WavTempoTag.h"
#include "TakeStretch.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <vector>

namespace TakeEdit
{
std::atomic<int> testLevelMutant { lvMutNone };

std::atomic<const char*> testEditPartToken { nullptr };
std::atomic<bool> testUntaggedWarp { false };

EditData EditData::sanitized (juce::int64 lengthSamples) const
{
    auto point = [lengthSamples] (juce::int64 v)
    {
        if (v < 0)
            return (juce::int64) -1;
        return lengthSamples >= 0 && v > lengthSamples ? lengthSamples : v;
    };
    EditData d;
    d.cropStart = point (cropStart);
    d.cropEnd = point (cropEnd);
    d.loopStart = point (loopStart);
    d.loopEnd = point (loopEnd);
    if (d.cropStart >= 0 && d.cropEnd >= 0 && d.cropEnd <= d.cropStart)
        d.cropStart = d.cropEnd = -1;
    if (lengthSamples >= 0 && (d.cropStart >= lengthSamples || d.cropEnd == 0))
        d.cropStart = d.cropEnd = -1; // nothing would be left
    if (d.loopStart < 0 || d.loopEnd <= d.loopStart)
        d.loopStart = d.loopEnd = -1;
    auto lim = [] (double v, double lo, double hi) { return std::isfinite (v) ? juce::jlimit (lo, hi, v) : 0.0; };
    d.fadeInMs = lim (fadeInMs, 0.0, kMaxUserFadeMs);
    d.fadeOutMs = lim (fadeOutMs, 0.0, kMaxUserFadeMs);
    d.loopXfadeMs = lim (loopXfadeMs, 0.0, kMaxLoopXfadeMs);
    d.gainDb = lim (gainDb, kMinGainDb, kMaxGainDb);
    d.reverse = reverse;
    return d;
}

juce::int64 snapToGrid (juce::int64 position, double sampleRate, double bpm, double downbeatSec)
{
    if (! gridUsable (bpm, downbeatSec) || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate))
        return position;
    const double beat = sampleRate * 60.0 / bpm, origin = downbeatSec * sampleRate;
    const double k = std::round (((double) position - origin) / beat);
    const double snapped = origin + k * beat;
    if (! (std::abs (snapped) < 9.0e18)) // llround is only defined inside int64
        return position;
    return (juce::int64) std::llround (snapped);
}

bool gridUsable (double bpm, double downbeatSec)
{
    return bpm >= kMinBpm && bpm <= kMaxBpm && std::isfinite (downbeatSec) && std::abs (downbeatSec) <= kMaxGridDownbeatSec;
}

juce::int64 nearestZeroCrossing (const float* mono, juce::int64 numSamples, juce::int64 position, juce::int64 window)
{
    if (mono == nullptr || position <= 0 || position >= numSamples || window <= 0)
        return position;
    auto crossing = [mono] (juce::int64 i)
    {
        const float a = mono[i - 1], b = mono[i];
        return b == 0.0f || (a < 0.0f && b > 0.0f) || (a > 0.0f && b < 0.0f);
    };
    for (juce::int64 d = 0; d <= window; ++d)
    {
        const juce::int64 lo = position - d, hi = position + d;
        if (lo >= 1 && crossing (lo))
            return lo;
        if (hi <= numSamples - 1 && crossing (hi))
            return hi;
    }
    return position;
}

BarsFit fitBars (juce::int64 regionSamples, double sampleRate, double bpm, int beatsPerBar)
{
    BarsFit f;
    if (regionSamples <= 0 || ! (sampleRate > 0.0) || ! (bpm >= kMinBpm && bpm <= kMaxBpm) || beatsPerBar <= 0)
        return f;
    const double bar = sampleRate * 60.0 / bpm * (double) beatsPerBar;
    const double n = juce::jmax (1.0, std::round ((double) regionSamples / bar));
    if (n > 1.0e6)
        return f;
    f.bars = (int) n;
    f.leftoverMs = 1000.0 * ((double) regionSamples - n * bar) / sampleRate;
    return f;
}

namespace
{
    using AbortFn = std::function<bool()>;
    constexpr double kHalfPi = 1.5707963267948966;

    EditResult run (const EditRequest& rq, const AbortFn& shouldAbort)
    {
        EditResult res;
        auto fail = [&res] (const juce::String& why)
        {
            res.ok = false;
            res.error = why;
            return res;
        };
        const AbortFn stopNow = [&shouldAbort] { return shouldAbort != nullptr && shouldAbort(); };
        const auto& opt = rq.options;

        if (opt.loopRepeats < 0 || opt.loopRepeats > kMaxRepeats)
            return fail ("the loop repeats must be 0 to " + juce::String (kMaxRepeats));
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

        // The points: clamp, grid, zero crossing.
        const juce::int64 len = res.inSamples;
        const double sr = res.sampleRate;
        const EditData d = rq.edit.sanitized (len);
        const bool loopRender = opt.loopRepeats >= 1;
        // A region the caller gave that lies outside the file is refused, not replaced
        // by the whole file. One that overlaps the file in part was clamped above.
        if (loopRender && rq.edit.hasLoop() && rq.edit.loopStart >= len)
            return fail ("the loop region is outside the file");
        if (! loopRender && (rq.edit.cropStart >= len || rq.edit.cropEnd == 0))
            return fail ("the crop region is outside the file");
        if (! loopRender && rq.edit.cropStart >= 0 && rq.edit.cropEnd >= 0 && rq.edit.cropEnd <= rq.edit.cropStart)
            return fail ("the crop region is empty");
        if (loopRender && ! d.hasLoop())
            return fail ("no loop region given");
        const bool warpRender = ! opt.warp.empty();
        if (warpRender && loopRender)
            return fail ("a loop render cannot be warped");
        juce::int64 cs = d.cropStart < 0 ? 0 : d.cropStart, ce = d.cropEnd < 0 ? len : d.cropEnd;
        juce::int64 ls = d.loopStart, le = d.loopEnd;
        const bool grid = opt.snapToGrid && gridUsable (opt.gridBpm, opt.gridDownbeatSec);
        res.gridSnapped = grid;
        if (grid)
        {
            if (d.cropStart >= 0)
                cs = juce::jlimit ((juce::int64) 0, len, snapToGrid (cs, sr, opt.gridBpm, opt.gridDownbeatSec));
            if (d.cropEnd >= 0)
                ce = juce::jlimit ((juce::int64) 0, len, snapToGrid (ce, sr, opt.gridBpm, opt.gridDownbeatSec));
            if (d.hasLoop())
            {
                // Start on a beat inside the file, then a whole number of beats from there:
                // the length does not depend on where the start fell between two frames.
                const double beat = sr * 60.0 / opt.gridBpm, origin = opt.gridDownbeatSec * sr;
                auto at = [beat, origin] (double k) { return (juce::int64) std::llround (origin + k * beat); };
                // Closed form, no stepping: the grid is usable, so every number here is
                // far inside int64 and each correction below is made at most once.
                double k = juce::jmax (std::round (((double) ls - origin) / beat), std::ceil (-origin / beat));
                if (at (k) < 0)
                    k += 1.0;
                const juce::int64 start = at (k);
                double beats = juce::jmin (juce::jmax (1.0, std::round ((double) (le - ls) / beat)), std::floor ((double) (len - start) / beat) + 1.0);
                if (beats >= 1.0 && start + (juce::int64) std::llround (beats * beat) > len)
                    beats -= 1.0;
                if (beats >= 1.0)
                {
                    ls = start;
                    le = start + (juce::int64) std::llround (beats * beat);
                }
                else
                    ls = le = -1;
            }
        }
        if (loopRender && (ls < 0 || le <= ls))
            return fail ("no whole beat of the loop region fits inside the file");
        if (opt.snapToZeroCrossing && ! loopRender && (d.cropStart >= 0 || d.cropEnd >= 0))
        {
            std::vector<float> mono ((size_t) len, 0.0f);
            for (int c = 0; c < res.channels; ++c)
                juce::FloatVectorOperations::add (mono.data(), in.getReadPointer (c), (int) len);
            const auto window = testLevelMutant.load() == lvMutNoZeroSnap ? (juce::int64) 0
                              : (juce::int64) std::llround (juce::jlimit (0.0, kMaxFadeMs, opt.zeroCrossingMs) * sr / 1000.0);
            const juce::int64 cs0 = cs, ce0 = ce;
            if (d.cropStart >= 0)
                cs = nearestZeroCrossing (mono.data(), len, cs, window);
            if (d.cropEnd >= 0)
                ce = nearestZeroCrossing (mono.data(), len, ce, window);
            res.zeroSnapped = true;
            res.zeroMoveStart = cs - cs0;
            res.zeroMoveEnd = ce - ce0;
        }
        if (! loopRender && ce <= cs)
            return fail ("the crop region is empty");
        if (stopNow())
            return fail ("aborted");

        const auto fadeAsked = (juce::int64) std::llround (juce::jlimit (0.0, kMaxFadeMs, opt.fadeMs) * sr / 1000.0);
        const bool revOn = d.reverse && ! warpRender && testLevelMutant.load() != lvMutNoReverse;
        // Loop seam length, known before the reverse so the pre-roll can be reversed with it.
        juce::int64 seamX = 0;
        if (loopRender)
        {
            const juce::int64 xUser = testLevelMutant.load() == lvMutNoXfade ? 0 : (juce::int64) std::llround (d.loopXfadeMs * sr / 1000.0);
            seamX = juce::jmin (juce::jmax (fadeAsked, xUser), ls, (le - ls) / 2);
            // Reversed, the audio before the loop start is the audio after its end, backwards.
            if (revOn && testLevelMutant.load() != lvMutRevSeamForward)
                seamX = juce::jmin (seamX, len - le);
        }
        // Reverse: the kept region backwards, after the crop and before the fades and gain.
        // A loop reverses [ls - x, le + x): the loop stays in place and its pre-roll is the
        // reversed audio after the loop end, so the seam fades from what plays.
        if (revOn)
        {
            const juce::int64 pad = loopRender && testLevelMutant.load() != lvMutRevSeamForward ? seamX : 0;
            const juce::int64 r0 = loopRender ? ls - pad : cs, r1 = loopRender ? le + pad : ce;
            if (r0 >= 0 && r1 > r0)
                for (int c = 0; c < in.getNumChannels(); ++c)
                    std::reverse (in.getWritePointer (c) + r0, in.getWritePointer (c) + r1);
        }
        juce::AudioBuffer<float> out;
        if (loopRender)
        {
            const juce::int64 cell = le - ls;
            res.outSamples = cell * (juce::int64) opt.loopRepeats;
            if ((double) res.outSamples > sr * kMaxSeconds)
                return fail ("the result would be longer than " + juce::String (kMaxSeconds / 60.0, 0) + " minutes");
            const juce::int64 x = seamX;
            res.seamFadeSamples = x;
            out.setSize (res.channels, (int) res.outSamples);
            // How alike the two overlapped segments are: the end of the loop and the audio
            // just before its start, pooled per channel (never the channel sum, which
            // cancels on out of phase stereo). It picks the law, see the header.
            double raw = 0.0;
            {
                double aa = 0.0, bb = 0.0, ab = 0.0;
                for (int c = 0; c < res.channels; ++c)
                {
                    const float* a = in.getReadPointer (c) + (le - x);
                    const float* b = in.getReadPointer (c) + (ls - x);
                    for (juce::int64 i = 0; i < x; ++i)
                    {
                        aa += (double) a[i] * (double) a[i];
                        bb += (double) b[i] * (double) b[i];
                        ab += (double) a[i] * (double) b[i];
                    }
                }
                raw = seamRawCorrelation (aa, bb, ab);
            }
            const double corr = juce::jlimit (0.0, 1.0, raw);
            res.seamCorrelation = raw;
            res.seamCorrelationUsed = corr;
            for (int c = 0; c < res.channels; ++c)
            {
                const float* src = in.getReadPointer (c);
                float* first = out.getWritePointer (c);
                std::copy (src + ls, src + le, first);
                // From the end of the loop into the audio just before its start: equal
                // power gains, divided so the power stays flat for the correlation.
                for (juce::int64 i = 0; i < x; ++i)
                {
                    const juce::int64 at = cell - x + i;
                    double ga = 0.0, gb = 0.0;
                    seamGains (i, x, corr, ga, gb);
                    first[at] = (float) ((double) first[at] * ga + (double) src[ls - x + i] * gb);
                }
                for (int r = 1; r < opt.loopRepeats; ++r)
                {
                    if (stopNow())
                        return fail ("aborted");
                    std::copy (first, first + cell, first + (juce::int64) r * cell);
                }
            }
            res.used.loopStart = ls;
            res.used.loopEnd = le;
        }
        else if (warpRender)
        {
            // The whole file, piecewise warped; crop and loop points are not used.
            out = TakeStretch::renderWarped (in, sr, opt.warp, stopNow);
            if (out.getNumSamples() <= 0)
                return fail (stopNow() ? "aborted" : "the warp markers are not usable");
            res.outSamples = out.getNumSamples();
        }
        else
        {
            res.outSamples = ce - cs;
            const juce::int64 f = juce::jmin (fadeAsked, res.outSamples / 2);
            // F1: the user fades, sine ramps from the crop start and to the crop end,
            // applied whether or not the point is a real cut; each at most the region.
            const bool noFade = testLevelMutant.load() == lvMutNoFade;
            const juce::int64 fIn = noFade ? 0 : juce::jmin ((juce::int64) std::llround (d.fadeInMs * sr / 1000.0), res.outSamples);
            const juce::int64 fOut = noFade ? 0 : juce::jmin ((juce::int64) std::llround (d.fadeOutMs * sr / 1000.0), res.outSamples);
            out.setSize (res.channels, (int) res.outSamples);
            for (int c = 0; c < res.channels; ++c)
            {
                const float* src = in.getReadPointer (c);
                float* dst = out.getWritePointer (c);
                std::copy (src + cs, src + ce, dst);
                // Only a real cut is faded; the file's own start and end stay as they are.
                for (juce::int64 i = 0; i < f; ++i)
                {
                    const float g = (float) std::sin ((double) i / (double) f * kHalfPi);
                    if (cs > 0)
                        dst[i] *= g;
                    if (ce < len)
                        dst[res.outSamples - 1 - i] *= g;
                }
                for (juce::int64 i = 0; i < fIn; ++i)
                    dst[i] *= (float) std::sin (((double) i + 0.5) / (double) fIn * kHalfPi);
                for (juce::int64 i = 0; i < fOut; ++i)
                    dst[res.outSamples - 1 - i] *= (float) std::sin (((double) i + 0.5) / (double) fOut * kHalfPi);
            }
            res.used.cropStart = cs;
            res.used.cropEnd = ce;
            if (ls >= 0 && le > ls)
            {
                res.used.loopStart = ls;
                res.used.loopEnd = le;
            }
        }
        in.setSize (0, 0);
        if (d.gainDb != 0.0 && testLevelMutant.load() != lvMutNoGain) // F1: the gain of the take on the whole result
            out.applyGain ((float) juce::Decibels::decibelsToGain (d.gainDb, -1000.0));
        if (opt.fitBars)
        {
            const auto fit = fitBars (res.outSamples / (loopRender ? opt.loopRepeats : 1), sr, opt.fitBpm, opt.fitBeatsPerBar);
            res.bars = fit.bars;
            res.leftoverMs = fit.leftoverMs;
        }

        // Write beside dest under a temporary name, then rename without replacing.
        const auto* fixedToken = testEditPartToken.load();
        const auto part = rq.dest.getSiblingFile (rq.dest.getFileName() + "." + (fixedToken != nullptr ? juce::String (fixedToken) : ExclusiveFile::uniqueToken (6))
                                                 + ".editing");
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
        const double tagBpm = warpRender ? (testUntaggedWarp.load() ? 0.0 : opt.warpBpm > 0.0 ? opt.warpBpm : 0.0)
                                         : (gridUsable (opt.gridBpm, opt.gridDownbeatSec) ? opt.gridBpm : 0.0);
        WavTempoTag::inject (part, tagBpm); // tempo tags for DAW warping; skipped when the tempo is unknown
        if (! ExclusiveFile::moveNoReplace (part, rq.dest))
            return fail ("the finished file could not be given its name (the destination appeared meanwhile?)");
        guard.keep = true;
        res.ok = true;
        return res;
    }
}

EditResult renderEditFile (const EditRequest& request, const std::function<bool()>& shouldAbort)
{
    try
    {
        return run (request, shouldAbort);
    }
    catch (const std::exception& e)
    {
        EditResult r;
        r.error = juce::String ("failed: ") + e.what();
        return r;
    }
    catch (...)
    {
    }
    EditResult r;
    r.error = "failed";
    return r;
}
}
