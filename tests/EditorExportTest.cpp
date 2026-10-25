// HOT-STEP LINK -- EditorExportTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// The take editor's export path end to end. Export loop and Apply crop in
// TakeEditorPanel go through onApply, which the editor binds to
// HotStepLinkProcessor::requestTakeEdit; that queues TakeEdit::renderEditFile, which
// writes the WAV beside the take (temporary name, exclusive create, WavTempoTag, rename
// without replace). Checks: the exported file is in the take's folder under
// editedTakeName; rate, channels, bit depth and frames match the take (and the warp
// result when warped); samples match the take audio (or TakeStretch::renderWarped)
// within a stated tolerance; acid and smpl tags as WavTempoTag writes them; a missing
// source, an existing (taken) target and a foreign temporary file give an error and
// leave no partial file.
// Writes only under <cwd>/editexport_scratch and removes it at the end.
#include "PluginProcessor.h"
#include "TakeEdit.h"
#include "TakeStretch.h"
#include "WarpMarkers.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
int failures = 0, passes = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? passes : failures)++;
}

void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double rate, int bits)
{
    f.deleteFile();
    f.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
    if (os == nullptr)
        return;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}
                                          .withSampleRate (rate)
                                          .withNumChannels (b.getNumChannels())
                                          .withBitsPerSample (bits)
                                          .withSampleFormat (bits == 32 ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                                        : juce::AudioFormatWriterOptions::SampleFormat::integral));
    if (w != nullptr)
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

struct WavInfo
{
    bool ok = false;
    double rate = 0.0;
    int channels = 0, bits = 0;
    bool isFloat = false;
    juce::int64 frames = 0;
    juce::AudioBuffer<float> audio;
};

WavInfo readWav (const juce::File& f)
{
    WavInfo r;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (new juce::FileInputStream (f), true));
    if (rd == nullptr)
        return r;
    r.ok = true;
    r.rate = rd->sampleRate;
    r.channels = (int) rd->numChannels;
    r.bits = (int) rd->bitsPerSample;
    r.isFloat = rd->usesFloatingPointData;
    r.frames = rd->lengthInSamples;
    r.audio.setSize (r.channels, (int) r.frames);
    rd->read (&r.audio, 0, (int) r.frames, 0, true, r.channels > 1);
    return r;
}

// Finds the RIFF chunk id; returns its payload or empty.
juce::MemoryBlock chunkOf (const juce::File& f, const char* id)
{
    juce::MemoryBlock all;
    f.loadFileAsData (all);
    const auto* p = static_cast<const unsigned char*> (all.getData());
    const size_t n = all.getSize();
    if (n < 12 || std::memcmp (p, "RIFF", 4) != 0 || std::memcmp (p + 8, "WAVE", 4) != 0)
        return {};
    size_t pos = 12;
    while (pos + 8 <= n)
    {
        const uint32_t sz = (uint32_t) p[pos + 4] | ((uint32_t) p[pos + 5] << 8) | ((uint32_t) p[pos + 6] << 16) | ((uint32_t) p[pos + 7] << 24);
        if (std::memcmp (p + pos, id, 4) == 0 && pos + 8 + sz <= n)
            return juce::MemoryBlock (p + pos + 8, sz);
        pos += 8 + sz + (sz & 1u);
    }
    return {};
}

uint32_t le32 (const juce::MemoryBlock& m, size_t at)
{
    const auto* p = static_cast<const unsigned char*> (m.getData());
    return (uint32_t) p[at] | ((uint32_t) p[at + 1] << 8) | ((uint32_t) p[at + 2] << 16) | ((uint32_t) p[at + 3] << 24);
}

float leFloat (const juce::MemoryBlock& m, size_t at)
{
    const uint32_t u = le32 (m, at);
    float v;
    std::memcpy (&v, &u, 4);
    return v;
}

