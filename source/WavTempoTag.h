// HOT-STEP LINK -- WavTempoTag.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#pragma once

#include <juce_core/juce_core.h>

// Tempo metadata for take WAVs so a DAW (Ableton) warps them at the take tempo.
// Rewrites a RIFF/WAVE file: every chunk is kept byte for byte (word aligned) except
// any existing "acid" and "smpl" chunks, which are dropped; then a "smpl" chunk with
// one forward loop over the whole file (0 .. frames - 1) and an "acid" chunk (tempo,
// beats = round (frames * bpm / 60 / sampleRate), loop not one-shot, stretch on, 4/4)
// are appended and the RIFF size is fixed. The result is written to a temporary file
// beside the target and swapped in; on any failure the file is left as it was.
namespace WavTempoTag
{
    // False (file untouched) when bpm <= 0 or not finite, the file is not a plain
    // RIFF/WAVE (RF64 is skipped), or its fmt/data chunks are missing or broken.
    bool inject (const juce::File& wav, double bpm, juce::String* error = nullptr);
}
