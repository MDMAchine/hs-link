// Test exes only: no test may read or write the real user settings file
// (AppData/Roaming/HOT-Step/LINK.settings). Declare one of these as the first line of
// main, before any processor or editor exists. When HSLINK_SETTINGS_FILE is not already
// set by the caller it points the plugin at a scratch file in a per-process temp folder,
// which is removed again when main ends. It prints the path in use (never the content).
#pragma once

#include "PluginProcessor.h"

#include <cstdio>
#include <cstdlib>
#include <process.h>

struct HslinkTestSettingsRedirect
{
    explicit HslinkTestSettingsRedirect (const char* tag)
    {
        if (juce::SystemStats::getEnvironmentVariable ("HSLINK_SETTINGS_FILE", {}).isEmpty())
        {
            dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile (juce::String ("hslink_") + tag + "_settings_" + juce::String ((int) _getpid()));
            _putenv_s ("HSLINK_SETTINGS_FILE", dir.getChildFile ("LINK.settings").getFullPathName().toRawUTF8());
        }
        std::printf ("settings file in use: %s%c", HotStepLinkProcessor::keyStoreFile().getFullPathName().toRawUTF8(), 10);
    }

    ~HslinkTestSettingsRedirect()
    {
        if (dir != juce::File())
            dir.deleteRecursively(); // only the folder this object made up
    }

    juce::File dir;
};
