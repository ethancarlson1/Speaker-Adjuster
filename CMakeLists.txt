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

# Sources contain UTF-8 text (grade reasons, symbols).
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

option(ARE_BUILD_PLUGIN "Build the JUCE plugin (downloads JUCE)" ON)
option(ARE_BUILD_TESTS "Build the C++ unit tests and the roomeq_cli analysis tool" ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# Analysis core: plain C++17, no JUCE. Port of prototype/roomeq (Python).
# pocketfft is the FFT numpy uses (header-only, BSD-3).
# ---------------------------------------------------------------------------
FetchContent_Declare(pocketfft
    GIT_REPOSITORY https://github.com/mreineck/pocketfft.git
    GIT_TAG        c90e55b3d529f8efa40ed01a20de22405f45fc65)   # branch "cpp"
FetchContent_MakeAvailable(pocketfft)

add_library(roomeq_core STATIC
    src/roomeq/averaging.cpp
    src/roomeq/capture.cpp
    src/roomeq/correction.cpp
    src/roomeq/dualfft.cpp
    src/roomeq/fft.cpp
    src/roomeq/filters.cpp
    src/roomeq/grading.cpp
    src/roomeq/iso226.cpp
    src/roomeq/levelmatch.cpp
    src/roomeq/loudness.cpp
    src/roomeq/showtrack.cpp
    src/roomeq/noise.cpp
    src/roomeq/spectrum.cpp
    src/roomeq/sweep.cpp
    src/roomeq/targets.cpp
    src/roomeq/voicing.cpp)
target_include_directories(roomeq_core PUBLIC src)
target_include_directories(roomeq_core SYSTEM PRIVATE ${pocketfft_SOURCE_DIR})
target_compile_definitions(roomeq_core PRIVATE POCKETFFT_NO_MULTITHREADING)
target_compile_features(roomeq_core PUBLIC cxx_std_17)
set_target_properties(roomeq_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(roomeq_core PRIVATE /W4)
else()
    target_compile_options(roomeq_core PRIVATE -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# ---------------------------------------------------------------------------
# Tests and the analysis CLI (used to cross-check the port against Python).
# ---------------------------------------------------------------------------
if(ARE_BUILD_TESTS)
    enable_testing()

    # doctest is header-only: fetch it without running its CMakeLists
    # (SOURCE_SUBDIR points nowhere), so its CMake policies can't break ours.
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  header-only)
    FetchContent_MakeAvailable(doctest)
    add_library(doctest_headers INTERFACE)
    target_include_directories(doctest_headers SYSTEM INTERFACE ${doctest_SOURCE_DIR})

    add_executable(roomeq_tests
        tests/cpp/main.cpp
        tests/cpp/test_averaging.cpp
        tests/cpp/test_capture.cpp
        tests/cpp/test_correction.cpp
        tests/cpp/test_dualfft.cpp
        tests/cpp/test_eq.cpp
        tests/cpp/test_filters.cpp
        tests/cpp/test_grading.cpp
        tests/cpp/test_levelmatch.cpp
        tests/cpp/test_loudness.cpp
        tests/cpp/test_showtrack.cpp
        tests/cpp/test_loudness_stage.cpp
        tests/cpp/test_noise.cpp
        tests/cpp/test_recorder.cpp
        tests/cpp/test_spectrum.cpp
        tests/cpp/test_sweep.cpp
        src/plugin/CaptureRecorder.cpp    # the real-time parts are JUCE-free, so they're tested here
        src/plugin/EqStages.cpp
        src/plugin/LoudnessStage.cpp
        src/plugin/TapRecorder.cpp)
    target_link_libraries(roomeq_tests PRIVATE roomeq_core doctest_headers)
    add_test(NAME roomeq_tests COMMAND roomeq_tests)

    add_executable(roomeq_cli tools/roomeq_cli.cpp)
    target_link_libraries(roomeq_cli PRIVATE roomeq_core)
endif()

if(NOT ARE_BUILD_PLUGIN)
    return()
endif()

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
        src/plugin/CaptureList.cpp
        src/plugin/CaptureRecorder.cpp
        src/plugin/EqStages.cpp
        src/plugin/LoudnessController.cpp
        src/plugin/LoudnessStage.cpp
        src/plugin/MeasurementEngine.cpp
        src/plugin/PluginEditor.cpp
        src/plugin/PluginProcessor.cpp
        src/plugin/ResponseGraph.cpp
        src/plugin/ShowController.cpp
        src/plugin/TapRecorder.cpp)

target_include_directories(AdaptiveRoomEQ PRIVATE src)

target_compile_definitions(AdaptiveRoomEQ
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AdaptiveRoomEQ
    PRIVATE
        roomeq_core
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Headless harness: runs the real processor + engine against a simulated room,
# checks the results and renders the editor to a PNG (see tools/plugin_harness.cpp).
# ---------------------------------------------------------------------------
if(ARE_BUILD_TESTS)
    juce_add_console_app(AdaptiveRoomEQ_Harness PRODUCT_NAME "AdaptiveRoomEQ Harness")
    target_sources(AdaptiveRoomEQ_Harness
        PRIVATE
            tools/plugin_harness.cpp
            src/plugin/CaptureList.cpp
            src/plugin/CaptureRecorder.cpp
            src/plugin/EqStages.cpp
            src/plugin/LoudnessController.cpp
            src/plugin/LoudnessStage.cpp
            src/plugin/MeasurementEngine.cpp
            src/plugin/PluginEditor.cpp
            src/plugin/PluginProcessor.cpp
            src/plugin/ResponseGraph.cpp
            src/plugin/ShowController.cpp
            src/plugin/TapRecorder.cpp)
    target_include_directories(AdaptiveRoomEQ_Harness PRIVATE src)
    target_compile_definitions(AdaptiveRoomEQ_Harness
        PRIVATE
            "JucePlugin_Name=\"${ARE_PRODUCT_NAME}\""
            JucePlugin_Build_Standalone=0
            JUCE_MODAL_LOOPS_PERMITTED=1   # lets the harness pump the message loop
            JUCE_WEB_BROWSER=0
            JUCE_USE_CURL=0)
    target_link_libraries(AdaptiveRoomEQ_Harness
        PRIVATE
            roomeq_core
            juce::juce_audio_utils
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
