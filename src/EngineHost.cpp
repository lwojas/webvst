#include "EngineHost.h"
#include "Trace.h"
#include <cmath>
#include <cstdlib>

namespace {

constexpr int kMetersIntervalFrames = 1024;       // ~21 ms at 48 kHz (the worklet posts every 8 x 128)
constexpr int kDiagnosticsIntervalFrames = 2560;  // 20 x 128, as the worklet
constexpr int kDefaultMaxVoices = 128;
constexpr int kOutputChannels = 2;

bool has(const juce::var& o, const char* key) { return o.hasProperty(key) && !o[key].isVoid(); }
double num(const juce::var& o, const char* key, double fallback) {
  if (!has(o, key)) return fallback;
  const double v = static_cast<double>(o[key]);
  return std::isfinite(v) ? v : fallback;
}
int32_t integer(const juce::var& o, const char* key, int32_t fallback) {
  return static_cast<int32_t>(num(o, key, fallback));
}
bool flag(const juce::var& o, const char* key) { return has(o, key) && static_cast<bool>(o[key]); }

// Wire value of TriggerParams.chokeGroup: a positive integer, else 0 (see engine-processor.ts).
int32_t chokeGroupOf(const juce::var& o) {
  if (!has(o, "chokeGroup")) return 0;
  const double g = static_cast<double>(o["chokeGroup"]);
  return std::isfinite(g) && g >= 1.0 ? static_cast<int32_t>(std::floor(g)) : 0;
}

juce::var object(std::initializer_list<std::pair<const char*, juce::var>> props) {
  auto* o = new juce::DynamicObject();
  for (auto& p : props) o->setProperty(p.first, p.second);
  return juce::var(o);
}

juce::String toBase64(const void* data, size_t bytes) { return juce::Base64::toBase64(data, bytes); }

const char* modulationName(int32_t domain) {
  static const char* names[] = {"none", "linear", "log"};
  return domain >= 0 && domain < 3 ? names[domain] : "none";
}

juce::var paramList(int32_t count, const webdsp::InstrumentParamInfo* (*info)(int32_t, int32_t), int32_t kind) {
  juce::Array<juce::var> params;
  for (int32_t i = 0; i < count; i++) {
    const webdsp::InstrumentParamInfo* p = info(kind, i);
    if (!p) continue;
    params.add(object({{"id", p->id},
                       {"name", juce::String(p->name)},
                       {"min", p->min},
                       {"max", p->max},
                       {"default", p->def},
                       {"unit", juce::String(p->unit)},
                       {"modulation", modulationName(static_cast<int32_t>(webdsp::resolveModDomain(*p)))}}));
  }
  return params;
}

webdsp::LfoConfig lfoConfig(const int32_t* i, const float* f) {
  webdsp::LfoConfig c;
  c.enabled = i[3] != 0;
  c.waveform = static_cast<webdsp::LfoWaveform>(i[4]);
  c.sync = i[5] != 0;
  c.rateHz = f[0];
  c.division = i[6];
  c.phase = f[1];
  c.amount = f[2];
  c.target = i[7];
  c.retrigger = i[8] != 0;
  return c;
}

// The engine's event queues are plain vectors that grow on first use. Grow them here, on the message
// thread, so that scheduling on the audio thread later never has to allocate: schedule a lot far in the
// future, cancel (capacity is kept), and drain the transport queue with one block.
void warmUp(webdsp::Engine& e) {
  constexpr int kEvents = 4096;
  constexpr int64_t kFar = int64_t{1} << 40;
  for (int i = 0; i < kEvents; i++) {
    e.scheduleEvent(kFar + i, 1, 1, 0, 1.0f, 1.0f, 0, -1, false, false, -1, 0.0f, -1, 0);
    e.scheduleInstrumentNote(kFar + i, 1, 1.0f, 0.0f, 0.0f, -1, 0);
    e.scheduleParam({kFar + i, 1, 0, 1, 0, 0.0f, 0.0f, false, 0});
    e.scheduleTransport(0, 0.0, 120.0);
  }
  e.cancelScheduled(0);
  e.process(0, 128);  // drains the (due) transport changes
}

}  // namespace

// The timer drains audio-thread events to the page. 120 Hz keeps MIDI under ~10 ms of added delay; the
// clock refresh and stall detection below are counted in ticks of this rate.
constexpr int kTimerHz = 120;

EngineHost::EngineHost() { startTimerHz(kTimerHz); }

const std::vector<juce::String>& EngineHost::handledMessageTypes() {
  static const std::vector<juce::String> types = {
      "__init", "sample-begin", "sample-chunk", "sample-end", "unload-sample", "trigger", "schedule", "release", "stop",
      "set-voice-param", "set-bus-param", "add-effect", "remove-effect", "move-effect", "set-effect-enabled",
      "set-effect-param", "set-effect-lfo", "set-instrument-lfo", "create-instrument", "remove-instrument",
      "instrument-note-on", "instrument-note-off", "set-instrument-param", "set-transport", "set-sample-regions",
      "set-sample-param", "schedule-params", "cancel-scheduled", "start-capture", "stop-capture", "arm-capture",
      "configure"};
  return types;
}

