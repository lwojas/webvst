#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "EngineHost.h"
#include "SessionState.h"

// Stereo instrument backed by the native webdsp engine (EngineHost). The page drives it through the bridge;
// DAW MIDI and transport are not routed yet.
class WebVstProcessor : public juce::AudioProcessor {
 public:
  WebVstProcessor();
  ~WebVstProcessor() override = default;

  void prepareToPlay(double sampleRate, int samplesPerBlock) override;
  void releaseResources() override {}
  bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
  void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

  juce::AudioProcessorEditor* createEditor() override;
  bool hasEditor() const override { return true; }

  const juce::String getName() const override { return "WebVST"; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  bool isMidiEffect() const override { return false; }
  double getTailLengthSeconds() const override { return 0.0; }

  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}

  EngineHost& engineHost() { return engineHost_; }
  SessionState& sessionState() { return sessionState_; }

  void getStateInformation(juce::MemoryBlock& destination) override;
  void setStateInformation(const void* data, int sizeInBytes) override;

 private:
  EngineHost engineHost_;
  SessionState sessionState_;
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WebVstProcessor)
};
