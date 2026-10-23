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

test("host state: publish sends begin, text chunks, new blobs, then commit; unchanged blobs are not resent", async () => {
  const { getHostState } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
  const state = getHostState();
  assert.ok(state, "the shim installs the host state bridge");
  const bigText = JSON.stringify({ notes: "x".repeat(450 * 1024) });
  const sample = new Uint8Array(250 * 1024).map((_, n) => n % 251).buffer;

  sent.length = 0;
  state.publish({ app: "webseq", format: 1, data: bigText, blobs: { 7: sample } });
  const types = sent.map((m) => m.type);
  assert.equal(types[0], "state-begin");
  assert.deepEqual({ app: sent[0].app, format: sent[0].format }, { app: "webseq", format: 1 });
  assert.equal(types.filter((t) => t === "state-text").length, 3, "450 kB of text in 200 kB chunks");
  assert.equal(sent.filter((m) => m.type === "state-text").map((m) => m.data).join(""), bigText);
  assert.equal(sent.filter((m) => m.type === "state-blob-begin").length, 1);
  const bytes = Buffer.concat(sent.filter((m) => m.type === "state-blob-chunk").sort((a, b) => a.offsetBytes - b.offsetBytes).map((c) => Buffer.from(c.data, "base64")));
  assert.deepEqual(new Uint8Array(bytes), new Uint8Array(sample));
  assert.deepEqual(types.at(-1), "state-commit");
  assert.deepEqual(sent.at(-1).keys, ["7"]);

  sent.length = 0;
  state.publish({ app: "webseq", format: 1, data: "{}", blobs: { 7: sample } }); // same buffer object: not sent again
  assert.deepEqual(sent.map((m) => m.type), ["state-begin", "state-text", "state-commit"]);

  sent.length = 0;
  state.publish({ app: "webseq", format: 1, data: "{}", blobs: { 7: sample.slice(0) } }); // a different buffer under the key: sent
  assert.ok(sent.some((m) => m.type === "state-blob-begin"));

  sent.length = 0;
  state.clear();
  assert.deepEqual(sent.map((m) => m.type), ["state-clear"]);
  state.publish({ app: "webseq", format: 1, data: "{}", blobs: { 7: sample } }); // cleared: the plugin holds nothing, so resend
  assert.ok(sent.some((m) => m.type === "state-blob-begin"));
});

test("host state: onRestore asks once, reassembles the restore, serves late subscribers, and unsubscribes", async () => {
  const { getHostState } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
  const state = getHostState();
  sent.length = 0;
  const first = [];
  const off = state.onRestore((snapshot) => first.push(snapshot));
  assert.deepEqual(sent.filter((m) => m.type === "state-ready").length, 1);
  state.onRestore(() => {});
  assert.equal(sent.filter((m) => m.type === "state-ready").length, 1, "asks only once");

  const blobA = Buffer.from(Array.from({ length: 150000 }, (_, n) => (n * 7) % 256));
  native({ type: "state-restore-begin", app: "webtracker", format: 2, blobs: [{ key: "a", bytes: blobA.length }] });
  native({ type: "state-restore-text", data: '{"song":' });
  native({ type: "state-restore-text", data: '"x"}' });
  for (let at = 0; at < blobA.length; at += 96 * 1024) {
    native({ type: "state-restore-blob", key: "a", offsetBytes: at, data: blobA.subarray(at, at + 96 * 1024).toString("base64") });
  }
  assert.equal(first.length, 0, "nothing is delivered until the end");
  native({ type: "state-restore-end" });
  assert.equal(first.length, 1);
  assert.equal(first[0].app, "webtracker");
  assert.equal(first[0].format, 2);
  assert.equal(first[0].data, '{"song":"x"}');
  assert.deepEqual(Buffer.from(first[0].blobs.a), blobA);

  const late = [];
  state.onRestore((snapshot) => late.push(snapshot));
  assert.equal(late.length, 1, "a subscriber that arrives later still gets it");

  off();
  native({ type: "state-restore-begin", app: "webtracker", format: 2, blobs: [] });
  native({ type: "state-restore-text", data: "{}" });
  native({ type: "state-restore-end" });
  assert.equal(first.length, 1, "unsubscribed");
  assert.equal(late.length, 2);

  state.publish({ app: "webtracker", format: 2, data: "{}", blobs: {} }); // the app's own state replaces what the host handed back
  const afterPublish = [];
  state.onRestore((snapshot) => afterPublish.push(snapshot));
  assert.deepEqual(afterPublish, [null], "the plugin has answered; the app's own state is not 'restored' state");
});

