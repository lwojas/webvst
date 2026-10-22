cmake_minimum_required(VERSION 3.22)
project(WebVST VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# webdsp is consumed as a header-only engine (native/src/engine.h). Nothing in webdsp's own build is used.
set(WEBVST_WEBDSP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../webdsp" CACHE PATH "Path to a webdsp checkout")

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

target_sources(WebVST PRIVATE
  src/PluginProcessor.cpp
  src/PluginEditor.cpp)

target_include_directories(WebVST PRIVATE "${WEBVST_WEBDSP_DIR}/native/src")

target_compile_definitions(WebVST PUBLIC
  JUCE_WEB_BROWSER=1
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(WebVST PRIVATE
  juce::juce_audio_utils
  juce::juce_gui_extra
  PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
