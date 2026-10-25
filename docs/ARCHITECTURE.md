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
| `src/PluginProcessor.*`, `src/PluginEditor.*` | JUCE plumbing. The editor is a `WebBrowserComponent` loading the plugin's URL (per-app default, env overrides), routing page messages to `SessionState` / `HostFiles` / `EngineHost`. |
| `CMakeLists.txt` | `webvst_add_plugin(...)`: one plugin per client from the same sources (WebSeq, WebTracker); each has its own plugin ID, app stamp and default URL. |

## MIDI: two paths, one rule

- **Delivery to the page.** Every channel message is posted to the page (a virtual Web MIDI input), with its engine time. The page uses it for
  note entry, CCs, learn.
- **Playing, natively (`set-midi-route`).** The page declares what a key plays (a sample or an instrument, with unity note, chop, loop, mono,
  hold-until-key-up, gain, choke group, channel). `EngineHost` then plays incoming notes **itself**: it renders the block in segments split at each
  MIDI event and makes immediate engine calls between them, so a note starts on its own sample and a note-off can never overtake its note-on (a
  scheduled note-on plus an immediate note-off would). No WebView in the path (about 10-30 ms plus a block saved) and it keeps playing with the
  window closed. The router's voice ids start at 2^30 and are never reported to the page; the route belongs to the engine it was declared for, so a page
  reload drops it and the page declares it again. Allocation-free (counted in the host test).
- **The rule that avoids double notes:** the page plays notes itself only when `capabilities.midiRouting` is false. A plugin reports it true
  (`ready.midiRouting`), so the clients declare a route and do not audition MIDI notes from the page.

## Native sequence

An app can hand the plugin the song as a webdsp `Sequence` (`runtime.setSequence()`, see webdsp `docs/native-sequence.md`). `EngineHost` then plays it itself, on the audio thread, against the DAW's transport: each block it finds the events in that block's slice of the timeline (`ppq mod lengthBeats`, shifted by the declared latency so it lines up after plugin delay compensation), and plays them between render segments split at their frames, merged in time order with routed MIDI. So it is sample-accurate, follows locates, loops and tempo, keeps playing with the editor closed and renders in an offline bounce.

- The sequence arrives as one `set-sequence` message, is built into plain structs on the message thread and swapped in by pointer (the old one is freed on the message thread through a dispose event), so the audio thread allocates nothing.
- Voices it starts use ids from `1<<29` up and are never reported to the page. A stop or locate releases what is sounding. A mono track releases its previous voice at the next event's frame.
- The engine's beat grid (tempo-aware synths such as Pulse) is anchored by the plugin itself at the song's beat 0 and the DAW's tempo, on start, locate and tempo change.
- A replaced engine (the page reloading) drops the sequence, because its ids belonged to the old engine; the app declares it again.
- Limits: tempo is read once per block; a locate inside a block is noticed at the next; at most 1024 events fire in one block.

## What crosses the bridge (the `"webvst"` event channel)

