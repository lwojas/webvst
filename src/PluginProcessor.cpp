#include "PluginProcessor.h"
#include "PluginEditor.h"

WebVstProcessor::WebVstProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

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

juce::AudioProcessorEditor* WebVstProcessor::createEditor() { return new WebVstEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WebVstProcessor(); }
