// HOT-STEP LINK -- TempoMeasure.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <cstddef>
#include <functional>

// Offline tempo measurement of one take. Pure: no JUCE, no globals, no statics, no
// file IO, the same input always gives the same result. It works on its own copy of
// the samples and hands back a plain struct, so it shares nothing with the audio
// thread. Never call it on the audio thread or the message thread: a take of a few
// minutes costs some tenths of a second.
namespace TempoMeasure
{
    enum class Level { none = 0, low = 1, medium = 2, high = 3 };

    struct Result
    {
        // true only for level medium or high: the tempo below may be shown as a number.
        bool valid = false;
        // Measured tempo. 0 for level none. For level low it is the best guess and must
        // not be shown as a number.
        double bpm = 0.0;
        double confidence = 0.0;     // 0..1
        Level level = Level::none;
        double downbeatSec = 0.0;    // guess: time of the first bar start in the file
        // How far the beats wander from a fixed grid at bpm: the largest mean distance
        // of the onsets from the grid over any window of some beats, in ms.
        double driftMs = 0.0;
        double driftEndMs = 0.0;     // the same for the last window, signed (+ = late)
        double firstBeatSec = 0.0;   // first grid beat at or after 0
        int beats = 0;               // grid beats inside the file
        // Relation of bpm to the asked tempo, set only with a hint and when bpm is within
        // kHintOctaveTol of hint * 2^k: 0 = the asked octave (or no octave of it at all),
        // +1 = double the asked tempo, -1 = half of it.
        int hintOctave = 0;
        // Grid fit at the octave nearest the hint over the fit at the measured octave.
        // 0 = the two are the same octave, or the hint octave is outside the search range.
        double hintFit = 0.0;
        // What the level was decided on, filled for every level (level none too) as far
        // as the analysis got. For the tempo-scan table and the tests; the cell does not
        // read it.
        struct Why
        {
            const char* rule = "not analysed"; // the rule that set the level
            int onsets = 0;              // onsets kept
            double coarseBpm = 0.0;      // tempo of the coarse stage
            double coarseStrength = 0.0; // its comb score over the curve's power
            double bpm = 0.0;            // best tempo, also when it is not shown
            double gridShare = 0.0;      // share of the onset weight on the grid, 0..1
            double significance = 0.0;   // gridShare over what random onset times reach
            double driftMs = 0.0;        // largest window drift, also when it is not shown
            double confidenceRaw = 0.0;  // confidence before the level rules
        };
        Why why;
    };

    // Search range of the coarse stage and the step of the fine stage.
    constexpr double kMinBpm = 50.0, kMaxBpm = 220.0, kFineStepBpm = 0.005;
    // Drift limits of the levels: above kSteadyMs the level is medium at most, above
    // kWanderMs it is low at most (the tempo wanders, one number does not describe it).
    constexpr double kSteadyMs = 15.0, kWanderMs = 40.0;
    constexpr double kMinSeconds = 4.0;  // shorter input: level none
    // Confidence limits of the levels.
    constexpr double kHighConfidence = 0.7, kMediumConfidence = 0.3, kLowConfidence = 0.15;
    // The octave nearest the hint is taken only when its grid fit is at least this share
    // of the fit at the octave the take itself points to.
    constexpr double kHintOctaveFit = 0.8;
    constexpr double kHintOctaveTol = 0.04; // bpm within 4 percent of hint * 2^k: an octave of it

    using AbortFn = std::function<bool()>;

    // mono: numSamples samples at sampleRate (sum a stereo file to mono first).
    // hintBpm: the tempo that was asked for, 0 = none. It gives its octaves a small lead
    // in the coarse stage, and between octave-related results (half, double) the one
    // nearest the hint is taken when it lies inside kMinBpm to kMaxBpm and the onsets fit
    // its grid nearly as well (kHintOctaveFit) as the octave the take points to. Else
    // the measured octave stays and Result::hintOctave says how it relates to the hint.
    // It never replaces the measurement.
    // shouldAbort: asked often; true ends the analysis early with level none.
    // Method: band energy onset curve (hop about 2.5 ms) -> onset list; coarse tempo by
    // a comb over the autocorrelation, 50 to 220 BPM; fine tempo and beat phase on a
    // 0.005 BPM grid around it (onset energy on the beat grid); half / double choice;
    // downbeat guess from the low band; drift of the onsets against the fixed grid.
    Result analyse (const float* mono, std::size_t numSamples, double sampleRate,
                    double hintBpm = 0.0, const AbortFn& shouldAbort = {});

    const char* levelName (Level level); // "none" / "low" / "medium" / "high"
}
