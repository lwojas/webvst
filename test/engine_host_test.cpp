// Headless check of EngineHost: drives it with the same JSON the page sends, renders blocks as a host
// would, and checks audio, events and (the point of the SPSC design) that steady-state rendering does
// not allocate. Run via: cmake -B build -DWEBVST_BUILD_TESTS=ON && cmake --build build --target WebVstHostTest
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
  Rig() {
    host.setEventSink([this](const juce::var& e) { events.push_back(e); });
    host.prepare(kSr);
  }
  // One host block; returns peak. `countAllocs` brackets the audio-thread call only.
  float block(bool countAllocs = false) {
    if (countAllocs) g_counting = true;
    host.process(buf, hostInfo);
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
    for (int i = 0; i < 16; i++) r.pump();  // 16 ticks with no audio blocks
    t = transports();
    CHECK(t.size() == 1 && !static_cast<bool>(t[0]["playing"]), "a silent host is reported stopped after ~250 ms (%d events)", static_cast<int>(t.size()));
  }

  std::printf(failures == 0 ? "\nALL PASSED\n" : "\n%d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}
