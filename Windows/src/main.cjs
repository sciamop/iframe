'use strict';
const { app, BrowserWindow, ipcMain, powerSaveBlocker } = require('electron');
const path = require('node:path');
const { Bonjour } = require('bonjour-service');
const { Session } = require('./session.cjs');
const { validateOptions } = require('./protocol.cjs');
const { WinKeyHook } = require('./winkey.cjs');
const smoke = process.argv.includes('--smoke-test');
if (smoke) app.setPath('userData', path.join(process.cwd(), 'test-output/profile'));
let window, discovery, browser, blocker, streaming = false, fullscreen = false, focused = false;
const session = new Session();
const hosts = new Map();
function emit(channel, value) { if (window && !window.isDestroyed()) window.webContents.send(channel, value); }
function stopBlocker() { if (blocker !== undefined) powerSaveBlocker.stop(blocker); blocker = undefined; }
const winKey = new WinKeyHook(value => emit('meta-key', value));
// Track window events: isFullScreen() can still report the old state while enter-full-screen fires.
function updateWinKey() { winKey.set(!smoke && streaming && fullscreen && focused); }
function setStreaming(value) { streaming = value; updateWinKey(); }
session.on('message', value => emit('stream-message', value));
session.on('rtt', value => emit('rtt', value));
session.on('state', value => {
  if (value.phase === 'streaming' && blocker === undefined) blocker = powerSaveBlocker.start('prevent-display-sleep');
  if (value.phase === 'failed') stopBlocker();
  setStreaming(value.phase === 'streaming');
  emit('state', value);
});
function trusted(event) { return window && event.sender === window.webContents && event.senderFrame === window.webContents.mainFrame; }
function handle(name, callback) {
  ipcMain.handle(name, (event, ...args) => { if (!trusted(event)) throw new Error('Untrusted caller'); return callback(...args); });
}
function listen(name, callback) {
  ipcMain.on(name, (event, ...args) => { if (!trusted(event)) return; try { callback(...args); }
    catch (e) { session.close(); stopBlocker(); setStreaming(false); emit('state', {phase: 'failed', message: e.message}); } });
}
handle('connect', options => { session.connect(validateOptions(options)); });
handle('disconnect', () => { session.close(); stopBlocker(); setStreaming(false); emit('state', {phase: 'idle'}); });
handle('fullscreen', () => { window.setFullScreen(!window.isFullScreen()); return window.isFullScreen(); });
handle('hosts', () => [...hosts.values()]);
listen('input', input => session.input(input));
listen('ack', (id, micros) => session.ack(id, micros));
listen('keyframe', () => session.keyframe());
function startDiscovery() {
  try {
    discovery = new Bonjour({}, () => emit('discovery-warning', 'Discovery is unavailable. Enter the Mac address below.'));
    browser = discovery.find({ type: 'iframe', protocol: 'tcp' });
    browser.on('up', service => {
      const host = service.addresses?.find(a => /^\d+\.\d+\.\d+\.\d+$/.test(a)) ?? service.host;
      if (!host) return;
      hosts.set(service.fqdn, { id: service.fqdn, name: service.name, host, port: service.port });
      emit('hosts', [...hosts.values()]);
    });
    browser.on('down', service => { hosts.delete(service.fqdn); emit('hosts', [...hosts.values()]); });
  } catch { emit('discovery-warning', 'Discovery is unavailable. Enter the Mac address below.'); }
}
app.whenReady().then(async () => {
  window = new BrowserWindow({ width: 1180, height: 820, minWidth: 760, minHeight: 620, show: !smoke,
    title: 'iFrame', backgroundColor: '#101416', icon: path.join(__dirname, '../assets/icon.png'),
    webPreferences: { preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true, nodeIntegration: false,
      sandbox: true, offscreen: smoke, backgroundThrottling: false, devTools: !app.isPackaged } });
  window.setMenu(null);
  window.webContents.setWindowOpenHandler(() => ({action: 'deny'}));
  window.webContents.on('will-navigate', event => event.preventDefault());
  window.webContents.session.setPermissionRequestHandler((_wc, _permission, callback) => callback(false));
  window.webContents.on('render-process-gone', () => { session.close(); stopBlocker(); setStreaming(false); });
  window.on('blur', () => { focused = false; updateWinKey(); emit('release-input'); });
  window.on('focus', () => { focused = true; updateWinKey(); });
  window.on('enter-full-screen', () => { fullscreen = true; updateWinKey(); });
  window.on('leave-full-screen', () => { fullscreen = false; updateWinKey(); });
  focused = window.isFocused();
  window.on('closed', () => { session.close(); stopBlocker(); window = null; focused = fullscreen = false; setStreaming(false); });
  await window.loadFile(path.join(__dirname, '../ui/index.html'));
  if (smoke) {
    try { await require('../test/smoke.cjs').run(window); app.exit(0); }
    catch (e) { console.error(e); app.exit(1); }
  } else startDiscovery();
});
app.on('window-all-closed', () => app.quit());
app.on('before-quit', () => { session.close(); stopBlocker(); winKey.set(false); browser?.stop(); discovery?.destroy(); });
