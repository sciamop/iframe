'use strict';

// iFrame browser client. Speaks the same protocol as the iPad app over a secure WebSocket:
// each binary message is [type u8][payload], numbers big-endian (see Shared/Protocol.swift).
// Video arrives as H.264 access units (length-prefixed NALs); WebCodecs decodes them on the GPU.

const MSG = {
  welcome: 0x01, format: 0x02, frame: 0x03, stats: 0x04, pong: 0x05, authFailed: 0x06, textFocus: 0x07,
  hello: 0x10, mouseMove: 0x11, mouseButton: 0x12, scroll: 0x13, key: 0x14, text: 0x15,
  requestKeyframe: 0x16, ping: 0x17, ack: 0x18, display: 0x19,
};
const PROTOCOL_VERSION = 1;

const $ = (id) => document.getElementById(id);
const canvas = $('screen');
const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });

const state = {
  ws: null,
  streaming: false,
  userClosed: false,
  welcome: null,
  hostStats: null,
  decoder: null,
  codec: null,
  paramKey: '',
  paramPrefix: new Uint8Array(0),   // parameter sets in Annex B form, prepended to keyframes
  waitingForKeyframe: true,
  submitted: new Map(),             // frame id -> performance.now() at decode()
  lastKeyframeRequest: 0,
  frames: 0,
  decodeMs: 0,
  rttMs: 0,
  timers: [],
  heldKeys: new Set(),
  heldMods: new Map(),
  buttonsDown: new Set(),
  lastDisplay: '',
  maxFPS: 60,
};

// ---------------------------------------------------------------- binary helpers

class Writer {
  constructor(size) { this.view = new DataView(new ArrayBuffer(size)); this.offset = 0; }
  u8(v) { this.view.setUint8(this.offset, v); this.offset += 1; return this; }
  u16(v) { this.view.setUint16(this.offset, v); this.offset += 2; return this; }
  u32(v) { this.view.setUint32(this.offset, v >>> 0); this.offset += 4; return this; }
  u64(v) { this.view.setBigUint64(this.offset, BigInt(v)); this.offset += 8; return this; }
  f32(v) { this.view.setFloat32(this.offset, v); this.offset += 4; return this; }
  get bytes() { return new Uint8Array(this.view.buffer); }
}

const encodeJSON = (value) => new TextEncoder().encode(JSON.stringify(value));
const decodeJSON = (bytes) => JSON.parse(new TextDecoder().decode(bytes));
const hex2 = (n) => n.toString(16).padStart(2, '0');
const nowMicros = () => Math.round(performance.now() * 1000);

function send(type, payload = new Uint8Array(0)) {
  const ws = state.ws;
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const message = new Uint8Array(payload.length + 1);
  message[0] = type;
  message.set(payload, 1);
  ws.send(message);
}

// ---------------------------------------------------------------- connect screen

function supportProblem() {
  if (!window.isSecureContext) return 'Open this page over https:// — browsers only allow hardware video decoding on secure pages.';
  if (!('VideoDecoder' in window)) return "This browser can't decode video with WebCodecs. Use a current Chrome, Edge, Firefox or Safari.";
  return null;
}

function loadPrefs() {
  const isApple = /Mac|iPhone|iPad/.test(navigator.userAgentData?.platform || navigator.platform || navigator.userAgent);
  $('pin').value = localStorage.getItem('iframe.pin') || '';
  $('mode').value = localStorage.getItem('iframe.mode') || 'match';
  const swap = localStorage.getItem('iframe.swap');
  $('swap').checked = swap === null ? !isApple : swap === '1';
}

function savePrefs() {
  try {
    localStorage.setItem('iframe.pin', $('pin').value.trim());
    localStorage.setItem('iframe.mode', $('mode').value);
    localStorage.setItem('iframe.swap', $('swap').checked ? '1' : '0');
  } catch { /* private mode */ }
}

function browserName() {
  const ua = navigator.userAgent;
  const browser = /Edg\//.test(ua) ? 'Edge' : /Firefox\//.test(ua) ? 'Firefox' : /Chrome\//.test(ua) ? 'Chrome' : /Safari\//.test(ua) ? 'Safari' : 'Browser';
  const os = /Windows/.test(ua) ? 'Windows' : /Mac OS X/.test(ua) ? 'Mac' : /Linux/.test(ua) ? 'Linux' : '';
  return os ? `${browser} on ${os}` : browser;
}

