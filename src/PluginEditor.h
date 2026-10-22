#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// Hosts the web client. The URL is the only coupling: WEBVST_URL overrides the dev default.
// The page talks to the engine through the shim (shim.js) over the "webvst" event channel.
class WebVstEditor : public juce::AudioProcessorEditor {
 public:
  explicit WebVstEditor(WebVstProcessor&);
  ~WebVstEditor() override;
  void resized() override;

 private:
  class Web;
  static juce::String resolveUrl();

  WebVstProcessor& processor_;
  bool pageReady_ = false;  // true from the page's first message until it navigates again
  std::unique_ptr<Web> web_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebVstEditor)
};
