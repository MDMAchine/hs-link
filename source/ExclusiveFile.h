// HOT-STEP LINK -- ExclusiveFile.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <mutex>

// Publishing a file under a name without ever replacing a file that is already there.
// juce::File::copyFileTo and juce::File::moveFileTo both delete an existing target
// first (juce_File.cpp, copyFileTo / moveFileTo), so neither can be the last step of
// a copy that two writers may aim at the same name.
namespace ExclusiveFile
{
    // Renames from to to. Fails (false) when to already exists; to is then left
    // untouched and from stays where it was. Both must be on the same volume.
    bool moveNoReplace (const juce::File& from, const juce::File& to);

    // Creates f as an empty file. Fails (false) when anything is already at that name;
    // what is there is then left untouched. One native call (Windows: CreateFileW with
    // CREATE_NEW; elsewhere open with O_CREAT | O_EXCL), so there is no gap between a
    // check and the create. True = this call made the file; nothing was at that name
    // before. The handle is closed again here, so a caller that then opens the file by
    // name (as the take writers do) leaves a short window in which another process
    // could swap it: the create rules out a file that was there first, not a later swap.
    bool createNew (const juce::File& f);

    // Short lowercase hex token, different for every call in this process and mixed
    // with the process id and a random value. chars is clamped to 4..16.
    juce::String uniqueToken (int chars = 6);

    // The i-th candidate for name in folder: name (i <= 1), then name_2, name_3 ...
    // before the extension.
    juce::File candidate (const juce::File& folder, const juce::String& name, int i);

    // Copied = the file is published. Failed = a file call failed. Aborted = shouldAbort
    // read true: the call stopped before its next blocking step and published nothing.
    enum class CopyResult { Copied, Failed, Aborted };
    // Read before every blocking step (file call) of ensureFolder and copyUnique. Empty =
    // never abort. Must be cheap and must not block; it runs on the copying thread.
    using AbortFn = std::function<bool()>;

    // Copies src into folder under name, or name_2, name_3 ... when the name is taken.
    // The bytes go to "<name>.<token>.part" in folder first; once that file has the
    // size of src it is renamed with moveNoReplace. If another writer took the name in
    // between, the next free name is tried (at most 50 lost renames). Never deletes or
    // overwrites a file this call did not create; on failure or abort only its own
    // .part file is removed (one native delete call), and not even that when the copy
    // failed because a file of that name was already there. Returns the published
    // file, or {}.
    // shouldAbort is read before the source probe, every folder step, the source size
    // read, the .part probe, the copy, the size check, every name probe and every
    // rename, and during the byte copy once per chunk (Windows: the progress routine
    // of CopyFileExW, about every 1 MB; elsewhere every 256 KB). A copy cancelled that
    // way removes its partial file and the call reports Aborted. Any other file call
    // that is already inside the OS is not interrupted by shouldAbort (see IoCanceller).
    // result, when given, tells an abort from a failure; a failure seen while
    // shouldAbort reads true is reported as Aborted.
    juce::File copyUnique (const juce::File& src, const juce::File& folder, const juce::String& name,
                           const AbortFn& shouldAbort = {}, CopyResult* result = nullptr);

    // Creates folder and its parents. It also succeeds when another writer creates the
    // same folder, or a parent, at the same moment. A try is repeated (up to 8 tries)
    // only when a create failed because its parent was not there (it vanished, or was
    // not made yet); any other create error ends the call at once. One probe of folder
    // per try; when it is missing, the nearest existing parent is looked up and the
    // missing levels are made top down, one native call each. shouldAbort is read before
    // every probe and every create; once it reads true no further step or try is made,
    // false is returned and *aborted (when given) is set.
    bool ensureFolder (const juce::File& folder, const AbortFn& shouldAbort = {}, bool* aborted = nullptr);

    // Lets another thread cancel the file call that a copying thread has in flight
    // inside ensureFolder or copyUnique. The copying thread calls attachCurrentThread()
    // once (it opens a handle to itself) and detach() before it ends. Between a step's
    // abort check and the next one the thread is "in a call"; cancelInFlight(), from any
    // other thread, then calls CancelSynchronousIo on the handle, which makes a
    // synchronous file call of that thread return with ERROR_OPERATION_ABORTED. It does
    // nothing when the thread is not in such a call, is detached, or is deleting its own
    // .part file and has been for less than cleanupGraceMs. The in-call flag and the
    // handle are read and changed under one small mutex that is never held across a
    // file call, so a cancel never reaches a file call made outside these two functions
    // and never a thread that has detached. Windows only; elsewhere every call is a no-op.
    // One call cancels only what is in flight at that instant: the caller repeats it.
    // Use only together with a shouldAbort that already reads true, so the cancelled
    // call is followed by an abort and not by another step.
    class IoCanceller
    {
    public:
        IoCanceller() = default;
        ~IoCanceller();
        IoCanceller (const IoCanceller&) = delete;
        IoCanceller& operator= (const IoCanceller&) = delete;
        void attachCurrentThread();
        void detach();           // on the attached thread
        bool cancelInFlight();   // true = CancelSynchronousIo was called
        bool isAttached();
        static constexpr int cleanupGraceMs = 1000;
        // Used by ensureFolder and copyUnique only.
        void setInCall (bool nowInCall, bool isCleanup);
    private:
        std::mutex lock;
        void* thread = nullptr;  // Windows HANDLE of the attached thread
        bool inCall = false;
        bool cleanup = false;
        juce::uint32 cleanupSince = 0;
    };
    // ioCancelEnabled false = cancelInFlight does nothing (teeth). testIoCancelCalls:
    // cancelInFlight calls that reached CancelSynchronousIo. testIoCancelHits: those for
    // which it reported success (a synchronous call was in flight and was marked
    // cancelled).
    extern std::atomic<bool> ioCancelEnabled;
    extern std::atomic<int> testIoCancelCalls;
    extern std::atomic<int> testIoCancelHits;