EngineHost::~EngineHost() {
  stopTimer();
  for (auto& kv : loading_) {
    for (int c = 0; c < kv.second->channels; c++) std::free(kv.second->channel[c]);
    delete kv.second;
  }
  Evt e;
  while (evts_.pop(e)) {
    if (e.type == EvtType::DisposeEngine) delete static_cast<webdsp::Engine*>(e.p);
    if (e.type == EvtType::DisposeSample) delete static_cast<SamplePayload*>(e.p);
    if (e.type == EvtType::CaptureComplete) std::free(e.p);
  }
}

void EngineHost::prepare(double sampleRate) {
  if (engine_ && sampleRate == sampleRate_) return;
  sampleRate_ = sampleRate;
  engine_ = std::make_unique<webdsp::Engine>();
  engine_->init(sampleRate_, kOutputChannels, kDefaultMaxVoices);
  warmUp(*engine_);
}

void EngineHost::setEventSink(std::function<void(const juce::var&)> sink) { sink_ = std::move(sink); }

int64_t EngineHost::toFrame(double seconds) const { return static_cast<int64_t>(std::llround(seconds * sampleRate_)); }

void EngineHost::send(const juce::var& event) {
  if (sink_) sink_(event);
}

void EngineHost::pushCmd(const Cmd& c) {
  if (!cmds_.push(c)) droppedCmds_.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------------
// Message thread: page -> native
// ---------------------------------------------------------------------------------------------

void EngineHost::beginInit(const juce::var& message) {
  const int maxVoices = juce::jlimit(1, 1024, integer(message, "maxVoices", kDefaultMaxVoices));
  for (auto& kv : loading_) {  // a reloaded page abandons any half-received sample
    for (int c = 0; c < kv.second->channels; c++) std::free(kv.second->channel[c]);
    delete kv.second;
  }
  loading_.clear();

  auto fresh = std::make_unique<webdsp::Engine>();
  fresh->init(sampleRate_, kOutputChannels, maxVoices);
  warmUp(*fresh);
  Cmd swap;
  swap.type = CmdType::SwapEngine;
  swap.p = fresh.release();
  pushCmd(swap);
  resendTransport_.store(true, std::memory_order_relaxed);
  lastSentTransport_ = {};
  send(buildReady(maxVoices));
  Trace::log("native: engine ready sent to the page");
}

juce::var EngineHost::buildReady(int maxVoices) {
  juce::Array<juce::var> instruments;
  for (int32_t kind = 0; kind < 64; kind++) {
    const webdsp::InstrumentKindMeta* m = webdsp::instrumentKindMeta(kind);
    if (!m || webdsp::instrumentParamCount(kind) <= 0) continue;
    instruments.add(object({{"kind", kind},
                            {"id", juce::String(m->id)},
                            {"name", juce::String(m->name)},
                            {"category", juce::String(m->category)},
                            {"params", paramList(webdsp::instrumentParamCount(kind), webdsp::instrumentParamInfo, kind)}}));
  }
  juce::Array<juce::var> effects;
  for (int32_t kind = 0; webdsp::fxParamCount(kind) > 0; kind++) {
    effects.add(object({{"kind", kind}, {"params", paramList(webdsp::fxParamCount(kind), webdsp::fxParamInfo, kind)}}));
  }
  juce::Array<juce::var> waveforms, divisions;
  for (int32_t i = 0; i < webdsp::kLfoWaveformCount; i++) waveforms.add(juce::String(webdsp::kLfoWaveformNames[i]));
  for (int32_t i = 0; i < webdsp::kLfoDivisionCount; i++) {
    divisions.add(object({{"name", juce::String(webdsp::kLfoDivisionNames[i])},
                          {"beats", static_cast<double>(webdsp::kLfoDivisionBeats[i])}}));
  }
  return object({{"type", "ready"},
                 {"protocolVersion", kProtocolVersion},
                 {"sampleRate", sampleRate_},
                 {"outputChannels", kOutputChannels},
                 {"maxVoices", maxVoices},
                 {"renderQuantumFrames", 128},
                 {"maxCaptureSeconds", webdsp::Engine::maxCaptureSeconds()},
                 {"instruments", instruments},
                 {"effects", effects},
                 {"modulation", object({{"maxLfos", webdsp::kMaxLfos},
                                        {"waveforms", waveforms},
                                        {"divisions", divisions},
                                        {"minRateHz", static_cast<double>(webdsp::kLfoMinRateHz)},
                                        {"maxRateHz", static_cast<double>(webdsp::kLfoMaxRateHz)}})},
                 {"clockFrame", static_cast<double>(framesRendered())}});
}

void EngineHost::handleMessage(const juce::var& msg) {
  const juce::String type = msg["type"].toString();
  Cmd c;

  // Fills a Trigger/ScheduleEvent command from sample-trigger params (TriggerParams / ScheduledEvent).
  auto fillTrigger = [&](const juce::var& p, int32_t voice, bool alwaysScheduled) {
    c.i[0] = voice;
    c.i[1] = integer(p, "sampleId", 0);
    c.i[2] = integer(p, "bus", 0);
    c.i[3] = integer(p, "start", 0);
    c.i[4] = integer(p, "end", -1);
    c.i[5] = flag(p, "loop");
    c.i[6] = flag(p, "reverse");
    c.i[7] = has(p, "duration") ? static_cast<int32_t>(toFrame(num(p, "duration", 0))) : -1;
    c.i[8] = has(p, "region") ? integer(p, "region", 0) : -1;
    c.i[9] = chokeGroupOf(p);
    c.i[10] = alwaysScheduled || has(p, "time");
    c.f[0] = static_cast<float>(num(p, "gain", 1.0));
    const double pitch = num(p, "pitch", 0.0);
    c.f[1] = static_cast<float>(num(p, "rate", 1.0) * (pitch != 0.0 ? std::pow(2.0, pitch / 12.0) : 1.0));
    c.f[2] = static_cast<float>(num(p, "pan", 0.0));
    c.d[0] = static_cast<double>(toFrame(num(p, "time", 0.0)));
  };

  if (type == "__init") {
    beginInit(msg);
  } else if (type == "sample-begin") {
    auto* s = new SamplePayload();
    s->id = integer(msg, "sampleId", 0);
    s->channels = juce::jlimit(1, 8, integer(msg, "channels", 1));
    s->length = integer(msg, "length", 0);
    s->sampleRate = integer(msg, "sampleRate", static_cast<int32_t>(sampleRate_));
    for (int ch = 0; ch < s->channels; ch++)
      s->channel[ch] = static_cast<float*>(std::calloc(static_cast<size_t>(juce::jmax(1, s->length)), sizeof(float)));
    if (auto it = loading_.find(s->id); it != loading_.end()) {
      for (int ch = 0; ch < it->second->channels; ch++) std::free(it->second->channel[ch]);
      delete it->second;
    }
    loading_[s->id] = s;
  } else if (type == "sample-chunk") {
    auto it = loading_.find(integer(msg, "sampleId", 0));
    if (it == loading_.end()) return;
    SamplePayload* s = it->second;
    const int ch = integer(msg, "channel", 0);
    const int64_t offsetBytes = static_cast<int64_t>(num(msg, "offsetBytes", 0));
    juce::MemoryOutputStream out;
    if (ch < 0 || ch >= s->channels || !juce::Base64::convertFromBase64(out, msg["data"].toString())) return;
    const int64_t room = static_cast<int64_t>(s->length) * 4 - offsetBytes;
    const int64_t bytes = juce::jmin<int64_t>(static_cast<int64_t>(out.getDataSize()), room);
    if (offsetBytes < 0 || bytes <= 0) return;
    std::memcpy(reinterpret_cast<char*>(s->channel[ch]) + offsetBytes, out.getData(), static_cast<size_t>(bytes));
  } else if (type == "sample-end") {
    auto it = loading_.find(integer(msg, "sampleId", 0));
    if (it == loading_.end()) return;
    c.type = CmdType::LoadSample;
    c.p = it->second;
    loading_.erase(it);
    pushCmd(c);
  } else if (type == "unload-sample") {
    c.type = CmdType::RemoveSample;
    c.i[0] = integer(msg, "sampleId", 0);
    pushCmd(c);
  } else if (type == "trigger") {
    c.type = CmdType::Trigger;
    fillTrigger(msg["params"], integer(msg, "voice", 0), false);
    pushCmd(c);
  } else if (type == "schedule") {
    const juce::var events = msg["events"];
    if (auto* arr = events.getArray()) {
      for (const auto& e : *arr) {
        Cmd s;
        c = s;
        c.type = CmdType::ScheduleEvent;
        fillTrigger(e, integer(e, "voice", 0), true);
        pushCmd(c);
      }
    }
  } else if (type == "release") {
    c.type = CmdType::Release;
    c.i[0] = integer(msg, "voice", 0);
    pushCmd(c);
  } else if (type == "stop") {
    c.type = CmdType::Stop;
    c.i[0] = integer(msg, "voice", 0);
    pushCmd(c);
  } else if (type == "set-voice-param") {
    c.type = CmdType::VoiceParam;
    c.i[0] = integer(msg, "voice", 0);
    c.i[1] = integer(msg, "param", 0);
    c.f[0] = static_cast<float>(num(msg, "value", 0));
    pushCmd(c);
  } else if (type == "set-bus-param") {
    c.type = CmdType::BusParam;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "param", 0);
    c.f[0] = static_cast<float>(num(msg, "value", 0));
    pushCmd(c);
  } else if (type == "add-effect") {
    c.type = CmdType::FxAdd;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "effect", 0);
    c.i[2] = integer(msg, "kind", 0);
    c.i[3] = integer(msg, "index", 0);
    pushCmd(c);
  } else if (type == "remove-effect") {
    c.type = CmdType::FxRemove;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "effect", 0);
    pushCmd(c);
  } else if (type == "move-effect") {
    c.type = CmdType::FxMove;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "effect", 0);
    c.i[2] = integer(msg, "index", 0);
    pushCmd(c);
  } else if (type == "set-effect-enabled") {
    c.type = CmdType::FxEnabled;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "effect", 0);
    c.i[2] = flag(msg, "enabled");
    pushCmd(c);
  } else if (type == "set-effect-param") {
    c.type = CmdType::FxParam;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, "effect", 0);
    c.i[2] = integer(msg, "param", 0);
    c.f[0] = static_cast<float>(num(msg, "value", 0));
    pushCmd(c);
  } else if (type == "set-effect-lfo" || type == "set-instrument-lfo") {
    const bool inst = type == "set-instrument-lfo";
    c.type = inst ? CmdType::InstLfo : CmdType::FxLfo;
    c.i[0] = integer(msg, "bus", 0);
    c.i[1] = integer(msg, inst ? "instrument" : "effect", 0);
    c.i[2] = integer(msg, "slot", 0);
    c.i[3] = flag(msg, "enabled");
    c.i[4] = integer(msg, "waveform", 0);
    c.i[5] = flag(msg, "sync");
    c.i[6] = integer(msg, "division", 0);
    c.i[7] = integer(msg, "target", 0);
    c.i[8] = inst && flag(msg, "retrigger");
    c.f[0] = static_cast<float>(num(msg, "rateHz", 1));
    c.f[1] = static_cast<float>(num(msg, "phase", 0));
    c.f[2] = static_cast<float>(num(msg, "amount", 0));
    pushCmd(c);
  } else if (type == "create-instrument") {
    c.type = CmdType::InstCreate;
    c.i[0] = integer(msg, "instrument", 0);
    c.i[1] = integer(msg, "kind", 0);
    c.i[2] = integer(msg, "bus", 0);
    pushCmd(c);
  } else if (type == "remove-instrument") {
    c.type = CmdType::InstRemove;
    c.i[0] = integer(msg, "instrument", 0);
    pushCmd(c);
  } else if (type == "instrument-note-on") {
    const juce::var p = msg["params"];
    c.type = CmdType::InstNoteOn;
    c.i[0] = integer(msg, "instrument", 0);
    c.i[1] = has(p, "duration") ? static_cast<int32_t>(toFrame(num(p, "duration", 0))) : -1;
    c.i[2] = chokeGroupOf(p);
    c.i[3] = has(p, "time");
    c.f[0] = static_cast<float>(num(p, "velocity", 1.0));
    c.f[1] = static_cast<float>(num(p, "pan", 0.0));
    c.f[2] = static_cast<float>(num(p, "pitch", 0.0));
    c.d[0] = static_cast<double>(toFrame(num(p, "time", 0.0)));
    pushCmd(c);
  } else if (type == "instrument-note-off") {
    c.type = CmdType::InstNoteOff;
    c.i[0] = integer(msg, "instrument", 0);
    pushCmd(c);
  } else if (type == "set-instrument-param") {
    c.type = CmdType::InstParam;
    c.i[0] = integer(msg, "instrument", 0);
    c.i[1] = integer(msg, "param", 0);
    c.f[0] = static_cast<float>(num(msg, "value", 0));
    pushCmd(c);
  } else if (type == "set-transport") {
    c.type = CmdType::Transport;
    c.i[0] = has(msg, "time");
    c.d[0] = static_cast<double>(has(msg, "time") ? toFrame(num(msg, "time", 0)) : 0);
    c.d[1] = static_cast<double>(toFrame(num(msg, "anchorTime", 0)));
    c.d[2] = num(msg, "bpm", 120);
    pushCmd(c);
  } else if (type == "set-sample-regions") {
    const int32_t sample = integer(msg, "sample", 0);
    c.type = CmdType::ClearRegions;
    c.i[0] = sample;
    pushCmd(c);
    if (auto* arr = msg["regions"].getArray()) {
      for (const auto& r : *arr) {
        Cmd a;
        a.type = CmdType::AddRegion;
        a.i[0] = sample;
        a.i[1] = integer(r, "start", 0);
        a.i[2] = integer(r, "end", 0);
        pushCmd(a);
      }
    }
  } else if (type == "set-sample-param") {
    c.type = CmdType::SampleParam;
    c.i[0] = integer(msg, "sample", 0);
    c.i[1] = integer(msg, "param", 0);
    c.f[0] = static_cast<float>(num(msg, "value", 0));
    pushCmd(c);
  } else if (type == "schedule-params") {
    if (auto* arr = msg["events"].getArray()) {
      for (const auto& e : *arr) {
        Cmd p;
        p.type = CmdType::ScheduleParam;
        p.d[0] = static_cast<double>(toFrame(num(e, "time", 0)));
        p.i[0] = integer(e, "target", 0);
        p.i[1] = integer(e, "bus", 0);
        p.i[2] = integer(e, "id", 0);
        p.i[3] = integer(e, "param", 0);
        p.i[4] = has(e, "from");
        p.i[5] = static_cast<int32_t>(toFrame(num(e, "ramp", 0)));
        p.f[0] = static_cast<float>(num(e, "value", 0));
        p.f[1] = static_cast<float>(num(e, "from", 0));
        pushCmd(p);
      }
    }
  } else if (type == "cancel-scheduled") {
    c.type = CmdType::CancelScheduled;
    c.i[0] = has(msg, "fromTime");
    c.d[0] = static_cast<double>(toFrame(num(msg, "fromTime", 0)));
    pushCmd(c);
  } else if (type == "start-capture") {
    c.type = CmdType::StartCapture;
    c.i[0] = integer(msg, "captureId", 0);
    c.i[1] = integer(msg, "bus", 0);
    pushCmd(c);
  } else if (type == "stop-capture") {
    c.type = CmdType::StopCapture;
    c.i[0] = integer(msg, "captureId", 0);
    c.i[1] = integer(msg, "resultSampleId", 0);
    pushCmd(c);
  } else if (type == "arm-capture") {
    c.type = CmdType::ArmCapture;
    c.i[0] = integer(msg, "captureId", 0);
    c.i[1] = integer(msg, "bus", 0);
    c.i[2] = integer(msg, "resultSampleId", 0);
    c.d[0] = static_cast<double>(toFrame(num(msg, "startTime", 0)));
    c.d[1] = static_cast<double>(toFrame(num(msg, "stopTime", 0)));
    pushCmd(c);
  } else if (type == "configure") {
    c.type = CmdType::ConfigureVoices;
    c.i[0] = integer(msg, "maxVoices", kDefaultMaxVoices);
    pushCmd(c);
  } else if (type.isNotEmpty()) {
    // A command this plugin does not know: tell the page instead of ignoring it, so a newer web app against an
    // older plugin shows up as an error rather than as a feature that silently does nothing.
    send(object({{"type", "error"}, {"message", "plugin does not understand command '" + type + "' (the plugin may be older than this app)"}}));
  }
}

