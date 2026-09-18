// Task 17: RED/GREEN repro for the phone caret-jumps-to-0 bug on #/config/N
// number inputs (and a System-page regression check), driven over raw CDP
// (no npm deps -- Node 25's native WebSocket + fetch).
const PORT = process.env.CDP_PORT || 9778;
const BASE = `http://127.0.0.1:${PORT}`;
const HOST = process.env.HOST || 'http://127.0.0.1:8123';

async function jfetch(p, opts) { const r = await fetch(BASE + p, opts); return r.json(); }
function send(ws, id, method, params) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('CDP timeout: ' + method)), 8000);
    function onmsg(ev) {
      const m = JSON.parse(ev.data);
      if (m.id === id) { clearTimeout(t); ws.removeEventListener('message', onmsg); resolve(m); }
    }
    ws.addEventListener('message', onmsg);
    ws.send(JSON.stringify({ id, method, params: params || {} }));
  });
}
function once(ws, method) {
  return new Promise((resolve) => {
    function onmsg(ev) { const m = JSON.parse(ev.data); if (m.method === method) { ws.removeEventListener('message', onmsg); resolve(m); } }
    ws.addEventListener('message', onmsg);
  });
}
async function evalJs(ws, id, expr, awaitPromise) {
  const r = await send(ws, id, 'Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: !!awaitPromise });
  if (r.result.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails));
  return r.result.result.value;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0;
function check(name, cond, detail) {
  if (cond) console.log('PASS:', name);
  else { console.log('FAIL:', name, detail !== undefined ? JSON.stringify(detail) : ''); failures++; }
}

async function newTab() {
  const target = await jfetch('/json/new?about:blank', { method: 'PUT' });
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((r) => ws.addEventListener('open', r));
  let id = 1;
  const nid = () => id++;
  await send(ws, nid(), 'Page.enable');
  await send(ws, nid(), 'Runtime.enable');
  await send(ws, nid(), 'Input.setIgnoreInputEvents', { ignore: false });
  await send(ws, nid(), 'Network.enable');
  await send(ws, nid(), 'Network.setCacheDisabled', { cacheDisabled: true });
  return { ws, nid, target };
}
async function closeTab(ws, target) {
  try { ws.close(); } catch (e) {}
  try { await fetch(BASE + '/json/close/' + target.id); } catch (e) {}
}
async function nav(ws, nid, url, w, h) {
  await send(ws, nid(), 'Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: w < 900 });
  const loaded = once(ws, 'Page.loadEventFired');
  await send(ws, nid(), 'Page.navigate', { url });
  await Promise.race([loaded, new Promise((_, rj) => setTimeout(() => rj(new Error('load timeout')), 10000))]);
}
// Types one printable character using CDP's 'char' key event -- this is
// routed through Chromium's real text-input pipeline (same mechanism
// Puppeteer's sendCharacter/type() uses), so it inserts at the REAL caret
// position the renderer is tracking, exactly like a phone's on-screen
// keyboard would -- unlike setting `.value` from JS, which bypasses the
// caret entirely.
async function typeChar(ws, nid, ch) {
  await send(ws, nid(), 'Input.dispatchKeyEvent', { type: 'char', text: ch, unmodifiedText: ch });
}

async function main() {
  console.log('\n==== Task 17: config-editor number-field caret survives a poll rerender ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/config/2', 390, 800);

  // Zone 2's config loads async (schema + doc fetch); wait for the WATER tab.
  let ready = false;
  for (let i = 0; i < 30 && !ready; i++) { await sleep(200); ready = await evalJs(ws, nid(), "document.querySelectorAll('.cfg-tab').length > 0"); }
  check('setup: zone 2 config editor loaded', ready);

  // WATER is not the default tab (ZONECFG is) -- click it, matching the
  // brief's own example field.
  await evalJs(ws, nid(), `
    var btns = Array.prototype.slice.call(document.querySelectorAll('.cfg-tab'));
    var water = btns.filter(function(b){return b.textContent==='WATER';})[0];
    water.click();
    'clicked'
  `);
  await sleep(50);
  const targetSel = 'input[data-cfg-group="WATER"][data-cfg-idx="0"][data-cfg-key="TARGET"]';
  const hasTarget = await evalJs(ws, nid(), `!!document.querySelector('${targetSel}')`);
  check('setup: WATER TARGET (shelf 0) field is on screen', hasTarget);
  const fieldType = await evalJs(ws, nid(), `document.querySelector('${targetSel}').type`);
  check('setup: TARGET field really is <input type=number>', fieldType === 'number', fieldType);

  // ---- Part A: DOM node identity -- does a poll tick rebuild the focused field? ----
  await evalJs(ws, nid(), `
    var el = document.querySelector('${targetSel}');
    el.value = '';
    el.dispatchEvent(new Event('input', {bubbles:true}));
    el.focus();
    window.__before = el;
    'focused'
  `);
  // Same code path HG.poll's 2s tick uses: HG.poll.tick() -> fetch /api/state
  // (mocked) -> HG.rerender(). Call it directly rather than waiting out the
  // real 2s cadence.
  await evalJs(ws, nid(), 'HG.poll.tick(); "ticked"');
  await sleep(250); // mock /api/state resolves after 120ms
  const sameNode = await evalJs(ws, nid(), `document.querySelector('${targetSel}') === window.__before`);
  const stillFocused = await evalJs(ws, nid(), `document.activeElement === document.querySelector('${targetSel}')`);
  check('DOM node for the focused TARGET field is NOT rebuilt by a poll tick', sameNode, { sameNode });
  check('TARGET field is still document.activeElement after the poll tick', stillFocused);

  // ---- Part B: the phone symptom itself -- type a digit, poll fires
  // mid-type, type the next digit; on the buggy build the caret resets to 0
  // and the digits land in the wrong order. ----
  await evalJs(ws, nid(), `
    var el = document.querySelector('${targetSel}');
    el.value = '';
    el.dispatchEvent(new Event('input', {bubbles:true}));
    el.focus();
    'reset'
  `);
  await typeChar(ws, nid, '1');
  await sleep(30);
  let midValue = await evalJs(ws, nid(), `document.querySelector('${targetSel}').value`);
  check('typed "1" lands correctly first', midValue === '1', midValue);

  await evalJs(ws, nid(), 'HG.poll.tick(); "ticked"');
  await sleep(250);

  await typeChar(ws, nid, '2');
  await sleep(30);
  const finalValue = await evalJs(ws, nid(), `document.querySelector('${targetSel}').value`);
  // Correct (fixed) behaviour: caret stayed at the end -> "12".
  // Buggy behaviour: caret reset to 0 -> the "2" lands in FRONT -> "21".
  check('typing "1", poll tick, typing "2" yields "12" (caret preserved, NOT "21")', finalValue === '12', finalValue);

  // ---- Part C: a NON-focused config page still updates on poll (header dot) ----
  await evalJs(ws, nid(), `document.activeElement && document.activeElement.blur && document.activeElement.blur(); 'blurred'`);
  await evalJs(ws, nid(), 'HG.state.online = false; "setoffline"');
  await evalJs(ws, nid(), 'HG.rerender(); "rerendered"');
  const dotCls = await evalJs(ws, nid(), `document.querySelector('.hdr .dot').className`);
  check('header dot reflects offline after a poll rerender with nothing focused', /offline/.test(dotCls), dotCls);
  await evalJs(ws, nid(), 'HG.state.online = true; HG.rerender(); "restored"');
  const dotCls2 = await evalJs(ws, nid(), `document.querySelector('.hdr .dot').className`);
  check('header dot reflects back online', dotCls2.split(/\s+/).indexOf('online') !== -1, dotCls2);

  // ---- Part D: header dot ALSO updates while a field IS focused (the
  // brief: "still update ... any header online/offline indicator") ----
  await evalJs(ws, nid(), `document.querySelector('${targetSel}').focus(); 'refocused'`);
  await evalJs(ws, nid(), 'HG.state.online = false; HG.rerender(); "rerendered"');
  const dotCls3 = await evalJs(ws, nid(), `document.querySelector('.hdr .dot').className`);
  check('header dot still updates to offline even while a field is focused', /offline/.test(dotCls3), dotCls3);
  const stillFocused2 = await evalJs(ws, nid(), `document.activeElement === document.querySelector('${targetSel}')`);
  check('...and the field is STILL focused (no rebuild happened)', stillFocused2);
  await evalJs(ws, nid(), 'HG.state.online = true; "restored"');

  await closeTab(ws, target);
}

async function dashboardTest() {
  console.log('\n==== dashboard poll still re-renders normally (nothing focused, no gate) ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/dashboard', 390, 800);
  await sleep(300);
  const before = await evalJs(ws, nid(), `window.__d = document.querySelector('.master-card'); !!window.__d`);
  check('setup: dashboard master card present', before);
  await evalJs(ws, nid(), 'HG.poll.tick(); "ticked"');
  await sleep(250);
  const rebuilt = await evalJs(ws, nid(), `document.querySelector('.master-card') !== window.__d`);
  check('dashboard IS rebuilt by a poll tick (no gate applies outside config/system)', rebuilt);
  await closeTab(ws, target);
}

async function systemPasswordTest() {
  console.log('\n==== System page: web-password field survives typing across a poll tick ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/system', 390, 800);
  let ready = false;
  for (let i = 0; i < 30 && !ready; i++) { await sleep(200); ready = await evalJs(ws, nid(), "!!document.getElementById('sys-pw-new')"); }
  check('setup: system page loaded (password field present)', ready);
  const fieldType = await evalJs(ws, nid(), `document.getElementById('sys-pw-new').type`);
  check('setup: sys-pw-new really is <input type=password>', fieldType === 'password', fieldType);

  await evalJs(ws, nid(), `
    var el = document.getElementById('sys-pw-new');
    el.value = '';
    el.focus();
    window.__before = el;
    'focused'
  `);
  await typeChar(ws, nid, 'a');
  await sleep(30);
  await evalJs(ws, nid(), 'HG.poll.tick(); "ticked"');
  await sleep(250);
  await typeChar(ws, nid, 'b');
  await sleep(30);
  const val = await evalJs(ws, nid(), `document.getElementById('sys-pw-new').value`);
  check('typed "a", poll tick, typed "b" yields "ab" (correct order)', val === 'ab', val);
  const sameNode = await evalJs(ws, nid(), `document.getElementById('sys-pw-new') === window.__before`);
  check('sys-pw-new DOM node not rebuilt by the poll tick while focused', sameNode);

  await closeTab(ws, target);
}

async function run() {
  await main();
  await dashboardTest();
  await systemPasswordTest();
  console.log(failures === 0 ? '\nALL PASS' : `\n${failures} FAILURE(S)`);
  process.exit(failures === 0 ? 0 : 1);
}
run().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