/** Display refresh rate, so a 120/144 Hz monitor gets a 120 Hz stream. */
function measureRefreshRate() {
  return new Promise((resolve) => {
    let start, count = 0;
    const tick = (t) => {
      if (start === undefined) start = t;
      if (++count < 30) return requestAnimationFrame(tick);
      const hz = (count - 1) * 1000 / (t - start);
      resolve(hz > 100 ? 120 : 60);
    };
    requestAnimationFrame(tick);
  });
}

function displayRequest() {
  const mode = $('mode').value;
  if (mode === 'mac') return null;
  const dpr = window.devicePixelRatio || 1;
  const uiScale = mode === 'larger' ? dpr * 1.25 : mode === 'space' ? dpr * 0.8 : dpr;
  return {
    width: Math.round(window.innerWidth * dpr),
    height: Math.round(window.innerHeight * dpr),
    uiScale,
  };
}

function showError(message) {
  $('errorText').textContent = message;
  $('errorDialog').showModal();
}

// ---------------------------------------------------------------- connection

async function connect() {
  const pin = $('pin').value.trim();
  if (!pin) return;
  savePrefs();
  state.userClosed = false;
  $('connectingText').textContent = `Connecting to ${location.hostname}…`;
  $('connecting').hidden = false;
  state.maxFPS = await measureRefreshRate();

  const ws = new WebSocket(`wss://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  state.ws = ws;

  ws.onopen = () => {
    const display = displayRequest();
    state.lastDisplay = JSON.stringify(display);
    send(MSG.hello, encodeJSON({
      version: PROTOCOL_VERSION, pin, name: browserName(),
      supportsHEVC: false,      // H.264 decodes everywhere; HEVC in WebCodecs is still patchy
      maxFPS: state.maxFPS,
      display,
    }));
    state.timers.push(setInterval(tick, 1000));
  };
  ws.onmessage = (event) => handle(new Uint8Array(event.data));
  ws.onclose = () => {
    if (state.ws !== ws) return;
    const wasStreaming = state.streaming;
    teardown();
    if (!state.userClosed) {
      showError(wasStreaming ? 'Disconnected from the Mac.' : "Couldn't reach iframe-host. Is it running?");
    }
  };
}

function disconnect() {
  state.userClosed = true;
  state.ws?.close();
  teardown();
}

function teardown() {
  releaseInput();
  state.timers.forEach(clearInterval);
  state.timers = [];
  const ws = state.ws;
  state.ws = null;
  if (ws && ws.readyState <= WebSocket.OPEN) ws.close();
  if (state.decoder && state.decoder.state !== 'closed') state.decoder.close();
  state.decoder = null;
  state.codec = null;
  state.paramKey = '';
  state.submitted.clear();
  state.streaming = false;
  state.welcome = null;
  state.hostStats = null;
  $('connecting').hidden = true;
  $('stream').hidden = true;
  $('connect').hidden = false;
  if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
}

function handle(bytes) {
  const type = bytes[0];
  const payload = bytes.subarray(1);
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  switch (type) {
    case MSG.welcome:
      state.welcome = decodeJSON(payload);
      if (!state.streaming) enterStream();
      break;
    case MSG.authFailed:
      state.userClosed = true;
      teardown();
      showError('Wrong PIN. Use the PIN printed by iframe-host.');
      break;
    case MSG.format: {
      const count = payload[1];
      const sets = [];
      let offset = 2;
      for (let i = 0; i < count; i++) {
        const length = view.getUint32(offset);
        sets.push(payload.slice(offset + 4, offset + 4 + length));
        offset += 4 + length;
      }
      configureDecoder(payload[0], sets);
      break;
    }
    case MSG.frame:
      decodeFrame(view.getUint32(0), (payload[12] & 1) !== 0, payload.subarray(13));
      break;
    case MSG.stats:
      state.hostStats = decodeJSON(payload);
      break;
    case MSG.pong:
      state.rttMs = (nowMicros() - Number(view.getBigUint64(0))) / 1000;
      break;
    default:
      break;  // textFocus: desktop clients have a real keyboard
  }
}

// ---------------------------------------------------------------- video

function configureDecoder(codecId, sets) {
  if (codecId !== 0) {  // only H.264 is requested by this client
    console.warn('unexpected codec', codecId);
    return;
  }
  const sps = sets.find((s) => (s[0] & 0x1f) === 7) || sets[0];
  const codec = `avc1.${hex2(sps[1])}${hex2(sps[2])}${hex2(sps[3])}`;
  const key = codec + '|' + sets.map((s) => Array.from(s, hex2).join('')).join('|');

  // Parameter sets in Annex B form, prepended to every keyframe.
  const prefixLength = sets.reduce((n, s) => n + 4 + s.length, 0);
  const prefix = new Uint8Array(prefixLength);
  let offset = 0;
  for (const s of sets) {
    prefix.set([0, 0, 0, 1], offset);
    prefix.set(s, offset + 4);
    offset += 4 + s.length;
  }
  state.paramPrefix = prefix;

  if (key === state.paramKey && state.decoder?.state === 'configured') return;
  state.paramKey = key;
  state.codec = codec;

  if (!state.decoder || state.decoder.state === 'closed') {
    state.decoder = new VideoDecoder({ output: onDecoded, error: onDecodeError });
  }
  flushOutstandingAcks();
  const config = { codec, optimizeForLatency: true, hardwareAcceleration: 'prefer-hardware' };
  try {
    state.decoder.configure(config);
  } catch {
    delete config.hardwareAcceleration;
    state.decoder.configure(config);
  }
  state.waitingForKeyframe = true;
}

/** AVCC (4-byte lengths) → Annex B (4-byte start codes): same size, so rewrite in place. */
function toAnnexB(avcc, prefix) {
  const out = new Uint8Array(prefix.length + avcc.length);
  out.set(prefix, 0);
  out.set(avcc, prefix.length);
  let i = prefix.length;
  while (i + 4 <= out.length) {
    const length = ((out[i] << 24) | (out[i + 1] << 16) | (out[i + 2] << 8) | out[i + 3]) >>> 0;
    out[i] = 0; out[i + 1] = 0; out[i + 2] = 0; out[i + 3] = 1;
    i += 4 + length;
  }
  return out;
}

function decodeFrame(id, isKeyframe, data) {
  const decoder = state.decoder;
  if (!decoder || decoder.state !== 'configured' || (state.waitingForKeyframe && !isKeyframe)) {
    ack(id, 0);  // every frame must be acked or the host's flow control stalls
    requestKeyframe();
    return;
  }
  if (isKeyframe) state.waitingForKeyframe = false;
  state.submitted.set(id, performance.now());
  try {
    decoder.decode(new EncodedVideoChunk({
      type: isKeyframe ? 'key' : 'delta',
      timestamp: id,
      data: toAnnexB(data, isKeyframe ? state.paramPrefix : new Uint8Array(0)),
    }));
  } catch (error) {
    console.warn('decode failed', error);
    onDecodeError(error);
  }
}

function onDecoded(frame) {
  const id = frame.timestamp;
  const started = state.submitted.get(id);
  state.submitted.delete(id);
  const elapsed = started === undefined ? 0 : performance.now() - started;
  ack(id, elapsed * 1000);
  state.frames += 1;
  state.decodeMs += elapsed;

  if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
    canvas.width = frame.displayWidth;
    canvas.height = frame.displayHeight;
  }
  ctx.drawImage(frame, 0, 0);
  frame.close();
}

function onDecodeError(error) {
  console.warn('decoder error', error);
  flushOutstandingAcks();
  if (state.decoder && state.decoder.state !== 'closed') state.decoder.close();
  state.decoder = null;
  state.paramKey = '';
  state.waitingForKeyframe = true;
  requestKeyframe();
}

function flushOutstandingAcks() {
  for (const id of state.submitted.keys()) ack(id, 0);
  state.submitted.clear();
}

function ack(id, micros) {
  send(MSG.ack, new Writer(8).u32(id).u32(Math.min(Math.round(micros), 0xffffffff)).bytes);
}

function requestKeyframe() {
  const now = performance.now();
  if (now - state.lastKeyframeRequest < 250) return;
  state.lastKeyframeRequest = now;
  send(MSG.requestKeyframe);
}

function tick() {
  send(MSG.ping, new Writer(8).u64(nowMicros()).bytes);
  const w = state.welcome, h = state.hostStats;
  if (w && !$('stats').hidden) {
    const lines = [`${w.hostName} · ${w.width}×${w.height} ${w.codec === 1 ? 'HEVC' : 'H.264'} @ ${w.fps}`];
    if (h) {
      lines.push(`${h.fps.toFixed(0)} fps · ${h.mbps.toFixed(1)} Mbps (target ${h.targetMbps.toFixed(0)})`);
      lines.push(`encode ${h.encodeMs.toFixed(1)} ms · decode ${(state.frames ? state.decodeMs / state.frames : 0).toFixed(1)} ms`);
      lines.push(`net RTT ${state.rttMs.toFixed(1)} ms · frame ack ${h.latencyMs.toFixed(1)} ms`);
      if (h.dropped > 0) lines.push(`skipped ${h.dropped} frames (congestion)`);
    }
    $('stats').textContent = lines.join('\n');
  }
  state.frames = 0;
  state.decodeMs = 0;
}

// ---------------------------------------------------------------- stream screen

function enterStream() {
  state.streaming = true;
  $('connecting').hidden = true;
  $('connect').hidden = true;
  $('stream').hidden = false;
  showToolbar(true);
  clearTimeout(state.toolbarTimer);
  state.toolbarTimer = setTimeout(() => showToolbar(false), 12000);
  canvas.focus();
}

function showToolbar(visible) {
  $('toolbar').hidden = !visible;
  $('grabber').hidden = visible;
}

function toggleFullscreen() {
  if (document.fullscreenElement) {
    document.exitFullscreen().catch(() => {});
  } else {
    document.documentElement.requestFullscreen({ navigationUI: 'hide' })
      .then(() => navigator.keyboard?.lock?.().catch(() => {}))  // Chrome/Edge: capture Esc, ⌘/Win shortcuts
      .catch(() => {});
  }
}

let resizeTimer;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => {
    if (!state.streaming) return;
    const display = displayRequest();
    const key = JSON.stringify(display);
    if (!display || key === state.lastDisplay) return;
    state.lastDisplay = key;
    send(MSG.display, encodeJSON(display));
  }, 400);
});

document.addEventListener('fullscreenchange', () => {
  $('fullscreenButton').classList.toggle('active', !!document.fullscreenElement);
  if (!document.fullscreenElement) navigator.keyboard?.unlock?.();
});

// ---------------------------------------------------------------- input

/** Pointer position normalized over the video (the canvas letterboxes with object-fit: contain). */
function normalized(event) {
  const rect = canvas.getBoundingClientRect();
  const scale = Math.min(rect.width / canvas.width, rect.height / canvas.height) || 1;
  const width = canvas.width * scale, height = canvas.height * scale;
  const x = (event.clientX - rect.left - (rect.width - width) / 2) / width;
  const y = (event.clientY - rect.top - (rect.height - height) / 2) / height;
  return [Math.min(Math.max(x, 0), 1), Math.min(Math.max(y, 0), 1)];
}

const mouseButton = (button) => (button === 2 ? 1 : button === 1 ? 2 : 0);  // DOM → left/right/middle

function sendMove(event) {
  const [x, y] = normalized(event);
  send(MSG.mouseMove, new Writer(8).f32(x).f32(y).bytes);
}

function sendButton(button, down, event) {
  const [x, y] = normalized(event);
  send(MSG.mouseButton, new Writer(10).u8(button).u8(down ? 1 : 0).f32(x).f32(y).bytes);
}

canvas.addEventListener('pointermove', (event) => {
  if (!state.streaming) return;
  sendMove(event);
});

canvas.addEventListener('pointerdown', (event) => {
  if (!state.streaming) return;
  event.preventDefault();
  canvas.setPointerCapture(event.pointerId);
  const button = mouseButton(event.button);
  state.buttonsDown.add(button);
  sendMove(event);
  sendButton(button, true, event);
});

canvas.addEventListener('pointerup', (event) => {
  if (!state.streaming) return;
  const button = mouseButton(event.button);
  state.buttonsDown.delete(button);
  sendButton(button, false, event);
});

canvas.addEventListener('contextmenu', (event) => event.preventDefault());

canvas.addEventListener('wheel', (event) => {
  if (!state.streaming) return;
  event.preventDefault();
  const unit = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? window.innerHeight : 1;
  // DOM deltas are "scroll amount"; the Mac wants wheel motion (positive = content moves down).
  send(MSG.scroll, new Writer(8).f32(-event.deltaX * unit).f32(-event.deltaY * unit).bytes);
}, { passive: false });

function macKeycode(code) {
  if ($('swap').checked) {
    // Ctrl does what ⌘ does on a Mac keyboard, and the Windows/Super key becomes Control.
    const swapped = { ControlLeft: 0x37, ControlRight: 0x36, MetaLeft: 0x3B, MetaRight: 0x3E, OSLeft: 0x3B, OSRight: 0x3E };
    if (code in swapped) return swapped[code];
  }
  return MAC_KEYCODES[code];
}

function currentMods(event) {
  let mods = 0;
  for (const bit of state.heldMods.values()) mods |= bit;
  if (event?.getModifierState?.('CapsLock')) mods |= MOD_CAPS;
  return mods;
}

function sendKey(keycode, action, mods) {
  send(MSG.key, new Writer(7).u16(keycode).u8(action).u32(mods).bytes);
}

function onKey(event, down) {
  if (!state.streaming || $('errorDialog').open) return;
  const keycode = macKeycode(event.code);
  if (keycode === undefined) return;
  event.preventDefault();
  event.stopPropagation();

  const modBit = MODIFIER_BITS[keycode];
  if (modBit) {
    if (down) state.heldMods.set(keycode, modBit);
    else state.heldMods.delete(keycode);
  } else if (down) {
    state.heldKeys.add(keycode);
  } else {
    state.heldKeys.delete(keycode);
  }
  sendKey(keycode, down ? (event.repeat ? 2 : 1) : 0, currentMods(event));

  // Mac browsers swallow keyup for keys pressed while ⌘ is held; release them with ⌘.
  if (!down && modBit === MOD_COMMAND) {
    for (const held of state.heldKeys) sendKey(held, 0, currentMods(event));
    state.heldKeys.clear();
  }
}

window.addEventListener('keydown', (event) => onKey(event, true), true);
window.addEventListener('keyup', (event) => onKey(event, false), true);

function releaseInput() {
  if (state.ws?.readyState === WebSocket.OPEN) {
    for (const keycode of [...state.heldKeys, ...state.heldMods.keys()]) sendKey(keycode, 0, 0);
    for (const button of state.buttonsDown) {
      send(MSG.mouseButton, new Writer(10).u8(button).u8(0).f32(0.5).f32(0.5).bytes);
    }
  }
  state.heldKeys.clear();
  state.heldMods.clear();
  state.buttonsDown.clear();
}

window.addEventListener('blur', releaseInput);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseInput(); });

// ---------------------------------------------------------------- wiring

$('form').addEventListener('submit', (event) => { event.preventDefault(); connect(); });
$('pin').addEventListener('input', () => { $('connectButton').disabled = !$('pin').value.trim() || !!supportProblem(); });
$('cancelButton').addEventListener('click', disconnect);
$('disconnectButton').addEventListener('click', disconnect);
$('fullscreenButton').addEventListener('click', toggleFullscreen);
$('statsButton').addEventListener('click', () => {
  $('stats').hidden = !$('stats').hidden;
  $('statsButton').classList.toggle('active', !$('stats').hidden);
  tick();
});
$('hideButton').addEventListener('click', () => showToolbar(false));
$('grabber').addEventListener('click', () => showToolbar(true));

loadPrefs();
const problem = supportProblem();
if (problem) {
  $('unsupported').textContent = problem;
  $('unsupported').hidden = false;
}
$('connectButton').disabled = !$('pin').value.trim() || !!problem;
$('connectButton').textContent = `Connect to ${location.hostname}`;
