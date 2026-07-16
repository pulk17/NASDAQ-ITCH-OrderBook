#!/usr/bin/env node
// Navigate to a URL, wait, then evaluate an expression in the page and print it.
// Also prints any console errors/exceptions. Usage: node tools/probe.js <url> <waitMs> '<expr>'
const { spawn } = require('child_process');
const url = process.argv[2], waitMs = +(process.argv[3] || 8000), expr = process.argv[4] || '1';
const PORT = 9334;
const chrome = spawn('chromium', ['--headless=new','--disable-gpu','--no-sandbox',
  `--remote-debugging-port=${PORT}`,'--window-size=1440,2200','about:blank'], { stdio:'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  let t;
  for(let i=0;i<40;i++){ try{ const l=await fetch(`http://localhost:${PORT}/json`).then(r=>r.json());
    t=l.find(x=>x.type==='page'); if(t) break; }catch(e){} await sleep(250); }
  const ws = new WebSocket(t.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let seq = 1; const logs = [];
  const call = (method, params={}) => new Promise(res => { const id = seq++;
    const on = e => { const m = JSON.parse(e.data); if(m.id===id){ ws.removeEventListener('message', on); res(m.result); } };
    ws.addEventListener('message', on); ws.send(JSON.stringify({ id, method, params })); });
  ws.addEventListener('message', e => { const m = JSON.parse(e.data);
    if(m.method === 'Runtime.consoleAPICalled') logs.push('console: ' + m.params.args.map(a=>a.value).join(' '));
    if(m.method === 'Runtime.exceptionThrown') logs.push('EXCEPTION: ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text)); });
  await call('Runtime.enable'); await call('Page.enable');
  await call('Page.navigate', { url });
  await sleep(waitMs);
  // awaitPromise so an async expression resolves before we read it; the caller is
  // responsible for returning something JSON-able (a bare object works too).
  const r = await call('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
  console.log('RESULT:', typeof r.result.value === 'string' ? r.result.value : JSON.stringify(r.result.value));
  if(logs.length) console.log(logs.slice(-15).join('\n'));
  ws.close(); chrome.kill(); process.exit(0);
})().catch(e => { console.error(e); chrome.kill(); process.exit(1); });
