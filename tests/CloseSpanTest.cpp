#ifndef HSLINK_STUB_DEFAULT_PATH
 #define HSLINK_STUB_DEFAULT_PATH "../HOT-Step-CPP/server/scripts/daw-contract-stub.mjs"
#endif
// HOT-STEP LINK -- CloseSpanTest.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
// Full host close span: from the host close request (editor delete begins) through the
// processor destructor and worker stop, to the exit of the last plugin-owned thread
// (pools, saver, daw worker, any HTTP thread). A repaint with a source upload is in
// flight (the stub holds POST /generate 5 s) and host bypass is toggled before the close.
// Thread exits are tracked explicitly: every thread created after the baseline whose
// start address is the CRT thread entry (exe or ucrtbase: juce::Thread / ThreadPool
// threads) is opened with SYNCHRONIZE and waited on. OS pool threads (WinINet, COM) start
// in system DLLs and are not counted.
// Asserts span < 2000 ms. Teeth: HSLINK_CLOSE_STALL_MS=<ms> holds the destructor that
// long through the testBeforePoolWait hook, which must make the test fail.
// Needs node and the contract stub (HSLINK_STUB, default HSLINK_STUB_DEFAULT_PATH from CMake).
// Scratch under <cwd>/close_span_scratch, removed at the end.
#include "PluginProcessor.h"

#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#pragma comment (lib, "dbghelp.lib")

#include <cmath>
#include <cstdio>
#include <set>
#include <vector>

namespace
{
int failures = 0, passes = 0;
int stallMs = 0;

void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    (ok ? passes : failures)++;
}

void stallHook (HotStepLinkProcessor&) { juce::Thread::sleep (stallMs); }

std::set<DWORD> threadIds()
{
    std::set<DWORD> ids;
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return ids;
    THREADENTRY32 te {};
    te.dwSize = sizeof (te);
    if (Thread32First (snap, &te))
        do { if (te.th32OwnerProcessID == pid) ids.insert (te.th32ThreadID); } while (Thread32Next (snap, &te));
    CloseHandle (snap);
    return ids;
}

using NtQIT = LONG (WINAPI*) (HANDLE, int, PVOID, ULONG, PULONG);

bool startsInExe (HANDLE h)
{
    static NtQIT q = (NtQIT) (void*) GetProcAddress (GetModuleHandleW (L"ntdll.dll"), "NtQueryInformationThread");
    if (q == nullptr) return false;
    void* start = nullptr;
    if (q (h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof (start), nullptr) != 0) return false;
    // juce::Thread starts through _beginthreadex: the start address is in this exe
    // (static CRT) or in the CRT DLL (ucrtbase, dynamic CRT). OS pool threads start in ntdll.
    HMODULE mod = nullptr;
    if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (LPCWSTR) start, &mod) || mod == nullptr)
        return false;
    if (mod == GetModuleHandleW (nullptr)) return true;
    wchar_t name[MAX_PATH] {};
    GetModuleFileNameW (mod, name, MAX_PATH);
    return juce::File (juce::String (name)).getFileName().equalsIgnoreCase ("ucrtbase.dll");
}

struct Tracked { DWORD id; HANDLE h; };

juce::String threadName (HANDLE h)
{
    using GTD = HRESULT (WINAPI*) (HANDLE, PWSTR*);
    static GTD g = (GTD) (void*) GetProcAddress (GetModuleHandleW (L"kernel32.dll"), "GetThreadDescription");
    if (g == nullptr) return "?";
    PWSTR d = nullptr;
    juce::String r;
    if (SUCCEEDED (g (h, &d)) && d != nullptr) { r = juce::String (d); LocalFree (d); }
    return r;
}

std::vector<Tracked> pluginThreads (const std::set<DWORD>& baseline)
{
    std::vector<Tracked> out;
    const DWORD self = GetCurrentThreadId();
    for (auto id : threadIds())
    {
        if (id == self || baseline.count (id) != 0) continue;
        HANDLE h = OpenThread (SYNCHRONIZE | THREAD_QUERY_INFORMATION, FALSE, id);
        if (h == nullptr) continue;
        if (startsInExe (h)) out.push_back ({ id, h });
        else CloseHandle (h);
    }
    return out;
}

