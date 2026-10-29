# Release build: what is left to do

Today's builds are for development: they load each client from a dev server URL and are ad-hoc signed (enough to load on
the machine that built them). Nothing below is implemented yet. This is the work, in the order it makes sense.

## 1. Ship the clients inside the plugin

Now: `WEBVST_URL_<APP>` / `WEBVST_URL`, defaulting to the GitHub Pages builds (`https://lwojas.github.io/webseq/`, `.../webtrack/`); dev servers are `http://localhost:5174` / `:5175`.

- Build each client (`npm run build` in webseq / webtracker) and copy its `dist/` into the bundle (a CMake packaging step
  here only; the clients stay unchanged and standalone). Pin the client commit like `third_party/webdsp` is pinned, so a
  release is reproducible and records which client version it contains.
- Serve the files to the WebView without a network: a JUCE resource provider (`WebBrowserComponent::Options::withResourceProvider`)
  with the dist files as the origin, instead of a `localhost` URL. Check what the clients assume about their origin
  (absolute asset paths, `base` in `vite.config`, service workers, `localStorage`/IndexedDB use) and that the shim and
  host-state bridge still attach.
- Keep the URL override for development (a debug build or env var still wins).
- Decide the version coupling: the plugin reports `protocolVersion` in `ready`, and the clients warn about an older plugin
  (`hostProtocolWarning`). A bundled release makes that a non-issue; hosted-by-URL builds still need it.
- Test with no network, with the dev servers stopped, in both DAWs.

## 2. Sign and notarize (macOS)

Builds use `codesign --sign -` (ad-hoc). For other machines:

- A Developer ID Application certificate and the hardened runtime (`codesign --options runtime`), with the entitlements the
  WebView needs (check at least JIT/`allow-jit` and network client if remote content is ever used).
- Sign every nested binary, then the bundle, last (JUCE writes `moduleinfo.json` after its own signing, which is why the
  POST_BUILD re-sign exists; keep `codesign --verify --strict` as the final step, CI already checks it).
- Notarize with `notarytool` and staple the ticket to the bundle (or to the installer).
- Test on a clean machine (or user account) after downloading the artefact through a browser, so Gatekeeper quarantine
  applies; load it in Reaper and Ableton.
- Secrets (certificate, notary credentials) go in CI secrets, never in the repo.

## 3. Package and install

- An installer (`.pkg`) that puts `WebSeq.vst3` and `WebTracker.vst3` in `/Library/Audio/Plug-Ins/VST3/`, or a signed zip with
  instructions. Uninstall notes.
- Universal binary (arm64 + x86_64) if Intel Macs are supported: `CMAKE_OSX_ARCHITECTURES`, and check the webdsp native
  engine builds for both.
- Version numbers: `project(WebVST VERSION ...)` is `0.0.1`; set the plugin version from a tag, and show the client
  snapshot version somewhere (e.g. the About/trace output).
- A release workflow in CI: tag -> build -> test (host test, shim tests, signature verify) -> sign -> notarize -> attach artefacts.

## 4. Other formats and platforms (if wanted)

- AU (macOS) and AAX are not built; VST3 only. AU is a one-line JUCE format addition but needs `auval` and a re-test of
  the signing and state handling.
- Windows: WebView2 (JUCE option and the runtime installer), the shim and file dialogs untested, MIDI through real ports,
  code signing certificate. Linux is out of scope for the WebView.

## 5. Licence

JUCE is AGPLv3 or commercial. Distributing binaries needs one of: open-sourcing the plugin under AGPL-compatible terms, or
a JUCE commercial licence. webdsp and the clients' licences also need stating. Decide before the first public release;
the repo's licence is still TBA.

## 6. Known gaps worth closing first

These are limits a user of a release would meet (see `ARCHITECTURE.md`, "Known limits"):

- Reopening the editor starts a fresh engine unless the page opts in to keeping it (`window.__webdspHost.adoptEngine = true`, see ARCHITECTURE "Keeping the engine across a reopen"); a page that does not opt in is silent until it has sent its song again.
- Pitch bend, CCs and sustain are not routed natively; no MIDI output; SysEx not forwarded; no hardware controllers in the
  macOS WebView.
- WebTracker's native sequence skips MIDI-output instruments, `bus.gain` cell automation and the poly voice limit.
- Loading a saved project over unsaved edits discards them (documented in the clients).
- Tempo is read once per block by the native sequencer.
