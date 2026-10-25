// HOT-STEP LINK -- WavTempoTag.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Open source plugin: must not include closed suite headers.
#include "WavTempoTag.h"

#include <cmath>
#include <cstring>

namespace WavTempoTag
{
namespace
{
    uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
    uint16_t rd16 (const uint8_t* p) { return (uint16_t) (p[0] | (p[1] << 8)); }
    void put32 (juce::MemoryOutputStream& o, uint32_t v) { o.writeInt ((int) v); }      // little endian
    void put16 (juce::MemoryOutputStream& o, uint16_t v) { o.writeShort ((short) v); }
    void putId (juce::MemoryOutputStream& o, const char* id) { o.write (id, 4); }

    bool fail (juce::String* error, const juce::String& msg)
    {
        if (error != nullptr)
            *error = msg;
        return false;
    }
}

bool inject (const juce::File& wav, double bpm, juce::String* error)
{
    if (! (bpm > 0.0) || ! std::isfinite (bpm))
        return fail (error, "no tempo");
    juce::MemoryBlock mb;
    if (! wav.loadFileAsData (mb))
        return fail (error, "cannot read the file");
    const auto* b = static_cast<const uint8_t*> (mb.getData());
    const size_t n = mb.getSize();
    if (n < 12 || std::memcmp (b, "RIFF", 4) != 0 || std::memcmp (b + 8, "WAVE", 4) != 0)
        return fail (error, "not a RIFF/WAVE file");

    juce::MemoryOutputStream out (n + 128);
    putId (out, "RIFF");
    put32 (out, 0); // fixed below
    putId (out, "WAVE");

    uint32_t sampleRate = 0, dataSize = 0;
    uint16_t blockAlign = 0;
    bool haveFmt = false, haveData = false;
    size_t pos = 12;
    while (pos + 8 <= n)
    {
        const uint8_t* h = b + pos;
        const uint32_t size = rd32 (h + 4);
        if ((uint64_t) pos + 8 + size > n)
            return fail (error, "chunk runs past the end of the file");
        const bool isAcid = std::memcmp (h, "acid", 4) == 0;
        const bool isSmpl = std::memcmp (h, "smpl", 4) == 0;
        if (std::memcmp (h, "fmt ", 4) == 0 && size >= 16)
        {
            sampleRate = rd32 (h + 12);
            blockAlign = rd16 (h + 20);
            haveFmt = true;
        }
        else if (std::memcmp (h, "data", 4) == 0)
        {
            dataSize = size;
            haveData = true;
        }
        if (! isAcid && ! isSmpl)
        {
            out.write (h, 8 + (size_t) size);
            if ((size & 1u) != 0)
                out.writeByte (0);
        }
        pos += 8 + (size_t) size + (size & 1u);
    }
    if (! haveFmt || ! haveData || sampleRate == 0 || blockAlign == 0)
        return fail (error, "fmt or data chunk missing");
    const uint32_t frames = dataSize / blockAlign;
    if (frames == 0)
        return fail (error, "no audio frames");

    // smpl: 36 byte header + one 24 byte loop.
    putId (out, "smpl");
    put32 (out, 36 + 24);
    put32 (out, 0);                                          // manufacturer
    put32 (out, 0);                                          // product
    put32 (out, (uint32_t) std::llround (1.0e9 / sampleRate)); // sample period, ns
    put32 (out, 60);                                         // MIDI unity note
    put32 (out, 0);                                          // pitch fraction
    put32 (out, 0);                                          // SMPTE format
    put32 (out, 0);                                          // SMPTE offset
    put32 (out, 1);                                          // number of loops
    put32 (out, 0);                                          // sampler data
    put32 (out, 0);                                          // cue point id
    put32 (out, 0);                                          // type: forward
    put32 (out, 0);                                          // start
    put32 (out, frames - 1);                                 // end (inclusive)
    put32 (out, 0);                                          // fraction
    put32 (out, 0);                                          // play count: infinite

    // acid: 24 bytes.
    const double beatsD = std::round ((double) frames * bpm / 60.0 / (double) sampleRate);
    const uint32_t beats = beatsD > 0.0 ? (uint32_t) beatsD : 0u;
    putId (out, "acid");
    put32 (out, 24);
    put32 (out, 0x02u | 0x04u);  // root note set, stretch on; one-shot (0x01) off = loop
    put16 (out, 60);             // root note
    put16 (out, 0x8000);         // unknown, as Acid writes it
    out.writeFloat (0.0f);       // unknown
    put32 (out, beats);
    put16 (out, 4);              // meter denominator
    put16 (out, 4);              // meter numerator
    out.writeFloat ((float) bpm);

    auto* o = static_cast<uint8_t*> (const_cast<void*> (out.getData()));
    const uint32_t riff = (uint32_t) (out.getDataSize() - 8);
    o[4] = (uint8_t) riff; o[5] = (uint8_t) (riff >> 8); o[6] = (uint8_t) (riff >> 16); o[7] = (uint8_t) (riff >> 24);
    if (out.getDataSize() > 0xFFFFFFFFull)
        return fail (error, "file too large for RIFF");

    const auto tmp = wav.getSiblingFile (wav.getFileName() + ".tagging");
    tmp.deleteFile();
    if (! tmp.replaceWithData (out.getData(), out.getDataSize()))
    {
        tmp.deleteFile();
        return fail (error, "cannot write the temporary file");
    }
    if (! tmp.replaceFileIn (wav))
    {
        tmp.deleteFile();
        return fail (error, "cannot swap the tagged file in");
    }
    return true;
}
}
