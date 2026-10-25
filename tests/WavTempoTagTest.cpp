// HOT-STEP LINK -- WavTempoTagTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Writes a bare 30 s 48 kHz stereo 16 bit WAV, checks it (teeth: the checks must fail on
// the bare file), injects tempo tags at 120 bpm, then parses the RIFF itself and checks
// acid tempo, beats, loop flag, smpl loop end, RIFF size and an unchanged data chunk.
#include "WavTempoTag.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
uint32_t rd32 (const uint8_t* p) { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }

struct Parsed
{
    bool hasAcid = false, hasSmpl = false, hasData = false;
    float tempo = 0.0f;
    uint32_t beats = 0, flags = 0, loops = 0, loopEnd = 0, riffSize = 0;
    juce::MemoryBlock data;
};

Parsed parse (const juce::MemoryBlock& mb)
{
    Parsed p;
    const auto* b = static_cast<const uint8_t*> (mb.getData());
    const size_t n = mb.getSize();
    p.riffSize = rd32 (b + 4);
    for (size_t pos = 12; pos + 8 <= n;)
    {
        const uint8_t* h = b + pos;
        const uint32_t size = rd32 (h + 4);
        if (pos + 8 + size > n)
            break;
        if (std::memcmp (h, "acid", 4) == 0 && size >= 24)
        {
            p.hasAcid = true;
            p.flags = rd32 (h + 8);
            p.beats = rd32 (h + 20);
            std::memcpy (&p.tempo, h + 28, 4);
        }
        else if (std::memcmp (h, "smpl", 4) == 0 && size >= 36)
        {
            p.hasSmpl = true;
            p.loops = rd32 (h + 8 + 28);
            if (p.loops >= 1 && size >= 60)
                p.loopEnd = rd32 (h + 8 + 36 + 12);
        }
        else if (std::memcmp (h, "data", 4) == 0)
        {
            p.hasData = true;
            p.data.replaceAll (h + 8, size);
        }
        pos += 8 + size + (size & 1u);
    }
    return p;
}

int checkFile (const juce::File& f, const juce::MemoryBlock& origData, const char* label)
{
    juce::MemoryBlock mb;
    f.loadFileAsData (mb);
    const auto p = parse (mb);
    int fails = 0;
    auto check = [&] (bool ok, const char* what) { std::printf ("  [%s] %s: %s\n", label, what, ok ? "PASS" : "FAIL"); fails += ok ? 0 : 1; };
    std::printf ("[%s] size=%llu riff=%u acid=%d tempo=%.4f beats=%u flags=0x%X smpl=%d loops=%u loopEnd=%u\n", label,
                 (unsigned long long) mb.getSize(), p.riffSize, (int) p.hasAcid, (double) p.tempo, p.beats, p.flags,
                 (int) p.hasSmpl, p.loops, p.loopEnd);
    check (p.hasAcid && std::abs (p.tempo - 120.0f) < 1e-3f, "acid tempo == 120.0");
    check (p.hasAcid && p.beats == 60, "acid beats == 60");
    check (p.hasAcid && (p.flags & 0x01u) == 0 && (p.flags & 0x04u) != 0, "loop (not one-shot) + stretch flags");
    check (p.hasSmpl && p.loops == 1 && p.loopEnd == 1439999, "smpl loop end == 1439999");
    check ((uint64_t) p.riffSize == mb.getSize() - 8, "RIFF size == file size - 8");
    check (p.hasData && p.data == origData, "data chunk byte-identical");
    return fails;
}
}

int main()
{
    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("hslink_wavtag_test");
    dir.createDirectory();
    const auto f = dir.getChildFile ("bare.wav");

    const uint32_t sr = 48000, frames = 30 * sr, ch = 2, align = ch * 2, dataSize = frames * align;
    juce::MemoryBlock data (dataSize, true);
    auto* s = static_cast<int16_t*> (data.getData());
    for (uint32_t i = 0; i < frames * ch; ++i)
        s[i] = (int16_t) (8000.0 * std::sin ((double) i * 0.01) + (double) (i % 7));
    juce::MemoryOutputStream o;
    o.write ("RIFF", 4); o.writeInt ((int) (4 + 8 + 16 + 8 + dataSize)); o.write ("WAVE", 4);
    o.write ("fmt ", 4); o.writeInt (16); o.writeShort (1); o.writeShort ((short) ch); o.writeInt ((int) sr);
    o.writeInt ((int) (sr * align)); o.writeShort ((short) align); o.writeShort (16);
    o.write ("data", 4); o.writeInt ((int) dataSize); o.write (data.getData(), dataSize);
    f.replaceWithData (o.getData(), o.getDataSize());

    const int bareFails = checkFile (f, data, "bare");
    juce::String err;
    const bool ok = WavTempoTag::inject (f, 120.0, &err);
    std::printf ("inject: %s %s\n", ok ? "ok" : "FAILED", err.toRawUTF8());
    const int tagFails = checkFile (f, data, "injected");
    // Second run replaces the chunks instead of adding more.
    WavTempoTag::inject (f, 120.0);
    const int againFails = checkFile (f, data, "re-injected");
    juce::int64 sz1 = f.getSize();
    WavTempoTag::inject (f, 120.0);
    const bool stable = f.getSize() == sz1;
    std::printf ("re-inject size stable: %s\n", stable ? "PASS" : "FAIL");
    const bool skip = ! WavTempoTag::inject (f, 0.0) && f.getSize() == sz1;
    std::printf ("bpm 0 skipped: %s\n", skip ? "PASS" : "FAIL");
    dir.deleteRecursively();

    const bool pass = ok && bareFails > 0 && tagFails == 0 && againFails == 0 && stable && skip;
    std::printf ("teeth: bare file failed %d checks (must be > 0)\n%s\n", bareFails, pass ? "ALL PASS" : "TEST FAILED");
    return pass ? 0 : 1;
}
