// VideoToolbox writes H.264 SPS without VUI bitstream_restriction. Chromium's decoders then
// assume the stream may reorder up to a full DPB (4+ frames) and hold output until it fills,
// which never happens with the host's three-frame flow-control window. Declaring
// max_num_reorder_frames = 0 is accurate (the host disables frame reordering) and makes the
// decoder output every frame as soon as it is decoded. Same technique as WebRTC's SpsVuiRewriter.

function unescape(bytes) {
  const out = []; let zeros = 0;
  for (const b of bytes) {
    if (zeros >= 2 && b === 3) { zeros = 0; continue; }
    zeros = b === 0 ? zeros + 1 : 0; out.push(b);
  }
  return out;
}
function escape(bytes) {
  const out = []; let zeros = 0;
  for (const b of bytes) {
    if (zeros >= 2 && b <= 3) { out.push(3); zeros = 0; }
    zeros = b === 0 ? zeros + 1 : 0; out.push(b);
  }
  return Uint8Array.from(out);
}

class Reader {
  constructor(bytes) { this.bytes = bytes; this.pos = 0; }
  bit() {
    if (this.pos >= this.bytes.length * 8) throw new Error('Truncated SPS');
    const b = (this.bytes[this.pos >> 3] >> (7 - (this.pos & 7))) & 1; this.pos++; return b;
  }
  bits(n) { let v = 0; for (let i = 0; i < n; i++) v = v * 2 + this.bit(); return v; }
  ue() { let zeros = 0; while (!this.bit()) if (++zeros > 31) throw new Error('Invalid SPS'); return 2 ** zeros - 1 + this.bits(zeros); }
  se() { const v = this.ue(); return v & 1 ? (v + 1) / 2 : -v / 2; }
}
class Writer {
  constructor() { this.out = []; this.pos = 0; }
  bit(b) { if (!(this.pos & 7)) this.out.push(0); if (b) this.out[this.out.length - 1] |= 0x80 >> (this.pos & 7); this.pos++; }
  bits(n, v) { for (let i = n - 1; i >= 0; i--) this.bit(Math.floor(v / 2 ** i) & 1); }
  ue(v) { const n = Math.floor(Math.log2(v + 1)); this.bits(n, 0); this.bits(n + 1, v + 1); }
  // Copy the bits a reader consumed since `from`.
  copy(r, from) { for (let p = from; p < r.pos; p++) this.bit((r.bytes[p >> 3] >> (7 - (p & 7))) & 1); }
}

function hrd(r) {
  const count = r.ue() + 1; r.bits(8);
  for (let i = 0; i < count; i++) { r.ue(); r.ue(); r.bit(); }
  r.bits(20);
}

/// Returns the SPS NAL unit (header byte included) with max_num_reorder_frames = 0, or the
/// original unit when it already says so. Tests pass `reorder = null` to strip the restriction.
export function lowDelaySPS(nal, reorder = 0) {
  const r = new Reader(unescape(nal.subarray(1))), w = new Writer();
  const profile = r.bits(8); r.bits(16); r.ue();
  if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135].includes(profile)) {
    const chroma = r.ue(); if (chroma === 3) r.bit();
    r.ue(); r.ue(); r.bit();
    if (r.bit()) for (let i = 0; i < (chroma === 3 ? 12 : 8); i++) if (r.bit()) {
      let last = 8, next = 8;
      for (let j = 0; j < (i < 6 ? 16 : 64) && next; j++) { next = (last + r.se() + 256) % 256; last = next || last; }
    }
  }
  r.ue();
  const poc = r.ue();
  if (poc === 0) r.ue();
  else if (poc === 1) { r.bit(); r.se(); r.se(); const n = r.ue(); for (let i = 0; i < n; i++) r.se(); }
  const refs = r.ue(); r.bit(); r.ue(); r.ue();
  if (!r.bit()) r.bit();
  r.bit();
  if (r.bit()) { r.ue(); r.ue(); r.ue(); r.ue(); }
  w.copy(r, 0);
  let mv = 1, bytesDenom = 2, bitsDenom = 1, mvH = 15, mvV = 15;
  const buffering = Math.max(1, refs);
  if (!r.bit()) {
    w.bit(1); w.bits(8, 0); // VUI with nothing but the restriction
  } else {
    w.bit(1); const start = r.pos;
    if (r.bit() && r.bits(8) === 255) r.bits(32);
    if (r.bit()) r.bit();
    if (r.bit()) { r.bits(4); if (r.bit()) r.bits(24); }
    if (r.bit()) { r.ue(); r.ue(); }
    if (r.bit()) { r.bits(32); r.bits(32); r.bit(); }
    const nalHRD = r.bit(); if (nalHRD) hrd(r);
    const vclHRD = r.bit(); if (vclHRD) hrd(r);
    if (nalHRD || vclHRD) r.bit();
    r.bit();
    w.copy(r, start);
    if (r.bit()) {
      mv = r.bit(); bytesDenom = r.ue(); bitsDenom = r.ue(); mvH = r.ue(); mvV = r.ue();
      if (r.ue() === reorder) return nal;
      r.ue(); // max_dec_frame_buffering: recomputed below
    }
  }
  if (reorder === null) { w.bit(0); return finish(nal, w); }
  w.bit(1); w.bit(mv); w.ue(bytesDenom); w.ue(bitsDenom); w.ue(mvH); w.ue(mvV); w.ue(reorder); w.ue(Math.max(buffering, reorder));
  return finish(nal, w);
}
function finish(nal, w) {
  w.bit(1); while (w.pos & 7) w.bit(0);
  const body = escape(w.out), result = new Uint8Array(1 + body.length);
  result[0] = nal[0]; result.set(body, 1); return result;
}
