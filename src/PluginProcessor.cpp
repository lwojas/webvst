#include "PluginProcessor.h"
#include "PluginEditor.h"

WebVstProcessor::WebVstProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)) {}

void WebVstProcessor::prepareToPlay(double, int) {}

bool WebVstProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void WebVstProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
  juce::ScopedNoDenormals noDenormals;
  buffer.clear();
}

juce::AudioProcessorEditor* WebVstProcessor::createEditor() { return new WebVstEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new WebVstProcessor(); }
