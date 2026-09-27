cmake_minimum_required(VERSION 3.22)

# These must be set before project(). They are ignored on non-Apple platforms.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures (universal by default)")

# Static MSVC runtime so the plugin doesn't depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(AdaptiveRoomEQ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Plugin identity.
# PLACEHOLDERS: set these for real before the plugin is used in saved sessions.
# Hosts key saved state on the codes, so changing them later orphans old sessions.
# ---------------------------------------------------------------------------
set(ARE_PRODUCT_NAME      "Adaptive Room EQ")
set(ARE_COMPANY_NAME      "Adaptive Room EQ Dev")
set(ARE_BUNDLE_ID         "com.adaptiveroomeq.plugin")
set(ARE_MANUFACTURER_CODE "Ardv")   # 4 chars, at least one upper-case
set(ARE_PLUGIN_CODE       "Areq")   # 4 chars, exactly one upper-case

# ---------------------------------------------------------------------------
# JUCE (fetched at configure time). To build against a local checkout instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
# ---------------------------------------------------------------------------
set(ARE_JUCE_TAG "8.0.15" CACHE STRING "JUCE git tag to fetch")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${ARE_JUCE_TAG}
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

# VST3 + Standalone everywhere; AU on macOS only.
set(ARE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND ARE_FORMATS AU)
endif()

juce_add_plugin(AdaptiveRoomEQ
    PRODUCT_NAME                  "${ARE_PRODUCT_NAME}"
    COMPANY_NAME                  "${ARE_COMPANY_NAME}"
    BUNDLE_ID                     "${ARE_BUNDLE_ID}"
    PLUGIN_MANUFACTURER_CODE      ${ARE_MANUFACTURER_CODE}
    PLUGIN_CODE                   ${ARE_PLUGIN_CODE}
    DESCRIPTION                   "Measured room correction with level-tracking loudness compensation"
    FORMATS                       ${ARE_FORMATS}
    IS_SYNTH                      FALSE
    NEEDS_MIDI_INPUT              FALSE
    NEEDS_MIDI_OUTPUT             FALSE
    IS_MIDI_EFFECT                FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS   FALSE
    COPY_PLUGIN_AFTER_BUILD       FALSE
    VST3_CATEGORIES               Fx EQ Analyzer
    AU_MAIN_TYPE                  kAudioUnitType_Effect
    # The standalone app records the measurement mic directly.
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT    "Adaptive Room EQ records a measurement microphone to measure the room.")

target_sources(AdaptiveRoomEQ
    PRIVATE
        src/plugin/PluginProcessor.cpp
        src/plugin/PluginEditor.cpp)

target_include_directories(AdaptiveRoomEQ PRIVATE src)

target_compile_definitions(AdaptiveRoomEQ
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AdaptiveRoomEQ
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
