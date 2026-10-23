#include "PluginEditor.h"
#include <BinaryData.h>

namespace {
// Each plugin's own default (WEBVST_DEFAULT_URL, set per target in CMakeLists.txt): its client's `npm run dev`.
constexpr const char* kDefaultUrl = WEBVST_DEFAULT_URL;
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

// The page to load: WEBVST_URL_<APP> (e.g. WEBVST_URL_WEBSEQ) for this plugin alone, else WEBVST_URL for any, else the
// default. The per-app variable lets two plugins in one host point at different servers.
juce::String WebVstEditor::resolveUrl() {
  const auto perApp = juce::SystemStats::getEnvironmentVariable("WEBVST_URL_" + juce::String(WEBVST_APP_ID).toUpperCase(), {});
  if (perApp.isNotEmpty()) return perApp;
  const auto any = juce::SystemStats::getEnvironmentVariable("WEBVST_URL", {});
  return any.isNotEmpty() ? any : juce::String(kDefaultUrl);
}

WebVstEditor::WebVstEditor(WebVstProcessor& p) : AudioProcessorEditor(&p), processor_(p) {
  auto options = juce::WebBrowserComponent::Options{}
                     .withNativeIntegrationEnabled()
                     .withUserScript(juce::String::createStringFromData(BinaryData::shim_js, BinaryData::shim_jsSize))
                     .withEventListener("webvst", [this](const juce::var& message) {
                       pageReady_ = true;  // the shim only speaks once the page has loaded
                       if (processor_.sessionState().handleMessage(message) || processor_.hostFiles().handleMessage(message)) return;
                       processor_.engineHost().handleMessage(message);
                     });

  web_ = std::make_unique<Web>(options, [this] { pageReady_ = false; });
  addAndMakeVisible(*web_);

  auto emit = [this](const juce::var& event) {
    if (pageReady_ && web_) web_->emitEventIfBrowserIsVisible("webvst", event);
  };
  processor_.engineHost().setEventSink(emit);
  processor_.sessionState().setEventSink(emit);
  processor_.hostFiles().setEventSink(emit);

  web_->goToURL(resolveUrl());
  setResizable(true, true);
  setSize(1100, 700);
}

WebVstEditor::~WebVstEditor() {
  processor_.engineHost().setEventSink(nullptr);
  processor_.sessionState().setEventSink(nullptr);
  processor_.hostFiles().setEventSink(nullptr);
}

void WebVstEditor::resized() {
  if (web_) web_->setBounds(getLocalBounds());
}
