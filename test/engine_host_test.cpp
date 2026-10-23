// Headless check of EngineHost: drives it with the same JSON the page sends, renders blocks as a host
// would, and checks audio, events and (the point of the SPSC design) that steady-state rendering does
// not allocate. Run via: cmake -B build -DWEBVST_BUILD_TESTS=ON && cmake --build build --target WebVstHostTest
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <new>
#include "EngineHost.h"

// ---- allocation counting (only while g_counting is set, i.e. inside the audio-thread call) ----------
static std::atomic<bool> g_counting{false};
static std::atomic<long> g_allocs{0};
void* operator new(size_t n) {
  if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    if (!(cond)) {                                \
      failures++;                                 \
      std::printf("FAIL: " __VA_ARGS__);          \
      std::printf("  (%s)\n", #cond);             \
    } else {                                      \
      std::printf("ok:   " __VA_ARGS__);          \
      std::printf("\n");                          \
    }                                             \
  } while (0)

static juce::var parse(const char* json) {
  juce::var v;
  juce::JSON::parse(juce::String(json), v);
  return v;
}

static const double kSr = 48000.0;
static const int kBlock = 512;

struct Rig {
  EngineHost host;
  std::vector<juce::var> events;
  juce::AudioBuffer<float> buf{2, kBlock};
  EngineHost::HostTimeInfo hostInfo;
  juce::MidiBuffer midiIn;
  Rig() {
    host.setEventSink([this](const juce::var& e) { events.push_back(e); });
    host.prepare(kSr);
  }
  // One host block; returns peak. `countAllocs` brackets the audio-thread call only.
  float block(bool countAllocs = false) {
    if (countAllocs) g_counting = true;
    host.process(buf, hostInfo, midiIn);
    g_counting = false;
    return buf.getMagnitude(0, kBlock);
  }
  void pump() { host.flushEvents(); }
  int count(const char* type) {
    int n = 0;
    for (auto& e : events)
      if (e["type"].toString() == type) n++;
    return n;
  }
  const juce::var* first(const char* type) {
    for (auto& e : events)
      if (e["type"].toString() == type) return &e;
    return nullptr;
  }
};

int main() {
  juce::ScopedJuceInitialiser_GUI init;
  {
    Rig r;
    r.host.handleMessage(parse(R"({"type":"__init","maxVoices":32,"outputChannels":2})"));
    const juce::var* ready = r.first("ready");
    CHECK(ready != nullptr, "init answers with a ready event");
    if (ready) {
      CHECK(static_cast<int>((*ready)["sampleRate"]) == 48000, "ready reports the host sample rate");
      CHECK((*ready)["instruments"].size() == 18, "ready lists 18 instrument kinds (got %d)", (*ready)["instruments"].size());
      CHECK((*ready)["effects"].size() == 9, "ready lists 9 effect kinds (got %d)", (*ready)["effects"].size());
      CHECK((*ready)["modulation"]["waveforms"].size() > 0, "ready lists LFO waveforms");
    }
    r.block();  // applies the engine swap

    // Instrument path: Kick (kind 0) on the master bus.
    r.host.handleMessage(parse(R"({"type":"create-instrument","instrument":1,"kind":0,"bus":0})"));
    r.host.handleMessage(parse(R"({"type":"instrument-note-on","instrument":1,"params":{"velocity":1}})"));
    float peak = r.block();
    CHECK(peak > 0.01f, "kick note-on produces audio (peak %.3f)", peak);

    // Steady state must not allocate on the audio thread: no commands pending, voice still sounding.
    long worst = 0;
    for (int i = 0; i < 20; i++) {
      g_allocs = 0;
      r.block(true);
      worst = std::max(worst, g_allocs.load());
    }
    CHECK(worst == 0, "steady-state blocks allocate nothing on the audio thread (worst %ld)", worst);

    // Per-command allocation on the audio thread, after the engine's warm-up.
    struct Case { const char* name; const char* json; };
    const Case cases[] = {
        {"instrument note-on (immediate)", R"({"type":"instrument-note-on","instrument":1,"params":{"velocity":0.8}})"},
        {"instrument note-on (scheduled)", R"({"type":"instrument-note-on","instrument":1,"params":{"velocity":0.8,"time":0.5}})"},
        {"instrument-note-off", R"({"type":"instrument-note-off","instrument":1})"},
        {"set-instrument-param", R"({"type":"set-instrument-param","instrument":1,"param":0,"value":0.5})"},
        {"set-transport", R"({"type":"set-transport","anchorTime":0.5,"bpm":128})"},
        {"schedule-params (ramp)", R"({"type":"schedule-params","events":[{"time":0.4,"target":1,"bus":0,"id":1,"param":0,"value":0.3,"ramp":0.1}]})"},
        {"cancel-scheduled", R"({"type":"cancel-scheduled"})"},
        {"set-bus-param", R"({"type":"set-bus-param","bus":0,"param":3,"value":0.9})"},
    };
    for (const Case& c : cases) {
      r.host.handleMessage(parse(c.json));
      g_allocs = 0;
      r.block(true);
      std::printf("info: %-34s allocations on audio thread: %ld\n", c.name, g_allocs.load());
      if (g_allocs.load() != 0) CHECK(false, "%s allocates on the audio thread", c.name);
    }
  }

  {  // Sample path + scheduling accuracy
    Rig r;
    r.host.handleMessage(parse(R"({"type":"__init","maxVoices":16,"outputChannels":2})"));
    r.block();
    // 1 s, mono, constant 0.5 -> base64 in two chunks to exercise offsets.
    const int len = 48000;
    std::vector<float> pcm(static_cast<size_t>(len), 0.5f);
    const size_t half = static_cast<size_t>(len / 2) * sizeof(float);
    auto b64 = [&](size_t from, size_t bytes) { return juce::Base64::toBase64(reinterpret_cast<const char*>(pcm.data()) + from, bytes); };
    r.host.handleMessage(parse(R"({"type":"sample-begin","sampleId":7,"channels":1,"sampleRate":48000,"length":48000})"));
    for (int part = 0; part < 2; part++) {
      auto* o = new juce::DynamicObject();
      o->setProperty("type", "sample-chunk");
      o->setProperty("sampleId", 7);
      o->setProperty("channel", 0);
      o->setProperty("offsetBytes", static_cast<double>(part * half));
      o->setProperty("data", b64(part * half, half));
      r.host.handleMessage(juce::var(o));
    }
    r.host.handleMessage(parse(R"({"type":"sample-end","sampleId":7})"));
    r.block();
    r.pump();

    // Schedule a trigger at exactly block 4 (frame 2048 -> t = 2048/48000).
    const double t = 2048.0 / kSr;
    juce::String cmd = "{\"type\":\"schedule\",\"events\":[{\"voice\":1,\"sampleId\":7,\"time\":" + juce::String(t, 9) + "}]}";
    r.host.handleMessage(parse(cmd.toRawUTF8()));
    // The first block after the swap already ran (frame 512) ... the sample load block was 2nd (1024).
    // So blocks now start at 1024 and 1536 (silent), then 2048 where the voice starts at sample 0.
    float p1 = r.block();  // 1024..1535
    float p2 = r.block();  // 1536..2047
    CHECK(p1 == 0.0f && p2 == 0.0f, "scheduled trigger is silent before its frame (%.4f, %.4f)", p1, p2);
    r.block();  // 2048..2559
    int firstNonZero = -1;
    for (int i = 0; i < kBlock; i++)
      if (std::fabs(r.buf.getReadPointer(0)[i]) > 1.0e-6f) {
        firstNonZero = i;
        break;
      }
    CHECK(firstNonZero >= 0 && firstNonZero <= 1, "scheduled voice starts on its frame (first non-zero sample index %d)", firstNonZero);
  }

  {  // Host transport tracking
    Rig r;
    r.host.handleMessage(parse(R"({"type":"__init","maxVoices":16,"outputChannels":2})"));
    auto transports = [&] {
      std::vector<juce::var> out;
      for (auto& e : r.events)
        if (e["type"].toString() == "host-transport") out.push_back(e);
      return out;
    };
    r.hostInfo.valid = true;
    r.hostInfo.bpm = 120.0;
    r.block();
    r.pump();
    auto t = transports();
    CHECK(t.size() == 1 && !static_cast<bool>(t[0]["playing"]), "a connecting page gets the stopped state once");

    // Start at beat 8: 120 bpm = 2 beats/s.
    r.events.clear();
    r.hostInfo.playing = true;
    r.hostInfo.ppq = 8.0;
    const double startFrame = static_cast<double>(r.host.framesRendered());
    r.block();
    r.pump();
    t = transports();
    CHECK(t.size() == 1 && static_cast<bool>(t[0]["playing"]), "start produces one playing event");
    if (t.size() == 1) {
      CHECK(std::fabs(static_cast<double>(t[0]["beat"]) - 8.0) < 1e-9, "beat is the host ppq at the block start");
      const double want = startFrame / kSr + EngineHost::kHostLatencySeconds;
      CHECK(std::fabs(static_cast<double>(t[0]["time"]) - want) < 1e-9, "time is the block frame plus the declared latency (%.4f)", want);
      CHECK(static_cast<int>(t[0]["epoch"]) >= 1, "start opens a new epoch");
    }
    const int epochAtStart = t.empty() ? 0 : static_cast<int>(t[0]["epoch"]);

    // Steady playback: no event for many blocks except the periodic refresh.
    r.events.clear();
    for (int i = 0; i < 12; i++) {
      r.hostInfo.ppq += kBlock / kSr * 2.0;
      r.block();
    }
    r.pump();
    t = transports();
    CHECK(t.size() <= 1, "steady playback sends at most the periodic refresh (%d events in 12 blocks)", static_cast<int>(t.size()));
    if (!t.empty()) CHECK(static_cast<int>(t[0]["epoch"]) == epochAtStart, "a refresh keeps the epoch");

    // Locate.
    r.events.clear();
    r.hostInfo.ppq = 32.0;
    r.block();
    r.pump();
    t = transports();
    CHECK(t.size() == 1 && static_cast<int>(t[0]["epoch"]) > epochAtStart, "a locate opens a new epoch");

    // Tempo change keeps the epoch.
    const int epochAfterLocate = t.empty() ? 0 : static_cast<int>(t[0]["epoch"]);
    for (int i = 0; i < 6; i++) {
      r.hostInfo.ppq += kBlock / kSr * 2.0;
      r.block();
    }
    r.events.clear();
    r.hostInfo.bpm = 140.0;
    r.hostInfo.ppq += kBlock / kSr * 2.0;
    r.block();
    r.pump();
    t = transports();
    CHECK(t.size() == 1 && std::fabs(static_cast<double>(t[0]["bpm"]) - 140.0) < 1e-9 && static_cast<int>(t[0]["epoch"]) == epochAfterLocate,
          "a tempo change is reported without a new epoch");

    // Loop info.
    r.events.clear();
    r.hostInfo.looping = true;
    r.hostInfo.loopStart = 16.0;
    r.hostInfo.loopEnd = 32.0;
    r.hostInfo.ppq += kBlock / kSr * 140.0 / 60.0;
    r.block();
    r.pump();
    t = transports();
    CHECK(t.size() == 1 && static_cast<bool>(t[0]["loop"].isObject()) && static_cast<double>(t[0]["loop"]["end"]) == 32.0, "loop points are reported");

    // Stop.
    r.events.clear();
    r.hostInfo.playing = false;
    r.block();
    r.pump();
    t = transports();
    CHECK(t.size() == 1 && !static_cast<bool>(t[0]["playing"]), "stop produces a stopped event");

    // Host that just stops calling process(): the message thread declares it stopped.
    r.hostInfo.playing = true;
    r.hostInfo.ppq = 4.0;
    r.block();
    r.pump();
    r.events.clear();
    for (int i = 0; i < 40; i++) r.pump();  // well over 250 ms of ticks with no audio blocks
    t = transports();
    CHECK(t.size() == 1 && !static_cast<bool>(t[0]["playing"]), "a silent host is reported stopped after ~250 ms (%d events)", static_cast<int>(t.size()));
  }

  {  // MIDI input
    Rig r;
    r.host.handleMessage(parse(R"({"type":"__init","maxVoices":16,"outputChannels":2})"));
    r.block();
    r.events.clear();
    r.midiIn.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 10);
    r.midiIn.addEvent(juce::MidiMessage::controllerEvent(1, 7, 64), 20);
    r.midiIn.addEvent(juce::MidiMessage::pitchWheel(2, 0x2000), 30);
    r.midiIn.addEvent(juce::MidiMessage::noteOff(1, 60), 400);
    const uint8_t sysex[] = {0xF0, 0x7E, 0x00, 0xF7};
    r.midiIn.addEvent(juce::MidiMessage(sysex, 4), 40);
    const double blockStart = static_cast<double>(r.host.framesRendered());
    g_allocs = 0;
    r.block(true);
    CHECK(g_allocs.load() == 0, "forwarding MIDI allocates nothing on the audio thread (%ld)", g_allocs.load());
    r.midiIn.clear();
    r.pump();
    const juce::var* m = r.first("midi");
    CHECK(m != nullptr, "MIDI is delivered as one 'midi' event");
    if (m) {
      auto* arr = (*m)["messages"].getArray();
      CHECK(arr && arr->size() == 4, "four channel messages forwarded, SysEx skipped (%d)", arr ? arr->size() : -1);
      if (arr && arr->size() == 4) {
        auto byte = [&](int i, int j) { return static_cast<int>((*(*arr)[i]["data"].getArray())[j]); };
        CHECK(byte(0, 0) == 0x90 && byte(0, 1) == 60 && byte(0, 2) == 100, "note on 0x90 60 100");
        CHECK(byte(1, 0) == 0xB0 && byte(1, 1) == 7 && byte(1, 2) == 64, "controller 0xB0 7 64");
        CHECK(byte(2, 0) == 0xE1, "pitch wheel on channel 2");
        CHECK(byte(3, 0) == 0x80 && byte(3, 1) == 60, "note off");
        const double t0 = static_cast<double>((*arr)[0]["time"]);
        CHECK(std::fabs(t0 - (blockStart + 10) / kSr) < 1e-9, "time is the sample-accurate engine time of the event");
        CHECK(static_cast<double>((*arr)[3]["time"]) > t0, "order and timing preserved");
      }
    }
  }

  {  // Protocol contract with webdsp
    const juce::File webdsp(WEBVST_WEBDSP_DIR_STR);
    const juce::String ts = webdsp.getChildFile("src/runtime/commandProtocol.ts").loadFileAsString();
    CHECK(ts.isNotEmpty(), "webdsp's commandProtocol.ts is readable at %s", WEBVST_WEBDSP_DIR_STR);

    const int unionStart = ts.indexOf("export type HostCommand =");
    const int unionEnd = ts.indexOf("export type WorkletEvent");
    CHECK(unionStart >= 0 && unionEnd > unionStart, "found the HostCommand union");
    const juce::String region = ts.substring(unionStart, unionEnd);

    // Every `{ type: "name"` in the union.
    std::vector<juce::String> commandTypes;
    for (int at = region.indexOf("type: \""); at >= 0; at = region.indexOf(at + 1, "type: \"")) {
      const int from = at + 7, to = region.indexOf(from, "\"");
      if (to > from) commandTypes.push_back(region.substring(from, to));
    }
    CHECK(commandTypes.size() >= 25, "parsed %d HostCommand types", static_cast<int>(commandTypes.size()));

    // load-sample is split into sample-begin/chunk/end by the shim; everything else must be handled by name.
    const auto& handled = EngineHost::handledMessageTypes();
    int missing = 0;
    for (const auto& name : commandTypes) {
      if (name == "load-sample") continue;
      if (std::find(handled.begin(), handled.end(), name) == handled.end()) {
        std::printf("      not handled by the plugin: %s\n", name.toRawUTF8());
        missing++;
      }
    }
    CHECK(missing == 0, "the plugin handles every HostCommand type in webdsp (%d missing)", missing);
    const juce::String shim = juce::File(WEBVST_WEBDSP_DIR_STR).getParentDirectory().getChildFile("webvst/src/shim.js").loadFileAsString();
    CHECK(shim.isEmpty() || shim.contains("load-sample"), "the shim translates load-sample");

    // And the list is true: sending a bare message of each type is not reported as unknown.
    {
      Rig r;
      r.host.handleMessage(parse(R"({"type":"__init","maxVoices":8})"));
      r.events.clear();
      for (const auto& name : handled) {
        if (name == "__init") continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("type", name);
        r.host.handleMessage(juce::var(o));
      }
      int unknown = 0;
      for (auto& e : r.events)
        if (e["type"].toString() == "error" && e["message"].toString().contains("does not understand")) unknown++;
      CHECK(unknown == 0, "every type in handledMessageTypes() is dispatched (%d reported unknown)", unknown);

      r.events.clear();
      r.host.handleMessage(parse(R"({"type":"from-the-future"})"));
      CHECK(r.count("error") == 1, "an unknown command is reported to the page");
    }

    // The ready event carries the version, and it matches webdsp's.
    const int versionAt = ts.indexOf("HOST_PROTOCOL_VERSION = ");
    const int tsVersion = versionAt >= 0 ? ts.substring(versionAt + 24).getIntValue() : -1;
    CHECK(tsVersion == EngineHost::kProtocolVersion, "plugin protocol version %d matches webdsp's %d", EngineHost::kProtocolVersion, tsVersion);
    Rig r;
    r.host.handleMessage(parse(R"({"type":"__init","maxVoices":8})"));
    const juce::var* ready = r.first("ready");
    CHECK(ready && static_cast<int>((*ready)["protocolVersion"]) == EngineHost::kProtocolVersion, "ready reports protocolVersion");
  }

  std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
