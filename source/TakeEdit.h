// HOT-STEP LINK -- TakeEdit.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "WarpMarkers.h"
#include <atomic>
#include <functional>

// Offline crop and loop render of a take: reads one WAV, writes another. No GUI, no
// processor, no globals; never the audio or the message thread. The call owns
// everything it touches (own reader, own buffers) and gives a plain result back.
namespace TakeEdit
{
    constexpr double kMinBpm = 30.0, kMaxBpm = 300.0;  // grid and bar fit tempo
    constexpr double kMaxSeconds = 600.0;              // source and result: 10 minutes
    constexpr double kMaxGridDownbeatSec = kMaxSeconds; // grid downbeat: at most this far from the file start, either side
    constexpr int kMaxChannels = 8;
    constexpr int kMaxRepeats = 1024;
    constexpr double kMaxFadeMs = 1000.0;
    constexpr double kDefaultFadeMs = 5.0, kDefaultZeroCrossingMs = 3.0;

    // The edit points a take keeps. All four are positions in SAMPLE FRAMES of the take's
    // own file at that file's own rate; -1 = not set. An end is exclusive (the first
    // frame that is not kept). This data lives beside the take (take entry, sidecar keys
    // crop_start, crop_end, loop_start, loop_end) and is never written into the audio
    // file. An unset crop start means the file start, an unset crop end the file end; a
    // loop needs both of its points.
    struct EditData
    {
        juce::int64 cropStart = -1, cropEnd = -1;
        juce::int64 loopStart = -1, loopEnd = -1;

        bool hasCrop() const { return cropStart >= 0 || cropEnd >= 0; }
        bool hasLoop() const { return loopStart >= 0 && loopEnd > loopStart; }
        bool isEmpty() const { return ! hasCrop() && ! hasLoop(); }
        // The same data with bad values taken out: a negative point is unset, a point
        // past lengthSamples is put at lengthSamples (lengthSamples < 0 = length not
        // known, no upper limit), a crop whose end is not after its start is unset as a
        // whole, and so is a loop with a missing point or an end not after its start.
        EditData sanitized (juce::int64 lengthSamples = -1) const;
        bool operator== (const EditData& o) const
        {
            return cropStart == o.cropStart && cropEnd == o.cropEnd && loopStart == o.loopStart && loopEnd == o.loopEnd;
        }
        bool operator!= (const EditData& o) const { return ! (*this == o); }
    };

    struct EditOptions
    {
        // Snap to the beat grid of the take: beats at gridDownbeatSec + k * 60 / gridBpm.
        // Needs a usable grid (gridUsable: gridBpm inside kMinBpm..kMaxBpm and
        // gridDownbeatSec finite and within kMaxGridDownbeatSec of the file start). With
        // any other value (NaN, infinite, a zero or huge tempo, a huge downbeat) the snap
        // is skipped, the points are used as given and the result says so (gridSnapped
        // false); the render does not fail for it and never waits on such a value.
        // Crop points that are set go to the nearest beat
        // each. The loop start goes to the nearest beat and the loop length to the
        // nearest whole number of beats (at least one, as many as fit in the file).
        bool snapToGrid = false;
        double gridBpm = 0.0, gridDownbeatSec = 0.0;
        // After the grid: a crop point that is set and inside the file goes to the
        // nearest zero crossing of the channel sum within zeroCrossingMs on either side
        // (it stays when there is none). Loop points are never moved by this, so a loop
        // keeps its length; its seam is handled by the crossfade.
        bool snapToZeroCrossing = false;
        double zeroCrossingMs = kDefaultZeroCrossingMs;
        // Crop: fade at a cut end. Loop: length of the crossfade at each seam.
        double fadeMs = kDefaultFadeMs;
        // 0 = crop only. N >= 1 = render the loop region N times in a row.
        int loopRepeats = 0;
        // Bars fit: reports the nearest whole bar count of the rendered region (the loop
        // region in a loop render, else the crop region) at fitBpm and the rest in ms.
        // Nothing is stretched here; the caller may chain a stretch from the numbers.
        bool fitBars = false;
        double fitBpm = 0.0;
        int fitBeatsPerBar = 4;
        // Warp (Mock-up B, round 3): markers in frames of the source (src -> dst, as
        // TakeStretch::renderWarped). Not empty = the whole file is warped piecewise
        // (no crop, no loop, no fade: a crop or loop render with markers is refused) and
        // the output length is the last dst. warpBpm > 0: the acid tempo of the result
        // (the tempo it plays at); 0 = the grid tempo as for a crop.
        std::vector<WarpMarkers::Marker> warp;
        double warpBpm = 0.0;
    };

