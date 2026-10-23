// Tests the page shim (src/shim.js) against a faked native backend, driving the REAL webdsp AudioRuntime
// through it: the runtime must be able to create itself, send commands, receive events and a clock, and see the
// host transport and the virtual MIDI input, with no browser. Run: node --test test/
// (webdsp is read from $WEBVST_WEBDSP_DIR, default the pinned third_party/webdsp; its committed lib/ is used.)
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const webdsp = path.resolve(process.env.WEBVST_WEBDSP_DIR ?? path.join(here, "../third_party/webdsp"));

// ---- fake JUCE backend -----------------------------------------------------------------------------------
const sent = [];
const listeners = {};
const native = (event) => listeners.webvst(event);
const READY = {
  type: "ready",
  protocolVersion: 1,
  sampleRate: 48000,
  outputChannels: 2,
  maxVoices: 64,
  renderQuantumFrames: 128,
  maxCaptureSeconds: 30,
  instruments: [{ kind: 0, id: "kick", name: "Kick", category: "drum", params: [] }],
  effects: [],
  modulation: { maxLfos: 2, waveforms: [], divisions: [], minRateHz: 0.1, maxRateHz: 20 },
  clockFrame: 96000,
};
globalThis.window = globalThis;
globalThis.__JUCE__ = {
  backend: {
    emitEvent: (_channel, message) => {
      sent.push(message);
      if (message.type === "__init") setTimeout(() => native(READY), 5);
    },
    addEventListener: (channel, fn) => ((listeners[channel] = fn), 1),
  },
};
Object.defineProperty(globalThis, "navigator", { value: {}, configurable: true });
eval(fs.readFileSync(path.join(here, "../src/shim.js"), "utf8"));

const { AudioRuntime, hasEmbeddedHost, hostBeatAt } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
const runtime = await AudioRuntime.create({ workletModuleUrl: "unused.js", maxVoices: 128 });

test("the shim marks the page as an embedded host", () => {
  assert.equal(hasEmbeddedHost(), true);
});

test("AudioRuntime creates itself through the fakes and reports the host's capabilities", () => {
  assert.equal(runtime.getCapabilities().sampleRate, 48000);
  assert.equal(runtime.getCapabilities().hostProtocolVersion, 1);
  const init = sent.find((m) => m.type === "__init");
  assert.equal(init.maxVoices, 128);
});

test("currentTime follows the native frame clock", async () => {
  const t0 = runtime.getCurrentTime();
  assert.ok(Math.abs(t0 - 2) < 0.05, `starts at the ready clock frame (2 s), got ${t0}`);
  await new Promise((r) => setTimeout(r, 200));
  const t1 = runtime.getCurrentTime();
  assert.ok(t1 - t0 > 0.15 && t1 - t0 < 0.3, `advances in real time (${t1 - t0})`);
  native({ type: "__clock", frame: 96000 + 48000 * 0.25, sampleRate: 48000 }); // native says: 2.25 s now
  assert.ok(runtime.getCurrentTime() >= t1, "never runs backwards");
});

test("commands cross in the wire format, with times in engine seconds", () => {
  sent.length = 0;
  const id = runtime.createInstrument(0, {});
  runtime.instrumentNoteOn(id, { velocity: 0.9, time: 5 });
  runtime.setTempo(120, { anchorTime: 4 });
  assert.deepEqual(
    sent.map((m) => m.type),
    ["create-instrument", "instrument-note-on", "set-transport"],
  );
  assert.equal(sent[1].params.time, 5);
});

test("host transport events reach the runtime with the mapping intact", () => {
  let got = null;
  runtime.onHostTransport((t) => (got = t));
  native({ type: "host-transport", playing: true, bpm: 120, beat: 8, time: 3, loop: null, timeSignature: { numerator: 4, denominator: 4 }, epoch: 2 });
  assert.equal(got.epoch, 2);
  assert.equal(hostBeatAt(got, 4), 10);
  assert.equal(runtime.getHostTransport().playing, true);
});

test("meters arrive as a Float32Array", () => {
  let snapshot = null;
  runtime.onMeters((m) => (snapshot = m));
  native({ type: "meters", peaks: [0.5, 0.4, 0, ...new Array(64).fill(0)] });
  assert.ok(snapshot);
});

