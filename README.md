# webvst

VST3 wrapper around the webdsp engine, hosting an unmodified web client (webseq / webtracker) in a WebView.
Independent of the web build pipelines: it consumes webdsp's header-only engine (`native/src/engine.h`) and
loads the client from a URL. See the feasibility report for the architecture.

Status: audio wired. The page's unmodified `AudioRuntime` drives a native webdsp engine through a bridge.
DAW MIDI, host transport and DAW state are not routed yet.

## How it fits together

- `src/shim.js` is injected into the WebView before any page script. It replaces `AudioContext` /
  `AudioWorkletNode` with fakes whose `port` carries webdsp's HostCommand/WorkletEvent wire protocol to
  native, and whose `currentTime` is the native frame clock. `OfflineAudioContext` (decoding) is untouched.
- `src/EngineHost.*` owns one `webdsp::Engine` per plugin instance and mirrors
  `webdsp/src/worklet/engine-processor.ts`. Message thread -> fixed-size commands on an SPSC FIFO -> applied
  on the audio thread at the top of each block. Events go back the same way. The audio thread never waits
  on the WebView.
- The engine's event queues are grown on the message thread (warm-up), so scheduling does not allocate on
  the audio thread. Instrument/effect creation, voice-pool changes and finishing a capture still do;
  they are rare, user-initiated, and no worse than the WASM worklet.

## Build (macOS)

    cmake -B build -DWEBVST_WEBDSP_DIR=../webdsp
    cmake --build build --target WebVST_VST3

The bundle lands under `build/WebVST_artefacts/`. Copy it to `~/Library/Audio/Plug-Ins/VST3/` to load it in a host.

## Tests

    cmake -B build -DWEBVST_BUILD_TESTS=ON
    cmake --build build --target WebVstHostTest && find build -name WebVstHostTest -type f -exec {} \;

Headless: drives EngineHost with the page's JSON, checks audio, scheduling accuracy, and counts audio-thread
allocations per command type.

## Client URL

Defaults to `http://localhost:5174` (webseq `npm run dev`). Override with the `WEBVST_URL` environment variable
of the host process.