    struct EditRequest
    {
        juce::File source, dest;
        EditData edit;
        EditOptions options;
    };

    struct BarsFit
    {
        int bars = 0;            // 0 = not asked or not possible
        double leftoverMs = 0.0; // region length minus bars whole bars, signed (+ = region longer)
    };

    struct EditResult
    {
        bool ok = false;
        juce::String error;       // plain text when ok is false
        juce::int64 inSamples = 0, outSamples = 0;
        double sampleRate = 0.0;
        int channels = 0;
        // The points the render used, after clamping and snapping. Crop render: the crop
        // points are always set (file start and end when they were not given) and the
        // loop points are the resolved ones, or -1 when no loop was given or it does not
        // fit. Loop render: the loop points; the crop points are -1 (not used).
        EditData used;
        bool gridSnapped = false;           // the grid snap was asked for and applied
        juce::int64 seamFadeSamples = 0;    // loop render: crossfade length at each seam
        // Loop render: the measured correlation of the two overlapped segments, signed,
        // -1..1 (see below). Near 1: the loop end runs into its own start. Near 0: the two
        // have nothing in common, or one of them is silent, or there is no crossfade
        // (seamFadeSamples 0). Clearly negative: the two are out of phase at the loop
        // point, and the level dips inside the fade (about -9.9 dB measured on a tone
        // looped half a period off with some noise on it, signed value -0.93): a badly
        // chosen loop point.
        double seamCorrelation = 0.0;
        // Loop render: the r the seam law was made from, seamCorrelation limited to 0..1.
        // 0 = equal power, 1 = equal gain. 0 when there is no crossfade.
        double seamCorrelationUsed = 0.0;
        int bars = 0;                       // BarsFit of the rendered region, see EditOptions::fitBars
        double leftoverMs = 0.0;
    };

