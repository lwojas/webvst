#include "PluginEditor.h"

namespace {
constexpr const char* kDefaultUrl = "http://localhost:5174";  // webseq's `npm run dev`
}

juce::String WebVstEditor::resolveUrl() {
  auto fromEnv = juce::SystemStats::getEnvironmentVariable("WEBVST_URL", {});
  return fromEnv.isNotEmpty() ? fromEnv : juce::String(kDefaultUrl);
}

WebVstEditor::WebVstEditor(WebVstProcessor& p) : AudioProcessorEditor(&p) {
  web_ = std::make_unique<juce::WebBrowserComponent>(juce::WebBrowserComponent::Options{});
  addAndMakeVisible(*web_);
  web_->goToURL(resolveUrl());
  setResizable(true, true);
  setSize(1100, 700);
}

void WebVstEditor::resized() {
  if (web_) web_->setBounds(getLocalBounds());
}