| Direction | Messages | Contract |
| --- | --- | --- |
| page → plugin | every `HostCommand` (`trigger`, `schedule`, `create-instrument`, ...), plus `__init`, `sample-begin/chunk/end` (the shim splits `load-sample`) | webdsp `src/runtime/commandProtocol.ts`; `EngineHost::handledMessageTypes()` |
| plugin → page | every `WorkletEvent` (`ready`, `meters`, `voice-ended`, `capture-complete`, ...), `error` | same; `ready.protocolVersion` |
| plugin → page | `analysis` (spectrum tap: the latest 2048 mono samples entering the tapped effect, ~23/s, only while `set-analysis-tap` is set; copied on the audio thread under a seqlock, sent from the message thread) | webdsp `docs/spectrum.md` |
| plugin → page | `host-transport` (mapping of the DAW's beat to engine time, tempo, loop, epoch) | webdsp `docs/host-transport.md` |
| plugin → page | `midi` (channel messages with engine times) | surfaced as a Web MIDI input (for note entry, CCs, learn) |
| page → plugin | `set-midi-route` (what a key plays) | webdsp `docs/midi-routing.md`; the plugin plays notes itself, see below |
| both | `state-*` | `SessionState.h`; webdsp `docs/host-state.md` |
| both | `files-*` | `HostFiles.h`; webdsp `docs/host-files.md` |

Transport: the plugin declares **100 ms latency** (plugin delay compensation) and sends the mapping shifted that far
ahead, so the page, which hears of a start after the fact, can sound the first beat on time while the DAW delays everything
else to match. Clock: the native rendered-frame count (~30 Hz); the page extrapolates `currentTime` from it.

### A rule for anything large sent to the page

Native → page events go through JUCE's `emitEventIfBrowserIsVisible`, which serialises the event and escapes it with
`String::replace` for `\` and `'`; that is **quadratic in the number of backslashes and quotes**. Text that is itself JSON
(a saved project) is full of both: 64 K characters took ~6 s in one event (measured; it made WebTracker's window take ~4.5 s to
show its project), against 10 ms as base64. So large text goes as base64 of its UTF-8 bytes (`state-restore-text {b64}`), and binary
already does. The host test measures this cost (`juceEmitCostMs`) and fails if a restore event becomes slow. The other direction
(the page's `JSON.stringify` → native `JSON::parse`) is linear: 1.6 ms for the same text.

## Build and test

```
git clone --recurse-submodules https://github.com/lwojas/webvst.git
cmake -B build -DWEBVST_BUILD_TESTS=ON            # JUCE is fetched at configure (a few minutes the first time)
cmake --build build --target WebSeq_VST3 WebTracker_VST3   # build/WebSeq_artefacts/VST3/WebSeq.vst3, build/WebTracker_artefacts/...
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
- `-DWEBVST_WEBDSP_DIR=../webdsp` builds against a working copy.

## Day to day

- **Develop a client or webdsp as usual**, in the browser. To see it in the plugin, run the client's dev server and load
  its plugin with the URL overridden (plugins default to `https://lwojas.github.io/webseq/` and `.../webtrack/`; dev servers are
  WebSeq `http://localhost:5174`, WebTracker `http://localhost:5175`; `WEBVST_URL_WEBSEQ` /
  `WEBVST_URL_WEBTRACKER` in the host's environment override one plugin, `WEBVST_URL` any plugin without its own). Restart the dev
  server after changing the `webdsp` dependency (Vite caches it).
- **Ableton Live does not list the plugin, though REAPER does?** Look in `~/Library/Preferences/Ableton/Live <version>/PluginScanner.txt`
  for the bundle's name. `Failed to load plugin: a sealed resource is missing or invalid` is a broken code signature. A clean build used to produce one:
  JUCE signs the bundle and only then adds `Contents/Resources/moduleinfo.json` (an incremental rebuild happens to seal the
  previous copy first, which hid it). The build now re-signs the finished bundle as its last step (`CMakeLists.txt`,
  `webvst_add_plugin`), and CI verifies a clean build's signature. Check a bundle with `codesign -vv --strict <bundle>`; it must say
  "valid on disk". REAPER does not check the seal, so a broken signature only shows up in Ableton.
- **Slow window open?** Turn on the trace: set `WEBVST_TRACE=1` in the host's environment, or create the file `~/.webvst-trace`, open
  the window, and read `~/Library/Logs/WebVST/trace.log`. It puts native events (editor created, page about to load / finished
  loading, first message, engine ready, saved project sent) and the page's own marks (shim started, DOMContentLoaded, window load,
  first paint, saved project received, engine ready received, app visible and no start veil) on one millisecond clock, so the gap
  that is slow is visible. Off by default (no file is touched). Remove the file / variable when done.
- **Rebuilt the plugin and the DAW still behaves the old way?** The DAW caches plugins: replace the bundle in
  `~/Library/Audio/Plug-Ins/VST3/` with the host closed, then rescan.
- **Bump webdsp:** `git -C third_party/webdsp fetch && git -C third_party/webdsp checkout <sha> && git add third_party/webdsp`,
  run the tests. webdsp's protocol is additive (`webdsp/docs/native-host.md`), so a bump is normally just a bump.
- **A new webdsp command** needs a case in `EngineHost::handleMessage` (and `handledMessageTypes`); the host test fails until
  it has one. A new instrument/effect kind or parameter needs nothing here (the engine describes itself).
- **Add a client:** it needs no plugin code. Add one line in `CMakeLists.txt`,
  `webvst_add_plugin(Name PRODUCT "Name" CODE Nxxx APP_ID name DEFAULT_URL "http://localhost:PORT")`: its own plugin ID (a
  4-character code, unique), the app name stamped on its saved state (the plugin refuses another app's), and its default URL.
  The client then uses the seams it wants (`getHostState`, `getHostFiles`, `onHostTransport`). Both existing plugins are built
  this way, from the same sources.

## Known limits and not done yet

- With no native sequence (an older plugin, or a browser) the page's JS sequencer is the timing authority: closing the editor
  stops playback and offline bounce is unsupported. With it (see "Native sequence"), the host plays the song.
- MIDI routing covers notes (and all-notes-off); pitch bend, CCs and sustain still go to the page only.
- No hardware MIDI controllers in a macOS plugin WebView (WKWebView has no Web MIDI); no MIDI output; SysEx not forwarded.
- Release packaging is not done: see [RELEASE.md](RELEASE.md). The plugin loads its client by URL. Planned for release: a versioned snapshot of each
  client's `dist/` shipped next to the plugin (a packaging step here only), a Developer ID signature and notarization (builds are ad-hoc signed, enough to load locally), installer, Windows
  (WebView2 options, real-port MIDI) untested. Licence TBA (JUCE is AGPL or commercial).
