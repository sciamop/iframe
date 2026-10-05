const net = require('node:net');
const fs = require('node:fs/promises');
const path = require('node:path');
const assert = require('node:assert/strict');
const { once } = require('node:events');
const { T, packet, Parser } = require('../src/protocol.cjs');

async function fixture() {
  const raw = await fs.readFile(path.join(__dirname,'desktop.h264'));
  const starts = [];
  for (let i=0;i<raw.length-3;i++) {
    if (raw[i]===0 && raw[i+1]===0 && raw[i+2]===1) {starts.push([i,i+3]);i+=2;}
    else if (raw[i]===0 && raw[i+1]===0 && raw[i+2]===0 && raw[i+3]===1) {starts.push([i,i+4]);i+=3;}
  }
  const nals = starts.map((s,i) => raw.subarray(s[1],starts[i+1]?.[0] ?? raw.length));
  const sets = nals.filter(n => [7,8].includes(n[0]&31));
  const groups=[]; let group=[];
  for (const n of nals) {
    if ((n[0]&31)===9) {if(group.length)groups.push(group);group=[];}
    if (![7,8,9].includes(n[0]&31)) group.push(n);
  }
  if(group.length) groups.push(group);
  const sized = n => {const length=Buffer.alloc(4);length.writeUInt32BE(n.length);return Buffer.concat([length,n]);};
  return {format:Buffer.concat([Buffer.from([0,sets.length]),...sets.map(sized)]),
    frames:groups.map((g,i) => {const head=Buffer.alloc(13);head.writeUInt32BE(i+1);head.writeBigUInt64BE(BigInt(i*16666667),4);head[12]=+(g.some(n => (n[0]&31)===5));return Buffer.concat([head,...g.map(sized)]);})};
}
exports.run = async window => {
  const out = path.join(process.cwd(),'test-output'); await fs.mkdir(out,{recursive:true});
  const errors=[];
  window.webContents.on('console-message', (_event,details) => { if (details.level === 'error') errors.push(details.message); });
  await window.webContents.executeJavaScript(`document.getElementById('host').value=''; document.getElementById('port').value='7878';`);
  await new Promise(resolve => setTimeout(resolve,500));
  await fs.writeFile(path.join(out,'connect.png'),(await window.webContents.capturePage(undefined,{stayHidden:true,stayAwake:true})).toPNG());
  const sample=await fixture(), received=[], peers=new Set(); let sent=0, rejection=false;
  const server=net.createServer(peer => {
    peers.add(peer);peer.on('close',()=>peers.delete(peer));
    const send = (type,data) => peer.write(packet(type,data));
    const parser=new Parser((type,data) => {
      received.push({type,data});
      if(type===T.hello) {
        if(rejection) return send(T.authFailed);
        assert.equal(JSON.parse(data).supportsHEVC,false);
        send(T.welcome,Buffer.from(JSON.stringify({width:160,height:96,codec:0,fps:60,hostName:'Test Mac',isVirtual:true})));
        send(T.format,sample.format);send(T.frame,sample.frames[sent++]);
      } else if(type===T.ack && sent<sample.frames.length) send(T.frame,sample.frames[sent++]);
      else if(type===T.ping) send(T.pong,data);
    });
    peer.on('data',chunk=>parser.push(chunk));
  });
  server.listen(0,'127.0.0.1');await once(server,'listening');
  const evaluate=code=>window.webContents.executeJavaScript(code);
  const until=async (predicate,label) => {
    const deadline=Date.now()+12000;
    while(!(await predicate())) {if(Date.now()>deadline) throw new Error(`Timeout: ${label}; renderer errors: ${errors.join('; ')}`);await new Promise(r=>setTimeout(r,50));}
  };
  try {
    await evaluate(`document.getElementById('host').value='127.0.0.1'; document.getElementById('port').value='${server.address().port}'; document.getElementById('pin').value='1234'; document.getElementById('connect-form').requestSubmit();`);
    await until(()=>received.filter(m=>m.type===T.ack).length===sample.frames.length,'decoded frame ACKs');
    assert.ok(sample.frames.length>=4);
    const pixel=await evaluate(`Array.from(document.getElementById('video').getContext('2d').getImageData(80,48,1,1).data)`);
    assert.ok(pixel.slice(0,3).some(v=>v>20),'Decoded pixels must be visible');
    assert.equal(await evaluate(`document.getElementById('waiting').hidden`),true);
    await fs.writeFile(path.join(out,'stream.png'),(await window.webContents.capturePage()).toPNG());
    window.webContents.sendInputEvent({type:'keyDown',keyCode:'A'});
    window.webContents.sendInputEvent({type:'keyUp',keyCode:'A'});
    await until(()=>received.filter(m=>m.type===T.key).length>=2,'keyboard passthrough');
    assert.equal(received.find(m=>m.type===T.key).data.readUInt16BE(),0);
    const keyCount = received.filter(m=>m.type===T.key).length;
    window.webContents.sendInputEvent({type:'keyDown',keyCode:'B'});
    await until(()=>received.filter(m=>m.type===T.key).length>keyCount,'held key');
    await evaluate(`document.getElementById('fullscreen').focus()`);
    // Offscreen windows never have OS focus; send the same notification as main's blur handler.
    window.webContents.send('release-input');
    await until(()=>received.some(m=>m.type===T.key && m.data.readUInt16BE()===11 && m.data[2]===0),'release held key on blur');
    window.webContents.sendInputEvent({type:'mouseDown',x:500,y:400,button:'right',clickCount:1});
    window.webContents.sendInputEvent({type:'mouseUp',x:500,y:400,button:'right',clickCount:1});
    await until(()=>received.filter(m=>m.type===T.mouseButton).length>=2,'right mouse click');
    assert.equal(received.find(m=>m.type===T.mouseButton).data[0],1);
    // Completing a click loses pointer capture, but must preserve held keyboard modifiers.
    window.webContents.sendInputEvent({type:'keyDown',keyCode:'Shift'});
    await until(()=>received.some(m=>m.type===T.key && m.data.readUInt16BE()===56 && m.data[2]===1),'held Shift');
    await evaluate(`document.getElementById('video').dispatchEvent(new PointerEvent('lostpointercapture'))`);
    await new Promise(resolve=>setTimeout(resolve,50));
    assert.equal(received.filter(m=>m.type===T.key && m.data.readUInt16BE()===56 && m.data[2]===0).length,0,'Click must not release Shift');
    window.webContents.sendInputEvent({type:'keyUp',keyCode:'Shift'});
    await evaluate(`document.getElementById('send-text').click(); document.getElementById('text-content').value='Hello, Mac — 你好'; document.getElementById('text-submit').click();`);
    await until(()=>received.some(m=>m.type===T.text),'Unicode text');
    assert.equal(received.find(m=>m.type===T.text).data.toString(),'Hello, Mac — 你好');
    await evaluate(`window.iframe.disconnect()`);
    await until(()=>evaluate(`!document.getElementById('connect-page').hidden`),'disconnect UI');
    rejection=true;
    await evaluate(`document.getElementById('connect-form').requestSubmit()`);
    await until(()=>evaluate(`document.getElementById('status').textContent.includes('Wrong PIN')`),'wrong PIN UI');
    assert.deepEqual(errors,[]);
    await fs.writeFile(path.join(out,'smoke-result.json'),JSON.stringify({passed:true,executable:process.execPath,frames:sample.frames.length,checks:['decode','ack','pixels','keyboard','mouse','unicode','focus release','disconnect','PIN rejection']},null,2));
    console.log(`PASS Electron smoke: ${sample.frames.length} real H.264 frames decoded and ACKed, pixels rendered, keyboard/mouse/Unicode input, focus release, disconnect and PIN rejection. Screenshots: ${out}`);
  } finally {for(const peer of peers)peer.destroy();server.close();}
};
