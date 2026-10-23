#include "PluginProcessor.h"
#include "PluginEditor.h"

WebVstProcessor::WebVstProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      sessionState_(juce::String(WEBVST_APP_ID)),
      hostFiles_([this](const HostFiles::ChooserRequest& request, std::function<void(juce::File)> done) {
        // The host's own dialog, native to the platform; released after its callback has returned.
        const auto flags = request.save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                              juce::FileBrowserComponent::warnAboutOverwriting
                                        : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        const juce::File start = request.save ? request.startDirectory.getChildFile(request.suggestedName) : request.startDirectory;
        fileChooser_ = std::make_unique<juce::FileChooser>(request.title, start, request.wildcard);
        fileChooser_->launchAsync(flags, [this, done = std::move(done)](const juce::FileChooser& chooser) {
          const juce::File chosen = chooser.getResult();
          juce::MessageManager::callAsync([this] { fileChooser_.reset(); });
          done(chosen);
        });
      }) {}

void WebVstProcessor::prepareToPlay(double sampleRate, int) {
  engineHost_.prepare(sampleRate);
  setLatencySamples(engineHost_.latencySamples());
}

bool WebVstProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void WebVstProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  EngineHost::HostTimeInfo host;
  if (auto* playHead = getPlayHead()) {
    if (auto position = playHead->getPosition()) {
      if (auto ppq = position->getPpqPosition()) {
        host.valid = true;
        host.ppq = *ppq;
        host.playing = position->getIsPlaying();
        if (auto bpm = position->getBpm()) host.bpm = *bpm;
        if (auto sig = position->getTimeSignature()) {
          host.sigNum = sig->numerator;
          host.sigDen = sig->denominator;
        }
        host.looping = position->getIsLooping();
        if (auto loop = position->getLoopPoints()) {
          host.loopStart = loop->ppqStart;
          host.loopEnd = loop->ppqEnd;
        }
      }
    }
  }
  engineHost_.process(buffer, host, midiMessages);
}

// The DAW session holds the application's project (SessionState). Nothing here waits on the page.
void WebVstProcessor::getStateInformation(juce::MemoryBlock& destination) { sessionState_.serialize(destination); }
void WebVstProcessor::setStateInformation(const void* data, int sizeInBytes) {
  sessionState_.restore(data, static_cast<size_t>(juce::jmax(0, sizeInBytes)));
}

juce::AudioProcessorEditor* WebVstProcessor::createEditor() { return new WebVstEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WebVstProcessor(); }
