#!/usr/bin/env node
// Screenshot a URL via the Chrome DevTools Protocol (reliable for SPAs with a
// perpetual rAF loop, unlike --virtual-time-budget). Launches its own headless
// chromium. Usage: node tools/shoot.js <url> <out.png> [waitMs=7000]
const { spawn } = require('child_process');
const fs = require('fs');

const url = process.argv[2], out = process.argv[3], waitMs = +(process.argv[4] || 7000);
const PORT = 9333;

const chrome = spawn('chromium', ['--headless=new', '--disable-gpu', '--no-sandbox',
  '--hide-scrollbars', `--remote-debugging-port=${PORT}`, '--window-size=1440,2600', 'about:blank'],
  { stdio: 'ignore' });

const sleep = ms => new Promise(r => setTimeout(r, ms));
const rpc = (ws, seq) => (method, params = {}) => new Promise(res => {
  const id = seq.n++;
  const on = e => { const m = JSON.parse(e.data); if(m.id === id){ ws.removeEventListener('message', on); res(m.result); } };
  ws.addEventListener('message', on);
  ws.send(JSON.stringify({ id, method, params }));
});

(async () => {
  let target;
  for(let i = 0; i < 40; i++){
    try{ const list = await fetch(`http://localhost:${PORT}/json`).then(r => r.json());
      target = list.find(t => t.type === 'page'); if(target) break; }catch(e){}
    await sleep(250);
  }
  if(!target){ console.error('no devtools target'); chrome.kill(); process.exit(1); }
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  const seq = { n: 1 }, call = rpc(ws, seq);
  await call('Page.enable');
  await call('Page.navigate', { url });
  await sleep(waitMs);
  const { contentSize } = await call('Page.getLayoutMetrics');
  const shot = await call('Page.captureScreenshot', {
    format: 'png', captureBeyondViewport: true,
    clip: { x: 0, y: 0, width: Math.ceil(contentSize.width), height: Math.min(6000, Math.ceil(contentSize.height)), scale: 1 },
  });
  fs.writeFileSync(out, Buffer.from(shot.data, 'base64'));
  const title = (await call('Runtime.evaluate', { expression: 'document.title', returnByValue: true })).result.value;
  console.log(`shot ${out} (${fs.statSync(out).size} bytes) title="${title}"`);
  ws.close(); chrome.kill(); process.exit(0);
})().catch(e => { console.error(e); chrome.kill(); process.exit(1); });
