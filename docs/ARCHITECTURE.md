# webvst: architecture and build pipeline

webvst is a **built target**, not a place where the web clients or the DSP live. It wraps webdsp's engine and loads an
unmodified web client (webseq, webtracker) in a WebView, so a project made in the browser can be used in a DAW. The
clients stay first-class standalone browser apps; webdsp stays the single source of truth for DSP. Nothing here changes
how those repos build.

## Shape

```
 DAW (REAPER, Ableton Live ...)                                     web client (loaded by URL, unmodified)
 ┌────────────────────────────────────────────────┐   WebView   ┌──────────────────────────────────────┐
 │ audio thread: processBlock(audio, MIDI, playhead)│            │ webseq / webtracker                   │
 │   EngineHost  ── SPSC cmd FIFO ──► webdsp Engine │            │   webdsp AudioRuntime (same protocol) │
 │   MIDI, transport events ◄─ SPSC event FIFO ─┐   │◄──────────►│   host seams (all null in a browser): │
 │ message thread (60/120 Hz timer, JUCE events) │   │  "webvst"  │     onHostTransport  getHostState     │
 │   SessionState · HostFiles · bridge           ◄───┘  channel   │     getHostFiles  Web MIDI input      │
 └────────────────────────────────────────────────┘             └──────────────────────────────────────┘
        src/shim.js is injected before any page script and makes the page's webdsp talk to native.
```

- **webdsp** (`third_party/webdsp`, pinned submodule): only `native/src/engine.h`, header-only, is compiled. No webdsp
  build step is used.
- **The page** needs no plugin conditionals. It uses webdsp's runtime as usual; the shim fakes `AudioContext` /
  `AudioWorkletNode` so that runtime's wire protocol reaches native, and installs the host seams on
  `globalThis.__webdspHost` (`hasEmbeddedHost()`, `getHostState()`, `getHostFiles()`; the transport rides the same wire as
  an event). In a browser those are absent and the apps do what they always did.
- **Realtime rule:** the audio thread never waits on, or calls into, the WebView. Everything crosses in fixed-size
  messages through lock-free queues (`juce::AbstractFifo`); allocation on the audio thread is counted per command type in
  the host test.

## Components

| File | Role |
| --- | --- |
| `src/shim.js` | Page side. Fake Web Audio surface, native frame clock, `requestMIDIAccess` (virtual "WebVST DAW MIDI" input; real ports kept where the WebView has them), host state bridge, host files bridge. |
| `src/EngineHost.*` | One `webdsp::Engine` per instance. Mirrors `webdsp/src/worklet/engine-processor.ts`: message thread parses a command into a fixed-size `Cmd`, the audio thread applies it at the top of a block and renders. Also host transport tracking and MIDI forwarding. |
| `src/SessionState.*` | The project in the DAW session: the page publishes an opaque, app-stamped snapshot (JSON + blobs); held in memory, saved gzip-compressed by `getStateInformation`, handed back on `state-ready`. |
| `src/HostFiles.*` | Open/save through native dialogs (injectable chooser; JUCE `FileChooser` in the plugin). |
| `src/PluginProcessor.*`, `src/PluginEditor.*` | JUCE plumbing. The editor is a `WebBrowserComponent` loading `WEBVST_URL` (default `http://localhost:5174`), routing page messages to `SessionState` / `HostFiles` / `EngineHost`. |

## What crosses the bridge (the `"webvst"` event channel)

