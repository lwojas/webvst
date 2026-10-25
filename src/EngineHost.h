#pragma once
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include "engine.h"

// Owns one webdsp::Engine per plugin instance and is the native counterpart of
// webdsp/src/worklet/engine-processor.ts: it speaks the same HostCommand/WorkletEvent wire protocol
// (webdsp/src/runtime/commandProtocol.ts), but with the engine on the host's audio thread.
//
// Threading, the hard rule: the audio thread never waits on, or calls into, the WebView.
//   message thread  -- parses a command into a fixed-size Cmd and pushes it on an SPSC FIFO
//   audio thread    -- drains the FIFO at the top of each block, applies it to the engine, renders
//   audio -> message-- fixed-size Evt on a second SPSC FIFO, drained by a timer and sent to the page
// The engine itself is touched by the audio thread only (it is not thread-safe).
//
// Known, deliberate allocation exceptions on the audio thread (all user-initiated, none per note or
// per block): instrument/effect creation inside the engine, a finished capture copying its PCM, the
// armed-capture table, and voice-pool reconfiguration. They match what the WASM worklet does today.
class EngineHost : private juce::Timer {
 public:
  EngineHost();
  ~EngineHost() override;

  // Message thread, before audio starts (prepareToPlay).
  void prepare(double sampleRate);
  double sampleRate() const { return sampleRate_; }

  // The webdsp command/event protocol version this host implements (webdsp/src/runtime/commandProtocol.ts,
  // HOST_PROTOCOL_VERSION). Reported in the ready event so a newer web app can notice an older plugin.
  static constexpr int kProtocolVersion = 3;  // 2: set-midi-route / ready.midiRouting; 3: set-sequence / ready.nativeSequence

  // Every message type handleMessage understands: the HostCommand types plus the shim's own sample-transfer
  // messages. Tests check that it covers webdsp's HostCommand union.
  static const std::vector<juce::String>& handledMessageTypes();

  // Message thread: one HostCommand (or one of the shim's own control messages) as sent by the page.
  void handleMessage(const juce::var& message);

  // Message thread: where engine events go (the editor's WebView). Null while no editor exists.
  void setEventSink(std::function<void(const juce::var&)> sink);

  // Message thread: delivers queued engine events now (the timer does this at 60 Hz; tests call it directly).
  void flushEvents() { timerCallback(); }

  // What the DAW's playhead says about this block (filled by the processor from AudioPlayHead).
  struct HostTimeInfo {
    bool valid = false;  // false: the host gave no usable position (treated as stopped)
    bool playing = false;
    double ppq = 0.0;  // quarter notes at the first sample of the block
    double bpm = 120.0;
    int sigNum = 4, sigDen = 4;
    bool looping = false;
    double loopStart = 0.0, loopEnd = 0.0;  // quarter notes
  };

  // Latency the plugin declares to the host (plugin delay compensation). The page learns of a transport
  // start only after the fact (event queue + WebView hop), so the mapping it receives is shifted this far
  // into the future: the page has this long to schedule the first notes, and the host delays everything
  // else to match. See docs/host-transport.md in webdsp.
  static constexpr double kHostLatencySeconds = 0.1;
  int latencySamples() const { return static_cast<int>(kHostLatencySeconds * sampleRate_); }

  // Audio thread.
  void process(juce::AudioBuffer<float>& buffer, const HostTimeInfo& host, const juce::MidiBuffer& midi);

  // Frames rendered so far; the page's clock (currentTime = frames / sampleRate).
  int64_t framesRendered() const { return frame_.load(std::memory_order_acquire); }

 private:
  enum class CmdType : uint8_t {
    SwapEngine, LoadSample, RemoveSample, Trigger, ScheduleEvent, Release, Stop, VoiceParam, BusParam,
    FxAdd, FxRemove, FxMove, FxEnabled, FxParam, FxLfo, InstCreate, InstRemove, InstNoteOn, InstNoteOff,
    InstParam, InstLfo, Transport, ClearRegions, AddRegion, SampleParam, ScheduleParam, CancelScheduled,
    StartCapture, StopCapture, ArmCapture, ConfigureVoices, SetMidiRoute, SetSequence, AnalysisTap
  };

