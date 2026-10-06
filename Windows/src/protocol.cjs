'use strict';
const MAX_MESSAGE = 32 << 20;
const T = Object.freeze({ welcome: 1, format: 2, frame: 3, stats: 4, pong: 5,
  authFailed: 6, textFocus: 7, cursor: 8, hello: 16, mouseMove: 17, mouseButton: 18,
  scroll: 19, key: 20, text: 21, requestKeyframe: 22, ping: 23, ack: 24, display: 25 });

function packet(type, payload = Buffer.alloc(0)) {
  if (payload.length > MAX_MESSAGE) throw new Error('Message too large');
  const header = Buffer.alloc(5);
  header[0] = type;
  header.writeUInt32BE(payload.length, 1);
  return Buffer.concat([header, payload]);
}

// Fixed header and one allocation per body, even when TCP fragments a large frame.
class Parser {
  constructor(deliver) { this.deliver = deliver; this.header = Buffer.alloc(5); this.used = 0; this.body = null; }
  push(chunk) {
    let offset = 0;
    while (offset < chunk.length) {
      const target = this.body ?? this.header;
      const count = Math.min(target.length - this.used, chunk.length - offset);
      chunk.copy(target, this.used, offset);
      this.used += count; offset += count;
      if (this.used !== target.length) continue;
      this.used = 0;
      if (this.body) {
        const body = this.body; this.body = null;
        this.deliver(this.header[0], body);
      } else {
        const length = this.header.readUInt32BE(1);
        if (length > MAX_MESSAGE) throw new Error('Host sent an oversized message');
        if (length) this.body = Buffer.allocUnsafe(length);
        else this.deliver(this.header[0], Buffer.alloc(0));
      }
    }
  }
}

function floats(...values) {
  const b = Buffer.alloc(values.length * 4);
  values.forEach((v, i) => b.writeFloatBE(v, i * 4)); return b;
}
function inputPacket(input) {
  const finite = (...values) => values.every(v => typeof v === 'number' && Number.isFinite(v));
  const point = () => finite(input.x, input.y) && input.x >= 0 && input.x <= 1 && input.y >= 0 && input.y <= 1;
  switch (input.kind) {
    case 'move': if (point()) return packet(T.mouseMove, floats(input.x, input.y)); break;
    case 'button':
      if (point() && [0, 1, 2].includes(input.button) && typeof input.down === 'boolean')
        return packet(T.mouseButton, Buffer.concat([Buffer.from([input.button, +input.down]), floats(input.x, input.y)]));
      break;
    case 'scroll': if (finite(input.dx, input.dy) && Math.abs(input.dx) <= 10000 && Math.abs(input.dy) <= 10000)
      return packet(T.scroll, floats(input.dx, input.dy)); break;
    case 'key': {
      if (!Number.isInteger(input.code) || input.code < 0 || input.code > 127 || ![0, 1, 2].includes(input.action) ||
          !Number.isInteger(input.mods) || input.mods < 0 || input.mods > 31) break;
      const b = Buffer.alloc(7); b.writeUInt16BE(input.code); b[2] = input.action; b.writeUInt32BE(input.mods, 3);
      return packet(T.key, b);
    }
    case 'text': if (typeof input.text === 'string' && input.text.length <= 16384)
      return packet(T.text, Buffer.from(input.text)); break;
  }
  throw new Error('Invalid input');
}

function validateOptions(o) {
  if (!o || typeof o.host !== 'string' || !o.host.trim() || o.host.length > 253 || /[\s/\\]/.test(o.host.trim()))
    throw new Error('Enter a Mac IP address or hostname, without a URL prefix.');
  if (!Number.isInteger(o.port) || o.port < 1 || o.port > 65535) throw new Error('Port must be between 1 and 65535.');
  if (typeof o.pin !== 'string' || o.pin.length > 256) throw new Error('Invalid PIN.');
  if (![30, 60, 120].includes(o.fps) || ![0, 1, 1.5, 2].includes(o.scale)) throw new Error('Invalid stream settings.');
  if (!Number.isInteger(o.width) || !Number.isInteger(o.height) || o.width < 640 || o.height < 480 || o.width > 7680 || o.height > 4320)
    throw new Error('Display must be between 640 × 480 and 7680 × 4320.');
  return { ...o, host: o.host.trim().replace(/^\[|\]$/g, ''), pin: o.pin.trim() };
}
module.exports = { T, MAX_MESSAGE, packet, Parser, floats, inputPacket, validateOptions };