// ---------------------------------------------------------------------------------------------
// Audio thread
// ---------------------------------------------------------------------------------------------

void EngineHost::postEvt(const Evt& e) { evts_.push(e); }  // a full event queue drops the event, never blocks

void EngineHost::applyCmd(const Cmd& c, int64_t blockStart) {
  // The engine only changes through SwapEngine / when it exists.
  if (c.type == CmdType::SwapEngine) {
    Evt dispose;
    dispose.type = EvtType::DisposeEngine;
    dispose.p = engine_.release();
    engine_.reset(static_cast<webdsp::Engine*>(c.p));
    armedCaptures_.clear();
    if (dispose.p) postEvt(dispose);
    return;
  }
  if (c.type == CmdType::LoadSample) {
    auto* payload = static_cast<SamplePayload*>(c.p);
    if (engine_) engine_->commitSample(payload->id, payload->channels, payload->length, payload->sampleRate, payload->channel);
    else
      for (int ch = 0; ch < payload->channels; ch++) std::free(payload->channel[ch]);
    Evt dispose;  // the engine now owns the PCM; only the descriptor is ours to free
    dispose.type = EvtType::DisposeSample;
    dispose.p = payload;
    postEvt(dispose);
    return;
  }
  if (!engine_) return;
  webdsp::Engine& e = *engine_;
  auto error = [&](ErrorCode code, int32_t a, int32_t b) {
    Evt ev;
    ev.type = EvtType::Error;
    ev.a = code;
    ev.b = a;
    ev.c = b;
    postEvt(ev);
  };

  switch (c.type) {
    case CmdType::Trigger:
      if (!c.i[10] || static_cast<int64_t>(c.d[0]) <= blockStart)
        e.trigger(c.i[0], c.i[1], c.i[2], c.f[0], c.f[1], c.i[3], c.i[4], c.i[5] != 0, c.i[6] != 0, c.i[7], c.f[2], c.i[8], c.i[9]);
      else
        e.scheduleEvent(static_cast<int64_t>(c.d[0]), c.i[0], c.i[1], c.i[2], c.f[0], c.f[1], c.i[3], c.i[4], c.i[5] != 0,
                        c.i[6] != 0, c.i[7], c.f[2], c.i[8], c.i[9]);
      break;
    case CmdType::ScheduleEvent:
      e.scheduleEvent(static_cast<int64_t>(c.d[0]), c.i[0], c.i[1], c.i[2], c.f[0], c.f[1], c.i[3], c.i[4], c.i[5] != 0,
                      c.i[6] != 0, c.i[7], c.f[2], c.i[8], c.i[9]);
      break;
    case CmdType::RemoveSample: e.removeSample(c.i[0]); break;
    case CmdType::Release: e.release(c.i[0]); break;
    case CmdType::Stop: e.stop(c.i[0]); break;
    case CmdType::VoiceParam: e.setVoiceParam(c.i[0], c.i[1], c.f[0]); break;
    case CmdType::BusParam: e.setBusParam(c.i[0], c.i[1], c.f[0]); break;
    case CmdType::FxAdd:
      if (!e.addEffect(c.i[0], c.i[1], c.i[2], c.i[3])) error(kErrAddEffect, c.i[1], c.i[0]);
      break;
    case CmdType::FxRemove: e.removeEffect(c.i[0], c.i[1]); break;
    case CmdType::FxMove: e.moveEffect(c.i[0], c.i[1], c.i[2]); break;
    case CmdType::FxEnabled: e.setEffectEnabled(c.i[0], c.i[1], c.i[2] != 0); break;
    case CmdType::FxParam: e.setEffectParam(c.i[0], c.i[1], c.i[2], c.f[0]); break;
    case CmdType::FxLfo:
      if (!e.setEffectLfo(c.i[0], c.i[1], c.i[2], lfoConfig(c.i, c.f)) && c.i[3]) error(kErrEffectLfo, c.i[1], c.i[7]);
      break;
    case CmdType::InstCreate:
      if (!e.createInstrument(c.i[0], c.i[1], c.i[2])) error(kErrCreateInstrument, c.i[0], 0);
      break;
    case CmdType::InstRemove: e.removeInstrument(c.i[0]); break;
    case CmdType::InstNoteOn:
      if (!c.i[3] || static_cast<int64_t>(c.d[0]) <= blockStart)
        e.instrumentNoteOn(c.i[0], c.f[0], c.f[1], c.f[2], c.i[1], c.i[2]);
      else
        e.scheduleInstrumentNote(static_cast<int64_t>(c.d[0]), c.i[0], c.f[0], c.f[1], c.f[2], c.i[1], c.i[2]);
      break;
    case CmdType::InstNoteOff: e.instrumentNoteOff(c.i[0]); break;
    case CmdType::InstParam: e.setInstrumentParam(c.i[0], c.i[1], c.f[0]); break;
    case CmdType::InstLfo:
      if (!e.setInstrumentLfo(c.i[1], c.i[2], lfoConfig(c.i, c.f)) && c.i[3]) error(kErrInstrumentLfo, c.i[1], c.i[7]);
      break;
    case CmdType::Transport: e.scheduleTransport(static_cast<int64_t>(c.d[0]), c.d[1], c.d[2]); break;
    case CmdType::ClearRegions: e.clearSampleRegions(c.i[0]); break;
    case CmdType::AddRegion: e.addSampleRegion(c.i[0], c.i[1], c.i[2]); break;
    case CmdType::SampleParam: e.setSampleParam(c.i[0], c.i[1], c.f[0]); break;
    case CmdType::ScheduleParam:
      e.scheduleParam({static_cast<int64_t>(c.d[0]), c.i[0], c.i[1], c.i[2], c.i[3], c.f[0], c.f[1], c.i[4] != 0, c.i[5]});
      break;
    case CmdType::CancelScheduled: e.cancelScheduled(c.i[0] ? static_cast<int64_t>(c.d[0]) : blockStart); break;
    case CmdType::StartCapture: e.startCapture(c.i[0], c.i[1]); break;
    case CmdType::StopCapture:
      e.stopCapture(c.i[0]);
      readCapture(c.i[0], c.i[1]);
      break;
    case CmdType::ArmCapture:
      if (e.armCapture(c.i[0], c.i[1], static_cast<int64_t>(c.d[0]), static_cast<int64_t>(c.d[1]))) armedCaptures_[c.i[0]] = c.i[2];
      else error(kErrArmCapture, c.i[0], 0);
      break;
    case CmdType::ConfigureVoices: e.reconfigureVoices(c.i[0]); break;
    default: break;
  }
}