  // Field use per type is documented where the message thread fills it (EngineHost.cpp, handleMessage).
  struct Cmd {
    CmdType type{};
    int32_t i[12]{};
    float f[4]{};
    double d[3]{};
    void* p = nullptr;
  };

  enum class EvtType : uint8_t { VoiceEnded, Meters, Diagnostics, CaptureComplete, Error, DisposeEngine, DisposeSample, HostTransport, Midi, DisposeSequence, Analysis };
  enum ErrorCode : int32_t { kErrAddEffect = 1, kErrCreateInstrument, kErrEffectLfo, kErrInstrumentLfo, kErrArmCapture, kErrQueueFull };

  struct Evt {
    EvtType type{};
    int32_t a = 0, b = 0, c = 0, len = 0;
    double dbl = 0.0;
    double x[4]{};
    void* p = nullptr;
    float peaks[webdsp::Engine::kMeterCount]{};
  };

  struct SamplePayload {
    int32_t id = 0, channels = 0, length = 0, sampleRate = 0;
    float* channel[8]{};
    int32_t filled = 0;  // bytes received per channel, message thread only
  };

  template <typename T>
  struct Fifo {
    explicit Fifo(int capacity) : fifo(capacity), slots(static_cast<size_t>(capacity)) {}
    bool push(const T& v) {
      int s1, n1, s2, n2;
      fifo.prepareToWrite(1, s1, n1, s2, n2);
      if (n1 + n2 < 1) return false;
      slots[static_cast<size_t>(n1 > 0 ? s1 : s2)] = v;
      fifo.finishedWrite(1);
      return true;
    }
    bool pop(T& v) {
      int s1, n1, s2, n2;
      fifo.prepareToRead(1, s1, n1, s2, n2);
      if (n1 + n2 < 1) return false;
      v = slots[static_cast<size_t>(n1 > 0 ? s1 : s2)];
      fifo.finishedRead(1);
      return true;
    }
    juce::AbstractFifo fifo;
    std::vector<T> slots;
  };

  void timerCallback() override;
  void send(const juce::var& event);
  void pushCmd(const Cmd& c);
  void beginInit(const juce::var& message);
  juce::var buildReady(int maxVoices);
  void applyCmd(const Cmd& c, int64_t blockStart);
  void forwardMidi(const juce::MidiBuffer& midi, int64_t blockStart);
  void renderFrames(juce::AudioBuffer<float>& buffer, int from, int count, int64_t blockStart, int outChannels);
  void applyRoutedMidi(const uint8_t* data, int size);
  void releaseRouted();
  void trackTransport(const HostTimeInfo& host, int64_t blockStart, int numFrames);
  void readCapture(int32_t captureId, int32_t resultSampleId);
  void postEvt(const Evt& e);
  int64_t toFrame(double seconds) const;

  double sampleRate_ = 48000.0;
  std::unique_ptr<webdsp::Engine> engine_;  // audio thread only once prepare() has returned
  std::atomic<int64_t> frame_{0};

  Fifo<Cmd> cmds_{16384};
  Fifo<Evt> evts_{4096};
  std::atomic<int> droppedCmds_{0};
  std::atomic<int> reportedDropped_{0};

  std::function<void(const juce::var&)> sink_;
  std::unordered_map<int32_t, SamplePayload*> loading_;  // message thread: samples being received

  // audio thread state: MIDI routing (webdsp docs/midi-routing.md). The page declares what a key plays; incoming notes are
  // played here, between engine render segments split at each MIDI event, so they are sample-accurate and in order.
  struct Route {
    bool active = false;
    bool instrument = false;  // else a sample
    int32_t id = 0, unityNote = 60, bus = 0, chokeGroup = 0, channel = -1;
    bool chop = false, loop = false, mono = false, hold = false;
    float gain = 1.0f;
  } route_;
  static constexpr int32_t kRouteVoiceBase = 1 << 30;  // voice ids from here up are the router's, never the page's
  int32_t nextRouteVoice_ = kRouteVoiceBase;
  int32_t heldVoice_[128] = {};  // sample voice per MIDI note, while held (hold routes)
  int32_t monoVoice_ = 0;        // the last voice of a mono route
  int32_t lastInstrumentNote_ = -1;

