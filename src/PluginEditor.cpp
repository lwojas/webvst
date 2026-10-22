#include "PluginEditor.h"
#include <BinaryData.h>

namespace {
constexpr const char* kDefaultUrl = "http://localhost:5174";  // webseq's `npm run dev`
}

// Tells the editor when the page navigates, so events are not emitted into a page whose JUCE
// frontend has not loaded yet (emitEventIfBrowserIsVisible would assert).
class WebVstEditor::Web : public juce::WebBrowserComponent {
 public:
  Web(const Options& options, std::function<void()> onNavigate) : WebBrowserComponent(options), onNavigate_(std::move(onNavigate)) {}
  bool pageAboutToLoad(const juce::String&) override {
    if (onNavigate_) onNavigate_();
    return true;
  }

 private:
  std::function<void()> onNavigate_;
};

juce::String WebVstEditor::resolveUrl() {
  auto fromEnv = juce::SystemStats::getEnvironmentVariable("WEBVST_URL", {});
  return fromEnv.isNotEmpty() ? fromEnv : juce::String(kDefaultUrl);
}

WebVstEditor::WebVstEditor(WebVstProcessor& p) : AudioProcessorEditor(&p), processor_(p) {
  auto options = juce::WebBrowserComponent::Options{}
                     .withNativeIntegrationEnabled()
                     .withUserScript(juce::String::createStringFromData(BinaryData::shim_js, BinaryData::shim_jsSize))
                     .withEventListener("webvst", [this](const juce::var& message) {
                       pageReady_ = true;  // the shim only speaks once the page has loaded
                       processor_.engineHost().handleMessage(message);
                     });

  web_ = std::make_unique<Web>(options, [this] { pageReady_ = false; });
  addAndMakeVisible(*web_);

  processor_.engineHost().setEventSink([this](const juce::var& event) {
    if (pageReady_ && web_) web_->emitEventIfBrowserIsVisible("webvst", event);
  });

  web_->goToURL(resolveUrl());
  setResizable(true, true);
  setSize(1100, 700);
}

WebVstEditor::~WebVstEditor() { processor_.engineHost().setEventSink(nullptr); }

void WebVstEditor::resized() {
  if (web_) web_->setBounds(getLocalBounds());
}