// Forwards the block's incoming MIDI to the page, which exposes it as a Web MIDI input. Channel voice and
// system realtime messages only (up to 3 bytes); SysEx is not forwarded. Each message carries its engine
// frame, so the page can time-stamp it exactly.
void EngineHost::forwardMidi(const juce::MidiBuffer& midi, int64_t blockStart) {
  for (const auto metadata : midi) {
    const int size = metadata.numBytes;
    const uint8_t* data = metadata.data;
    if (size < 1 || size > 3 || data[0] < 0x80 || data[0] == 0xF0) continue;
    Evt ev;
    ev.type = EvtType::Midi;
    ev.a = data[0] | (size > 1 ? data[1] << 8 : 0) | (size > 2 ? data[2] << 16 : 0);
    ev.b = size;
    ev.dbl = static_cast<double>(blockStart + metadata.samplePosition);
    postEvt(ev);
  }
}

// Watches the host playhead and posts a HostTransport event when something a follower must know changed:
// start/stop and locates/loop wraps (new epoch), tempo changes (same epoch, at most every ~43 ms), loop or
// signature edits, a page (re)connecting, and a ~170 ms refresh while playing. No allocation, no waiting.
void EngineHost::trackTransport(const HostTimeInfo& h, int64_t blockStart, int numFrames) {
  const bool playing = h.valid && h.playing;
  const double bpm = h.valid && h.bpm > 0.0 ? h.bpm : lastBpm_;
  const double ppq = h.valid ? h.ppq : 0.0;
  const bool loopOn = h.valid && h.looping && h.loopEnd > h.loopStart;
  const double loopStart = loopOn ? h.loopStart : 0.0, loopEnd = loopOn ? h.loopEnd : 0.0;

  framesSinceTransportEvent_ += numFrames;
  framesSinceTempoEvent_ += numFrames;

  bool emit = resendTransport_.exchange(false, std::memory_order_relaxed);
  if (playing != lastPlaying_) {
    epoch_++;
    emit = true;
  } else if (playing && std::fabs(ppq - expectedPpq_) > 0.02) {  // a locate or a loop wrap
    epoch_++;
    emit = true;
  }
  if (std::fabs(bpm - lastBpm_) > 0.001 && framesSinceTempoEvent_ >= 2048) emit = true;
  if (loopOn != lastLoopOn_ || loopStart != lastLoopStart_ || loopEnd != lastLoopEnd_) emit = true;
  if (h.valid && (h.sigNum != lastSigNum_ || h.sigDen != lastSigDen_)) emit = true;
  if (playing && framesSinceTransportEvent_ >= 8192) emit = true;

  if (emit) {
    Evt ev;
    ev.type = EvtType::HostTransport;
    ev.a = (playing ? 1 : 0) | (loopOn ? 2 : 0) | (h.valid ? 4 : 0);
    ev.b = h.sigNum;
    ev.c = h.sigDen;
    ev.len = epoch_;
    ev.dbl = ppq;
    ev.x[0] = bpm;
    ev.x[1] = static_cast<double>(blockStart);
    ev.x[2] = loopStart;
    ev.x[3] = loopEnd;
    postEvt(ev);
    framesSinceTransportEvent_ = 0;
    framesSinceTempoEvent_ = 0;
    lastBpm_ = bpm;
    lastLoopOn_ = loopOn;
    lastLoopStart_ = loopStart;
    lastLoopEnd_ = loopEnd;
    if (h.valid) {
      lastSigNum_ = h.sigNum;
      lastSigDen_ = h.sigDen;
    }
  }
  lastPlaying_ = playing;
  expectedPpq_ = playing ? ppq + static_cast<double>(numFrames) / sampleRate_ * bpm / 60.0 : ppq;
}