    // Output length, exact to the sample:
    //   crop only (loopRepeats 0): used.cropEnd - used.cropStart. The output is the
    //     source from crop start to crop end. A cut end that lies inside the file gets a
    //     fade of fadeMs (at most half the region); an end at the file's own start or end
    //     is left as it is. Loop points do not change the audio.
    //   loop render (loopRepeats N >= 1): N * (used.loopEnd - used.loopStart). Only the
    //     loop region is rendered. The audio before the loop start and after the loop end
    //     is NOT in the output, and the crop points are not used. The one exception is the
    //     seam: the last seamFadeSamples of every repeat (the last one too) are a
    //     crossfade from the end of the loop region into the audio that lies just
    //     before the loop start, so every repeat ends on the frame before the loop start
    //     and the next one begins on the loop start itself. The file therefore also loops
    //     without a step when it is played end to end, and it has no fade at its own start
    //     or end. seamFadeSamples = fadeMs, at most the audio there is before the loop
    //     start and at most half the loop; a loop that starts at frame 0 has no audio
    //     before it and gets a plain cut (seamFadeSamples 0).
    //     Seam law: r = the normalised correlation of the two overlapped segments over
    //     the fade length, pooled per channel (a = loop tail, b = audio before the start):
    //       r = sum_c (sum (a_c * b_c)) / sqrt (sum_c (sum (a_c * a_c)) * sum_c (sum (b_c * b_c)))
    //     It is never taken from the channel sum: that cancels on wide or out of phase
    //     stereo (L = -R reads 0 there although each channel runs into its own start).
    //     One r and one gain for all channels. For the law r is limited to 0..1 (a
    //     negative one counts as 0, and so does a silent segment).
    //     With th = t * pi / 2, t from 0 to 1 over the fade:
    //       out = (tail * cos (th) + before * sin (th)) / sqrt (1 + 2 r cos (th) sin (th))
    //     r = 0 is the equal power law (two unrelated signals keep their power); r = 1 is
    //     an equal gain law (the two gains add up to 1, so a loop whose end runs into its
    //     own start keeps its level and its peak instead of rising by up to 3 dB); in
    //     between the expected power stays flat for the measured r. No gain is above 1.
    //     EditResult::seamCorrelationUsed has r; EditResult::seamCorrelation has the
    //     signed value before the limit.
    // Points outside the file: a point past the file end is put at the file end, so a
    // crop or a loop that overlaps the file in part is cut down to the part inside it. A
    // crop render fails ("the crop region is outside the file") when a crop start that
    // was given lies at or past the file end or a crop end that was given is 0, and
    // ("the crop region is empty") when both were given and the end is not after the
    // start; it never falls back to the whole file. A loop render fails ("the loop
    // region is outside the file") when the loop start lies at or past the file end.
    // Nothing is written in these cases (no dest, no temporary file). In a crop render a
    // loop that is outside the file only reads as not set in used.
    // Sample rate, channel count and sample format of the source are kept (16 and 24 bit
    // stay so, 32 bit float stays float).
    // The source is opened read only and never modified. Refused: dest equal to source,
    // a dest that exists (nothing is ever overwritten), a source or dest that is not a
    // .wav, a header that is not sane, a source or a result longer than kMaxSeconds, a
    // loop render without a loop region, loopRepeats outside 0..kMaxRepeats, an empty
    // crop region, a crop or loop region outside the file.
    // The audio is written to a temporary name beside dest and renamed (no replace) when
    // complete; on failure or abort the partial file is removed and dest is not created.
    // shouldAbort is polled per block. Never throws: every failure is ok = false.
    EditResult renderEditFile (const EditRequest& request, const std::function<bool()>& shouldAbort = {});

    // Pure helpers.
    // The beat of the grid (downbeatSec + k * 60 / bpm) nearest to position, in frames.
    // Not clamped to any file (it can be negative or past the end). A grid that is not
    // usable (gridUsable), a rate that is not positive and finite, or a beat that would
    // not fit an int64 gives position back. There is no loop in it: it returns at once
    // for any value.
    juce::int64 snapToGrid (juce::int64 position, double sampleRate, double bpm, double downbeatSec);
    // True when bpm is inside kMinBpm..kMaxBpm and downbeatSec is finite and not further
    // than kMaxGridDownbeatSec from 0. NaN and infinite values are not usable.
    bool gridUsable (double bpm, double downbeatSec);
    // The zero crossing of mono nearest to position within window frames on either side;
    // position when there is none, when position is 0 or numSamples (a file edge) or
    // outside the data. A crossing at i means mono[i] is 0 or has the other sign than
    // mono[i - 1]. Equal distance: the earlier one.
    juce::int64 nearestZeroCrossing (const float* mono, juce::int64 numSamples, juce::int64 position, juce::int64 window);
    // Nearest whole bar count (at least 1) to regionSamples and the signed rest. bars 0
    // when the tempo is outside kMinBpm..kMaxBpm or an argument is not positive.
    BarsFit fitBars (juce::int64 regionSamples, double sampleRate, double bpm, int beatsPerBar);

    // Tests only, null in the plugin: when not null it replaces the random token of the
    // temporary name ("<dest name>.<token>.editing"); the pointer must stay valid while
    // set.
    extern std::atomic<const char*> testEditPartToken;
    // Tests only (teeth), false in the plugin: a warp render writes no tempo tags.
    extern std::atomic<bool> testUntaggedWarp;
}