test("host state: the plugin answering 'nothing saved' reaches subscribers as null, early and late", async () => {
  const { getHostState } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
  const state = getHostState();
  const seen = [];
  state.onRestore((snapshot) => seen.push(snapshot));
  native({ type: "state-restore-none" });
  assert.equal(seen.at(-1), null);
  const late = [];
  state.onRestore((snapshot) => late.push(snapshot));
  assert.deepEqual(late, [null]);
});

test("host files: saveFile sends the bytes in chunks and resolves with the saved name, null on cancel, rejects on error", async () => {
  const { getHostFiles } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
  const files = getHostFiles();
  assert.ok(files, "the shim installs the host files bridge");

  sent.length = 0;
  const text = JSON.stringify({ project: "x".repeat(250 * 1024) });
  const saving = files.saveFile({ suggestedName: "Song.webseq.json", data: text });
  const begin = sent.find((m) => m.type === "files-save-begin");
  assert.equal(begin.name, "Song.webseq.json");
  assert.equal(begin.bytes, Buffer.byteLength(text));
  assert.equal(sent.at(-1).type, "files-save-end");
  const body = Buffer.concat(sent.filter((m) => m.type === "files-save-chunk").sort((a, b) => a.offsetBytes - b.offsetBytes).map((c) => Buffer.from(c.data, "base64")));
  assert.equal(body.toString("utf8"), text, "text is saved as UTF-8");
  native({ type: "files-save-result", id: begin.id, name: "Song.webseq.json" });
  assert.deepEqual(await saving, { name: "Song.webseq.json" });

  sent.length = 0;
  const cancelled = files.saveFile({ suggestedName: "a.json", data: new Uint8Array([1, 2, 3]).buffer });
  const id2 = sent.find((m) => m.type === "files-save-begin").id;
  native({ type: "files-save-result", id: id2, name: null });
  assert.equal(await cancelled, null);

  sent.length = 0;
  const failing = files.saveFile({ suggestedName: "b.json", data: "x" });
  native({ type: "files-save-result", id: sent.find((m) => m.type === "files-save-begin").id, name: null, error: "could not write /x/b.json" });
  await assert.rejects(failing, /could not write/);
});

test("host files: openFile asks with the extensions, reassembles the chunks, and resolves null on cancel", async () => {
  const { getHostFiles } = await import(pathToFileURL(path.join(webdsp, "lib/index.js")).href);
  const files = getHostFiles();

  sent.length = 0;
  const opening = files.openFile({ extensions: ["json"] });
  const request = sent.find((m) => m.type === "files-open");
  assert.deepEqual(request.extensions, ["json"]);
  const content = Buffer.from(Array.from({ length: 200000 }, (_, n) => (n * 13) % 256));
  native({ type: "files-open-begin", id: request.id, name: "Song.webseq.json", bytes: content.length });
  for (let at = 0; at < content.length; at += 96 * 1024) {
    native({ type: "files-open-chunk", id: request.id, offsetBytes: at, data: content.subarray(at, at + 96 * 1024).toString("base64") });
  }
  native({ type: "files-open-end", id: request.id });
  const picked = await opening;
  assert.equal(picked.name, "Song.webseq.json");
  assert.deepEqual(Buffer.from(picked.data), content);

  sent.length = 0;
  const cancelled = files.openFile();
  native({ type: "files-open-result", id: sent.find((m) => m.type === "files-open").id, cancelled: true });
  assert.equal(await cancelled, null);
  assert.deepEqual(sent.find((m) => m.type === "files-open").extensions, []);

  sent.length = 0;
  const failing = files.openFile();
  native({ type: "files-open-result", id: sent.find((m) => m.type === "files-open").id, error: "could not read /x" });
  await assert.rejects(failing, /could not read/);
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