// Copies a finished capture's PCM out of the engine (planar, one malloc) and hands it to the message
// thread. An allocation, but only when the user finishes a resample.
void EngineHost::readCapture(int32_t captureId, int32_t resultSampleId) {
  if (!engine_) return;
  if (const webdsp::Capture* cap = engine_->capture(captureId)) {
    const int32_t channels = cap->channels(), length = cap->length();
    if (channels > 0 && length > 0) {
      auto* pcm = static_cast<float*>(std::malloc(sizeof(float) * static_cast<size_t>(channels) * static_cast<size_t>(length)));
      if (pcm) {
        for (int32_t ch = 0; ch < channels; ch++)
          std::memcpy(pcm + static_cast<size_t>(ch) * static_cast<size_t>(length), cap->channelData(ch), sizeof(float) * static_cast<size_t>(length));
        Evt ev;
        ev.type = EvtType::CaptureComplete;
        ev.a = captureId;
        ev.b = resultSampleId;
        ev.c = channels;
        ev.len = length;
        ev.p = pcm;
        if (!evts_.push(ev)) std::free(pcm);
      }
    }
  }
  engine_->discardCapture(captureId);
}

void EngineHost::process(juce::AudioBuffer<float>& buffer, const HostTimeInfo& host, const juce::MidiBuffer& midi) {
  const int numFrames = buffer.getNumSamples();
  const int64_t blockStart = frame_.load(std::memory_order_relaxed);

  Cmd cmd;
  while (cmds_.pop(cmd)) applyCmd(cmd, blockStart);
  trackTransport(host, blockStart, numFrames);
  forwardMidi(midi, blockStart);

  buffer.clear();
  if (engine_) {
    const int outChannels = juce::jmin(buffer.getNumChannels(), engine_->outputChannels());
    for (int done = 0; done < numFrames;) {
      const int chunk = juce::jmin(webdsp::kMaxRenderQuantum, numFrames - done);
      engine_->process(blockStart + done, chunk);
      for (int ch = 0; ch < outChannels; ch++)
        std::memcpy(buffer.getWritePointer(ch) + done, engine_->outputChannel(ch), sizeof(float) * static_cast<size_t>(chunk));

      for (const int32_t voice : engine_->endedVoices()) {
        Evt ev;
        ev.type = EvtType::VoiceEnded;
        ev.a = voice;
        postEvt(ev);
      }
      for (const int32_t id : engine_->finishedCaptures()) {
        auto it = armedCaptures_.find(id);
        if (it == armedCaptures_.end()) continue;
        const int32_t resultId = it->second;
        armedCaptures_.erase(it);
        readCapture(id, resultId);
      }
      done += chunk;
    }

    framesSinceMeters_ += numFrames;
    if (framesSinceMeters_ >= kMetersIntervalFrames) {
      framesSinceMeters_ = 0;
      const float* peaks = engine_->readMeters();
      bool active = false;
      for (int i = 0; i < webdsp::Engine::kMeterCount; i++)
        if (peaks[i] != 0.0f) {
          active = true;
          break;
        }
      if (active || metersWereActive_) {
        Evt ev;
        ev.type = EvtType::Meters;
        std::memcpy(ev.peaks, peaks, sizeof(ev.peaks));
        postEvt(ev);
      }
      metersWereActive_ = active;
    }

    framesSinceDiagnostics_ += numFrames;
    if (framesSinceDiagnostics_ >= kDiagnosticsIntervalFrames) {
      framesSinceDiagnostics_ = 0;
      Evt ev;
      ev.type = EvtType::Diagnostics;
      ev.a = engine_->activeVoiceCount();
      ev.b = engine_->loadedSampleCount();
      ev.dbl = static_cast<double>(engine_->sampleMemoryBytes());
      postEvt(ev);
    }
  }

  frame_.store(blockStart + numFrames, std::memory_order_release);
}

