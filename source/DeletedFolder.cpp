// HOT-STEP LINK -- DeletedFolder.cpp
// Copyright (C) 2026 A&E Concepts. SPDX-License-Identifier: AGPL-3.0-or-later
#include "DeletedFolder.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #include <windows.h>
#else
 #include <stdio.h>
 #include <errno.h>
 #include <sys/stat.h>
#endif

namespace DeletedFolder
{
std::atomic<bool> testFailRename { false };
std::atomic<int> renameCalls { 0 };
std::atomic<bool> testSourceUnknown { false };

juce::String folderName() { return "Deleted"; }

juce::File folderFor (const juce::File& file)
{
    return file.getParentDirectory().getChildFile (folderName());
}

Source classifySource (bool attributesRead, bool isDirectory, bool notFoundError, bool goneBelowAncestor)
{
    if (attributesRead)
        return isDirectory ? Source::missing : Source::file;
    return (notFoundError && goneBelowAncestor) ? Source::missing : Source::unknown;
}

Source classifyChain (const std::vector<Probe>& chain)
{
    if (chain.empty())
        return Source::unknown;
    const auto first = chain[0];
    if (first == Probe::file)
        return classifySource (true, false, false, false);
    if (first == Probe::directory || first == Probe::link)
        return classifySource (true, true, false, false);
    if (first != Probe::notFound)
        return classifySource (false, false, false, false);
    // The file reads not found. Walk up to the nearest ancestor whose attributes
    // were read; only a plain directory or a plain file there can confirm it, and
    // only when the entry right below it on the path reads not found as well.
    bool gone = false;
    for (size_t i = 1; i < chain.size(); ++i)
    {
        const auto a = chain[i];
        if (a == Probe::notFound || a == Probe::unreadable)
            continue;
        gone = (a == Probe::directory || a == Probe::file) && chain[i - 1] == Probe::notFound;
        break;
    }
    return classifySource (false, false, true, gone);
}

Source sourceStateWith (const juce::File& file, const std::function<Probe (const juce::File&)>& read)
{
    std::vector<Probe> chain;
    chain.push_back (read (file));
    if (chain[0] == Probe::notFound)
    {
        auto cur = file;
        for (int depth = 0; depth < maxWalkDepth; ++depth)
        {
            const auto parent = cur.getParentDirectory();
            if (parent == cur)
                break; // the root was the last one asked
            const auto p = read (parent);
            chain.push_back (p);
            if (p != Probe::notFound && p != Probe::unreadable)
                break;
            cur = parent;
        }
    }
    return classifyChain (chain);
}

// Not juce::File::existsAsFile: that answers false for every failure, so a drive that
// is not there would read as "the file is gone".
Probe probePath (const juce::File& path)
{
   #if JUCE_WINDOWS
    const DWORD attr = GetFileAttributesW (path.getFullPathName().toWideCharPointer());
    if (attr != INVALID_FILE_ATTRIBUTES)
    {
        if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return Probe::file;
        return (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ? Probe::link : Probe::directory;
    }
    const DWORD err = GetLastError();
    return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? Probe::notFound : Probe::unreadable;
   #else
    // Same intent with lstat, stat and errno. Not compiled or run on the Windows build machine.
    struct stat st;
    if (::lstat (path.getFullPathName().toRawUTF8(), &st) == 0)
    {
        if (S_ISLNK (st.st_mode))
        {
            struct stat target;
            if (::stat (path.getFullPathName().toRawUTF8(), &target) == 0 && ! S_ISDIR (target.st_mode))
                return Probe::file;
            return Probe::link; // a link to a directory, or a link whose target cannot be read
        }
        return S_ISDIR (st.st_mode) ? Probe::directory : Probe::file;
    }
    return errno == ENOENT ? Probe::notFound : Probe::unreadable;
   #endif
}

Source sourceState (const juce::File& file)
{
    if (testSourceUnknown.load())
        return Source::unknown;
    return sourceStateWith (file, [] (const juce::File& f) { return probePath (f); });
}

juce::String nameForAttempt (const juce::String& fileName, int attempt)
{
    if (attempt <= 1)
        return fileName;
    const auto dot = fileName.lastIndexOfChar ('.');
    const auto stem = dot > 0 ? fileName.substring (0, dot) : fileName;
    const auto ext = dot > 0 ? fileName.substring (dot) : juce::String();
    return stem + " (" + juce::String (attempt) + ")" + ext;
}

juce::String pickFreeName (const juce::String& fileName,
                           const std::function<bool (const juce::String&)>& exists, int maxTries)
{
    for (int n = 1; n <= maxTries; ++n)
    {
        const auto name = nameForAttempt (fileName, n);
        if (! exists (name))
            return name;
    }
    return {};
}

// A rename that fails when the target exists and never copies. No JUCE call:
// juce::File::moveFileTo deletes an existing target first.
static bool renameNoReplace (const juce::File& from, const juce::File& to)
{
    if (testFailRename.load())
        return false;
    ++renameCalls;
   #if JUCE_WINDOWS
    // Flags 0: no MOVEFILE_COPY_ALLOWED (fails across volumes), no MOVEFILE_REPLACE_EXISTING.
    return MoveFileExW (from.getFullPathName().toWideCharPointer(),
                        to.getFullPathName().toWideCharPointer(), 0) != 0;
   #elif JUCE_MAC
    return ::renamex_np (from.getFullPathName().toRawUTF8(), to.getFullPathName().toRawUTF8(), RENAME_EXCL) == 0;
   #else
    // ::rename replaces an existing target, so the target is checked first. The check
    // and the rename are two steps here.
    if (to.exists())
        return false;
    return ::rename (from.getFullPathName().toRawUTF8(), to.getFullPathName().toRawUTF8()) == 0;
   #endif
}

bool renameBack (const juce::File& from, const juce::File& to)
{
    return renameNoReplace (from, to);
}
} // namespace DeletedFolder

MoveToDeleted moveToDeletedFolder (const juce::File& file, juce::File* whereItWent)
{
    using namespace DeletedFolder;
    // alreadyGone only when the file is positively not there; a path that cannot be
    // read (drive or share gone, no access) is kept, so the caller says so.
    const auto state = sourceState (file);
    if (state != Source::file)
        return state == Source::missing ? MoveToDeleted::alreadyGone : MoveToDeleted::kept;
    const auto folder = folderFor (file);
    if (! folder.isDirectory())
    {
        if (folder.exists() || ! folder.createDirectory().wasOk() || ! folder.isDirectory())
            return MoveToDeleted::kept; // a file has that name, or the folder cannot be made
    }
    const auto fileName = file.getFileName();
    for (int n = 1; n <= maxNameTries; ++n)
    {
        const auto dest = folder.getChildFile (nameForAttempt (fileName, n));
        if (dest.exists())
            continue; // never overwrite: next name
        if (renameNoReplace (file, dest))
        {
            if (whereItWent != nullptr)
                *whereItWent = dest;
            return MoveToDeleted::moved;
        }
        const auto after = sourceState (file);
        if (after == Source::missing)
            return MoveToDeleted::alreadyGone; // someone else took it; nothing to keep
        if (after == Source::unknown)
            return MoveToDeleted::kept; // cannot tell: say it was kept, never drop it silently
        if (! dest.exists())
            return MoveToDeleted::kept; // a real failure (file in use, no access): file untouched
        // The name was taken between the check and the rename: next name.
    }
    return MoveToDeleted::kept;
}
