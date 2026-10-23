// Injected by webvst at document start, before any page script. It makes the unmodified web client's
// webdsp AudioRuntime talk to the plugin's native engine instead of Web Audio:
//
//   AudioRuntime.create()  ->  new AudioContext() / audioWorklet.addModule() / new AudioWorkletNode()
//
// are replaced by fakes whose node.port.postMessage(cmd) sends the same HostCommand wire protocol to
// native, and whose node.port.onmessage receives the same WorkletEvents back. ctx.currentTime is the
// native engine's frame clock. OfflineAudioContext is left alone: the clients use it only to decode.
//
// Nothing here is reachable from a normal browser: it exists only in the plugin's WebView.
(function () {
  'use strict';
  if (window.__webvstShim) return;
  var backend = window.__JUCE__ && window.__JUCE__.backend;
  if (!backend) return;
  window.__webvstShim = true;
  // Tells webdsp (hasEmbeddedHost) that the audio backend is embedded: no user gesture is needed.
  window.__webdspHost = { kind: 'native' };

  var CHANNEL = 'webvst';

  // Opt-in timing marks (the plugin sets initialisation data `trace` when tracing is on; see Trace.h). Each mark is sent to
  // native with the page clock, so one log shows native and page events together.
  var tracing = !!(window.__JUCE__.initialisationData && window.__JUCE__.initialisationData.trace && window.__JUCE__.initialisationData.trace[0]);
  function mark(name) {
    if (tracing) backend.emitEvent(CHANNEL, { type: '__trace', mark: name, t: performance.now() });
  }
  if (tracing) {
    mark('shim started (document start)');
    document.addEventListener('DOMContentLoaded', function () { mark('DOMContentLoaded'); });
    window.addEventListener('load', function () { mark('window load'); });
    try {
      new PerformanceObserver(function (list) {
        list.getEntries().forEach(function (e) { mark(e.name + ' at ' + Math.round(e.startTime) + ' ms'); });
      }).observe({ type: 'paint', buffered: true });
    } catch (e) {}
    var veilWatch = setInterval(function () {
      if (document.querySelector('.gesture-veil')) return;
      if (!document.body || document.body.children.length === 0) return;
      mark('app visible and no start veil');
      clearInterval(veilWatch);
    }, 50);
  }
  var CHUNK_BYTES = 96 * 1024; // raw PCM bytes per message; a multiple of 4

  function post(message) {
    backend.emitEvent(CHANNEL, message);
  }

  function bytesToBase64(u8) {
    var s = '';
    for (var i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
    return btoa(s);
  }

  function base64ToBuffer(b64) {
    var bin = atob(b64);
    var u8 = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
    return u8.buffer;
  }

  // ---- clock ----------------------------------------------------------------------------------
  // Native posts its rendered-frame count ~30x/s. A reading can only arrive late, so (engine time -
  // local time) measured per message underestimates the true offset; the largest recent value is the
  // best estimate. A drop of more than 100 ms means the engine stalled, so snap to it.
  var sampleRate = 48000;
  var offset = null;
  var lastTime = 0;

  function localSeconds() {
    return performance.now() / 1000;
  }

  function onClock(frame, rate) {
    if (rate) sampleRate = rate;
    var measured = frame / sampleRate - localSeconds();
    if (offset === null || measured > offset || measured < offset - 0.1) offset = measured;
    else offset += (measured - offset) * 0.002;
  }

  function currentTime() {
    if (offset === null) return 0;
    var t = localSeconds() + offset;
    if (t < lastTime) t = lastTime; // never run backwards
    lastTime = t;
    return t;
  }

  // ---- fake Web Audio surface -------------------------------------------------------------------
  var activeNode = null;

  function FakePort(node) {
    this._node = node;
    this._handler = null;
    this._backlog = [];
  }
  Object.defineProperty(FakePort.prototype, 'onmessage', {
    get: function () {
      return this._handler;
    },
    set: function (fn) {
      this._handler = fn;
      if (fn) {
        var pending = this._backlog.splice(0);
        for (var i = 0; i < pending.length; i++) fn({ data: pending[i] });
      }
    },
  });
  FakePort.prototype._deliver = function (event) {
    if (this._handler) this._handler({ data: event });
    else this._backlog.push(event);
  };
  FakePort.prototype.postMessage = function (command) {
    sendCommand(command);
  };

  function FakeNode(context, name, options) {
    this.context = context;
    this.port = new FakePort(this);
    this.numberOfInputs = 0;
    this.numberOfOutputs = 1;
    activeNode = this;
    var po = (options && options.processorOptions) || {};
    post({ type: '__init', maxVoices: po.maxVoices, outputChannels: po.outputChannels });
  }
  FakeNode.prototype.connect = function (destination) {
    return destination;
  };
  FakeNode.prototype.disconnect = function () {};

  function FakeContext() {
    this.audioWorklet = {
      addModule: function () {
        return Promise.resolve();
      },
    };
    this.destination = { channelCount: 2, maxChannelCount: 2 };
    this.state = 'running';
    this.baseLatency = 0;
    this.outputLatency = 0;
  }
  Object.defineProperty(FakeContext.prototype, 'currentTime', {
    get: currentTime,
  });
  Object.defineProperty(FakeContext.prototype, 'sampleRate', {
    get: function () {
      return sampleRate;
    },
  });
  FakeContext.prototype.resume = function () {
    return Promise.resolve();
  };
  FakeContext.prototype.suspend = function () {
    return Promise.resolve();
  };
  FakeContext.prototype.close = function () {
    return Promise.resolve();
  };

  window.AudioContext = FakeContext;
  window.webkitAudioContext = FakeContext;
  window.AudioWorkletNode = FakeNode;


  // ---- Web MIDI: the DAW's MIDI as a virtual input ------------------------------------------------
  // The page asks navigator.requestMIDIAccess() as it would in a browser. We hand back a MIDIAccess that has
  // one extra input, "WebVST DAW MIDI", fed by the host's MIDI. Real hardware ports, where the WebView has
  // them (WebView2), are kept; where it has none (WKWebView has no Web MIDI), the virtual input is all there
  // is. The port name matches no controller profile, so midi-core connects it generically and the
  // trackers' "any input" note paths take it as a keyboard. No outputs are offered (MIDI out is not routed).
  var VIRTUAL_NAME = 'WebVST DAW MIDI';
  var virtualInputs = []; // one per MIDIAccess handed out, like a browser's per-access port objects

  function makeEventTarget(owner) {
    var listeners = {};
    owner.addEventListener = function (type, fn) {
      (listeners[type] = listeners[type] || []).push(fn);
    };
    owner.removeEventListener = function (type, fn) {
      var list = listeners[type];
      if (list) listeners[type] = list.filter(function (f) {
        return f !== fn;
      });
    };
    owner.dispatchEvent = function (event) {
      var list = (listeners[event.type] || []).slice();
      var handler = owner['on' + event.type];
      if (typeof handler === 'function') handler.call(owner, event);
      for (var i = 0; i < list.length; i++) list[i].call(owner, event);
      return true;
    };
  }

  function makeVirtualInput() {
    var input = {
      id: 'webvst-daw-midi-in',
      name: VIRTUAL_NAME,
      manufacturer: 'WebVST',
      version: '1',
      type: 'input',
      state: 'connected',
      connection: 'closed',
      onmidimessage: null,
      onstatechange: null,
      open: function () {
        input.connection = 'open';
        return Promise.resolve(input);
      },
      close: function () {
        input.connection = 'closed';
        return Promise.resolve(input);
      },
    };
    makeEventTarget(input);
    virtualInputs.push(input);
    return input;
  }

  function deliverMidi(messages) {
    for (var m = 0; m < messages.length; m++) {
      var msg = messages[m];
      // The event carries the engine time of the message; the page's timestamps are on performance.now().
      var stamp = offset === null ? performance.now() : (msg.time - offset) * 1000;
      for (var i = 0; i < virtualInputs.length; i++) {
        var input = virtualInputs[i];
        input.connection = 'open'; // like a real port, listening opens it
        input.dispatchEvent({ type: 'midimessage', data: Uint8Array.from(msg.data), timeStamp: stamp, target: input, currentTarget: input });
      }
    }
  }

  function makeAccess(real) {
    var access = { sysexEnabled: real ? real.sysexEnabled : false, onstatechange: null };
    makeEventTarget(access);
    var virtualInput = makeVirtualInput();
    function rebuild() {
      var inputs = new Map();
      if (real) real.inputs.forEach(function (port, id) {
        inputs.set(id, port);
      });
      inputs.set(virtualInput.id, virtualInput);
      access.inputs = inputs;
      var outputs = new Map();
      if (real) real.outputs.forEach(function (port, id) {
        outputs.set(id, port);
      });
      access.outputs = outputs;
    }
    rebuild();
    if (real) {
      real.addEventListener('statechange', function (event) {
        rebuild();
        access.dispatchEvent({ type: 'statechange', port: event.port, target: access, currentTarget: access });
      });
    }
    return access;
  }

  var realRequest = typeof navigator.requestMIDIAccess === 'function' ? navigator.requestMIDIAccess.bind(navigator) : null;
  Object.defineProperty(navigator, 'requestMIDIAccess', {
    configurable: true,
    writable: true,
    value: function (options) {
      var start = realRequest
        ? Promise.resolve(realRequest(options)).catch(function () {
            return null; // permission refused or unavailable: the virtual input still works
          })
        : Promise.resolve(null);
      return start.then(makeAccess);
    },
  });


  // ---- host state: the project lives in the DAW session ------------------------------------------------
  // webdsp's getHostState() returns this bridge. The page hands over an opaque snapshot (its JSON text plus
  // binary blobs it refers to by key); the plugin keeps the latest in memory and saves it with the session, and
  // hands it back when the page asks (onRestore) -- also when the session is reopened or the page reloaded.
  var TEXT_CHUNK_CHARS = 200 * 1024;
  var committedBlobs = new Map(); // key -> the ArrayBuffer the plugin already holds under it
  var restoreListeners = [];
  var lastRestore = null;
  var restoreAnswered = false; // the plugin has said what it holds (a snapshot, or nothing)
  var restoreAssembly = null;
  var announcedReady = false;

  function postBlob(key, buffer) {
    var bytes = new Uint8Array(buffer);
    post({ type: 'state-blob-begin', key: key, bytes: bytes.length });
    for (var at = 0; at < bytes.length; at += CHUNK_BYTES) {
      post({ type: 'state-blob-chunk', key: key, offsetBytes: at, data: bytesToBase64(bytes.subarray(at, at + CHUNK_BYTES)) });
    }
  }

  var hostState = {
    publish: function (snapshot) {
      lastRestore = null; // whatever the host handed back is no longer the current state
      post({ type: 'state-begin', app: snapshot.app, format: snapshot.format });
      var text = snapshot.data;
      for (var at = 0; at < text.length; at += TEXT_CHUNK_CHARS) post({ type: 'state-text', data: text.slice(at, at + TEXT_CHUNK_CHARS) });
      var keys = Object.keys(snapshot.blobs || {});
      var next = new Map();
      for (var i = 0; i < keys.length; i++) {
        var key = keys[i];
        var buffer = snapshot.blobs[key];
        if (committedBlobs.get(key) !== buffer) postBlob(key, buffer); // the plugin already holds an identical one otherwise
        next.set(key, buffer);
      }
      post({ type: 'state-commit', keys: keys });
      committedBlobs = next;
    },
    clear: function () {
      lastRestore = null;
      committedBlobs = new Map();
      post({ type: 'state-clear' });
    },
    onRestore: function (fn) {
      restoreListeners.push(fn);
      if (restoreAnswered) fn(lastRestore);
      if (!announcedReady) {
        announcedReady = true;
        post({ type: 'state-ready' });
      }
      return function () {
        restoreListeners = restoreListeners.filter(function (f) {
          return f !== fn;
        });
      };
    },
  };
  window.__webdspHost.state = hostState;

  function answerRestore(snapshot) {
    mark(snapshot ? 'saved project received' : 'no saved project (answer received)');
    restoreAnswered = true;
    lastRestore = snapshot;
    restoreListeners.slice().forEach(function (fn) {
      fn(snapshot);
    });
  }

  function onStateEvent(event) {
    if (event.type === 'state-restore-none') {
      answerRestore(null);
    } else if (event.type === 'state-restore-begin') {
      restoreAssembly = { app: event.app, format: event.format, text: [], blobs: {}, bytes: {} };
      for (var i = 0; i < event.blobs.length; i++) restoreAssembly.bytes[event.blobs[i].key] = new Uint8Array(event.blobs[i].bytes);
    } else if (event.type === 'state-restore-text' && restoreAssembly) {
      restoreAssembly.text.push(event.data);
    } else if (event.type === 'state-restore-blob' && restoreAssembly) {
      var target = restoreAssembly.bytes[event.key];
      if (target) target.set(new Uint8Array(base64ToBuffer(event.data)), event.offsetBytes);
    } else if (event.type === 'state-restore-end' && restoreAssembly) {
      var blobs = {};
      Object.keys(restoreAssembly.bytes).forEach(function (key) {
        blobs[key] = restoreAssembly.bytes[key].buffer;
        committedBlobs.set(key, blobs[key]);
      });
      var snapshot = { app: restoreAssembly.app, format: restoreAssembly.format, data: restoreAssembly.text.join(''), blobs: blobs };
      restoreAssembly = null;
      answerRestore(snapshot);
    }
  }


  // ---- host files: the host's own open/save dialogs ---------------------------------------------------
  // webdsp's getHostFiles() returns this bridge. A plugin's WebView (macOS WKWebView) has no file input and ignores
  // downloads, so the page asks the plugin to show the dialog and read/write the file.
  var nextFileId = 1;
  var fileCalls = {};

  var hostFiles = {
    saveFile: function (options) {
      return new Promise(function (resolve, reject) {
        var id = nextFileId++;
        var data = options.data;
        var bytes = typeof data === 'string' ? new TextEncoder().encode(data) : new Uint8Array(data);
        fileCalls[id] = { resolve: resolve, reject: reject };
        post({ type: 'files-save-begin', id: id, name: options.suggestedName, bytes: bytes.length });
        for (var at = 0; at < bytes.length; at += CHUNK_BYTES) {
          post({ type: 'files-save-chunk', id: id, offsetBytes: at, data: bytesToBase64(bytes.subarray(at, at + CHUNK_BYTES)) });
        }
        post({ type: 'files-save-end', id: id });
      });
    },
    openFile: function (options) {
      return new Promise(function (resolve, reject) {
        var id = nextFileId++;
        fileCalls[id] = { resolve: resolve, reject: reject, assembly: null };
        post({ type: 'files-open', id: id, extensions: (options && options.extensions) || [] });
      });
    },
  };
  window.__webdspHost.files = hostFiles;

  function onFilesEvent(event) {
    var call = fileCalls[event.id];
    if (!call) return;
    if (event.type === 'files-save-result') {
      delete fileCalls[event.id];
      if (event.error) call.reject(new Error(event.error));
      else call.resolve(event.name === null || event.name === undefined ? null : { name: event.name });
    } else if (event.type === 'files-open-result') {
      delete fileCalls[event.id];
      if (event.error) call.reject(new Error(event.error));
      else call.resolve(null); // cancelled
    } else if (event.type === 'files-open-begin') {
      call.assembly = { name: event.name, bytes: new Uint8Array(event.bytes) };
    } else if (event.type === 'files-open-chunk' && call.assembly) {
      call.assembly.bytes.set(new Uint8Array(base64ToBuffer(event.data)), event.offsetBytes);
    } else if (event.type === 'files-open-end' && call.assembly) {
      delete fileCalls[event.id];
      call.resolve({ name: call.assembly.name, data: call.assembly.bytes.buffer });
    }
  }

  // ---- page -> native ---------------------------------------------------------------------------
  function sendCommand(command) {
    if (command.type === 'load-sample') return sendSample(command.sample);
    post(command);
  }

  function sendSample(sample) {
    post({
      type: 'sample-begin',
      sampleId: sample.sampleId,
      channels: sample.channels,
      sampleRate: sample.sampleRate,
      length: sample.length,
    });
    for (var ch = 0; ch < sample.channelData.length; ch++) {
      var bytes = new Uint8Array(sample.channelData[ch]);
      for (var offsetBytes = 0; offsetBytes < bytes.length; offsetBytes += CHUNK_BYTES) {
        post({
          type: 'sample-chunk',
          sampleId: sample.sampleId,
          channel: ch,
          offsetBytes: offsetBytes,
          data: bytesToBase64(bytes.subarray(offsetBytes, offsetBytes + CHUNK_BYTES)),
        });
      }
    }
    post({ type: 'sample-end', sampleId: sample.sampleId });
  }

  // ---- native -> page ---------------------------------------------------------------------------
  backend.addEventListener(CHANNEL, function (event) {
    if (!event) return;
    if (event.type && event.type.indexOf('files-') === 0) {
      onFilesEvent(event);
      return;
    }
    if (event.type && event.type.indexOf('state-restore-') === 0) {
      onStateEvent(event);
      return;
    }
    if (event.type === 'midi') {
      deliverMidi(event.messages);
      return;
    }
    if (event.type === '__clock') {
      onClock(event.frame, event.sampleRate);
      return;
    }
    if (event.type === 'ready') {
      mark('engine ready received');
      sampleRate = event.sampleRate;
      offset = null;
      onClock(event.clockFrame, event.sampleRate);
    } else if (event.type === 'meters') {
      event.peaks = Float32Array.from(event.peaks);
    } else if (event.type === 'capture-complete') {
      event.channelData = event.channelData.map(base64ToBuffer);
    }
    if (activeNode) activeNode.port._deliver(event);
  });
})();
