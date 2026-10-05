import { Video } from './video.mjs';
import { attachInput } from './input.mjs';
const $ = id => document.getElementById(id), api = window.iframe;
let active = false, welcome, rtt = 0, hostStats = {}, frames = 0, decodeTotal = 0;
const settings = ['host','port','resolution','fps','scale'];
try {
  const saved = JSON.parse(localStorage.getItem('iframe-settings') ?? '{}');
  for (const id of settings) if (saved[id] !== undefined) $(id).value = saved[id];
  $('swap').checked = saved.swap !== false;
} catch { /* Ignore an unreadable preference file. The PIN is never persisted. */ }
const status = message => { $('status').textContent = message; };
function saveSettings() {
  try { localStorage.setItem('iframe-settings', JSON.stringify(Object.fromEntries([...settings.map(id => [id, $(id).value]), ['swap', $('swap').checked]]))); } catch {}
}
async function fail(message) { await api.disconnect(); status(message); }
const video = new Video($('video'), api, fail, ms => { frames++; decodeTotal += ms; $('waiting').hidden = true; },
  message => { $('waiting').textContent = message; });
const releaseInput = attachInput($('video'), api, {
  active: () => active, swap: () => $('swap').checked,
  shortcut: code => { if (code === 'KeyF') toggleFullscreen(); else if (code === 'KeyQ') disconnect(); else $('fullscreen').focus(); }
});
function disconnect() { releaseInput(); return api.disconnect(); }
async function toggleFullscreen() {
  const fullscreen = await api.fullscreen(); $('fullscreen').textContent = fullscreen ? 'Exit fullscreen' : 'Fullscreen';
  if (active) $('video').focus();
}
$('connect-form').addEventListener('submit', async e => {
  e.preventDefault(); status('');
  let width, height;
  if ($('resolution').value === 'screen') {
    width = Math.max(640, Math.min(7680, Math.round(screen.width * devicePixelRatio / 2) * 2));
    height = Math.max(480, Math.min(4320, Math.round(screen.height * devicePixelRatio / 2) * 2));
  } else [width, height] = $('resolution').value.split('x').map(Number);
  saveSettings();
  try {
    if (typeof VideoDecoder === 'undefined') throw new Error('Video decoding is unavailable on this Windows installation.');
    await api.connect({host:$('host').value, port:Number($('port').value), pin:$('pin').value,
      width, height, scale:Number($('scale').value), fps:Number($('fps').value)});
  } catch (e) { status(e.message.replace(/^Error invoking remote method '[^']+': Error: /, '')); }
});
$('cancel').onclick = disconnect; $('disconnect').onclick = disconnect; $('fullscreen').onclick = toggleFullscreen;
$('refresh').onclick = () => { api.keyframe(); $('video').focus(); };
$('stats-toggle').onclick = () => {
  $('stats').hidden = !$('stats').hidden; $('stats-toggle').setAttribute('aria-pressed', String(!$('stats').hidden)); $('video').focus();
};
$('send-text').onclick = () => { releaseInput(); $('text-dialog').showModal(); $('text-content').focus(); };
$('text-dialog').addEventListener('close', () => {
  if ($('text-dialog').returnValue === 'send' && active) { api.input({kind:'text', text:$('text-content').value}); $('text-content').value = ''; }
  if (active) $('video').focus();
});
api.onState(state => {
  const connecting = state.phase === 'connecting';
  $('connect').disabled = connecting; $('cancel').hidden = !connecting;
  for (const input of document.querySelectorAll('#connect-form input, #connect-form select, #discovered')) input.disabled = connecting;
  if (state.phase === 'streaming') {
    active = true; welcome = state.welcome;
    $('connect-page').hidden = true; $('stream-page').hidden = false; $('waiting').hidden = false;
    $('waiting').textContent = 'Waiting for the first frame…';
    $('host-name').textContent = welcome.hostName;
    $('stream-info').textContent = `${welcome.width} × ${welcome.height} · H.264 · ${welcome.fps} fps`;
    $('keyboard-mode').textContent = $('swap').checked ? 'Ctrl → ⌘ Command' : 'Windows → ⌘ Command';
    $('pin').value = ''; $('video').focus();
  } else if (!connecting) {
    releaseInput(); active = false; video.close(); welcome = null; frames = 0; decodeTotal = 0; hostStats = {}; rtt = 0;
    $('text-dialog').close('cancel'); $('connect-page').hidden = false; $('stream-page').hidden = true;
    status(state.phase === 'failed' ? state.message : ''); $('connect').focus();
  } else status('Connecting to your Mac…');
});
api.onMessage(({type, data}) => {
  try {
    if (type === 1) { video.close(); $('waiting').hidden = false; }
    else if (type === 2) video.configure(data);
    else if (type === 3) video.frame(data);
    else if (type === 4) hostStats = JSON.parse(new TextDecoder().decode(data));
  } catch (e) { fail(e.message); }
});
api.onRtt(value => { rtt = value; });
let knownHosts = [];
function showHosts(hosts) {
  knownHosts = hosts; const selected = $('discovered').value;
  $('discovered').replaceChildren(new Option(hosts.length ? 'Choose a Mac…' : 'No Macs found yet · enter an address below', ''));
  for (const host of hosts) $('discovered').add(new Option(`${host.name} · ${host.host}`, host.id));
  $('discovered').value = selected;
}
api.onHosts(showHosts); api.hosts().then(showHosts);
api.onDiscoveryWarning(message => { $('discovery-hint').textContent = message; });
$('discovered').onchange = () => {
  const host = knownHosts.find(h => h.id === $('discovered').value); if (!host) return;
  $('host').value = host.host; $('port').value = host.port; $('pin').focus();
};
setInterval(() => {
  if (active) {
    const n = v => Number.isFinite(v) ? v.toFixed(1) : '—';
    $('stats').textContent = `${welcome?.isVirtual ? 'Virtual display' : 'Mac display'} · H.264\nReceived    ${frames} fps\nDecode      ${n(frames ? decodeTotal/frames : 0)} ms\nRound trip  ${n(rtt)} ms\nBitrate     ${n(hostStats.mbps)} Mbps\nHost encode ${n(hostStats.encodeMs)} ms\nHost drops  ${hostStats.dropped ?? '—'}`;
  }
  frames = 0; decodeTotal = 0;
}, 1000);
