// HOT-STEP LINK -- ExclusiveFile.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#include "ExclusiveFile.h"
#include <cstring>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
 #include <windows.h>
#else
 #include <unistd.h>
 #include <fcntl.h>
 #include <sys/file.h>
 #include <sys/stat.h>
 #include <cerrno>
#endif

namespace ExclusiveFile
{
std::atomic<bool> testLegacyOverwrite { false };
std::atomic<int> testLostRenames { 0 };
std::function<void (const juce::File& picked)> testBeforePublish;
std::atomic<bool> testIgnoreAbort { false };
std::atomic<void (*) (const char* step)> testStepHook { nullptr };
std::atomic<int> testStepsByTag[tagCount] {};
std::atomic<bool> ioCancelEnabled { true };
std::atomic<int> testIoCancelCalls { 0 };
std::atomic<int> testIoCancelHits { 0 };
std::atomic<bool> copyCancelEnabled { true };
std::atomic<int (*) (juce::int64 done, juce::int64 total)> testCopyProgressHook { nullptr };
std::atomic<int> testCopiesStarted { 0 };
std::atomic<int> testCopiesCancelled { 0 };
std::atomic<int> testCopyProgressCalls { 0 };
std::atomic<int> testCleanupCalls { 0 };
std::atomic<bool> testLegacyPartCleanup { false };
std::atomic<const char*> testPartToken { nullptr };
std::atomic<int> testPartExistsFailures { 0 };
std::atomic<bool> testLegacyPartDelete { false };
std::atomic<int> testCreateFault { 0 };
std::atomic<int> testCreateFaultsLeft { -1 };
std::atomic<int> testCreateCalls { 0 };
std::atomic<int> testFolderTries { 0 };
std::atomic<bool> testLegacyFolderRetry { false };

namespace
{
    int& threadTagSlot()
    {
        thread_local int tag = tagOther;
        return tag;
    }
    IoCanceller*& cancellerSlot()
    {
        thread_local IoCanceller* c = nullptr;
        return c;
    }
    void ioCall (bool inCall, bool cleanup = false)
    {
        if (auto* c = cancellerSlot())
            c->setInCall (inCall, cleanup);
    }
    // Ends "in a call" on every way out of ensureFolder and copyUnique.
    struct IoCallEnd
    {
        ~IoCallEnd() { ioCall (false); }
    };
}

IoCanceller::~IoCanceller()
{
    std::lock_guard<std::mutex> g (lock);
   #if JUCE_WINDOWS
    if (thread != nullptr)
        CloseHandle ((HANDLE) thread);
   #endif
    thread = nullptr;
}

void IoCanceller::attachCurrentThread()
{
    std::lock_guard<std::mutex> g (lock);
   #if JUCE_WINDOWS
    if (thread == nullptr)
    {
        HANDLE h = nullptr;
        // A real handle to this thread (GetCurrentThread is a pseudo handle), with the
        // THREAD_TERMINATE access CancelSynchronousIo asks for and nothing else.
        if (DuplicateHandle (GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                             THREAD_TERMINATE, FALSE, 0) != 0)
            thread = (void*) h;
    }
   #endif
    inCall = false;
    cleanup = false;
    cancellerSlot() = this;
}

void IoCanceller::detach()
{
    std::lock_guard<std::mutex> g (lock);
   #if JUCE_WINDOWS
    if (thread != nullptr)
        CloseHandle ((HANDLE) thread);
   #endif
    thread = nullptr;
    inCall = false;
    cleanup = false;
    if (cancellerSlot() == this)
        cancellerSlot() = nullptr;
}

bool IoCanceller::isAttached()
{
    std::lock_guard<std::mutex> g (lock);
    return thread != nullptr;
}

void IoCanceller::setInCall (bool nowInCall, bool isCleanup)
{
    std::lock_guard<std::mutex> g (lock);
    inCall = nowInCall;
    cleanup = nowInCall && isCleanup;
    if (cleanup)
        cleanupSince = juce::Time::getMillisecondCounter();
}

bool IoCanceller::cancelInFlight()
{
    if (! ioCancelEnabled.load())
        return false;
    std::lock_guard<std::mutex> g (lock);
    if (thread == nullptr || ! inCall)
        return false;
    // The delete of the thread's own .part file gets time to finish first.
    if (cleanup && juce::Time::getMillisecondCounter() - cleanupSince < (juce::uint32) cleanupGraceMs)
        return false;
   #if JUCE_WINDOWS
    ++testIoCancelCalls;
    if (CancelSynchronousIo ((HANDLE) thread) != 0)
        ++testIoCancelHits; // otherwise ERROR_NOT_FOUND: no synchronous call was in flight
    return true;
   #else
    return false;
   #endif
}

int threadTag() { return threadTagSlot(); }
ScopedThreadTag::ScopedThreadTag (int tag) : previous (threadTagSlot()) { threadTagSlot() = tag; }
ScopedThreadTag::~ScopedThreadTag() { threadTagSlot() = previous; }
std::atomic<bool> sessionLockEnabled { true };
std::atomic<bool> testSessionLockFails { false };
std::atomic<bool> testNoLegacyGuard { false };
std::atomic<int> testOwnerAcquires { 0 };

SessionLock& SessionLock::operator= (SessionLock&& o) noexcept
{
    if (this != &o)
    {
        release();
        handle = o.handle;
        fd = o.fd;
        held = o.held;
        o.handle = nullptr;
        o.fd = -1;
        o.held = false;
    }
    return *this;
}

SessionLock::Result SessionLock::tryAcquire (const juce::File& folder)
{
    ++testOwnerAcquires;
    release();
    if (testSessionLockFails.load())
        return Result::Failed;
    return acquire (folder, true);
}

SessionLock::Result SessionLock::tryAcquireExisting (const juce::File& folder)
{
    release();
    return acquire (folder, testNoLegacyGuard.load());
}

SessionLock::Result SessionLock::acquire (const juce::File& folder, bool create)
{
    if (! sessionLockEnabled.load())
    {
        held = true; // teeth: nothing is locked
        return Result::Held;
    }
    const auto file = folder.getChildFile (fileName());
   #if JUCE_WINDOWS
    // No write and no delete sharing: a second open with these arguments, from any
    // thread or process, fails with ERROR_SHARING_VIOLATION while this handle is open.
    const HANDLE h = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        const auto err = GetLastError();
        if (! create && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND))
            return Result::NoLockFile;
        return err == ERROR_SHARING_VIOLATION ? Result::Busy : Result::Failed;
    }
    handle = (void*) h;
   #else
    // flock is per open file description, so a second open in this process conflicts
    // too, which is what two plugin instances in one host need.
    const int f = ::open (file.getFullPathName().toRawUTF8(), O_RDWR | (create ? O_CREAT : 0) | O_CLOEXEC, 0644);
    if (f < 0)
        return ! create && errno == ENOENT ? Result::NoLockFile : Result::Failed;
    if (::flock (f, LOCK_EX | LOCK_NB) != 0)
    {
        const bool busy = errno == EWOULDBLOCK || errno == EAGAIN;
        ::close (f);
        return busy ? Result::Busy : Result::Failed;
    }
    fd = f;
   #endif
    held = true;
    return Result::Held;
}

