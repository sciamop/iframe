'use strict';
const net = require('node:net');
const os = require('node:os');
const { EventEmitter } = require('node:events');
const { T, packet, Parser, inputPacket, validateOptions } = require('./protocol.cjs');

class Session extends EventEmitter {
  constructor() { super(); this.socket = null; this.ready = false; this.pending = new Map(); }
  connect(options) {
    const o = validateOptions(options);
    this.close();
    const socket = new net.Socket(); this.socket = socket;
    let welcomed = false;
    const fail = message => { if (this.socket !== socket) return; this.close(); this.emit('state', { phase: 'failed', message }); };
    const parser = new Parser((type, data) => {
      if (this.socket !== socket) return;
      if (type === T.authFailed) return fail('Wrong PIN or incompatible host protocol. Check the PIN on your Mac.');
      if (type === T.welcome) {
        const w = JSON.parse(data.toString());
        if (w.codec !== 0) return fail('The host is forcing HEVC. Restart it with H.264 or automatic codec selection.');
        if (!Number.isInteger(w.width) || !Number.isInteger(w.height) || w.width < 1 || w.height < 1 || w.width > 16384 || w.height > 16384)
          return fail('Invalid display size from host.');
        welcomed = true; this.ready = true; clearTimeout(this.handshake);
        this.emit('message', { type, data });
        this.emit('state', { phase: 'streaming', welcome: w });
      } else if (!welcomed) {
        // The Mac's cursor shape can arrive first; anything else is a protocol error.
        if (type !== T.cursor) throw new Error('Unexpected message before welcome');
        this.emit('message', { type, data });
      } else if (type === T.pong) {
        if (data.length !== 8) throw new Error('Invalid ping response');
        const elapsed = process.hrtime.bigint() - data.readBigUInt64BE();
        this.emit('rtt', Math.max(0, Number(elapsed) / 1e6));
      } else {
        if (type === T.frame) {
          if (data.length < 14) throw new Error('Truncated video frame');
          if (this.pending.size >= 32) throw new Error('Too many unacknowledged frames');
          this.pending.set(data.readUInt32BE(), Date.now());
        }
        this.emit('message', { type, data });
      }
    });
    socket.setNoDelay(true); socket.setKeepAlive(true, 5000); socket.setTimeout(15000);
    socket.on('timeout', () => fail('The Mac stopped responding. Check the network and reconnect.'));
    socket.on('data', chunk => { try { parser.push(chunk); } catch (e) { fail(`Stream error: ${e.message}`); } });
    socket.on('error', e => fail(`Cannot connect to ${o.host}:${o.port} (${e.code ?? e.message}).`));
    socket.on('close', () => fail('The Mac closed the connection. Check its screen recording permission and host log.'));
    socket.on('connect', () => {
      this.send(T.hello, Buffer.from(JSON.stringify({ version: 1, pin: o.pin, name: os.hostname(), supportsHEVC: false,
        // localCursor: the Mac sends its cursor shape and leaves the pointer out of the video.
        localCursor: true, maxFPS: o.fps, display: { width: o.width, height: o.height, uiScale: o.scale } })));
      this.timer = setInterval(() => {
        if (!this.ready) return;
        if ([...this.pending.values()].some(time => Date.now() - time > 10000))
          return fail('Video decoding stalled. Reconnect or try a lower resolution.');
        const ping = Buffer.alloc(8); ping.writeBigUInt64BE(process.hrtime.bigint()); this.send(T.ping, ping);
      }, 1000);
    });
    this.handshake = setTimeout(() => fail('Connection timed out. Confirm iframe-host is running and the port is reachable.'), 15000);
    this.emit('state', { phase: 'connecting' });
    socket.connect(o.port, o.host);
  }
  send(type, payload) { if (this.socket && !this.socket.destroyed) this.socket.write(packet(type, payload)); }
  input(input) {
    if (!this.ready) return;
    const p = inputPacket(input);
    // Never allow a blocked network to accumulate seconds of stale input.
    if (this.socket.writableLength > 256 * 1024) {
      this.close(); this.emit('state', { phase: 'failed', message: 'Network is too slow for interactive input. Reconnect.' }); return;
    }
    this.socket.write(p);
  }
  ack(id, micros) {
    if (!this.ready || !Number.isInteger(id) || !this.pending.delete(id)) return;
    const b = Buffer.alloc(8); b.writeUInt32BE(id); b.writeUInt32BE(Math.max(0, Math.min(0xffffffff, Math.round(micros) || 0)), 4);
    this.send(T.ack, b);
  }
  keyframe() {
    if (!this.ready || Date.now() - (this.lastKeyframe ?? 0) < 250) return;
    this.lastKeyframe = Date.now(); this.send(T.requestKeyframe);
  }
  close() {
    clearTimeout(this.handshake); clearInterval(this.timer);
    const socket = this.socket; this.socket = null; this.ready = false;
    socket?.destroy(); this.pending.clear(); this.lastKeyframe = 0;
  }
}
module.exports = { Session };
