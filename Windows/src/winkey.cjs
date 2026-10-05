'use strict';
// Windows hands the Windows key to the shell (Start menu, Win+D, Win+E…) before any window
// sees it. While the stream is fullscreen and focused, a low-level keyboard hook takes the key
// so the Mac can receive it as Command. Windows+L and Ctrl+Alt+Delete stay with the system.
const VK_LWIN = 0x5b, VK_RWIN = 0x5c, WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104, WH_KEYBOARD_LL = 13;

class WinKeyHook {
  constructor(onKey) { this.onKey = onKey; this.hook = null; this.held = new Set(); }
  load() {
    if (this.api) return this.api;
    const koffi = require('koffi');
    const user32 = koffi.load('user32.dll'), kernel32 = koffi.load('kernel32.dll');
    const KBD = koffi.struct('KBDLLHOOKSTRUCT', { vkCode: 'uint32', scanCode: 'uint32', flags: 'uint32', time: 'uint32', extra: 'uintptr' });
    const Proc = koffi.proto('intptr __stdcall LowLevelKeyboardProc(int code, uintptr wParam, void *lParam)');
    const api = {
      koffi, KBD, Proc,
      set: user32.func('void * __stdcall SetWindowsHookExW(int id, LowLevelKeyboardProc *proc, void *module, uint32 thread)'),
      unhook: user32.func('bool __stdcall UnhookWindowsHookEx(void *hook)'),
      next: user32.func('intptr __stdcall CallNextHookEx(void *hook, int code, uintptr wParam, void *lParam)'),
      module: kernel32.func('void * __stdcall GetModuleHandleW(const char16_t *name)')
    };
    api.callback = koffi.register((code, wParam, lParam) => {
      if (code >= 0) {
        const { vkCode } = koffi.decode(lParam, KBD);
        if (vkCode === VK_LWIN || vkCode === VK_RWIN) {
          const key = vkCode === VK_LWIN ? 'MetaLeft' : 'MetaRight', down = wParam === WM_KEYDOWN || wParam === WM_SYSKEYDOWN;
          // Pass through a release whose press Windows already saw, or its key state sticks.
          if (down || this.held.has(key)) {
            if (down) this.held.add(key); else this.held.delete(key);
            try { this.onKey({ code: key, down }); } catch {}
            return 1;
          }
        }
      }
      return api.next(null, code, wParam, lParam);
    }, koffi.pointer(Proc));
    return this.api = api;
  }
  set(enabled) {
    if (process.platform !== 'win32' || enabled === !!this.hook) return;
    try {
      const api = this.load();
      if (enabled) { this.hook = api.set(WH_KEYBOARD_LL, api.callback, api.module(null), 0) || null; return; }
      api.unhook(this.hook); this.hook = null;
    } catch { this.hook = null; return; }
    for (const code of this.held) this.onKey({ code, down: false });
    this.held.clear();
  }
}
module.exports = { WinKeyHook };
