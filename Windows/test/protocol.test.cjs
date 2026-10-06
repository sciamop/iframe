const { test } = require('node:test');
const assert = require('node:assert/strict');
const net = require('node:net');
const { once } = require('node:events');
const { T, MAX_MESSAGE, packet, Parser, inputPacket, validateOptions } = require('../src/protocol.cjs');
const { Session } = require('../src/session.cjs');
const options = {host:'127.0.0.1', port:7878, pin:'1234', width:1920, height:1080, fps:60, scale:2};

test('framing handles every split, concatenation and empty messages', () => {
  const expected = [[T.welcome,Buffer.from('hello')],[T.requestKeyframe,Buffer.alloc(0)],[T.frame,Buffer.alloc(8192,71)]];
  const stream = Buffer.concat(expected.map(([t,b]) => packet(t,b)));
  for (const size of [1,2,3,4,5,7,512,stream.length]) {
    const got = [], parser = new Parser((t,b) => got.push([t,b]));
    for (let i = 0; i < stream.length; i += size) parser.push(stream.subarray(i,i+size));
    assert.deepEqual(got,expected);
  }
});
test('oversized payload rejected before allocation', () => {
  const header = Buffer.alloc(5); header.writeUInt32BE(MAX_MESSAGE+1,1);
  assert.throws(() => new Parser(() => {}).push(header), /oversized/);
});
test('Swift wire compatibility for mouse, key modifiers and scroll', () => {
  assert.equal(inputPacket({kind:'move',x:0.5,y:1}).toString('hex'),'11000000083f0000003f800000');
  assert.equal(inputPacket({kind:'button',button:1,down:true,x:0,y:1}).toString('hex'),'120000000a0101000000003f800000');
  assert.equal(inputPacket({kind:'key',code:55,action:1,mods:8}).toString('hex'),'140000000700370100000008');
  assert.equal(inputPacket({kind:'scroll',dx:-1,dy:2}).toString('hex'),'1300000008bf80000040000000');
  assert.throws(() => inputPacket({kind:'move',x:NaN,y:1}));
  assert.throws(() => inputPacket({kind:'key',code:0,action:1,mods:32}));
});
test('connection options reject URLs, invalid sizes and ports', () => {
  assert.equal(validateOptions({...options,host:' [::1] '}).host,'::1');
  for (const invalid of [{host:'http://mac'},{port:0},{width:20000},{scale:3},{fps:90}])
    assert.throws(() => validateOptions({...options,...invalid}));
});
test('AVCC conversion and malformed bitstreams', async () => {
  const {annexB,parseFormat} = await import('../ui/video.mjs');
  assert.deepEqual([...annexB([0,0,0,2,0x65,0x11])],[0,0,0,1,0x65,0x11]);
  assert.throws(() => annexB([0,0,0,9,0x65]),/Truncated/);
  assert.throws(() => parseFormat([1,0]),/H.264/);
  assert.throws(() => parseFormat([0,1,0,0,0,9,0x67]),/Invalid/);
});
test('letterboxing and key mapping preserve remote coordinates', async () => {
  const {normalizedPoint,mapKey,modifiers} = await import('../ui/input.mjs');
  const rect = {left:0,top:0,width:1000,height:1000};
  assert.equal(normalizedPoint(500,100,rect,1920,1080),null);
  assert.deepEqual(normalizedPoint(500,500,rect,1920,1080),{x:0.5,y:0.5});
  assert.deepEqual(normalizedPoint(1200,1200,rect,1920,1080,true),{x:1,y:1});
  assert.equal(mapKey('ControlLeft',true),55); assert.equal(mapKey('ControlLeft',false),59);
  assert.equal(mapKey('KeyA',false),0);
  assert.equal(modifiers({ctrlKey:true,shiftKey:false,altKey:false,metaKey:false,getModifierState:()=>false},true),8);
});

test('loopback handshake, decode acknowledgements, ping and input', async t => {
  const server = net.createServer(); server.listen(0,'127.0.0.1'); await once(server,'listening');
  const session = new Session(); t.after(() => { session.close(); server.close(); });
  const connected = once(server,'connection'); session.connect({...options,port:server.address().port});
  const [peer] = await connected; t.after(() => peer.destroy());
  const messages = []; let notify;
  peer.on('data', chunk => parser.push(chunk));
  const parser = new Parser((type,data) => { messages.push({type,data}); notify?.(); });
  const wait = async predicate => {
    const timeout = setTimeout(() => notify?.(true),2000);
    while (!predicate()) { const expired = await new Promise(resolve => {notify = resolve;}); if (expired) throw new Error('Timed out'); }
    clearTimeout(timeout);
  };
  await wait(() => messages.length > 0);
  const hello = JSON.parse(messages[0].data); assert.equal(hello.pin,'1234'); assert.equal(hello.supportsHEVC,false);
  assert.equal(hello.display.uiScale,2); assert.equal(hello.localCursor,true);
  // The Mac sends its cursor shape before the welcome; that must not end the session.
  const cursor = once(session,'message');
  peer.write(packet(T.cursor,Buffer.from([0,4,0,2,0,17,0,23,0x89,0x50,0x4e,0x47])));
  assert.equal((await cursor)[0].type,T.cursor);
  const streaming = once(session,'state');
  peer.write(packet(T.welcome,Buffer.from(JSON.stringify({width:1920,height:1080,codec:0}))));
  assert.equal((await streaming)[0].phase,'streaming');
  const received = once(session,'message'); const frame = Buffer.alloc(14); frame.writeUInt32BE(42);
  peer.write(packet(T.frame,frame)); await received;
  assert.equal(messages.filter(m => m.type === T.ack).length,0,'ACK must wait for decoder');
  session.ack(42,1500); session.ack(42,1500);
  session.input({kind:'key',code:0,action:1,mods:8});
  await wait(() => messages.some(m => m.type === T.key));
  const acks = messages.filter(m => m.type === T.ack); assert.equal(acks.length,1); assert.equal(acks[0].data.readUInt32BE(),42);
  const rtt = once(session,'rtt'); const ping = Buffer.alloc(8); ping.writeBigUInt64BE(process.hrtime.bigint());
  peer.write(packet(T.pong,ping)); assert.ok((await rtt)[0] >= 0);
});
test('wrong PIN returns actionable error and closes socket', async t => {
  const server = net.createServer(peer => { peer.on('data', () => peer.write(packet(T.authFailed))); });
  server.listen(0,'127.0.0.1'); await once(server,'listening');
  const session = new Session(); t.after(() => {session.close();server.close();});
  session.connect({...options,port:server.address().port});
  const [state] = await once(session,'state'); assert.equal(state.phase,'failed'); assert.match(state.message,/Wrong PIN/);
  assert.equal(session.socket,null);
});
