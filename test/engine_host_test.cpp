// Headless check of EngineHost: drives it with the same JSON the page sends, renders blocks as a host
// would, and checks audio, events and (the point of the SPSC design) that steady-state rendering does
// not allocate. Run via: cmake -B build -DWEBVST_BUILD_TESTS=ON && cmake --build build --target WebVstHostTest
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <new>
#include "EngineHost.h"
#include "HostFiles.h"
#include "SessionState.h"

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

// What JUCE's WebBrowserComponent::emitEventIfBrowserIsVisible does to an event before handing it to the web view
// (juce_WebBrowserComponent.cpp, emitEvent): serialise, escape, quote. Measured here because its cost decides how big an
// event may be.
static double juceEmitCostMs(const juce::var& event) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto asString = juce::JSON::toString(event, true);
  const auto escaped = asString.replace("\\", "\\\\").replace("'", "\\'");
  const auto script = "window.__JUCE__.backend.emitByBackend(" + juce::String("webvst").quoted() + ", " + escaped.quoted('\'') + ");";
  const auto t1 = std::chrono::steady_clock::now();
  return script.isEmpty() ? -1.0 : std::chrono::duration<double, std::milli>(t1 - t0).count();
}

int main() {
  juce::ScopedJuceInitialiser_GUI init;
  {  // A saved project is JSON text, so every event carrying it is dense with quotes and backslashes.
    juce::String project = "{\"song\":{\"patterns\":[";
    for (int i = 0; i < 1100; i++) project += "{\"note\":\"C-4\",\"instrument\":0,\"velocity\":127,\"fx\":{\"a\":1}},";
    project += "{}]}}";
    for (int chunk : {static_cast<int>(project.length()), 32 * 1024, 16 * 1024, 8 * 1024, 4 * 1024, 2 * 1024}) {
      double total = 0;
      int events = 0;
      for (int at = 0; at < project.length(); at += chunk, events++) {
        auto* o = new juce::DynamicObject();
        o->setProperty("type", "state-restore-text");
        o->setProperty("data", project.substring(at, at + chunk));
        total += juceEmitCostMs(juce::var(o));
      }
      std::printf("info: %d chars of project JSON in %2d event(s) of up to %5d chars: %8.1f ms in JUCE's emit\n", project.length(), events, chunk, total);
    }
    {  // The same text as base64 (no quotes, no backslashes: nothing for JUCE's escaping to do), in one event and in 96 KB chunks.
      const auto utf8 = project.toRawUTF8();
      const size_t bytes = std::strlen(utf8);
      for (size_t chunk : {bytes, size_t{96 * 1024}}) {
        double total = 0;
        int events = 0;
        for (size_t at = 0; at < bytes; at += chunk, events++) {
          auto* o = new juce::DynamicObject();
          o->setProperty("type", "state-restore-text");
          o->setProperty("b64", juce::Base64::toBase64(utf8 + at, std::min(chunk, bytes - at)));
          total += juceEmitCostMs(juce::var(o));
        }
        std::printf("info: %zu bytes of project JSON as base64 in %d event(s): %8.1f ms in JUCE's emit\n", bytes, events, total);
      }
    }
    {  // The other direction: the page publishes the project as text chunks (JSON.stringify'd by the page, parsed natively).
      auto* o = new juce::DynamicObject();
      o->setProperty("eventId", "webvst");
      auto* payload = new juce::DynamicObject();
      payload->setProperty("type", "state-text");
      payload->setProperty("data", project);
      o->setProperty("payload", juce::var(payload));
      const auto text = juce::JSON::toString(juce::var(o), true);
      const auto t0 = std::chrono::steady_clock::now();
      juce::var parsed;
      juce::JSON::parse(text, parsed);
      const auto t1 = std::chrono::steady_clock::now();
      std::printf("info: parsing the page's %d-char publish event (%d chars with escapes): %.1f ms\n", project.length(), text.length(), std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
  }
  {  // How long the page waits on the message thread when it connects (the engine is built before `ready` is sent).
    using Clock = std::chrono::steady_clock;
    EngineHost host;
    const auto t0 = Clock::now();
    host.prepare(48000.0);
    const auto t1 = Clock::now();
    host.handleMessage(parse(R"({"type":"__init","maxVoices":128,"outputChannels":2})"));
    const auto t2 = Clock::now();
    const double prepareMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double initMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf("info: engine build + warm-up: prepare %.0f ms, page connect (__init) %.0f ms\n", prepareMs, initMs);
    CHECK(initMs < 500.0, "connecting a page does not stall the message thread (%.0f ms)", initMs);
  }
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
    const juce::String shim = juce::File(WEBVST_SHIM_PATH_STR).loadFileAsString();
    CHECK(shim.contains("load-sample"), "the shim translates load-sample");

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

  {  // Session state: the project saved in the DAW session
    auto message = [](std::initializer_list<std::pair<const char*, juce::var>> props) {
      auto* o = new juce::DynamicObject();
      for (auto& p : props) o->setProperty(p.first, p.second);
      return juce::var(o);
    };
    // What the shim does on publish(): begin, text chunks, blobs in chunks, commit.
    auto publish = [&](SessionState& s, const juce::String& app, const juce::String& text,
                       const std::map<juce::String, std::vector<uint8_t>>& blobs, const std::vector<juce::String>& resend) {
      s.handleMessage(message({{"type", "state-begin"}, {"app", app}, {"format", 3}}));
      for (int at = 0; at < text.length(); at += 1000) s.handleMessage(message({{"type", "state-text"}, {"data", text.substring(at, at + 1000)}}));
      juce::Array<juce::var> keys;
      for (const auto& [key, bytes] : blobs) {
        keys.add(key);
        if (std::find(resend.begin(), resend.end(), key) == resend.end()) continue;
        s.handleMessage(message({{"type", "state-blob-begin"}, {"key", key}, {"bytes", static_cast<double>(bytes.size())}}));
        for (size_t at = 0; at < bytes.size(); at += 5000) {
          const size_t n = std::min<size_t>(5000, bytes.size() - at);
          s.handleMessage(message({{"type", "state-blob-chunk"}, {"key", key}, {"offsetBytes", static_cast<double>(at)},
                                   {"data", juce::Base64::toBase64(bytes.data() + at, n)}}));
        }
      }
      s.handleMessage(message({{"type", "state-commit"}, {"keys", keys}}));
    };
    std::vector<uint8_t> sampleA(40000), sampleB(7000);
    for (size_t i = 0; i < sampleA.size(); i++) sampleA[i] = static_cast<uint8_t>((i * 31) ^ (i >> 3));
    for (size_t i = 0; i < sampleB.size(); i++) sampleB[i] = static_cast<uint8_t>(255 - i % 251);
    juce::String text = R"({"project":"x","notes":[)";
    for (int i = 0; i < 3000; i++) text += juce::String(i) + ",";
    text += "0]}";

    SessionState a("webseq");
    CHECK(!a.hasState(), "starts with no state");
    juce::MemoryBlock empty;
    a.serialize(empty);
    CHECK(empty.getSize() == 0, "no state saves nothing");

    publish(a, "webseq", text, {{"1", sampleA}, {"2", sampleB}}, {"1", "2"});
    CHECK(a.hasState() && a.app() == "webseq", "a committed snapshot is held");

    juce::MemoryBlock saved;
    a.serialize(saved);
    CHECK(saved.getSize() > 0 && saved.getSize() < text.length() + sampleA.size() + sampleB.size() + 200, "the saved blob is compressed (%d bytes)", static_cast<int>(saved.getSize()));

    // A re-publish that changes the text but resends no blob keeps the held blobs.
    publish(a, "webseq", text + " ", {{"1", sampleA}, {"2", sampleB}}, {});
    juce::MemoryBlock resaved;
    a.serialize(resaved);
    CHECK(resaved.getSize() > 0, "a re-publish without resending blobs still saves them");

    // A commit naming a blob that was never sent is refused and the old snapshot survives.
    {
      SessionState b("webseq");
      std::vector<juce::var> errors;
      b.setEventSink([&](const juce::var& e) { errors.push_back(e); });
      publish(b, "webseq", "{}", {{"1", sampleA}}, {"1"});
      publish(b, "webseq", "{\"new\":1}", {{"1", sampleA}, {"9", sampleB}}, {"1"});  // key 9 never sent
      CHECK(errors.size() == 1 && errors[0]["type"].toString() == "error", "a commit with a missing blob is reported");
      juce::MemoryBlock still;
      b.serialize(still);
      SessionState c("webseq");
      std::vector<juce::var> ev;
      c.setEventSink([&](const juce::var& e) { ev.push_back(e); });
      CHECK(c.restore(still.getData(), still.getSize()), "the surviving snapshot restores");
      c.handleMessage(message({{"type", "state-ready"}}));
      CHECK(ev.size() >= 3 && ev[1]["type"].toString() == "state-restore-text" && ev[1]["b64"].toString() == juce::Base64::toBase64("{}", 2), "and it is the OLD snapshot, not the refused one");
    }

    // Host load -> page restore, reassembled exactly as the shim does.
    SessionState loaded("webseq");
    std::vector<juce::var> events;
    loaded.setEventSink([&](const juce::var& e) { events.push_back(e); });
    CHECK(loaded.restore(saved.getData(), saved.getSize()), "a saved blob restores");
    CHECK(events.empty(), "nothing is sent while no page is listening");
    loaded.handleMessage(message({{"type", "state-ready"}}));
    CHECK(!events.empty() && events.front()["type"].toString() == "state-restore-begin" && events.back()["type"].toString() == "state-restore-end", "the page is sent begin ... end");
    juce::String gotText;
    std::map<juce::String, std::vector<uint8_t>> gotBlobs;
    for (auto& e : events) {
      const juce::String t = e["type"].toString();
      if (t == "state-restore-begin") {
        CHECK(static_cast<int>(e["format"]) == 3 && e["app"].toString() == "webseq", "app and format are carried");
        for (auto& b : *e["blobs"].getArray()) gotBlobs[b["key"].toString()].assign(static_cast<size_t>(static_cast<double>(b["bytes"])), 0);
      } else if (t == "state-restore-text") {
        juce::MemoryOutputStream decoded;
        juce::Base64::convertFromBase64(decoded, e["b64"].toString());
        gotText += decoded.toUTF8();
      } else if (t == "state-restore-blob") {
        juce::MemoryOutputStream out;
        juce::Base64::convertFromBase64(out, e["data"].toString());
        std::memcpy(gotBlobs[e["key"].toString()].data() + static_cast<size_t>(static_cast<double>(e["offsetBytes"])), out.getData(), out.getDataSize());
      }
    }
    CHECK(gotText == text, "the text round-trips through save, load and restore (%d chars)", gotText.length());
    {  // JUCE escapes an event's string with String::replace, quadratic in the number of backslashes/quotes: a restore must
       // never send the project text as a plain string (that took 6 s for 64 K characters in a real host).
      double worst = 0;
      for (auto& e : events) worst = std::max(worst, juceEmitCostMs(e));
      CHECK(worst < 100.0, "no restore event is slow for JUCE's emit to hand to the web view (worst %.1f ms)", worst);
    }
    CHECK(gotBlobs["1"] == sampleA && gotBlobs["2"] == sampleB, "binary blobs round-trip byte for byte");

    // A page already listening is handed a host-loaded state at once.
    events.clear();
    SessionState live("webseq");
    live.setEventSink([&](const juce::var& e) { events.push_back(e); });
    live.handleMessage(message({{"type", "state-ready"}}));
    CHECK(events.size() == 1 && events[0]["type"].toString() == "state-restore-none", "no state: the page is told so, definitively");
    events.clear();
    live.restore(saved.getData(), saved.getSize());
    CHECK(!events.empty() && events.back()["type"].toString() == "state-restore-end", "a listening page gets a host-loaded state immediately");

    {  // A realistically large, quote-dense project (what WebTracker saves): restoring it must not stall the message thread.
      juce::String big = "{\"song\":{\"patterns\":[";
      for (int i = 0; i < 1100; i++) big += "{\"note\":\"C-4\",\"instrument\":0,\"velocity\":127,\"fx\":{\"a\":1}},";
      big += "{}]}}";
      SessionState large("webseq");
      publish(large, "webseq", big, {{"1", sampleB}}, {"1"});
      std::vector<juce::var> out;
      large.setEventSink([&](const juce::var& e) { out.push_back(e); });
      large.handleMessage(message({{"type", "state-ready"}}));
      double total = 0;
      for (auto& e : out) total += juceEmitCostMs(e);
      CHECK(total < 150.0, "restoring a %d-char project costs JUCE's emit %.0f ms in all (it was 6000 ms as plain strings)", big.length(), total);
    }

    // Refusals: the other app's state, corrupt data, nothing.
    SessionState other("webtracker");
    CHECK(!other.restore(saved.getData(), saved.getSize()) && !other.hasState(), "state stamped for another app is refused");
    SessionState any;
    CHECK(any.restore(saved.getData(), saved.getSize()), "a plugin with no required app takes any state");
    CHECK(any.app() == "webseq", "and keeps the app stamp (%s)", any.app().toRawUTF8());
    juce::MemoryBlock corrupt(saved.getData(), saved.getSize() / 2);
    SessionState fresh("webseq");
    CHECK(!fresh.restore(corrupt.getData(), corrupt.getSize()) && !fresh.hasState(), "a truncated blob is refused");
    CHECK(!fresh.restore("junkjunk", 8) && !fresh.restore(nullptr, 0), "junk and empty input are refused");

    // New project.
    a.handleMessage(message({{"type", "state-clear"}}));
    juce::MemoryBlock cleared;
    a.serialize(cleared);
    CHECK(!a.hasState() && cleared.getSize() == 0, "clear drops the snapshot");
  }

  {  // Host files: the dialogs are injected, so this runs headless
    auto message = [](std::initializer_list<std::pair<const char*, juce::var>> props) {
      auto* o = new juce::DynamicObject();
      for (auto& p : props) o->setProperty(p.first, p.second);
      return juce::var(o);
    };
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("webvst-hostfiles-test");
    dir.deleteRecursively();
    dir.createDirectory();

    juce::File nextChoice;                 // what the fake dialog "picks"
    std::vector<HostFiles::ChooserRequest> requests;
    std::function<void(juce::File)> parked;  // a dialog left open
    bool park = false;
    HostFiles files([&](const HostFiles::ChooserRequest& r, std::function<void(juce::File)> done) {
      requests.push_back(r);
      if (park) parked = std::move(done);
      else done(nextChoice);
    });
    std::vector<juce::var> events;
    files.setEventSink([&](const juce::var& e) { events.push_back(e); });

    std::vector<uint8_t> payload(250000);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = static_cast<uint8_t>((i * 29) ^ (i >> 5));
    auto save = [&](int id, const juce::String& name, const std::vector<uint8_t>& bytes) {
      files.handleMessage(message({{"type", "files-save-begin"}, {"id", id}, {"name", name}, {"bytes", static_cast<double>(bytes.size())}}));
      for (size_t at = 0; at < bytes.size(); at += 96 * 1024) {
        const size_t n = std::min<size_t>(96 * 1024, bytes.size() - at);
        files.handleMessage(message({{"type", "files-save-chunk"}, {"id", id}, {"offsetBytes", static_cast<double>(at)},
                                     {"data", juce::Base64::toBase64(bytes.data() + at, n)}}));
      }
      files.handleMessage(message({{"type", "files-save-end"}, {"id", id}}));
    };

    // Save: the dialog is asked for with the suggested name and the file's extension, then the bytes land on disk.
    nextChoice = dir.getChildFile("Song.webseq.json");
    save(1, "Song.webseq.json", payload);
    CHECK(requests.size() == 1 && requests[0].save && requests[0].suggestedName == "Song.webseq.json" && requests[0].wildcard == "*.json", "save asks for a dialog named after the file (%s)", requests.empty() ? "" : requests[0].wildcard.toRawUTF8());
    CHECK(events.size() == 1 && events[0]["type"].toString() == "files-save-result" && events[0]["name"].toString() == "Song.webseq.json", "save reports the name it was saved under");
    juce::MemoryBlock onDisk;
    nextChoice.loadFileAsData(onDisk);
    CHECK(onDisk.getSize() == payload.size() && std::memcmp(onDisk.getData(), payload.data(), payload.size()) == 0, "the saved bytes are exact (%d)", static_cast<int>(onDisk.getSize()));

    // The next dialog starts where the last file was.
    events.clear();
    nextChoice = juce::File();
    save(2, "again.json", {1, 2, 3});
    CHECK(requests.back().startDirectory == dir, "the next dialog opens in the folder last used");
    CHECK(events.size() == 1 && events[0]["name"].isVoid(), "a cancelled save reports null");

    // Unwritable target: reported as an error, not a crash.
    events.clear();
    nextChoice = dir.getChildFile("no-such-folder/x.json");
    save(3, "x.json", {1});
    CHECK(events.size() == 1 && events[0]["error"].toString().isNotEmpty(), "a failed write is reported with a message");

    // Open: bytes stream back in chunks, reassemble exactly; the extension filter reaches the dialog.
    events.clear();
    nextChoice = dir.getChildFile("Song.webseq.json");
    files.handleMessage(message({{"type", "files-open"}, {"id", 4}, {"extensions", juce::Array<juce::var>{"json", ".webseq"}}}));
    CHECK(requests.back().wildcard == "*.json;*.webseq" && !requests.back().save, "open filters by the requested extensions (%s)", requests.back().wildcard.toRawUTF8());
    std::vector<uint8_t> got;
    juce::String name;
    for (auto& e : events) {
      const juce::String t = e["type"].toString();
      if (t == "files-open-begin") {
        name = e["name"].toString();
        got.assign(static_cast<size_t>(static_cast<double>(e["bytes"])), 0);
      } else if (t == "files-open-chunk") {
        juce::MemoryOutputStream out;
        juce::Base64::convertFromBase64(out, e["data"].toString());
        std::memcpy(got.data() + static_cast<size_t>(static_cast<double>(e["offsetBytes"])), out.getData(), out.getDataSize());
      }
    }
    CHECK(name == "Song.webseq.json" && got == payload && events.back()["type"].toString() == "files-open-end", "an opened file arrives byte for byte, begin ... end");

    events.clear();
    nextChoice = juce::File();
    files.handleMessage(message({{"type", "files-open"}, {"id", 5}}));
    CHECK(events.size() == 1 && static_cast<bool>(events[0]["cancelled"]), "a cancelled open reports cancelled");
    CHECK(requests.back().wildcard == "*", "no extensions means any file");

    events.clear();
    nextChoice = dir.getChildFile("missing.json");
    files.handleMessage(message({{"type", "files-open"}, {"id", 6}}));
    CHECK(events.size() == 1 && events[0]["error"].toString().isNotEmpty(), "an unreadable file is reported with a message");

    // One dialog at a time.
    events.clear();
    park = true;
    files.handleMessage(message({{"type", "files-open"}, {"id", 7}}));
    files.handleMessage(message({{"type", "files-open"}, {"id", 8}}));
    CHECK(events.size() == 1 && static_cast<int>(events[0]["id"]) == 8 && events[0]["error"].toString().contains("already open"), "a second request while a dialog is open is refused");
    park = false;
    nextChoice = juce::File();
    parked(juce::File());
    events.clear();
    files.handleMessage(message({{"type", "files-open"}, {"id", 9}}));
    CHECK(events.size() == 1 && static_cast<bool>(events[0]["cancelled"]), "and dialogs work again once it closes");

    CHECK(!files.handleMessage(message({{"type", "state-ready"}})), "other messages are not claimed");
    dir.deleteRecursively();
  }

  std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
