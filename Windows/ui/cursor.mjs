// The Mac's cursor, drawn as our own pointer. Hosts that send cursor shapes leave the cursor out
// of the video, so the pointer moves with Windows' own zero-latency mouse instead of a frame late.
export class MacCursor {
  constructor(canvas) { this.canvas = canvas; this.shape = null; this.welcome = null; this.version = 0; }

  /** Payload: hotspot x, y, size w, h (u16 big endian, Mac points), then a PNG at 2x. */
  update(bytes) {
    const data = new Uint8Array(bytes);
    if (data.length <= 8) return;
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const shape = {hotspotX: view.getUint16(0), hotspotY: view.getUint16(2), width: view.getUint16(4), height: view.getUint16(6)};
    const version = ++this.version;
    createImageBitmap(new Blob([data.subarray(8)], {type: 'image/png'})).then(bitmap => {
      if (version !== this.version) { bitmap.close(); return; }  // a newer shape already arrived
      this.shape?.bitmap.close();
      this.shape = {...shape, bitmap};
      this.apply();
    }).catch(() => {});
  }

  setWelcome(welcome) { this.welcome = welcome; this.apply(); }

  reset() {
    this.version++; this.shape?.bitmap.close(); this.shape = null; this.welcome = null;
    this.canvas.style.cursor = '';   // back to the stylesheet: hidden while the cursor is in the video
  }

  /** Scales the shape to how big the Mac's screen appears in the window, as on the Mac itself. */
  apply() {
    const {shape, welcome, canvas} = this;
    if (!shape || !welcome?.pointWidth || !welcome.width || !welcome.height) return;
    const rect = canvas.getBoundingClientRect();
    if (!rect.width || !rect.height) return;
    const shownWidth = welcome.width * Math.min(rect.width / welcome.width, rect.height / welcome.height);
    const scale = shownWidth / welcome.pointWidth;   // CSS px per Mac point
    // Chromium ignores cursor images over 128 CSS px.
    const cssW = Math.max(1, Math.min(shape.width * scale, 128)), cssH = Math.max(1, Math.min(shape.height * scale, 128));
    const render = ratio => {
      const c = document.createElement('canvas');
      c.width = Math.max(1, Math.round(cssW * ratio)); c.height = Math.max(1, Math.round(cssH * ratio));
      const g = c.getContext('2d'); g.imageSmoothingQuality = 'high'; g.drawImage(shape.bitmap, 0, 0, c.width, c.height);
      return c.toDataURL('image/png');
    };
    const hx = Math.min(Math.round(shape.hotspotX * scale), Math.round(cssW) - 1);
    const hy = Math.min(Math.round(shape.hotspotY * scale), Math.round(cssH) - 1);
    const ratio = window.devicePixelRatio || 1;
    canvas.style.cursor = `image-set(url("${render(ratio)}") ${ratio}x) ${hx} ${hy}, url("${render(1)}") ${hx} ${hy}, default`;
  }
}