// ---------------------------------------------------------------------------------------------
// Message thread: native -> page
// ---------------------------------------------------------------------------------------------

void EngineHost::timerCallback() {
  static int tick = 0;
  Evt ev;
  juce::Array<juce::var> midiMessages;
  while (evts_.pop(ev)) {
    switch (ev.type) {
      case EvtType::Midi: {
        juce::Array<juce::var> bytes;
        for (int i = 0; i < ev.b; i++) bytes.add((ev.a >> (8 * i)) & 0xFF);
        midiMessages.add(object({{"data", bytes}, {"time", ev.dbl / sampleRate_}}));
        break;
      }
      case EvtType::DisposeEngine: delete static_cast<webdsp::Engine*>(ev.p); break;
      case EvtType::DisposeSample: delete static_cast<SamplePayload*>(ev.p); break;
      case EvtType::HostTransport: {
        const bool playing = (ev.a & 1) != 0, loopOn = (ev.a & 2) != 0, hasSig = (ev.a & 4) != 0;
        juce::var event = object(
            {{"type", "host-transport"},
             {"playing", playing},
             {"bpm", ev.x[0]},
             {"beat", ev.dbl},
             {"time", ev.x[1] / sampleRate_ + kHostLatencySeconds},
             {"loop", loopOn ? object({{"start", ev.x[2]}, {"end", ev.x[3]}}) : juce::var()},
             {"timeSignature", hasSig ? object({{"numerator", ev.b}, {"denominator", ev.c}}) : juce::var()},
             {"epoch", ev.len}});
        lastSentTransport_ = {true, playing, event};
        send(event);
        break;
      }
      case EvtType::VoiceEnded: send(object({{"type", "voice-ended"}, {"voice", ev.a}})); break;
      case EvtType::Diagnostics:
        send(object({{"type", "diagnostics"}, {"activeVoices", ev.a}, {"loadedSamples", ev.b}, {"sampleMemoryBytes", ev.dbl}}));
        break;
      case EvtType::Meters: {
        juce::Array<juce::var> peaks;
        for (float p : ev.peaks) peaks.add(static_cast<double>(p));
        send(object({{"type", "meters"}, {"peaks", peaks}}));
        break;
      }
      case EvtType::CaptureComplete: {
        auto* pcm = static_cast<float*>(ev.p);
        juce::Array<juce::var> channelData;  // base64 per channel; the shim turns them back into ArrayBuffers
        for (int ch = 0; ch < ev.c; ch++)
          channelData.add(toBase64(pcm + static_cast<size_t>(ch) * static_cast<size_t>(ev.len), sizeof(float) * static_cast<size_t>(ev.len)));
        std::free(pcm);
        send(object({{"type", "capture-complete"},
                     {"captureId", ev.a},
                     {"resultSampleId", ev.b},
                     {"channels", ev.c},
                     {"sampleRate", sampleRate_},
                     {"length", ev.len},
                     {"channelData", channelData}}));
        break;
      }
      case EvtType::Error: {
        juce::String message;
        switch (ev.a) {
          case kErrAddEffect: message = "add-effect " + juce::String(ev.b) + " on bus " + juce::String(ev.c) + " rejected"; break;
          case kErrCreateInstrument: message = "create-instrument " + juce::String(ev.b) + " rejected"; break;
          case kErrEffectLfo: message = "set-effect-lfo rejected (effect " + juce::String(ev.b) + ", target " + juce::String(ev.c) + ")"; break;
          case kErrInstrumentLfo: message = "set-instrument-lfo rejected (instrument " + juce::String(ev.b) + ", target " + juce::String(ev.c) + ")"; break;
          case kErrArmCapture: message = "arm-capture " + juce::String(ev.b) + " refused: window exceeds capture capacity"; break;
          default: message = "engine error"; break;
        }
        send(object({{"type", "error"}, {"message", message}}));
        break;
      }
    }
  }

  if (!midiMessages.isEmpty()) send(object({{"type", "midi"}, {"messages", midiMessages}}));

  const int dropped = droppedCmds_.load(std::memory_order_relaxed);
  if (dropped != reportedDropped_.load(std::memory_order_relaxed)) {
    reportedDropped_.store(dropped, std::memory_order_relaxed);
    send(object({{"type", "error"}, {"message", "command queue full: " + juce::String(dropped) + " commands dropped"}}));
  }

  // Some hosts stop calling the plugin the moment their transport stops, so no "stopped" block ever
  // arrives. If the frame counter has not moved for ~250 ms while the last transport we sent was playing,
  // tell the page it stopped.
  const int64_t frameNow = framesRendered();
  stalledTicks_ = frameNow == lastSeenFrame_ ? stalledTicks_ + 1 : 0;
  lastSeenFrame_ = frameNow;
  if (stalledTicks_ >= kTimerHz / 4 && lastSentTransport_.valid && lastSentTransport_.playing) {
    if (auto* o = lastSentTransport_.event.getDynamicObject()) o->setProperty("playing", false);
    lastSentTransport_.playing = false;
    send(lastSentTransport_.event);
  }

  if ((++tick % (kTimerHz / 30)) == 0) send(object({{"type", "__clock"}, {"frame", static_cast<double>(framesRendered())}, {"sampleRate", sampleRate_}}));
}
