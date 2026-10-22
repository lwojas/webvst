# webvst

VST3 wrapper around the webdsp engine, hosting an unmodified web client (webseq / webtracker) in a WebView.
Independent of the web build pipelines: it consumes webdsp's header-only engine (`native/src/engine.h`) and
loads the client from a URL. See the feasibility report for the architecture.

Status: shell only (stereo out, MIDI in, silence, WebView editor). No engine or bridge yet.

## Build (macOS)

    cmake -B build -DWEBVST_WEBDSP_DIR=../webdsp
    cmake --build build --target WebVST_VST3

The bundle lands under `build/WebVST_artefacts/`. Copy it to `~/Library/Audio/Plug-Ins/VST3/` to load it in a host.

## Client URL

Defaults to `http://localhost:5174` (webseq `npm run dev`). Override with the `WEBVST_URL` environment variable
of the host process.
