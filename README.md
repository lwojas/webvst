# webvst

VST3 wrapper around the webdsp engine, hosting an unmodified web client (webseq / webtracker) in a WebView.
Independent of the web build pipelines: it consumes webdsp's header-only engine (`native/src/engine.h`) and
loads the client from a URL. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the architecture, the bridge protocol and the
build pipeline.

Status: audio, DAW transport, DAW MIDI, the project saved in the DAW session, and open/save dialogs work; tested in REAPER and
Ableton Live on macOS. Not done: closing the editor stops playback (the page sequences), native MIDI routing, release packaging.

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

    git clone --recurse-submodules https://github.com/lwojas/webvst.git    # or: git submodule update --init
    cmake -B build
    cmake --build build --target WebVST_VST3

webdsp is pinned as a submodule (`third_party/webdsp`); this repo is a built target that pulls webdsp and
the web clients rather than owning them. Bump webdsp with
`git -C third_party/webdsp fetch && git -C third_party/webdsp checkout <sha> && git add third_party/webdsp`, then run the
tests. To build against a working copy, pass `-DWEBVST_WEBDSP_DIR=../webdsp`. Webdsp's protocol is additive
(`docs/native-host.md` there); the host test fails if the plugin stops handling a webdsp command.

The bundle lands under `build/WebVST_artefacts/`. Copy it to `~/Library/Audio/Plug-Ins/VST3/` to load it in a host.

## MIDI and transport

- Host MIDI (channel messages) is posted to the page and appears as a Web MIDI input, "WebVST DAW MIDI". The shim wraps
  `navigator.requestMIDIAccess`: real ports are kept where the WebView has them (WebView2), WKWebView has none, so
  the virtual input is all there is on macOS. The name matches no controller profile, so Push/Launchpad logic
  ignores it. No outputs are offered. SysEx is not forwarded. Hardware controllers are not reachable from a
  macOS plugin WebView yet (that would be a native MIDI bridge).
- Host transport: see webdsp `docs/host-transport.md`. The plugin declares 100 ms latency (plugin delay
  compensation) so the page can start on the first beat.

## Tests

    cmake -B build -DWEBVST_BUILD_TESTS=ON
    cmake --build build --target WebVstHostTest && find build -name WebVstHostTest -type f -exec {} \;
    node --test test/        # the page shim against the real webdsp runtime, no browser

Headless: drives EngineHost with the page's JSON, checks audio, scheduling accuracy, and counts audio-thread
allocations per command type.

## DAW state (the project in the session)

The page publishes its project (opaque JSON plus sample blobs, stamped with the app and format) through
webdsp's `getHostState()`; `SessionState` keeps the latest in memory and `getStateInformation` saves it gzip-compressed
(`WVST` header, end marker), `setStateInformation` loads it and hands it to the page when it asks, or says nothing is
saved. Set `-DWEBVST_APP_ID=webseq` (or `webtracker`) to make a plugin refuse the other app's state. Saving never waits on
the page, so a session saves correctly with the editor closed. `e2e/hoststate-e2e.cjs` is a manual browser check of
the round trip against the real clients.

## Client URL

Defaults to `http://localhost:5174` (webseq `npm run dev`). Override with the `WEBVST_URL` environment variable
of the host process.