  // audio thread state: the native sequence (webdsp docs/native-sequence.md). The page sends one cycle of the song in quarter notes;
  // each block, the events falling in the block's slice of the DAW timeline are due, and are played between render segments split
  // at their frames (as MIDI routing does), so they are sample-accurate and a mono release lands exactly at the next note.
  struct SeqEvent {
    double beat = 0.0;
    uint8_t kind = 0;  // 0 sample, 1 instrument, 2 param
    bool loop = false, mono = false, hasFrom = false;
    int32_t id = 0, bus = 0, region = -1, chokeGroup = 0, track = -1, param = 0, target = 0;
    float gain = 1.0f, pan = 0.0f, pitch = 0.0f, value = 0.0f, from = 0.0f;
    double durationBeats = -1.0, rampBeats = 0.0;
  };
  struct Sequence {
    double lengthBeats = 0.0;
    std::vector<SeqEvent> events;  // sorted by beat
  };
  struct DueEvent { int at; const SeqEvent* event; };
  static constexpr int32_t kSeqVoiceBase = 1 << 29;  // sequence voice ids: [1<<29, 1<<30); like the router's, never the page's
  static constexpr int kMaxDuePerBlock = 1024;
  static constexpr int kSeqTracks = 64, kSeqSustained = 32;
  std::unique_ptr<Sequence> sequence_;
  int32_t nextSeqVoice_ = kSeqVoiceBase;
  int32_t seqTrackVoice_[kSeqTracks] = {};
  int32_t seqRecentVoices_[256] = {};  // ring of the latest voices, released on stop/locate (releasing an ended voice is harmless)
  int seqRecentNext_ = 0;
  int32_t seqSustained_[kSeqSustained] = {};  // instruments given a timed note, ended on stop/locate
  int seqSustainedCount_ = 0;
  bool seqWasPlaying_ = false;
  double seqGridBpm_ = 0.0;
  double seqExpectedPpq_ = 0.0;
  DueEvent due_[kMaxDuePerBlock];
  int dueCount_ = 0;
  double dueBpm_ = 120.0;
  void runSequence(const HostTimeInfo& host, int numFrames, int64_t blockStart);
  void applySequenceEvent(const SeqEvent& e, double bpm, int64_t frame);
  void releaseSequence();

  // audio thread state: host transport tracking
  bool lastPlaying_ = false, lastLoopOn_ = false;
  double lastBpm_ = 120.0, expectedPpq_ = 0.0, lastLoopStart_ = 0.0, lastLoopEnd_ = 0.0;
  int lastSigNum_ = 4, lastSigDen_ = 4;
  int32_t epoch_ = 0;
  int64_t framesSinceTransportEvent_ = 0, framesSinceTempoEvent_ = 1 << 30;
  std::atomic<bool> resendTransport_{true};  // set by the message thread when a page (re)connects

  // message thread state: last transport event sent, to synthesize a stop if the host stops calling us
  struct SentTransport { bool valid = false, playing = false; juce::var event; } lastSentTransport_;
  int64_t lastSeenFrame_ = -1;
  int stalledTicks_ = 0;

  // audio thread state
  std::unordered_map<int32_t, int32_t> armedCaptures_;  // captureId -> result sample id
  int32_t framesSinceMeters_ = 0, framesSinceDiagnostics_ = 0, framesSinceAnalysis_ = 0;
  bool metersWereActive_ = false;
  // Spectrum taps (webdsp "analysis" events): per slot, the audio thread copies the engine's latest window
  // into analysisBuf_[slot] under a seqlock (odd = being written) and posts a small Analysis event; the
  // message thread reads it back and sends the samples. Only the audio thread touches the other state.
  static constexpr int kTapSlots = webdsp::kMaxAnalysisTaps;
  std::array<bool, kTapSlots> analysisActive_{};
  std::array<uint32_t, kTapSlots> analysisLastWritten_{};
  std::array<std::atomic<uint32_t>, kTapSlots> analysisSeq_{};
  std::array<std::array<float, webdsp::kAnalysisWindow>, kTapSlots> analysisBuf_{};

  juce::CriticalSection sinkLock_;
};
