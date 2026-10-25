// HOT-STEP LINK -- DeletedFolder.h
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
//
// The one way LINK "deletes" a user file: a plain rename into a folder named
// "Deleted" in the same directory as the file (so always the same volume). No shell
// call, no OS trash, no copy, no overwrite, and nothing is ever deleted here. LINK
// never empties a Deleted folder; the user does that by hand. The one Deleted folder
// that does not last is the one of an unsaved take: it sits inside the temp session
// folder, and that whole folder is removed at plugin close with the rest of the
// session temp files.
#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <vector>

// moved:       the file is now inside the Deleted folder.
// kept:        the file is still at its original path, untouched.
// alreadyGone: the file is positively known not to be at its path (at the start, or
//              after a failed rename). See DeletedFolder::classifySource for the rule:
//              a drive or folder that cannot be reached is kept, never alreadyGone.
enum class MoveToDeleted { moved, kept, alreadyGone };

// Any thread except audio. whereItWent is set only for moved.
MoveToDeleted moveToDeletedFolder (const juce::File& file, juce::File* whereItWent = nullptr);

namespace DeletedFolder
{
    // "Deleted"
    juce::String folderName();
    // file.getParentDirectory()/Deleted
    juce::File folderFor (const juce::File& file);

    // What is known about the path of a file that is to be moved.
    //   file:    a file is there.
    //   missing: positively no file there.
    //   unknown: cannot tell (drive not ready, share gone, no access, no folder above
    //            the file can be read). The caller treats this as kept.
    enum class Source { file, missing, unknown };
    // What one read of the attributes of one path said.
    //   file:       read, not a directory.
    //   directory:  read, a plain directory.
    //   link:       read, a directory that is a reparse point (junction, symlink, mount
    //               point, cloud folder). What is below it may be on a target that is
    //               not there, so it never confirms that something is missing.
    //   notFound:   Windows ERROR_FILE_NOT_FOUND or ERROR_PATH_NOT_FOUND (elsewhere ENOENT).
    //   unreadable: every other error, among them ERROR_NOT_READY, ERROR_BAD_NETPATH,
    //               ERROR_BAD_NET_NAME and ERROR_ACCESS_DENIED.
    enum class Probe { file, directory, link, notFound, unreadable };
    // Pure, the last step of the rule:
    //   attributes read, not a directory                     -> file
    //   attributes read, a directory                         -> missing (not a file to move)
    //   read failed with not found and goneBelowAncestor     -> missing
    //   anything else                                        -> unknown
    // goneBelowAncestor: see classifyChain.
    Source classifySource (bool attributesRead, bool isDirectory, bool notFoundError, bool goneBelowAncestor);
    // Pure, the whole rule. chain[0] is the read of the file, chain[1] of its parent
    // folder, and so on upward; the walk stops at the first entry that was read (file,
    // directory or link) or at the root.
    //   chain[0] file                    -> file
    //   chain[0] directory or link       -> missing (not a file to move)
    //   chain[0] unreadable              -> unknown
    //   chain[0] notFound: the nearest ancestor that was read decides.
    //     a plain directory, and the entry right below it on the path reads notFound
    //                                    -> missing (the folder of the file is gone)
    //     a plain file, same condition   -> missing (a file holds nothing, so there is
    //                                       no folder and no file below it)
    //     a link                         -> unknown
    //     the entry below it unreadable  -> unknown
    //     no ancestor was read up to and including the root (drive not ready, share
    //     gone, no access, or the root itself reads notFound) -> unknown
    Source classifyChain (const std::vector<Probe>& chain);
    // The walk, with the reader handed in (tests: a table instead of a dead drive).
    constexpr int maxWalkDepth = 256;
    Source sourceStateWith (const juce::File& file, const std::function<Probe (const juce::File&)>& read);
    // One read from the file system (Windows: GetFileAttributesW).
    Probe probePath (const juce::File& path);
    // sourceStateWith (file, probePath).
    Source sourceState (const juce::File& file);
    // A plain rename with the rules of the move (no copy, never replaces), used to put
    // a file back from the Deleted folder to the path it came from. false: nothing changed.
    bool renameBack (const juce::File& from, const juce::File& to);

    // The names tried inside the Deleted folder, at most this many, then kept.
    constexpr int maxNameTries = 100;
    // Pure. attempt 1 = fileName itself; attempt n >= 2 = "stem (n).ext".
    juce::String nameForAttempt (const juce::String& fileName, int attempt);
    // Pure. The first of the maxTries names for which exists() is false; empty = none.
    juce::String pickFreeName (const juce::String& fileName,
                               const std::function<bool (const juce::String&)>& exists,
                               int maxTries = maxNameTries);

    // Test seam, false in the plugin. true = the rename step is not made and reports
    // failure, so moveToDeletedFolder returns kept with the file where it was.
    extern std::atomic<bool> testFailRename;
    // Times the real rename call was made (tests).
    extern std::atomic<int> renameCalls;
    // Test seam, false in the plugin. true = sourceState answers unknown for every
    // path (a drive that cannot be reached), so moveToDeletedFolder returns kept.
    extern std::atomic<bool> testSourceUnknown;
}