    // True for the partial files copyUnique writes ("*.part").
    bool isPartial (const juce::File& f);

    // True when f is a complete RIFF/WAVE file by its header: at least 20 bytes, "RIFF"
    // at 0, "WAVE" at 8, the RIFF size field (bytes 4 to 7, little-endian) plus 8 equal
    // to the file size, and a "data" chunk of more than 0 bytes that ends inside the
    // file. A file cut off anywhere, and a streamed header (size field 0 or
    // 0xFFFFFFFF), is false. Reads the 12 header bytes and up to 64 chunk headers of 8
    // bytes; no audio data, no audio format code. The name of f is not looked at.
    bool isCompleteWav (const juce::File& f);

    // Who may touch a temp session folder: only the holder of "<folder>/owner.lock".
    // The hold is exclusive between threads, plugin instances and processes, and the
    // operating system drops it when the holding process ends, however it ends.
    // Staleness rule: a lock is stale exactly when it can be acquired. There is no
    // time, pid or heartbeat judgement. An owner.lock FILE left by a dead process is
    // only a file: anyone can open and lock it, so it never blocks a rescue.
    // Windows: CreateFileW (read + write access, FILE_SHARE_READ only, OPEN_ALWAYS),
    // handle kept open; a second such open fails with a sharing violation. While it is
    // held the lock file cannot be deleted, so release() comes before the folder is
    // removed. Other files in the folder are not affected. Elsewhere: open + flock.
    class SessionLock
    {
    public:
        // Held = this object holds the lock now. Busy = someone alive holds it.
        // Failed = the lock file could not be created or opened at all.
        // NoLockFile = tryAcquireExisting only: the folder has no owner.lock; nothing was
        // created and nothing is held.
        enum class Result { Held, Busy, Failed, NoLockFile };
        SessionLock() = default;
        ~SessionLock() { release(); }
        SessionLock (SessionLock&& o) noexcept { *this = std::move (o); }
        SessionLock& operator= (SessionLock&& o) noexcept;
        SessionLock (const SessionLock&) = delete;
        SessionLock& operator= (const SessionLock&) = delete;
        // One create-or-open call, never waits. folder must exist. A lock that is
        // already held by this object is released first.
        // Owner mode: only the instance that owns folder calls this. It is the only call
        // that creates owner.lock, so the presence of that file in a folder means "made
        // by a build that locks".
        Result tryAcquire (const juce::File& folder);
        // Rescue and cleanup mode, for a folder of someone else: opens owner.lock only
        // when it is already there (Windows OPEN_EXISTING, elsewhere open without
        // O_CREAT). A folder without the file gives NoLockFile and stays byte for byte as
        // it was. Otherwise as tryAcquire.
        Result tryAcquireExisting (const juce::File& folder);
        bool isHeld() const { return held; }
        void release();
        static const char* fileName() { return "owner.lock"; }
    private:
        Result acquire (const juce::File& folder, bool create);
        void* handle = nullptr; // Windows HANDLE
        int fd = -1;            // elsewhere
        bool held = false;
    };
    // Test hooks. sessionLockEnabled false = tryAcquire always reports Held and locks
    // nothing (rescue and cleanup do not care who is alive). testSessionLockFails
    // true = the owner-mode tryAcquire reports Failed (a lock file that cannot be
    // created); tryAcquireExisting is not affected. testOwnerAcquires counts owner-mode
    // calls. testNoLegacyGuard true (teeth only) = tryAcquireExisting creates the lock
    // file like tryAcquire, so a folder without owner.lock counts as a dead session, and
    // the lockless-instance registry of DawWorker is ignored (the behaviour of 3cf372b).
    extern std::atomic<bool> sessionLockEnabled;
    extern std::atomic<bool> testSessionLockFails;
    extern std::atomic<bool> testNoLegacyGuard;
    extern std::atomic<int> testOwnerAcquires;