| Direction | Messages | Contract |
| --- | --- | --- |
| page → plugin | every `HostCommand` (`trigger`, `schedule`, `create-instrument`, ...), plus `__init`, `sample-begin/chunk/end` (the shim splits `load-sample`) | webdsp `src/runtime/commandProtocol.ts`; `EngineHost::handledMessageTypes()` |
| plugin → page | every `WorkletEvent` (`ready`, `meters`, `voice-ended`, `capture-complete`, ...), `error` | same; `ready.protocolVersion` |
| plugin → page | `host-transport` (mapping of the DAW's beat to engine time, tempo, loop, epoch) | webdsp `docs/host-transport.md` |
| plugin → page | `midi` (channel messages with engine times) | surfaced as a Web MIDI input |
| both | `state-*` | `SessionState.h`; webdsp `docs/host-state.md` |
| both | `files-*` | `HostFiles.h`; webdsp `docs/host-files.md` |

Transport: the plugin declares **100 ms latency** (plugin delay compensation) and sends the mapping shifted that far
ahead, so the page, which hears of a start after the fact, can sound the first beat on time while the DAW delays everything
else to match. Clock: the native rendered-frame count (~30 Hz); the page extrapolates `currentTime` from it.

## Build and test

```
git clone --recurse-submodules https://github.com/lwojas/webvst.git
cmake -B build -DWEBVST_BUILD_TESTS=ON            # JUCE is fetched at configure (a few minutes the first time)
cmake --build build --target WebVST_VST3          # build/WebVST_artefacts/VST3/WebVST.vst3
cmake --build build --target WebVstHostTest && find build -name WebVstHostTest -type f -exec {} \;
node --test test/                                  # the shim against the real webdsp runtime, no browser
```

- **Host test** (`test/engine_host_test.cpp`, headless): engine host (audio, scheduling accuracy, allocation counts per
  command, transport, MIDI), protocol parity with webdsp (the plugin handles every `HostCommand` type; protocol versions
  match), `SessionState` (round trip, atomic commit, refusals), `HostFiles` (injected dialogs).
- **Shim tests** (`test/shim.test.mjs`): drive the real `AudioRuntime` through the shim against a faked backend.
- **Manual e2e** (`e2e/hoststate-e2e.cjs`): the real clients in Chromium with the real shim and a fake native.
- **CI** (`.github/workflows/ci.yml`): build + tests against the pinned webdsp on push/PR; a nightly job builds and tests
  against webdsp `main` to catch drift early.
- `-DWEBVST_WEBDSP_DIR=../webdsp` builds against a working copy; `-DWEBVST_APP_ID=webseq|webtracker` makes a plugin
  refuse the other app's saved state.

## Day to day

- **Develop a client or webdsp as usual**, in the browser. To see it in the plugin, run the client's dev server and load
  the plugin (default URL `http://localhost:5174`; `WEBVST_URL` in the host's environment overrides). Restart the dev server after
  changing the `webdsp` dependency (Vite caches it).
- **Rebuilt the plugin and the DAW still behaves the old way?** The DAW caches plugins: replace the bundle in
  `~/Library/Audio/Plug-Ins/VST3/` with the host closed, then rescan.
- **Bump webdsp:** `git -C third_party/webdsp fetch && git -C third_party/webdsp checkout <sha> && git add third_party/webdsp`,
  run the tests. webdsp's protocol is additive (`webdsp/docs/native-host.md`), so a bump is normally just a bump.
- **A new webdsp command** needs a case in `EngineHost::handleMessage` (and `handledMessageTypes`); the host test fails until
  it has one. A new instrument/effect kind or parameter needs nothing here (the engine describes itself).
- **Add a client:** it needs no plugin code. Give it a default URL (a second `juce_add_plugin` target with its own plugin ID
  and `WEBVST_APP_ID`), and have it use the seams it wants (`getHostState`, `getHostFiles`, `onHostTransport`).

## Known limits and not done yet

- The page's JS sequencer is the timing authority: closing the editor stops playback, tempo changes restart the schedule,
  offline bounce is unsupported. Fix: a beat-domain event list pushed to native (WebView stays UI-only).
- Live MIDI notes go DAW → native → page → engine (≈10–30 ms + a block); native note routing is planned.
- No hardware MIDI controllers in a macOS plugin WebView (WKWebView has no Web MIDI); no MIDI output; SysEx not forwarded.
- Release packaging is not done: the plugin loads its client by URL. Planned for release: a versioned snapshot of each
  client's `dist/` shipped next to the plugin (a packaging step here only), signing and notarization, installer, Windows
  (WebView2 options, real-port MIDI) untested. Licence TBA (JUCE is AGPL or commercial).