// Acid tag check as WavTempoTag writes it: tempo, beats = round (frames * bpm / 60 / sr),
// stretch on and not one-shot, 4/4; smpl with one loop 0 .. frames - 1.
bool tagsOk (const juce::File& f, double wantBpm, juce::int64 frames, double sr, const char* label)
{
    const auto acid = chunkOf (f, "acid");
    const auto smpl = chunkOf (f, "smpl");
    if (acid.getSize() != 24 || smpl.getSize() < 60)
    {
        std::printf ("  %s: acid bytes=%d smpl bytes=%d (want 24 and 60)\n", label, (int) acid.getSize(), (int) smpl.getSize());
        return false;
    }
    const uint32_t flags = le32 (acid, 0), beats = le32 (acid, 12);
    const float tempo = leFloat (acid, 20);
    const auto* a = static_cast<const unsigned char*> (acid.getData());
    const int den = a[16] | (a[17] << 8), num = a[18] | (a[19] << 8);
    const auto wantBeats = (uint32_t) std::llround ((double) frames * wantBpm / 60.0 / sr);
    const uint32_t loops = le32 (smpl, 28), lStart = le32 (smpl, 36 + 8), lEnd = le32 (smpl, 36 + 12);
    std::printf ("  %s: acid tempo=%.4f (want %.4f) beats=%u (want %u) flags=0x%x meter=%d/%d; smpl loops=%u %u..%u (want 0..%lld)\n", label, (double) tempo, wantBpm,
                 beats, wantBeats, flags, num, den, loops, lStart, lEnd, (long long) (frames - 1));
    return tempo == (float) wantBpm && beats == wantBeats && (flags & 0x01u) == 0 && (flags & 0x04u) != 0 && num == 4 && den == 4 && loops == 1 && lStart == 0
           && (juce::int64) lEnd == frames - 1;
}

// A steady drum-like beat at bpm: a decaying 60 Hz thump and a click on every beat over a quiet bed.
juce::AudioBuffer<float> beatAudio (double bpm, double seconds, double sr, int channels)
{
    const int n = (int) std::llround (seconds * sr);
    juce::AudioBuffer<float> b (channels, n);
    const double beat = 60.0 / bpm * sr;
    for (int i = 0; i < n; ++i)
    {
        const double ph = std::fmod ((double) i, beat);
        const double t = ph / sr;
        double v = 0.7 * std::exp (-t * 18.0) * std::sin (6.283185307179586 * 60.0 * t);
        if (ph < 0.004 * sr)
            v += 0.3 * std::sin (6.283185307179586 * 2500.0 * t) * (1.0 - ph / (0.004 * sr));
        v += 0.02 * std::sin (6.283185307179586 * 220.0 * (double) i / sr);
        for (int c = 0; c < channels; ++c)
            b.setSample (c, i, (float) (v * (c == 0 ? 1.0 : 0.9)));
    }
    return b;
}

