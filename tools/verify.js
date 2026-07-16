#!/usr/bin/env node
// Replay a tape's chunks through the wasm engine and assert the slice is sane.
// Usage: node tools/verify.js <id>   (e.g. 2021-07-13)
const zlib = require('zlib'), fs = require('fs'), path = require('path');

const id = process.argv[2] || '2021-07-13';
const dir = path.join('report/tapes', id);
const meta = JSON.parse(fs.readFileSync(path.join(dir, 'meta.json')));

require(path.resolve('report/engine.js'))().then(M => {
  M._engine_create();
  const ptr = M._malloc(1 << 20);
  for (const c of meta.chunks) {
    const raw = zlib.gunzipSync(fs.readFileSync(path.join(dir, c.file)));
    for (let p = 0; p < raw.length; p += (1 << 20)) {
      const n = Math.min(1 << 20, raw.length - p);
      M.HEAPU8.set(raw.subarray(p, p + n), ptr);
      M._engine_feed(ptr, n);
    }
  }
  const nb = M._malloc(16), actives = [];
  for (let i = 0; ; i++) { const l = M._engine_active_at(i, nb); if (!l) break; actives.push([M.UTF8ToString(nb), l]); }
  const lv = M._malloc(8 * 4);
  let crossed = 0, quoted = 0;
  for (const [, l] of actives) {
    if (!M._engine_levels(l, 0, lv, 1)) continue;
    const bid = new Uint32Array(M.HEAPU32.buffer, lv, 2)[0];
    if (!M._engine_levels(l, 1, lv, 1)) continue;
    const ask = new Uint32Array(M.HEAPU32.buffer, lv, 2)[0];
    quoted++; if (bid >= ask) crossed++;
  }
  const clockH = M._engine_last_ts() / 3.6e12;
  const ok = actives.length === meta.symbols.length && crossed === 0 && quoted > 0;
  console.log(`msgs ${M._engine_msg_count()}  actives ${actives.length}/${meta.symbols.length}` +
              `  quoted ${quoted}  crossed ${crossed}  clock ${clockH.toFixed(2)}h  trades ${M._trade_count()}`);
  console.log(ok ? 'PASS' : 'FAIL');
  process.exit(ok ? 0 : 1);
}).catch(e => { console.error(e); process.exit(1); });
