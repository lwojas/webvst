// Manual end-to-end check of the host-state round trip (not part of CI: it needs a browser and the two clients'
// dev servers). Start webseq and webtracker (e.g. `npx vite --port 5190` / `--port 5191`), then:
//   PLAYWRIGHT_MODULE=<path to playwright> node test/e2e/hoststate-e2e.cjs
// It loads each client in Chromium with the real shim and a fake native that implements the SessionState protocol,
// edits the BPM, checks the project reached the "host", reloads, and checks it was restored.
// End to end: real client dev servers + the real shim in Chromium, with a fake native implementing the session protocol.
const fs = require("fs");
const { chromium } = require(process.env.PLAYWRIGHT_MODULE ?? "playwright");
const SHIM = fs.readFileSync(require("path").join(__dirname, "../../src/shim.js"), "utf8");
const SEQ_URL = process.env.WEBSEQ_URL ?? "http://localhost:5190";
const TRACKER_URL = process.env.WEBTRACKER_URL ?? "http://localhost:5191";

function fakeNative() {
  const native = { state: null, pending: null, blobs: {}, pendingBlobs: {}, messages: [], page: null };
  const send = (event) => native.page.evaluate((e) => window.__juce(e), event);
  native.handle = async (m) => {
    native.messages.push(m.type);
    switch (m.type) {
      case "__init":
        await send({ type: "ready", protocolVersion: 1, sampleRate: 48000, outputChannels: 2, maxVoices: 64, renderQuantumFrames: 128, maxCaptureSeconds: 30, instruments: [], effects: [], modulation: { maxLfos: 2, waveforms: ["sine"], divisions: [], minRateHz: 0.1, maxRateHz: 20 }, clockFrame: 96000 });
        break;
      case "state-begin": native.pending = { app: m.app, format: m.format, text: "" }; native.pendingBlobs = {}; break;
      case "state-text": native.pending.text += m.data; break;
      case "state-blob-begin": native.pendingBlobs[m.key] = Buffer.alloc(m.bytes); break;
      case "state-blob-chunk": Buffer.from(m.data, "base64").copy(native.pendingBlobs[m.key], m.offsetBytes); break;
      case "state-commit": {
        const blobs = {};
        for (const k of m.keys) blobs[k] = native.pendingBlobs[k] ?? native.blobs[k];
        native.state = { ...native.pending, blobs }; native.blobs = blobs; native.pending = null; break;
      }
      case "state-clear": native.state = null; native.blobs = {}; break;
      case "state-ready":
        if (!native.state) { await send({ type: "state-restore-none" }); break; }
        await send({ type: "state-restore-begin", app: native.state.app, format: native.state.format, blobs: Object.entries(native.state.blobs).map(([key, b]) => ({ key, bytes: b.length })) });
        for (let i = 0; i < native.state.text.length; i += 100000) await send({ type: "state-restore-text", data: native.state.text.slice(i, i + 100000) });
        for (const [key, b] of Object.entries(native.state.blobs)) for (let at = 0; at < b.length; at += 96 * 1024) await send({ type: "state-restore-blob", key, offsetBytes: at, data: b.subarray(at, at + 96 * 1024).toString("base64") });
        await send({ type: "state-restore-end" });
        break;
    }
  };
  return native;
}

async function openHosted(browser, native, url) {
  const ctx = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const page = await ctx.newPage();
  native.page = page;
  const errors = [];
  page.on("pageerror", (e) => errors.push("pageerror: " + e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push("console: " + m.text()); });
  await page.exposeFunction("__nativePost", (m) => native.handle(m));
  await page.addInitScript(() => {
    window.__JUCE__ = { backend: { emitEvent: (_c, m) => window.__nativePost(JSON.parse(JSON.stringify(m))), addEventListener: (_c, fn) => { window.__juce = fn; return 1; } } };
  });
  await page.addInitScript(SHIM);
  await page.goto(url, { waitUntil: "load" });
  return { page, errors };
}

const bpmInput = (page) => page.locator("input[type=number]").first();

async function run(name, url, setBpm) {
  const browser = await chromium.launch();
  const native = fakeNative();
  console.log(`\n== ${name}`);

  // 1) new session: the host answers "nothing saved"; edit; the project must reach the host.
  let { page, errors } = await openHosted(browser, native, url);
  await page.waitForTimeout(2500);
  console.log("host state before any edit:", native.state ? "present (BAD: pristine project published)" : "none (good)");
  const before = await bpmInput(page).inputValue();
  await bpmInput(page).fill(String(setBpm));
  await page.waitForTimeout(3500);
  console.log(`bpm field ${before} -> ${setBpm}; host holds:`, native.state ? `${native.state.app} format ${native.state.format}, ${native.state.text.length} chars, blobs ${Object.keys(native.state.blobs).length}` : "NOTHING");
  await page.close();

  // 2) the DAW reopens the session: a fresh page, the same host state.
  ({ page, errors } = await openHosted(browser, native, url));
  await page.waitForTimeout(5000);
  const restored = await bpmInput(page).inputValue();
  console.log("after reload the bpm field shows:", restored, restored === String(setBpm) ? "(restored)" : "(NOT restored)");
  await page.screenshot({ path: `${process.env.OUT_DIR ?? "."}/${name}-restored.png` });
  console.log("errors:", JSON.stringify(errors));
  await page.close();
  await browser.close();
}

(async () => {
  await run("webseq", SEQ_URL, 133);
  await run("webtracker", TRACKER_URL, 137);
})();
