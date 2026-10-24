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

# The page shim is a real .js file, embedded as BinaryData::shim_js.
juce_add_binary_data(WebVstData SOURCES src/shim.js)

# One plugin per web client, from the same sources: each has its own plugin ID and name (so a DAW lists them separately),
# the application id stamped on its saved state (a plugin refuses the other app's state; see SessionState), and the URL
# its editor opens by default (overridable at run time, see PluginEditor.cpp).
function(webvst_add_plugin target)
  cmake_parse_arguments(P "" "PRODUCT;CODE;APP_ID;DEFAULT_URL" "" ${ARGN})

  juce_add_plugin(${target}
    PRODUCT_NAME "${P_PRODUCT}"
    COMPANY_NAME "lwojas"
    PLUGIN_MANUFACTURER_CODE Lwjs
    PLUGIN_CODE ${P_CODE}
    FORMATS VST3
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE)

  target_sources(${target} PRIVATE
    src/EngineHost.cpp
    src/SessionState.cpp
    src/HostFiles.cpp
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

  target_include_directories(${target} SYSTEM PRIVATE "${WEBVST_WEBDSP_DIR}/native/src")

  target_compile_definitions(${target} PUBLIC
    WEBVST_APP_ID="${P_APP_ID}"
    WEBVST_DEFAULT_URL="${P_DEFAULT_URL}"
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

  target_link_libraries(${target} PRIVATE
    WebVstData
    juce::juce_audio_utils
    juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

  # macOS: a CLEAN build leaves the bundle with a broken code signature. JUCE signs it, then writes
  # Contents/Resources/moduleinfo.json in a later POST_BUILD step, so the seal reports "file added" (an incremental rebuild
  # happens to seal the previous moduleinfo.json first, so it looks fine: that is why a plugin that had been rebuilt many
  # times loaded and a brand-new one did not). Ableton Live's plugin scanner enforces the seal and silently refuses the
  # plugin ("a sealed resource is missing or invalid ... not a plugin" in PluginScanner.txt); REAPER does not check. Sign
  # the finished bundle as the very last step, so every build is valid (ad hoc: enough to load locally; a release build
  # signs with a Developer ID instead). Defined after juce_add_plugin, so it runs after JUCE's own post-build steps.
  if(APPLE)
    add_custom_command(TARGET ${target}_VST3 POST_BUILD
      COMMAND codesign --force --sign - --timestamp=none "$<GENEX_EVAL:$<TARGET_PROPERTY:${target}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
      COMMAND codesign --verify --strict "$<GENEX_EVAL:$<TARGET_PROPERTY:${target}_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
      COMMENT "Signing ${P_PRODUCT}.vst3 (after moduleinfo.json was added)"
      VERBATIM)
  endif()
endfunction()

# Default URLs are the clients hosted on GitHub Pages. For local development point at a dev server with
# WEBVST_URL_WEBSEQ / WEBVST_URL_WEBTRACKER (e.g. http://localhost:5174 / :5175).
webvst_add_plugin(WebSeq PRODUCT "WebSeq" CODE Wsqr APP_ID webseq DEFAULT_URL "https://lwojas.github.io/webseq/")
webvst_add_plugin(WebTracker PRODUCT "WebTracker" CODE Wtrk APP_ID webtracker DEFAULT_URL "https://lwojas.github.io/webtrack/")

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
