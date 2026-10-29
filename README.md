# webvst

VST3 wrapper around the webdsp engine, hosting an unmodified web client (webseq / webtracker) in a WebView.
Independent of the web build pipelines: it consumes webdsp's header-only engine (`native/src/engine.h`) and
loads the client from a URL. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the architecture (and [docs/RELEASE.md](docs/RELEASE.md) for what a release build still needs), the bridge protocol and the
build pipeline.

Status: audio, DAW transport and DAW MIDI are tested in REAPER and Ableton Live on macOS. The project saved in the DAW session and
the open/save dialogs are verified headless and in Chromium against the real clients, not yet in a DAW. Not done: closing the editor stops playback (the page sequences), native MIDI routing, release packaging.

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
    cmake --build build --target WebSeq_VST3 WebTracker_VST3

webdsp is pinned as a submodule (`third_party/webdsp`); this repo is a built target that pulls webdsp and
the web clients rather than owning them. Bump webdsp with
`git -C third_party/webdsp fetch && git -C third_party/webdsp checkout <sha> && git add third_party/webdsp`, then run the
tests. To build against a working copy, pass `-DWEBVST_WEBDSP_DIR=../webdsp`. Webdsp's protocol is additive
(`docs/native-host.md` there); the host test fails if the plugin stops handling a webdsp command.

Two plugins are built from the same sources, one per web client: **WebSeq** and **WebTracker**. The bundles land under
`build/WebSeq_artefacts/VST3/` and `build/WebTracker_artefacts/VST3/`; copy them to `~/Library/Audio/Plug-Ins/VST3/` to load
them in a host. Each has its own plugin ID (so the DAW lists them separately), the app name stamped on its saved state (a
plugin refuses the other app's state), and its own default URL. Pulse.8 builds a third plugin from these same sources, declared in its own repo (`vst/CMakeLists.txt` there), where the page is the client and the song plays natively.

(The earlier single "WebVST" bundle is gone; delete
`WebVST.vst3` from the plug-ins folder. Sessions saved with it do not load in the new plugins.)

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
saved. Each plugin refuses the other app's state (its `WEBVST_APP_ID`). Saving never waits on
the page, so a session saves correctly with the editor closed. `e2e/hoststate-e2e.cjs` is a manual browser check of
the round trip against the real clients.

## Client URL

Each plugin opens its hosted client by default: WebSeq `https://lwojas.github.io/webseq/`, WebTracker `https://lwojas.github.io/webtrack/`.
For development, point at a client's dev server (`npm run dev`: WebSeq `http://localhost:5174`, WebTracker `http://localhost:5175`). Override in the host process's environment: `WEBVST_URL_WEBSEQ` / `WEBVST_URL_WEBTRACKER` for one plugin,
or `WEBVST_URL` for any plugin without its own variable.
