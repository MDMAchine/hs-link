# HOT-STEP LINK -- DAW bridge to a local HOT-Step server
# Copyright (C) 2026 A&E Concepts. Licensed under AGPL-3.0-or-later (see LICENSE).
# Open source: self-contained, depends only on JUCE, the vendored MIT headers in third_party and clap-juce-extensions (MIT) (see THIRD_PARTY_NOTICES.md).
cmake_minimum_required(VERSION 3.22)

set(PLUGIN_TARGET "HOTSTEP_LINK")
set(PLUGIN_CODE   "Hslk")

project(${PLUGIN_TARGET} VERSION 0.5.14)
# project(VERSION) is the single version source: juce_add_plugin inherits it as JucePlugin_VersionString;
# the console test app gets the same define below.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# CLAP via clap-juce-extensions (MIT), pinned to the same commit and patch as the MDMAchine suite.
include(FetchContent)
FetchContent_Declare(clap-juce-extensions
    GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions
    GIT_TAG        55525c9858d4b25687be7759a5e0f70eccef218e
    # one combined patch: host param rescan after stateLoad, and stateLoad returns false for a chunk
    # that is not a JUCE state (clap-validator state-invalid-random). Fails configure if it no longer applies.
    PATCH_COMMAND ${CMAKE_COMMAND} "-DPATCH_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/clap-juce-extensions-state.patch" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/apply_git_patch.cmake")
FetchContent_MakeAvailable(clap-juce-extensions)

if(DEFINED ENV{JUCE_DIR})
    set(_hsl_juce_default "$ENV{JUCE_DIR}")
else()
    set(_hsl_juce_default "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE")
endif()
set(JUCE_DIR "${_hsl_juce_default}" CACHE PATH "Path to a JUCE 8 checkout")
if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory(${JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.12 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

juce_add_plugin(${PLUGIN_TARGET}
    COMPANY_NAME             "HOT-Step"
    BUNDLE_ID                com.hotstep.link
    PLUGIN_MANUFACTURER_CODE Hstp
    PLUGIN_CODE              ${PLUGIN_CODE}
    FORMATS                  VST3 Standalone CLAP
    PRODUCT_NAME             "HOT-STEP LINK"
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    COPY_PLUGIN_AFTER_BUILD  FALSE
)

clap_juce_extensions_plugin(TARGET ${PLUGIN_TARGET}
    CLAP_ID       "com.mdmachine.hotstep-link"
    CLAP_FEATURES "audio-effect" "utility")
# clap-juce-extensions builds its own _CLAP target, which JUCE does not give the generated
# VERSIONINFO resource (VST3 and Standalone get it). Add the same .rc so the .clap file shows the version.
if(WIN32)
    target_sources(${PLUGIN_TARGET}_CLAP PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/${PLUGIN_TARGET}_artefacts/JuceLibraryCode/${PLUGIN_TARGET}_resources.rc")
endif()

target_sources(${PLUGIN_TARGET} PRIVATE
    source/PluginProcessor.cpp source/PluginProcessor.h
    source/PluginEditor.cpp    source/PluginEditor.h
    source/CollectionStore.cpp source/CollectionStore.h
    source/ExclusiveFile.cpp   source/ExclusiveFile.h
    source/DeletedFolder.cpp   source/DeletedFolder.h
    source/TempoMeasure.cpp    source/TempoMeasure.h
    source/TakeStretch.cpp     source/TakeStretch.h
    source/TakeEdit.cpp        source/TakeEdit.h
    source/TakePeaks.cpp       source/TakePeaks.h
    source/WavTempoTag.cpp     source/WavTempoTag.h
    source/TakeEditorPanel.cpp source/TakeEditorPanel.h
    source/FactoryPresets.h)

# Only the plugin source folder and the vendored headers (third_party) are on the include path.
target_include_directories(${PLUGIN_TARGET} PRIVATE source third_party)

target_compile_definitions(${PLUGIN_TARGET} PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)

target_link_libraries(${PLUGIN_TARGET} PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Console tests: host sync (R1b) and capture ring + WAV writer (R2a).
# -DHSLINK_TEETH=BAD_SYNC, NO_RESAMPLE, NO_GAIN, NO_AUD_RESAMPLE, BAD_PPQ, NO_XFADE, NO_LOOP_END, NO_REGION, NO_SOURCE_GATE, KEY_IN_STATE, NO_MODEL, NO_SEED_KEY, NO_PEAKS, NO_LYRICS, NO_CAPTION_STATE, NO_POLL_GATE, BAD_PACK, NO_AB, NO_SANITIZE, NO_ROLE, EA_BYPASS_CUT, EA_FLAG_FIRST, EA_BYPASS_PUBLISH, NO_TEMPO_GATE, URL_LEAK, URL_TYPED_SAVE, NO_PHASE, EA_NO_TRY or PRESETS
# builds a deliberately broken copy.
juce_add_console_app(HSLINK_Tests PRODUCT_NAME "HSLINK_Tests")
target_sources(HSLINK_Tests PRIVATE
    tests/HslinkTests.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_Tests PRIVATE source third_party)
target_compile_definitions(HSLINK_Tests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 HSLINK_TEST_HOOKS=1
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
if(HSLINK_TEETH)
    target_compile_definitions(HSLINK_Tests PRIVATE HSLINK_TEETH_${HSLINK_TEETH}=1)
endif()
target_link_libraries(HSLINK_Tests PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Console test for the WAV tempo tag injector (acid + smpl chunks).
juce_add_console_app(HSLINK_WavTagTest PRODUCT_NAME "HSLINK_WavTagTest")
target_sources(HSLINK_WavTagTest PRIVATE tests/WavTempoTagTest.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_WavTagTest PRIVATE source)
target_compile_definitions(HSLINK_WavTagTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_WavTagTest PRIVATE juce::juce_core juce::juce_recommended_config_flags)

# Console test for the CLAP state-load verdict (accept / reject, params unchanged on reject).
juce_add_console_app(HSLINK_ClapVerdictTest PRODUCT_NAME "HSLINK_ClapVerdictTest")
target_sources(HSLINK_ClapVerdictTest PRIVATE
    tests/ClapVerdictTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_ClapVerdictTest PRIVATE source third_party)
target_compile_definitions(HSLINK_ClapVerdictTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_ClapVerdictTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Console test for prepareToPlay at a new rate on a live processor (44.1 -> 96 -> 48 kHz).
juce_add_console_app(HSLINK_SampleRateChangeTest PRODUCT_NAME "HSLINK_SampleRateChangeTest")
target_sources(HSLINK_SampleRateChangeTest PRIVATE
    tests/SampleRateChangeTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_SampleRateChangeTest PRIVATE source third_party)
target_compile_definitions(HSLINK_SampleRateChangeTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_SampleRateChangeTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# The take editor's export path end to end (requestTakeEdit, renderEditFile, tags, failures).
juce_add_console_app(HSLINK_EditorExportTest PRODUCT_NAME "HSLINK_EditorExportTest")
target_sources(HSLINK_EditorExportTest PRIVATE
    tests/EditorExportTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_EditorExportTest PRIVATE source third_party)
target_compile_definitions(HSLINK_EditorExportTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_EditorExportTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# The take editor panel Apply flow with callback fakes (options sent, reports, no host tempo).
juce_add_console_app(HSLINK_TakeEditorFlowTest PRODUCT_NAME "HSLINK_TakeEditorFlowTest")
target_sources(HSLINK_TakeEditorFlowTest PRIVATE
    tests/TakeEditorFlowTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_TakeEditorFlowTest PRIVATE source third_party)
target_compile_definitions(HSLINK_TakeEditorFlowTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_TakeEditorFlowTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Console test for host bypass while a job is still sending its source upload.
juce_add_console_app(HSLINK_BypassUploadTest PRODUCT_NAME "HSLINK_BypassUploadTest")
target_sources(HSLINK_BypassUploadTest PRIVATE
    tests/BypassUploadTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_BypassUploadTest PRIVATE source third_party)
target_compile_definitions(HSLINK_BypassUploadTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_BypassUploadTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Full host close span (editor delete to last plugin-owned thread exit) with an upload in flight.
juce_add_console_app(HSLINK_CloseSpanTest PRODUCT_NAME "HSLINK_CloseSpanTest")
target_sources(HSLINK_CloseSpanTest PRIVATE
    tests/CloseSpanTest.cpp source/PluginProcessor.cpp source/PluginEditor.cpp source/CollectionStore.cpp
    source/ExclusiveFile.cpp source/DeletedFolder.cpp source/TempoMeasure.cpp source/TakeStretch.cpp
    source/TakeEdit.cpp source/TakePeaks.cpp source/TakeEditorPanel.cpp source/WavTempoTag.cpp)
target_include_directories(HSLINK_CloseSpanTest PRIVATE source third_party)
target_compile_options(HSLINK_CloseSpanTest PRIVATE /Zi)
target_link_options(HSLINK_CloseSpanTest PRIVATE /DEBUG)
target_compile_definitions(HSLINK_CloseSpanTest PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JucePlugin_Name="HOT-STEP LINK" JucePlugin_VersionString="${PROJECT_VERSION}" NOMINMAX=1 WIN32_LEAN_AND_MEAN=1)
target_link_libraries(HSLINK_CloseSpanTest PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_dsp
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