    // Test hooks. Defaults are the shipped behaviour.
    extern std::atomic<bool> testLegacyOverwrite;  // true = pick a free name, then juce copyFileTo
    extern std::atomic<int> testLostRenames;       // renames refused because the name was taken
    // Called on the copying thread after the name was picked and before the file is
    // published under it. Set and cleared only while no copy is running.
    extern std::function<void (const juce::File& picked)> testBeforePublish;
    // true = shouldAbort is never read (the behaviour before the abort check; teeth only).
    extern std::atomic<bool> testIgnoreAbort;
    // Called on the copying thread before each blocking step of ensureFolder and
    // copyUnique, after the abort check, with the step's name: "source", "folder probe",
    // "parent probe", "create", "created probe", "source size", "part probe", "copy",
    // "size check", "name probe", "rename". Null in the shipped plugin. A test may delay
    // or hold the thread in it. It runs "in a call" for IoCanceller, so a synchronous
    // read it blocks in is ended by cancelInFlight.
    extern std::atomic<void (*) (const char* step)> testStepHook;

    // The byte copy of copyUnique. copyCancelEnabled false (teeth) = once the copy has
    // started neither shouldAbort nor the hook's answer stops it (the behaviour of the
    // single CopyFileW call). testCopyProgressHook, when set, is called on the copying
    // thread from the copy's progress routine before shouldAbort is read: on Windows
    // once before the first byte (done == 0) and then after every chunk of about 1 MB,
    // so at least twice for any file that copies completely, whatever its size
    // (measured: 2 calls for 4 KB, 64 KB and 1 MB, 3 for 2 MB, 9 for 8 MB, 65 for 64
    // MB); elsewhere before every 256 KB chunk. It may block. A non-zero return cancels
    // the copy like shouldAbort does. Counters: testCopiesStarted = byte copies
    // started, testCopiesCancelled = byte copies cancelled (by shouldAbort, by the hook
    // or by IoCanceller), testCopyProgressCalls = progress routine calls.
    extern std::atomic<bool> copyCancelEnabled;
    extern std::atomic<int (*) (juce::int64 done, juce::int64 total)> testCopyProgressHook;
    extern std::atomic<int> testCopiesStarted;
    extern std::atomic<int> testCopiesCancelled;
    extern std::atomic<int> testCopyProgressCalls;

    // Cleanup of the .part file. testCleanupCalls counts the file-system calls made to
    // remove a .part file on failure or abort (1 per copyUnique call that removes one;
    // 0 when the copy was cancelled in its progress routine, because the copy removed
    // its own partial file, and 0 when the copy failed on an existing name).
    // testLegacyPartCleanup true (teeth) = the three calls juce::File::deleteFile makes
    // (exists, isDirectory, delete), each counted.
    extern std::atomic<int> testCleanupCalls;
    extern std::atomic<bool> testLegacyPartCleanup;

    // The .part name. testPartToken, when not null, replaces the random token of every
    // copyUnique call ("<name>.<token>.part"); the pointer must stay valid while set.
    // copyUnique probes that name before the copy ("part probe") and fails there when it
    // exists, so a test of the copy's own fail-if-exists path creates the file in
    // testStepHook at the "copy" step. testPartExistsFailures counts copies that failed
    // because the name existed. testLegacyPartDelete true (teeth) = that failure deletes
    // the file like any other failure (the behaviour before).
    extern std::atomic<const char*> testPartToken;
    extern std::atomic<int> testPartExistsFailures;
    extern std::atomic<bool> testLegacyPartDelete;

    // ensureFolder. testCreateFault: 0 = none; 1 = creates fail with a hard error (as
    // access denied or a dead share would); 2 = they fail with "parent missing". A
    // faulted create makes no file call. testCreateFaultsLeft: how many creates the
    // fault applies to; -1 = all while testCreateFault is set; a positive number counts
    // down to 0, after which creates are real again. testCreateCalls counts create
    // attempts (faulted or real), testFolderTries the tries (loop passes) of all
    // ensureFolder calls. testLegacyFolderRetry true (teeth) = 8 tries on any failure.
    extern std::atomic<int> testCreateFault;
    extern std::atomic<int> testCreateFaultsLeft;
    extern std::atomic<int> testCreateCalls;
    extern std::atomic<int> testFolderTries;
    extern std::atomic<bool> testLegacyFolderRetry;

    // Which thread makes a blocking step. A thread carries a tag (default tagOther):
    // the take saver thread sets tagSaver for its whole run, registerTake and the
    // DawWorker thread set tagWorker, and the processor's destructor sets tagClosing on
    // the thread that runs it. testStepsByTag[tag] counts every step that passed its
    // abort check, in the shipped plugin too (one atomic increment per file step).
    // threadTag() can be read inside testStepHook. Tests reset the counters themselves.
    enum ThreadTag { tagOther = 0, tagWorker = 1, tagSaver = 2, tagClosing = 3, tagCount = 4 };
    int threadTag();
    struct ScopedThreadTag
    {
        explicit ScopedThreadTag (int tag);
        ~ScopedThreadTag();
        int previous;
    };
    extern std::atomic<int> testStepsByTag[tagCount];
}