test("the virtual MIDI input is offered through requestMIDIAccess, and delivers host MIDI", async () => {
  const access = await navigator.requestMIDIAccess({ sysex: true });
  const ports = [...access.inputs.values()];
  assert.equal(ports.length, 1);
  assert.equal(ports[0].name, "WebVST DAW MIDI");
  assert.equal(access.outputs.size, 0);

  const seen = [];
  ports[0].addEventListener("midimessage", (e) => seen.push(Array.from(e.data)));
  native({ type: "midi", messages: [{ data: [0x90, 60, 100], time: 2.5 }, { data: [0xb0, 7, 64], time: 2.6 }] });
  assert.deepEqual(seen, [[0x90, 60, 100], [0xb0, 7, 64]]);

  // A second MIDIAccess gets its own port object (like a browser), and both receive.
  const second = await navigator.requestMIDIAccess();
  const other = [...second.inputs.values()][0];
  assert.notEqual(other, ports[0]);
  const seen2 = [];
  other.onmidimessage = (e) => seen2.push(e.data[0]);
  native({ type: "midi", messages: [{ data: [0x80, 60, 0], time: 3 }] });
  assert.deepEqual(seen2, [0x80]);
  assert.equal(seen.length, 3);
});

test("real MIDI ports are kept alongside the virtual one when the WebView has Web MIDI", async () => {
  // Fresh shim instance over a fake navigator that already has requestMIDIAccess (WebView2).
  const realPort = { id: "real-1", name: "Some Keyboard", type: "input", state: "connected", addEventListener() {} };
  const real = { sysexEnabled: true, inputs: new Map([["real-1", realPort]]), outputs: new Map([["out-1", { id: "out-1", type: "output" }]]), addEventListener() {} };
  const saved = navigator.requestMIDIAccess;
  const restore = () => Object.defineProperty(navigator, "requestMIDIAccess", { configurable: true, writable: true, value: saved });
  // Re-run only the MIDI wrapper by evaluating the shim in a sandboxed global with a real implementation.
  const vm = await import("node:vm");
  const sandbox = { performance, Uint8Array, Map, Promise, Object, setTimeout, console, btoa, atob };
  sandbox.window = sandbox;
  sandbox.navigator = { requestMIDIAccess: async () => real };
  sandbox.__JUCE__ = { backend: { emitEvent() {}, addEventListener() {} } };
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(here, "../src/shim.js"), "utf8"), sandbox);
  const access = await sandbox.navigator.requestMIDIAccess();
  assert.deepEqual([...access.inputs.keys()].sort(), ["real-1", "webvst-daw-midi-in"]);
  assert.equal(access.outputs.size, 1);
  restore();
});

test("samples are sent in chunks and reassemble to the same bytes", () => {
  sent.length = 0;
  const floats = new Float32Array(100000).map((_, i) => Math.sin(i / 50));
  const channelData = [floats.buffer.slice(0)];
  // What AudioRuntime.loadSample posts after decoding. This creates a second node, which becomes the shim's
  // active one, so this test runs last.
  const node = new AudioWorkletNode({}, "webdsp-engine", { processorOptions: { maxVoices: 8 } });
  node.port.postMessage({ type: "load-sample", sample: { sampleId: 3, name: "s", channels: 1, sampleRate: 48000, length: floats.length, channelData } });
  const types = sent.map((m) => m.type);
  assert.equal(types[0], "__init");
  assert.equal(types[1], "sample-begin");
  assert.equal(types.at(-1), "sample-end");
  const chunks = sent.filter((m) => m.type === "sample-chunk");
  assert.ok(chunks.length >= 4, "400 kB in 96 kB chunks");
  const bytes = Buffer.concat(chunks.sort((a, b) => a.offsetBytes - b.offsetBytes).map((c) => Buffer.from(c.data, "base64")));
  assert.deepEqual(new Float32Array(bytes.buffer, bytes.byteOffset, bytes.length / 4).slice(0, 4), floats.slice(0, 4));
  assert.equal(bytes.length, floats.length * 4);
});

