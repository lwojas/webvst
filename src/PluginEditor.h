#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// Hosts the web client. The URL is the only coupling: WEBVST_URL overrides the dev default.
class WebVstEditor : public juce::AudioProcessorEditor {
 public:
  explicit WebVstEditor(WebVstProcessor&);
  void resized() override;

 private:
  static juce::String resolveUrl();

  std::unique_ptr<juce::WebBrowserComponent> web_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebVstEditor)
};
