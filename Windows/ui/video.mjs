import { lowDelaySPS } from './sps.mjs';
export function parseFormat(bytes) {
  const b = new Uint8Array(bytes), view = new DataView(b.buffer, b.byteOffset, b.byteLength);
  if (b.length < 2 || b[0] !== 0) throw new Error('Only H.264 is supported. Disable forced HEVC on the Mac.');
  let offset = 2; const sets = [];
  for (let i = 0; i < b[1]; i++) {
    if (offset + 4 > b.length) throw new Error('Truncated video format');
    const length = view.getUint32(offset); offset += 4;
    if (!length || offset + length > b.length) throw new Error('Invalid parameter set');
    const set = b.slice(offset, offset + length); offset += length;
    sets.push((set[0] & 31) === 7 ? lowDelaySPS(set) : set);
  }
  if (offset !== b.length) throw new Error('Unexpected video format data');
  const sps = sets.find(s => (s[0] & 31) === 7), pps = sets.find(s => (s[0] & 31) === 8);
  if (!sps || sps.length < 4 || !pps) throw new Error('Missing H.264 parameter sets');
  return { sets, codec: 'avc1.' + [...sps.slice(1, 4)].map(v => v.toString(16).padStart(2, '0')).join('') };
}
// WebCodecs Annex B mode: convert the host's four-byte AVCC lengths to start codes.
export function annexB(bytes, sets = []) {
  const b = new Uint8Array(bytes), view = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const prefix = sets.reduce((n, s) => n + 4 + s.length, 0);
  const result = new Uint8Array(prefix + b.length); let out = 0;
  for (const set of sets) { result.set([0, 0, 0, 1], out); out += 4; result.set(set, out); out += set.length; }
  let offset = 0;
  while (offset < b.length) {
    if (offset + 4 > b.length) throw new Error('Truncated NAL length');
    const length = view.getUint32(offset); offset += 4;
    if (!length || offset + length > b.length) throw new Error('Truncated NAL unit');
    result.set([0, 0, 0, 1], out); out += 4;
    result.set(b.subarray(offset, offset + length), out); out += length; offset += length;
  }
  if (!b.length) throw new Error('Empty video frame');
  return result;
}

export class Video {
  constructor(canvas, api, onError, onFrame = () => {}, onStatus = () => {}) {
    this.canvas = canvas; this.context = canvas.getContext('2d', {alpha: false, desynchronized: true});
    this.api = api; this.onError = onError; this.onFrame = onFrame; this.onStatus = onStatus;
    this.pending = new Map(); this.serial = 0; this.recoveries = 0;
    this.acceleration = 'no-preference';
  }
  stopDecoder() {
    clearTimeout(this.watchdog);
    this.generation = (this.generation ?? 0) + 1;
    if (this.decoder && this.decoder.state !== 'closed') this.decoder.close();
    this.decoder = null;
  }
  close() {
    this.stopDecoder();
    for (const {id} of this.pending.values()) this.api.ack(id, 0);
    this.pending.clear(); this.waitKey = true;
  }
  configure(bytes) {
    this.close(); this.format = parseFormat(bytes);
    this.createDecoder();
  }
  createDecoder() {
    const generation = this.generation;
    this.decoder = new VideoDecoder({
      output: frame => {
        try {
          if (generation !== this.generation) return;
          const pending = this.pending.get(frame.timestamp);
          if (!pending) throw new Error('Decoder returned an unexpected frame timestamp');
          if (this.canvas.width !== frame.displayWidth || this.canvas.height !== frame.displayHeight) {
            this.canvas.width = frame.displayWidth; this.canvas.height = frame.displayHeight;
          }
          this.context.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
          if (pending) {
            const micros = (performance.now() - pending.start) * 1000;
            this.api.ack(pending.id, micros); this.pending.delete(frame.timestamp); this.onFrame(micros / 1000);
            this.recoveries = 0; this.armWatchdog();
          }
        } catch (error) { this.recover(error);
        } finally { frame.close(); }
      },
      error: error => { if (generation === this.generation) this.recover(error); }
    });
    this.decoder.configure({ codec: this.format.codec, optimizeForLatency: true, hardwareAcceleration: this.acceleration });
  }
  armWatchdog() {
    clearTimeout(this.watchdog);
    if (!this.pending.size) return;
    const generation = this.generation;
    this.watchdog = setTimeout(() => {
      if (generation !== this.generation || !this.pending.size) return;
      if (this.acceleration === 'no-preference') this.useSoftwareDecoder();
      else this.recover(new Error('Software decoder did not output a frame'));
    }, this.acceleration === 'no-preference' ? 1000 : 2000);
  }
  useSoftwareDecoder() {
    // The host permits only three frames in flight. A hardware decoder that buffers
    // four or more before output deadlocks that window, even with optimizeForLatency.
    // Replay the initial keyframe and its deltas without acknowledging undecoded data.
    const waiting = [...this.pending.entries()];
    this.acceleration = 'prefer-software';
    this.onStatus('Trying the compatibility video decoder…');
    if (!waiting.length || !waiting[0][1].key) {
      this.recover(new Error('Hardware decoder stopped producing frames')); return;
    }
    this.stopDecoder();
    try {
      this.createDecoder();
      for (const [timestamp, frame] of waiting)
        this.decoder.decode(new EncodedVideoChunk({type:frame.key ? 'key' : 'delta', timestamp, data:frame.data}));
      this.waitKey = false; this.armWatchdog();
    } catch (error) { this.recover(error); }
  }
  recover(error) {
    // Set the host's keyframe flag before freeing its flow-control window.
    this.api.keyframe();
    this.close();
    if (++this.recoveries > 3) return this.onError(`H.264 decoding failed: ${error.message}. Try 1920 × 1080 or reconnect.`);
    this.acceleration = 'prefer-software';
    try { this.createDecoder(); this.api.keyframe(); } catch (e) { this.onError(e.message); }
  }
  frame(bytes) {
    const b = new Uint8Array(bytes);
    if (b.length < 14) throw new Error('Truncated frame');
    const view = new DataView(b.buffer, b.byteOffset, b.byteLength), id = view.getUint32(0), key = !!(b[12] & 1);
    if (!this.decoder || (this.waitKey && !key)) { this.api.ack(id, 0); this.api.keyframe(); return; }
    try {
      const data = annexB(b.subarray(13), key ? this.format.sets : []);
      // Unique local timestamps also work across host display restarts and repeated PTS.
      const timestamp = ++this.serial * 16667;
      this.pending.set(timestamp, {id, start: performance.now(), key, data});
      this.decoder.decode(new EncodedVideoChunk({type: key ? 'key' : 'delta', timestamp, data}));
      this.waitKey = false;
      // Do not postpone the deadline each time an additional buffered frame arrives.
      if (this.pending.size === 1) this.armWatchdog();
    } catch (e) { this.api.ack(id, 0); this.recover(e); }
  }
}
