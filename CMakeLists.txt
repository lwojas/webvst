cmake_minimum_required(VERSION 3.22)
project(WebVST VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# webdsp is consumed as a header-only engine (native/src/engine.h). Nothing in webdsp's own build is used.
# It is pinned as a git submodule (third_party/webdsp): bumping webdsp is an explicit commit here. To build against
# a working copy instead (e.g. while developing webdsp), pass -DWEBVST_WEBDSP_DIR=../webdsp.
set(WEBVST_WEBDSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webdsp" CACHE PATH "Path to a webdsp checkout")
if(NOT EXISTS "${WEBVST_WEBDSP_DIR}/native/src/engine.h")
  message(FATAL_ERROR "webdsp not found at ${WEBVST_WEBDSP_DIR}. Run: git submodule update --init   (or pass -DWEBVST_WEBDSP_DIR=<path to a webdsp checkout>)")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.4
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(WebVST
  PRODUCT_NAME "WebVST"
  COMPANY_NAME "lwojas"
  PLUGIN_MANUFACTURER_CODE Lwjs
  PLUGIN_CODE Wvst
  FORMATS VST3
  IS_SYNTH TRUE
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  NEEDS_WEB_BROWSER TRUE
  COPY_PLUGIN_AFTER_BUILD FALSE)

# The page shim is a real .js file, embedded as BinaryData::shim_js.
juce_add_binary_data(WebVstData SOURCES src/shim.js)

target_sources(WebVST PRIVATE
  src/EngineHost.cpp
  src/SessionState.cpp
  src/HostFiles.cpp
  src/PluginProcessor.cpp
  src/PluginEditor.cpp)

target_include_directories(WebVST SYSTEM PRIVATE "${WEBVST_WEBDSP_DIR}/native/src")

# The application this plugin hosts, stamped on saved state; empty accepts any (see SessionState).
set(WEBVST_APP_ID "" CACHE STRING "Application id stamped on saved state (empty: any)")

target_compile_definitions(WebVST PUBLIC
  WEBVST_APP_ID="${WEBVST_APP_ID}"
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(WebVST PRIVATE
  WebVstData
  juce::juce_audio_utils
  juce::juce_gui_extra
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Headless test of EngineHost (command queue, dispatcher, events, allocation behaviour): -DWEBVST_BUILD_TESTS=ON
option(WEBVST_BUILD_TESTS "Build the EngineHost test executable" OFF)
if(WEBVST_BUILD_TESTS)
  juce_add_console_app(WebVstHostTest PRODUCT_NAME "WebVstHostTest")
  target_sources(WebVstHostTest PRIVATE test/engine_host_test.cpp src/EngineHost.cpp src/SessionState.cpp src/HostFiles.cpp)
  target_include_directories(WebVstHostTest PRIVATE src)
  target_compile_definitions(WebVstHostTest PRIVATE WEBVST_WEBDSP_DIR_STR="${WEBVST_WEBDSP_DIR}" WEBVST_SHIM_PATH_STR="${CMAKE_CURRENT_SOURCE_DIR}/src/shim.js")
  target_include_directories(WebVstHostTest SYSTEM PRIVATE "${WEBVST_WEBDSP_DIR}/native/src")
  target_link_libraries(WebVstHostTest PRIVATE
    juce::juce_core juce::juce_events juce::juce_audio_basics
    PUBLIC juce::juce_recommended_config_flags)
endif()