int partsIn (const juce::File& d) { return d.findChildFiles (juce::File::findFiles, false, "*.editing").size(); }
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const auto root = juce::File::getCurrentWorkingDirectory().getChildFile ("editexport_scratch");
    root.deleteRecursively();
    root.createDirectory();
    const double sr = 48000.0;

    // A: Export loop through the processor, the way the editor's onApply does it.
    {
        auto p = std::make_unique<HotStepLinkProcessor>();
        p->setStableRoot (root.getChildFile ("stable"));
        p->setCollectionRoot (root.getChildFile ("coll"));
        DawJob j;
        j.localId = 1;
        j.request.caption = "editor export case";
        j.request.expectedBackend = "sa3";
        j.request.bpm = 120.0;
        j.request.seed = 4242;
        j.request.durationS = 16.0;
        const auto dl = root.getChildFile ("dl").getChildFile ("take0.wav");
        writeWav (dl, beatAudio (120.0, 16.0, sr, 2), sr, 32);
        j.takeFiles.add (dl);
        j.takeSeeds.add (4242);
        p->registerTakeAndWait (j, 0);
        p->waitTakeMeasureIdle();
        const auto takes = p->getTakes();
        const auto take = takes.empty() ? HslinkTakeEntry() : takes[0];
        const auto src = readWav (take.temp);
        std::printf ("  A take: listed=%d temp='%s' rate=%.0f ch=%d bits=%d float=%d frames=%lld measured=%d valid=%d bpm=%.4f\n", (int) takes.size(),
                     take.temp.getFileName().toRawUTF8(), src.rate, src.channels, src.bits, (int) src.isFloat, (long long) src.frames, take.tempoState,
                     (int) take.tempo.valid, take.tempo.bpm);
        check (takes.size() == 1 && src.ok && src.frames == (juce::int64) (16.0 * sr), "A: one 16 s take listed with a readable temp file");
        check (take.tempo.valid && std::abs (take.tempo.bpm - 120.0) < 0.1, "A: take measured within 0.1 BPM of 120 (its grid tags the export)");

        TakeEdit::EditData edit;
        edit.loopStart = 96000;  // 2.0 s
        edit.loopEnd = 192000;   // 4.0 s = 4 beats
        TakeEdit::EditOptions opt;
        opt.loopRepeats = 3;
        opt.snapToGrid = true;
        const auto wantName = HotStepLinkProcessor::editedTakeName (take.name, edit, opt);
        const auto folder = take.temp.getParentDirectory();
        juce::String whyNot;
        const auto ticket = p->requestTakeEdit (take.temp, edit, opt, &whyNot);
        p->waitTakeEditIdle();
        p->waitTakeMeasureIdle();
        p->waitSaverIdle();
        HotStepLinkProcessor::TakeEditReport rep;
        const bool got = ticket != 0 && p->getTakeEditReport (ticket, rep);
        const auto out = folder.getChildFile (wantName);
        const auto o = readWav (out);
        const juce::int64 L = rep.result.used.loopEnd - rep.result.used.loopStart;
        std::printf ("  A export: ticket=%llu why='%s' report=%d ok=%d err='%s' newName='%s' want='%s' file exists=%d; used loop %lld..%lld L=%lld seam=%lld\n",
                     (unsigned long long) ticket, whyNot.toRawUTF8(), (int) got, (int) rep.ok, rep.result.error.toRawUTF8(), rep.newName.toRawUTF8(), wantName.toRawUTF8(),
                     (int) out.existsAsFile(), (long long) rep.result.used.loopStart, (long long) rep.result.used.loopEnd, (long long) L, (long long) rep.result.seamFadeSamples);
        check (got && rep.ok && out.existsAsFile() && rep.newName == wantName, "A: exported file exists in the take's folder under editedTakeName");
        std::printf ("  A format: rate=%.0f/%.0f ch=%d/%d bits=%d/%d float=%d/%d frames=%lld (want 3 x %lld = %lld)\n", o.rate, src.rate, o.channels, src.channels, o.bits,
                     src.bits, (int) o.isFloat, (int) src.isFloat, (long long) o.frames, (long long) L, (long long) (3 * L));
        check (o.ok && o.rate == src.rate && o.channels == src.channels && o.bits == src.bits && o.isFloat == src.isFloat && L > 0 && o.frames == 3 * L
                   && rep.result.outSamples == o.frames,
               "A: rate, channels, bit depth match the take; frames = repeats x loop length");
        // Samples: every repeat is the loop region; the last seam frames of a repeat are the crossfade.
        double maxDiff = 1.0e9;
        juce::int64 compared = 0;
        if (o.ok && L > 0 && o.frames == 3 * L)
        {
            maxDiff = 0.0;
            const juce::int64 seam = rep.result.seamFadeSamples;
            for (int c = 0; c < o.channels; ++c)
                for (int k = 0; k < 3; ++k)
                    for (juce::int64 i = 0; i < L - seam; ++i)
                    {
                        const double d = std::abs ((double) o.audio.getSample (c, (int) (k * L + i)) - (double) src.audio.getSample (c, (int) (rep.result.used.loopStart + i)));
                        maxDiff = juce::jmax (maxDiff, d);
                        ++compared;
                    }
        }
        std::printf ("  A samples: compared %lld, max abs diff %.3e (tolerance 1e-6, 32 bit float)\n", (long long) compared, maxDiff);
        check (maxDiff <= 1.0e-6 && compared > (juce::int64) (0.9 * 3 * L * o.channels), "A: samples match the take's loop region outside the seam crossfade");
        check (tagsOk (out, take.tempo.bpm, o.frames, sr, "A tags"), "A: acid tempo = take grid tempo, beats and smpl loop as WavTempoTag writes");
        check (partsIn (folder) == 0, "A: no temporary .editing file left in the take folder");

        // The same export again: the name is taken, nothing is overwritten or listed.
        const auto before = out.getLastModificationTime();
        juce::MemoryBlock bytes;
        out.loadFileAsData (bytes);
        const auto t2 = p->requestTakeEdit (take.temp, edit, opt, &whyNot);
        p->waitTakeEditIdle();
        HotStepLinkProcessor::TakeEditReport rep2;
        const bool got2 = t2 != 0 && p->getTakeEditReport (t2, rep2);
        juce::MemoryBlock bytes2;
        out.loadFileAsData (bytes2);
        std::printf ("  A again: ticket=%llu report=%d ok=%d err='%s' newName='%s' takes=%d bytes same=%d parts=%d\n", (unsigned long long) t2, (int) got2, (int) rep2.ok,
                     rep2.result.error.toRawUTF8(), rep2.newName.toRawUTF8(), (int) p->getTakes().size(), (int) (bytes == bytes2), partsIn (folder));
        check ((t2 == 0 || (got2 && ! rep2.ok && rep2.newName.isEmpty())) && bytes == bytes2 && out.getLastModificationTime() == before && partsIn (folder) == 0
                   && p->getTakes().size() == 2,
               "A: a second export to a taken name fails, the file is untouched, nothing new listed, no partial file");

        // An unlisted (missing) take: refused, nothing written.
        const auto ghost = folder.getChildFile ("ghost.wav");
        const auto t3 = p->requestTakeEdit (ghost, edit, opt, &whyNot);
        p->waitTakeEditIdle();
        const auto ghostOut = folder.getChildFile (HotStepLinkProcessor::editedTakeName ("ghost", edit, opt));
        std::printf ("  A missing: ticket=%llu why='%s' out exists=%d parts=%d\n", (unsigned long long) t3, whyNot.toRawUTF8(), (int) ghostOut.exists(), partsIn (folder));
        check (t3 == 0 && whyNot.isNotEmpty() && ! ghostOut.exists() && partsIn (folder) == 0, "A: export of an unlisted take is refused with a reason and writes nothing");
    }

    // B: a warped export (Stretch to fit): renderEditFile with markers, 24 bit mono.
    {
        const auto d = root.getChildFile ("warp");
        d.createDirectory();
        const auto in = beatAudio (120.0, 8.0, sr, 1);
        const auto srcFile = d.getChildFile ("src.wav");
        writeWav (srcFile, in, sr, 24);
        const auto srcBack = readWav (srcFile);
        std::vector<WarpMarkers::Marker> base { { 0, 0 }, { (std::int64_t) in.getNumSamples(), (std::int64_t) in.getNumSamples() } };
        TakeEdit::EditRequest rq;
        rq.source = srcFile;
        rq.dest = d.getChildFile ("warped.wav");
        rq.options.warp = WarpMarkers::scaleToTempo (base, 120.0, 124.0);
        rq.options.warpBpm = 124.0;
        const auto r = TakeEdit::renderEditFile (rq);
        const auto o = readWav (rq.dest);
        const auto ref = TakeStretch::renderWarped (srcBack.audio, sr, rq.options.warp);
        const auto wantFrames = rq.options.warp.empty() ? -1 : (juce::int64) rq.options.warp.back().dstFrame;
        double maxDiff = 1.0e9;
        if (o.ok && o.frames == ref.getNumSamples())
        {
            maxDiff = 0.0;
            for (int i = 0; i < ref.getNumSamples(); ++i)
                maxDiff = juce::jmax (maxDiff, std::abs ((double) o.audio.getSample (0, i) - (double) juce::jlimit (-1.0f, 1.0f, ref.getSample (0, i))));
        }
        std::printf ("  B warp: ok=%d err='%s' rate=%.0f ch=%d bits=%d frames=%lld (want last dst %lld, renderWarped %d) max abs diff %.3e (tolerance 2e-7, 24 bit step 1.2e-7)\n",
                     (int) r.ok, r.error.toRawUTF8(), o.rate, o.channels, o.bits, (long long) o.frames, (long long) wantFrames, ref.getNumSamples(), maxDiff);
        check (r.ok && o.ok && o.rate == sr && o.channels == 1 && o.bits == 24 && ! o.isFloat && o.frames == wantFrames && o.frames == ref.getNumSamples(),
               "B: warped export keeps rate, channels, 24 bit; frames = last marker dst = renderWarped length");
        check (maxDiff <= 2.0e-7, "B: warped samples match TakeStretch::renderWarped within one 24 bit step");
        check (tagsOk (rq.dest, 124.0, o.frames, sr, "B tags"), "B: acid tempo = warpBpm, beats and smpl loop as WavTempoTag writes");
        check (partsIn (d) == 0, "B: no temporary file left");
    }

    // C: failures leave no file and no partial.
    {
        const auto d = root.getChildFile ("fail");
        d.createDirectory();
        const auto srcFile = d.getChildFile ("src.wav");
        writeWav (srcFile, beatAudio (120.0, 4.0, sr, 2), sr, 32);
        TakeEdit::EditRequest rq;
        rq.edit.cropStart = 48000;
        rq.edit.cropEnd = 144000;

        rq.source = d.getChildFile ("missing.wav");
        rq.dest = d.getChildFile ("out_missing.wav");
        const auto r1 = TakeEdit::renderEditFile (rq);
        std::printf ("  C missing source: ok=%d err='%s' dest exists=%d parts=%d\n", (int) r1.ok, r1.error.toRawUTF8(), (int) rq.dest.exists(), partsIn (d));
        check (! r1.ok && r1.error.isNotEmpty() && ! rq.dest.exists() && partsIn (d) == 0, "C: missing source gives an error and no file");

        rq.source = srcFile;
        rq.dest = d.getChildFile ("taken.wav");
        rq.dest.replaceWithText ("someone else's file");
        const auto r2 = TakeEdit::renderEditFile (rq);
        std::printf ("  C taken target: ok=%d err='%s' content kept=%d parts=%d\n", (int) r2.ok, r2.error.toRawUTF8(),
                     (int) (rq.dest.loadFileAsString() == "someone else's file"), partsIn (d));
        check (! r2.ok && r2.error.isNotEmpty() && rq.dest.loadFileAsString() == "someone else's file" && partsIn (d) == 0,
               "C: a target that exists is refused and left byte for byte");

        // Exclusive temporary name: a foreign file at the part name is neither written nor removed.
        static const char* token = "t4b22x";
        TakeEdit::testEditPartToken.store (token);
        rq.dest = d.getChildFile ("locked.wav");
        const auto part = d.getChildFile ("locked.wav." + juce::String (token) + ".editing");
        part.replaceWithText ("foreign");
        const auto r3 = TakeEdit::renderEditFile (rq);
        TakeEdit::testEditPartToken.store (nullptr);
        std::printf ("  C foreign part file: ok=%d err='%s' dest exists=%d part kept=%d\n", (int) r3.ok, r3.error.toRawUTF8(), (int) rq.dest.exists(),
                     (int) (part.loadFileAsString() == "foreign"));
        check (! r3.ok && r3.error.isNotEmpty() && ! rq.dest.exists() && part.loadFileAsString() == "foreign",
               "C: a foreign file at the temporary name blocks the export and is left alone");
        part.deleteFile();

        // Abort mid write: no dest, no partial.
        rq.dest = d.getChildFile ("aborted.wav");
        int partSeen = 0; // abort only once the temporary file is on disk
        const auto r4 = TakeEdit::renderEditFile (rq, [&] { const bool there = partsIn (d) > 0; partSeen += there ? 1 : 0; return there; });
        std::printf ("  C abort: ok=%d err='%s' part seen while writing=%d dest exists=%d parts after=%d\n", (int) r4.ok, r4.error.toRawUTF8(), partSeen,
                     (int) rq.dest.exists(), partsIn (d));
        check (! r4.ok && partSeen > 0 && ! rq.dest.exists() && partsIn (d) == 0, "C: an export aborted mid write leaves no file and no partial");
    }

    root.deleteRecursively();
    std::printf ("EditorExportTest: %d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
