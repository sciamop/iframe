export const keyMap = {
  KeyA:0,KeyS:1,KeyD:2,KeyF:3,KeyH:4,KeyG:5,KeyZ:6,KeyX:7,KeyC:8,KeyV:9,IntlBackslash:10,KeyB:11,
  KeyQ:12,KeyW:13,KeyE:14,KeyR:15,KeyY:16,KeyT:17,Digit1:18,Digit2:19,Digit3:20,Digit4:21,Digit6:22,Digit5:23,
  Equal:24,Digit9:25,Digit7:26,Minus:27,Digit8:28,Digit0:29,BracketRight:30,KeyO:31,KeyU:32,BracketLeft:33,
  KeyI:34,KeyP:35,Enter:36,KeyL:37,KeyJ:38,Quote:39,KeyK:40,Semicolon:41,Backslash:42,Comma:43,Slash:44,
  KeyN:45,KeyM:46,Period:47,Tab:48,Space:49,Backquote:50,Backspace:51,Escape:53,MetaRight:54,MetaLeft:55,
  ShiftLeft:56,CapsLock:57,AltLeft:58,ControlLeft:59,ShiftRight:60,AltRight:61,ControlRight:62,
  NumpadDecimal:65,NumpadMultiply:67,NumpadAdd:69,NumLock:71,NumpadDivide:75,NumpadEnter:76,NumpadSubtract:78,
  NumpadEqual:81,Numpad0:82,Numpad1:83,Numpad2:84,Numpad3:85,Numpad4:86,Numpad5:87,Numpad6:88,Numpad7:89,Numpad8:91,Numpad9:92,
  F1:122,F2:120,F3:99,F4:118,F5:96,F6:97,F7:98,F8:100,F9:101,F10:109,F11:103,F12:111,
  F13:105,F14:107,F15:113,F16:106,F17:64,F18:79,F19:80,F20:90,
  Insert:114,Home:115,PageUp:116,Delete:117,End:119,PageDown:121,ArrowLeft:123,ArrowRight:124,ArrowDown:125,ArrowUp:126
};
export function mapKey(code, swap) {
  if (swap) code = ({ControlLeft:'MetaLeft',ControlRight:'MetaRight',MetaLeft:'ControlLeft',MetaRight:'ControlRight'})[code] ?? code;
  return keyMap[code];
}
export function modifiers(e, swap) {
  return (+e.shiftKey) | ((swap ? +e.metaKey : +e.ctrlKey) << 1) | (+e.altKey << 2) |
    ((swap ? +e.ctrlKey : +e.metaKey) << 3) | (+e.getModifierState('CapsLock') << 4);
}
export function normalizedPoint(x, y, rect, width, height, clamp = false) {
  const scale = Math.min(rect.width / width, rect.height / height);
  const w = width * scale, h = height * scale;
  const nx = (x - rect.left - (rect.width - w) / 2) / w, ny = (y - rect.top - (rect.height - h) / 2) / h;
  if (!Number.isFinite(nx) || !Number.isFinite(ny) || (!clamp && (nx < 0 || nx > 1 || ny < 0 || ny > 1))) return null;
  return {x: Math.max(0, Math.min(1, nx)), y: Math.max(0, Math.min(1, ny))};
}
export function attachInput(canvas, api, options) {
  const keys = new Map(), buttons = new Set(); let position = {x: 0.5, y: 0.5}, move = null, raf;
  const flushMove = () => { if (move) { api.input({kind:'move', ...move}); move = null; } };
  const releaseButtons = () => {
    cancelAnimationFrame(raf); move = null;
    for (const button of buttons) api.input({kind:'button', button, down:false, ...position}); buttons.clear();
  };
  const release = () => {
    for (const code of keys.values()) api.input({kind:'key', code, action:0, mods:0}); keys.clear();
    releaseButtons();
  };
  // Windows keys captured by the fullscreen hook arrive here instead of as DOM events; they are always Command.
  const hooked = () => keys.has('HookMetaLeft') || keys.has('HookMetaRight') ? 8 : 0;
  const heldMods = () => [...keys.values()].reduce((m, c) => m | (({56:1, 60:1, 59:2, 62:2, 58:4, 61:4, 55:8, 54:8})[c] ?? 0), 0);
  api.onMetaKey(({code, down}) => {
    const id = 'Hook' + code; if (!options.active() || (!down && !keys.has(id))) return;
    const action = down ? (keys.has(id) ? 2 : 1) : 0;
    if (down) keys.set(id, keyMap[code]); else keys.delete(id);
    api.input({kind:'key', code:keyMap[code], action, mods:heldMods()});
  });
  const point = (e, clamp) => normalizedPoint(e.clientX, e.clientY, canvas.getBoundingClientRect(), canvas.width, canvas.height, clamp);
  canvas.addEventListener('pointermove', e => {
    if (!options.active()) return;
    const p = point(e, buttons.size > 0); if (!p) return;
    position = p; move = p; cancelAnimationFrame(raf); raf = requestAnimationFrame(flushMove);
  });
  canvas.addEventListener('pointerdown', e => {
    if (!options.active()) return;
    const p = point(e, false), button = ({0:0, 1:2, 2:1})[e.button];
    if (!p || button === undefined) return;
    e.preventDefault(); canvas.focus(); canvas.setPointerCapture(e.pointerId); position = p; flushMove(); buttons.add(button);
    api.input({kind:'button', button, down:true, ...p});
  });
  canvas.addEventListener('pointerup', e => {
    const button = ({0:0, 1:2, 2:1})[e.button]; if (!buttons.delete(button)) return;
    position = point(e, true) ?? position; flushMove(); api.input({kind:'button', button, down:false, ...position});
  });
  canvas.addEventListener('lostpointercapture', releaseButtons);
  canvas.addEventListener('pointercancel', releaseButtons);
  canvas.addEventListener('contextmenu', e => e.preventDefault());
  canvas.addEventListener('wheel', e => {
    if (!options.active() || !point(e, false)) return;
    e.preventDefault(); const unit = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 800 : 1;
    api.input({kind:'scroll', dx:Math.max(-10000,Math.min(10000,-e.deltaX*unit)), dy:Math.max(-10000,Math.min(10000,-e.deltaY*unit))});
  }, {passive:false});
  for (const type of ['keydown','keyup']) canvas.addEventListener(type, e => {
    if (!options.active()) return;
    e.preventDefault(); e.stopPropagation();
    if (e.ctrlKey && e.altKey && ['KeyF','KeyQ','KeyR'].includes(e.code)) {
      if (type === 'keydown' && !e.repeat) { release(); options.shortcut(e.code); } return;
    }
    const code = keys.get(e.code) ?? mapKey(e.code, options.swap()); if (code === undefined) return;
    if (type === 'keyup' && !keys.has(e.code)) return;
    api.input({kind:'key', code, action:type === 'keyup' ? 0 : e.repeat ? 2 : 1, mods:modifiers(e, options.swap()) | hooked()});
    if (type === 'keyup') keys.delete(e.code); else keys.set(e.code, code);
  });
  canvas.addEventListener('blur', release); window.addEventListener('blur', release); api.onReleaseInput(release);
  return release;
}
