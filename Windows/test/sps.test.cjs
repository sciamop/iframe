const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

test('SPS rewrite declares zero reorder frames for VideoToolbox-style VUI', async () => {
  const { lowDelaySPS } = await import('../ui/sps.mjs');
  const raw = fs.readFileSync(path.join(__dirname, 'desktop.h264'));
  const start = raw.findIndex((b, i) => b === 1 && raw[i-1] === 0 && raw[i-2] === 0 && (raw[i+1] & 31) === 7) + 1;
  let end = raw.indexOf(Buffer.from([0, 0, 1]), start); while (raw[end-1] === 0) end--;
  const original = new Uint8Array(raw.subarray(start, end));
  // Remove the restriction the way VideoToolbox omits it, then restore it.
  const stripped = lowDelaySPS(original, null), fixed = lowDelaySPS(stripped);
  assert.notDeepEqual(stripped, original);
  assert.deepEqual(lowDelaySPS(stripped, null), stripped);
  assert.deepEqual(lowDelaySPS(fixed), fixed);
  assert.deepEqual(lowDelaySPS(lowDelaySPS(original, 4)), lowDelaySPS(original));
  assert.equal(fixed[0], original[0]);
});