void SessionLock::release()
{
   #if JUCE_WINDOWS
    if (handle != nullptr)
        CloseHandle ((HANDLE) handle);
   #else
    if (fd >= 0)
        ::close (fd); // closing the descriptor drops the flock
   #endif
    handle = nullptr;
    fd = -1;
    held = false;
}

namespace
{
// Before every blocking step. false = aborted: the caller makes no further file call
// apart from deleting its own .part file.
bool abortedNow (const AbortFn& shouldAbort)
{
    return shouldAbort && ! testIgnoreAbort.load() && shouldAbort();
}

// After a step that passed, the thread counts as "in a call" for IoCanceller until the
// next step, or until the function returns (IoCallEnd).
bool step (const char* name, const AbortFn& shouldAbort)
{
    ioCall (false);
    if (abortedNow (shouldAbort))
        return false;
    ++testStepsByTag[juce::jlimit (0, (int) tagCount - 1, threadTag())];
    ioCall (true);
    if (auto* hook = testStepHook.load())
        hook (name);
    return true;
}

enum class MadeDir { failed, parentMissing, created, wasThere };

// One native call, one level, no probing (juce createDirectory probes every level).
MadeDir makeOneDir (const juce::File& dir)
{
    ++testCreateCalls;
    if (const int fault = testCreateFault.load())
    {
        const int left = testCreateFaultsLeft.load();
        if (left != 0)
        {
            if (left > 0)
                --testCreateFaultsLeft;
            return fault == 2 ? MadeDir::parentMissing : MadeDir::failed;
        }
    }
   #if JUCE_WINDOWS
    if (CreateDirectoryW (dir.getFullPathName().toWideCharPointer(), nullptr) != 0)
        return MadeDir::created;
    const auto err = GetLastError();
    return err == ERROR_ALREADY_EXISTS ? MadeDir::wasThere
         : err == ERROR_PATH_NOT_FOUND ? MadeDir::parentMissing : MadeDir::failed;
   #else
    if (::mkdir (dir.getFullPathName().toRawUTF8(), 0777) == 0)
        return MadeDir::created;
    return errno == EEXIST ? MadeDir::wasThere : errno == ENOENT ? MadeDir::parentMissing : MadeDir::failed;
   #endif
}

// done = dest holds the bytes. existed = dest was already there and was not touched.
// cancelled = stopped by shouldAbort or the test hook; the partial dest is removed.
// interrupted = ended by IoCanceller; dest may be there. failed = any other error;
// dest may be there.
enum class CopyOutcome { done, failed, existed, cancelled, interrupted };

// One poll of the running byte copy. true = cancel it.
bool copyPoll (const AbortFn& shouldAbort, juce::int64 done, juce::int64 total)
{
    ++testCopyProgressCalls;
    bool cancel = false;
    if (auto* hook = testCopyProgressHook.load())
        cancel = hook (done, total) != 0;
    cancel = cancel || abortedNow (shouldAbort);
    return cancel && copyCancelEnabled.load();
}

#if JUCE_WINDOWS
DWORD CALLBACK copyProgress (LARGE_INTEGER total, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                             DWORD, DWORD, HANDLE, HANDLE, LPVOID context)
{
    return copyPoll (*static_cast<const AbortFn*> (context), (juce::int64) done.QuadPart, (juce::int64) total.QuadPart)
               ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
#endif

// Never replaces a file (juce copyFileTo probes the source and deletes the target
// first). Windows: one CopyFileExW call, fail-if-exists, with a progress routine that
// reads shouldAbort before the first byte and after every chunk; a cancelled
// CopyFileExW deletes its own partial destination (probed on NTFS: error 1235, no
// file left, for 4 KB to 64 MB, cancelled at the first or the second routine call).
// Elsewhere: an exclusive create and a loop of 256 KB chunks.
CopyOutcome copyToNew (const juce::File& src, const juce::File& dest, const AbortFn& shouldAbort)
{
    ++testCopiesStarted;
   #if JUCE_WINDOWS
    if (CopyFileExW (src.getFullPathName().toWideCharPointer(), dest.getFullPathName().toWideCharPointer(),
                     copyProgress, (LPVOID) &shouldAbort, nullptr, COPY_FILE_FAIL_IF_EXISTS) != 0)
        return CopyOutcome::done;
    const auto err = GetLastError();
    if (err == ERROR_REQUEST_ABORTED)
    {
        ++testCopiesCancelled;
        return CopyOutcome::cancelled;
    }
    if (err == ERROR_OPERATION_ABORTED)
    {
        ++testCopiesCancelled;
        return CopyOutcome::interrupted;
    }
    return err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS ? CopyOutcome::existed : CopyOutcome::failed;
   #else
    const int in = ::open (src.getFullPathName().toRawUTF8(), O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return CopyOutcome::failed;
    const int out = ::open (dest.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0)
    {
        const bool existed = errno == EEXIST;
        ::close (in);
        return existed ? CopyOutcome::existed : CopyOutcome::failed;
    }
    struct stat st {};
    const juce::int64 total = ::fstat (in, &st) == 0 ? (juce::int64) st.st_size : 0;
    const size_t chunk = 256 * 1024;
    juce::HeapBlock<char> buf (chunk);
    juce::int64 done = 0;
    auto outcome = CopyOutcome::done;
    for (;;)
    {
        if (copyPoll (shouldAbort, done, total))
        {
            outcome = CopyOutcome::cancelled;
            break;
        }
        const auto got = ::read (in, buf.get(), chunk);
        if (got == 0)
            break;
        if (got < 0)
        {
            outcome = CopyOutcome::failed;
            break;
        }
        ssize_t put = 0;
        while (put < got)
        {
            const auto w = ::write (out, buf.get() + put, (size_t) (got - put));
            if (w <= 0)
                break;
            put += w;
        }
        if (put < got)
        {
            outcome = CopyOutcome::failed;
            break;
        }
        done += got;
    }
    ::close (in);
    if (::close (out) != 0 && outcome == CopyOutcome::done)
        outcome = CopyOutcome::failed;
    if (outcome == CopyOutcome::cancelled)
    {
        ++testCopiesCancelled;
        ::unlink (dest.getFullPathName().toRawUTF8()); // created by this call
    }
    return outcome;
   #endif
}

// Removes a .part file this call created: one native call.
void removePart (const juce::File& part)
{
    ioCall (true, true);
   #if JUCE_WINDOWS
    const auto path = part.getFullPathName();
    if (testLegacyPartCleanup.load())
    {
        // Teeth: what juce::File::deleteFile does (exists, isDirectory, delete).
        ++testCleanupCalls;
        if (GetFileAttributesW (path.toWideCharPointer()) != INVALID_FILE_ATTRIBUTES)
        {
            ++testCleanupCalls;
            const bool isDir = (GetFileAttributesW (path.toWideCharPointer()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
            ++testCleanupCalls;
            if (isDir)
                RemoveDirectoryW (path.toWideCharPointer());
            else
                DeleteFileW (path.toWideCharPointer());
        }
    }
    else
    {
        ++testCleanupCalls;
        DeleteFileW (path.toWideCharPointer());
    }
   #else
    if (testLegacyPartCleanup.load())
    {
        testCleanupCalls += 3;
        part.deleteFile();
    }
    else
    {
        ++testCleanupCalls;
        ::unlink (part.getFullPathName().toRawUTF8());
    }
   #endif
    ioCall (false);
}
}

bool moveNoReplace (const juce::File& from, const juce::File& to)
{
    if (from == juce::File() || to == juce::File())
        return false;
   #if JUCE_WINDOWS
    // MoveFileW without MOVEFILE_REPLACE_EXISTING: fails with ERROR_ALREADY_EXISTS
    // when the target is there.
    return MoveFileW (from.getFullPathName().toWideCharPointer(),
                      to.getFullPathName().toWideCharPointer()) != 0;
   #else
    // link fails with EEXIST when the target is there.
    if (::link (from.getFullPathName().toRawUTF8(), to.getFullPathName().toRawUTF8()) != 0)
        return false;
    ::unlink (from.getFullPathName().toRawUTF8());
    return true;
   #endif
}

bool createNew (const juce::File& f)
{
    if (f == juce::File())
        return false;
   #if JUCE_WINDOWS
    const HANDLE h = CreateFileW (f.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle (h);
    return true;
   #else
    const int fd = ::open (f.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return false;
    ::close (fd);
    return true;
   #endif
}

juce::String uniqueToken (int chars)
{
    static std::atomic<juce::uint32> counter { 0 };
    chars = juce::jlimit (4, 16, chars);
    const auto n = counter.fetch_add (1);
   #if JUCE_WINDOWS
    const auto pid = (juce::uint32) GetCurrentProcessId();
   #else
    const auto pid = (juce::uint32) ::getpid();
   #endif
    juce::Random r; // own generator, seeded from the clock: no shared state between threads
    r.setSeedRandomly();
    const auto mixed = ((juce::uint64) (juce::uint32) r.nextInt() << 32 | (juce::uint32) r.nextInt())
                       ^ ((juce::uint64) pid * 0x9E3779B97F4A7C15ULL);
    // Random and process id first, the call counter in the last two characters, so two
    // calls in one process differ whatever the random part is (up to 256 apart).
    auto s = juce::String::toHexString ((juce::int64) mixed).paddedLeft ('0', 16).substring (0, chars - 2)
             + juce::String::toHexString ((int) (n & 0xff)).paddedLeft ('0', 2);
    return s.toLowerCase();
}

juce::File candidate (const juce::File& folder, const juce::String& name, int i)
{
    const auto f = folder.getChildFile (name);
    if (i <= 1)
        return f;
    return folder.getChildFile (f.getFileNameWithoutExtension() + "_" + juce::String (i) + f.getFileExtension());
}

bool ensureFolder (const juce::File& folder, const AbortFn& shouldAbort, bool* aborted)
{
    if (aborted != nullptr)
        *aborted = false;
    auto stop = [aborted] { if (aborted != nullptr) *aborted = true; return false; };
    IoCallEnd callEnd;
    // Another writer may make or remove a level at the same moment: a try that fails
    // because a parent is missing is repeated. Any other create error (access denied, a
    // share that does not answer) ends the call: more tries would cost the same probes
    // again. Every step of every try reads shouldAbort first.
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        ++testFolderTries;
        if (! step ("folder probe", shouldAbort))
            return stop();
        if (folder.isDirectory())
            return true;
        // Missing levels, deepest first, up to the nearest parent that exists.
        juce::Array<juce::File> missing;
        missing.add (folder);
        for (auto f = folder.getParentDirectory(); f != f.getParentDirectory(); f = f.getParentDirectory())
        {
            if (! step ("parent probe", shouldAbort))
                return stop();
            if (f.isDirectory())
                break;
            missing.add (f);
        }
        bool made = true;
        for (int i = missing.size(); --i >= 0 && made;)
        {
            if (! step ("create", shouldAbort))
                return stop();
            const auto r = makeOneDir (missing.getReference (i));
            if (r == MadeDir::wasThere)
            {
                // Made by another writer just now, or a file is in the way.
                if (! step ("created probe", shouldAbort))
                    return stop();
                if (! missing.getReference (i).isDirectory())
                    return false; // a file has the name: no try can succeed
            }
            if (r == MadeDir::failed && ! testLegacyFolderRetry.load())
                return false;
            made = r != MadeDir::failed && r != MadeDir::parentMissing;
        }
        if (made)
            return true;
    }
    return false;
}

bool isPartial (const juce::File& f)
{
    return f.getFileName().endsWithIgnoreCase (".part");
}

bool isCompleteWav (const juce::File& f)
{
    const auto fileSize = f.getSize();
    if (fileSize < 20)
        return false;
    juce::FileInputStream in (f);
    if (! in.openedOk())
        return false;
    auto u32 = [] (const unsigned char* p) {
        return (juce::int64) ((juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24));
    };
    unsigned char h[12];
    if (in.read (h, 12) != 12 || std::memcmp (h, "RIFF", 4) != 0 || std::memcmp (h + 8, "WAVE", 4) != 0)
        return false;
    // A streamed header carries 0 or 0xFFFFFFFF here and a cut-off file a larger
    // number than it has bytes: both fail this one comparison.
    if (u32 (h + 4) + 8 != fileSize)
        return false;
    // Chunk headers only (8 bytes each), at most 64 of them; no audio is read.
    juce::int64 pos = 12;
    for (int i = 0; i < 64 && pos + 8 <= fileSize; ++i)
    {
        unsigned char c[8];
        if (! in.setPosition (pos) || in.read (c, 8) != 8)
            return false;
        const auto size = u32 (c + 4);
        if (std::memcmp (c, "data", 4) == 0)
            return size > 0 && pos + 8 + size <= fileSize;
        pos += 8 + size + (size & 1); // chunks are padded to an even length
    }
    return false;
}

juce::File copyUnique (const juce::File& src, const juce::File& folder, const juce::String& name,
                       const AbortFn& shouldAbort, CopyResult* result)
{
    // A failure while shouldAbort reads true is an abort (the failed call may be one
    // that IoCanceller ended).
    auto finish = [result, &shouldAbort] (CopyResult r)
    {
        if (r == CopyResult::Failed && abortedNow (shouldAbort))
            r = CopyResult::Aborted;
        if (result != nullptr)
            *result = r;
        return juce::File();
    };
    IoCallEnd callEnd;
    if (name.isEmpty())
        return finish (CopyResult::Failed);
    if (! step ("source", shouldAbort))
        return finish (CopyResult::Aborted);
    if (! src.existsAsFile())
        return finish (CopyResult::Failed);
    bool folderAborted = false;
    if (! ensureFolder (folder, shouldAbort, &folderAborted))
        return finish (folderAborted ? CopyResult::Aborted : CopyResult::Failed);
    const int kMaxIndex = 10000;

    if (testLegacyOverwrite.load())
    {
        int i = 1;
        auto dest = candidate (folder, name, i);
        while (dest.exists() && i < kMaxIndex)
            dest = candidate (folder, name, ++i);
        if (testBeforePublish)
            testBeforePublish (dest);
        if (! src.copyFileTo (dest))
            return finish (CopyResult::Failed);
        if (result != nullptr)
            *result = CopyResult::Copied;
        return dest;
    }

    if (! step ("source size", shouldAbort))
        return finish (CopyResult::Aborted);
    const auto size = src.getSize();
    // Not in the folder before this call: the token differs per call and per process.
    const auto* fixedToken = testPartToken.load();
    const auto part = folder.getChildFile (name + "." + (fixedToken != nullptr ? juce::String (fixedToken) : uniqueToken (12)) + ".part");
    if (! step ("part probe", shouldAbort))
        return finish (CopyResult::Aborted);
    if (part.exists())
        return finish (CopyResult::Failed);
    if (! step ("copy", shouldAbort))
        return finish (CopyResult::Aborted); // nothing written yet
    auto drop = [&part, &finish] (CopyResult r) { removePart (part); return finish (r); };
    switch (copyToNew (src, part, shouldAbort))
    {
        case CopyOutcome::done:        break;
        case CopyOutcome::cancelled:   return finish (CopyResult::Aborted); // the copy removed its partial file
        case CopyOutcome::interrupted: return drop (CopyResult::Aborted);
        case CopyOutcome::failed:      return drop (CopyResult::Failed);
        case CopyOutcome::existed:
            // A file of that name appeared after the probe (or the token collided): it
            // is not ours and stays.
            ++testPartExistsFailures;
            return testLegacyPartDelete.load() ? drop (CopyResult::Failed) : finish (CopyResult::Failed);
    }
    // From here on the .part file exists and is ours: every way out removes it.
    if (! step ("size check", shouldAbort))
        return drop (CopyResult::Aborted);
    if (part.getSize() != size)
        return drop (CopyResult::Failed);
    int lost = 0;
    bool hooked = false;
    for (int i = 1; i < kMaxIndex && lost < 50; ++i)
    {
        const auto dest = candidate (folder, name, i);
        if (! step ("name probe", shouldAbort))
            return drop (CopyResult::Aborted);
        if (dest.exists())
            continue;
        if (testBeforePublish && ! hooked)
            testBeforePublish (dest); // first pick only
        hooked = true;
        if (! step ("rename", shouldAbort))
            return drop (CopyResult::Aborted);
        if (moveNoReplace (part, dest))
        {
            if (result != nullptr)
                *result = CopyResult::Copied;
            return dest;
        }
        if (! step ("name probe", shouldAbort))
            return drop (CopyResult::Aborted);
        if (! dest.exists())
            break; // refused for another reason than a taken name
        ++lost;
        ++testLostRenames;
    }
    return drop (CopyResult::Failed);
}
}