void dumpStack (DWORD tid)
{
    HANDLE h = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (h == nullptr) return;
    HANDLE proc = GetCurrentProcess();
    static bool symInit = false;
    if (! symInit) { SymSetOptions (SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS); SymInitialize (proc, juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFullPathName().toRawUTF8(), TRUE); symInit = true; }
    SuspendThread (h);
    CONTEXT ctx {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext (h, &ctx))
    {
        STACKFRAME64 f {};
        f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < 24 && StackWalk64 (IMAGE_FILE_MACHINE_AMD64, proc, h, &f, &ctx, nullptr,
                                                SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i)
        {
            char buf[sizeof (SYMBOL_INFO) + 512] {};
            auto* si = (SYMBOL_INFO*) buf;
            si->SizeOfStruct = sizeof (SYMBOL_INFO);
            si->MaxNameLen = 511;
            DWORD64 disp = 0;
            std::printf ("    #%d %s\n", i, SymFromAddr (proc, f.AddrPC.Offset, &disp, si) ? si->Name : "?");
        }
    }
    ResumeThread (h);
    CloseHandle (h);
}

juce::AudioBuffer<float> noise (int ch, int n, juce::uint32 seed)
{
    juce::AudioBuffer<float> b (ch, n);
    juce::uint32 s = seed;
    for (int c = 0; c < ch; ++c)
        for (int i = 0; i < n; ++i)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            b.setSample (c, i, 0.5f * ((float) (s & 0xffff) / 32768.0f - 1.0f));
        }
    return b;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    stallMs = juce::SystemStats::getEnvironmentVariable ("HSLINK_CLOSE_STALL_MS", "0").getIntValue();
    auto scratch = juce::File::getCurrentWorkingDirectory().getChildFile ("close_span_scratch");
    scratch.deleteRecursively();
    scratch.createDirectory();
    std::printf ("Close span (host close to last plugin thread exit), stall=%d ms\n", stallMs);

    auto stub = juce::File (juce::SystemStats::getEnvironmentVariable (
        "HSLINK_STUB", HSLINK_STUB_DEFAULT_PATH));
    if (! stub.existsAsFile())
    {
        std::printf ("  [INFO] stub not found, skipped\n");
        return 1;
    }
    juce::ChildProcess server;
    check (server.start ("node \"" + stub.getFullPathName() + "\" --ok --port 3923", 0), "stub started");
    juce::Thread::sleep (500);

    auto src = scratch.getChildFile ("src.wav");
    {
        juce::AudioBuffer<float> b (2, 24000);
        for (int i = 0; i < 24000; ++i)
        {
            const float v = 0.3f * (float) std::sin (0.05 * i);
            b.setSample (0, i, v);
            b.setSample (1, i, v);
        }
        std::unique_ptr<juce::OutputStream> os (src.createOutputStream());
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (48000.0)
                                              .withNumChannels (2).withBitsPerSample (32)
                                              .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (w != nullptr) w->writeFromAudioSampleBuffer (b, 0, 24000);
    }

    if (stallMs > 0)
        HotStepLinkProcessor::testBeforePoolWait = &stallHook;

    // Exclusion by identity: the JUCE shared TimerThread ("<version>: Timer", unnamed in
    // Release) is a SharedResourcePointer held by every live juce::Timer object in the
    // process (proven by stack: juce::Timer::TimerThread::run). The plugin's APVTS timer
    // spawns it in the ctor; after the close it stays up while any other Timer object in the
    // process (JUCE Desktop / GUI singletons) is alive. A real host keeps Timers alive for its
    // whole life, so the thread is host-owned, not a plugin leak. Hold one Timer for the whole
    // test, as a host does, and spawn it before the baseline so it is never tracked.
    // Everything else born after the baseline is still tracked and must exit.
    struct Warm : juce::Timer { void timerCallback() override {} } warm;
    warm.startTimer (1000);
    warm.stopTimer();
    juce::Thread::sleep (50);
    const auto baseline = threadIds();
    auto takes = scratch.getChildFile ("takes");
    double span = -1.0, editorMs = 0.0, dtorMs = 0.0, joinMs = 0.0;
    size_t nThreads = 0, leftAlive = 0;
    {
        auto p = std::make_unique<HotStepLinkProcessor>();
        const auto afterCtor = threadIds();
        const int bs = 512;
        p->setPlayConfigDetails (2, 2, 48000.0, bs);
        p->prepareToPlay (48000.0, bs);
        const auto afterPrep = threadIds();
        p->daw.setEndpoint ("http://127.0.0.1:3923", "test-key");
        p->daw.setTakesFolder (takes);
        p->daw.start();
        const auto afterStart = threadIds();
        std::unique_ptr<juce::AudioProcessorEditor> ed (p->createEditorIfNeeded());
        const auto afterEditor = threadIds();
        check (ed != nullptr, "editor created");

        bool online = false;
        for (int i = 0; i < 600 && ! online; ++i)
        {
            online = p->daw.getSnapshot().online;
            if (! online) juce::Thread::sleep (20);
        }
        check (online, "worker online before the submit");

        DawJobRequest r;
        r.mode = "repaint";
        r.caption = "slowsubmit"; // stub holds the generate answer 5 s
        r.takes = 1;
        r.sourceWav = src;
        r.hasRegion = true;
        r.regionStartS = 0.1;
        r.regionEndS = 0.4;
        const int id = p->daw.submitGenerate (r);
        check (id != 0, "job accepted");
        bool sending = false;
        for (int i = 0; i < 300 && ! sending; ++i)
        {
            DawJob j;
            if (p->daw.getJob (id, j)) sending = j.submitting;
            if (! sending) juce::Thread::sleep (10);
        }
        check (sending, "upload in flight before the close");

        // Toggle bypass: normal, bypassed, normal, bypassed (left bypassed at the close).
        for (int k = 0; k < 40; ++k)
        {
            auto b = noise (2, bs, 0x7000u + (juce::uint32) k);
            juce::MidiBuffer m;
            if ((k / 10) % 2 == 1) p->processBlockBypassed (b, m); else p->processBlock (b, m);
        }
        juce::Thread::sleep (50);

        auto tracked = pluginThreads (baseline);
        nThreads = tracked.size();
        std::printf ("  plugin-owned threads tracked: %d\n", (int) nThreads);
        check (nThreads >= 2, "plugin threads found to track (worker, saver, pools)");

        // Host close: editor delete, then processor release.
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        p->editorBeingDeleted (ed.get());
        ed.reset();
        const double t1 = juce::Time::getMillisecondCounterHiRes();
        p.reset();
        const double t2 = juce::Time::getMillisecondCounterHiRes();
        double last = t2;
        for (auto& t : tracked)
        {
            const double left = 10000.0 - (juce::Time::getMillisecondCounterHiRes() - t0);
            if (WaitForSingleObject (t.h, (DWORD) juce::jmax (0.0, left)) == WAIT_OBJECT_0)
                last = juce::jmax (last, juce::Time::getMillisecondCounterHiRes());
            else
            {
                ++leftAlive;
                std::printf ("  still alive: tid=%u name=%s born %s\n", (unsigned) t.id, threadName (t.h).toRawUTF8(),
                             afterCtor.count (t.id) ? "in the ctor" : afterPrep.count (t.id) ? "in prepareToPlay"
                             : afterStart.count (t.id) ? "in daw.start" : afterEditor.count (t.id) ? "with the editor" : "after the editor");
                dumpStack (t.id);
            }
            CloseHandle (t.h);
        }
        editorMs = t1 - t0;
        dtorMs = t2 - t1;
        joinMs = last - t2;
        span = leftAlive == 0 ? last - t0 : 10000.0;
    }
    std::printf ("  CLOSE_SPAN_MS=%.1f editor=%.1f dtor=%.1f lastThreadAfterDtor=%.1f alive=%d\n",
                 span, editorMs, dtorMs, joinMs, (int) leftAlive);
    check (leftAlive == 0, "every plugin-owned thread exited");
    check (span >= 0.0 && span < 2000.0, "close span under 2000 ms");

    server.kill();
    scratch.deleteRecursively();
    std::printf ("\n%d passed, %d failed\n", passes, failures);
    return failures == 0 ? 0 : 1;
}
