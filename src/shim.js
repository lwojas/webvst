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
    if (event.type === '__clock') {
      onClock(event.frame, event.sampleRate);
      return;
    }
    if (event.type === 'ready') {
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
