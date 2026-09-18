/* SP4 final fix wave -- FRONTEND regression suite (FE1..FE5).
 * Same harness as verify_task15_fr*.js / t17_caret.js: headless Chrome driven
 * over the raw CDP WebSocket (Node's native WebSocket/fetch, no npm deps),
 * against `python -m http.server` serving a web/ directory with ?mock=1.
 *   HOST=http://127.0.0.1:8123 CDP_PORT=9788 node verify_fe_fixes.js
 * Run it twice: once against a pre-fix web/ (RED) and once against the fixed
 * one (GREEN).
 */
const PORT = process.env.CDP_PORT || 9788;
const BASE = `http://127.0.0.1:${PORT}`;
const HOST = process.env.HOST || 'http://127.0.0.1:8123';

async function jfetch(p, opts) { const r = await fetch(BASE + p, opts); return r.json(); }
function once(ws, method) {
  return new Promise((resolve) => {
    function onmsg(ev) { const m = JSON.parse(ev.data); if (m.method === method) { ws.removeEventListener('message', onmsg); resolve(m); } }
    ws.addEventListener('message', onmsg);
  });
}
function send(ws, id, method, params) {
  return new Promise((resolve, reject) => {
    const t = setTimeout(() => reject(new Error('CDP timeout: ' + method)), 10000);
    function onmsg(ev) {
      const m = JSON.parse(ev.data);
      if (m.id === id) { clearTimeout(t); ws.removeEventListener('message', onmsg); resolve(m); }
    }
    ws.addEventListener('message', onmsg);
    ws.send(JSON.stringify({ id, method, params: params || {} }));
  });
}
async function evalJs(ws, id, expr, awaitPromise) {
  const r = await send(ws, id, 'Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: !!awaitPromise });
  if (r.result.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails));
  return r.result.result.value;
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let failures = 0, passes = 0;
function check(name, cond, detail) {
  if (cond) { console.log('PASS:', name); passes++; }
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
  await Promise.race([loaded, new Promise((_, rj) => setTimeout(() => rj(new Error('load timeout')), 12000))]);
}
async function waitFor(ws, nid, expr, tries = 40, ms = 150) {
  for (let i = 0; i < tries; i++) {
    if (await evalJs(ws, nid(), expr)) return true;
    await sleep(ms);
  }
  return false;
}

/* Sets a config field's value the way a keystroke does: through the real
 * delegated `input` listener, so HG.actions.cfgFieldInput records it in
 * HG.state.cfgDirty exactly as it would for an operator. */
const setField = (group, key, val) => `
  (function () {
    var el = document.querySelector('[data-cfg-group="${group}"][data-cfg-key="${key}"]');
    if (!el) return 'NOFIELD';
    el.value = ${JSON.stringify(String(val))};
    el.dispatchEvent(new Event('input', { bubbles: true }));
    return el.value;
  })()`;
const fieldVal = (group, key) => `
  (function () {
    var el = document.querySelector('[data-cfg-group="${group}"][data-cfg-key="${key}"]');
    return el ? el.value : null;
  })()`;
const clickTab = (group) => `
  (function () {
    var b = document.querySelector('[data-action="cfg-tab"][data-group="${group}"]');
    if (!b) return 'NOTAB';
    b.click();
    return 'ok';
  })()`;

/* ========== FE1: #/config/0 renders the master's real values ========== */
async function fe1() {
  console.log('\n==== FE1: master config page shows real STA_SSID / TZ / HOSTNAME ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/config/0', 360, 800);
  const ok = await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  check('setup: master config page loaded (mgroup tabs present)', ok);

  const sta = await evalJs(ws, nid(), fieldVal('WIFI', 'STA_SSID'));
  check('WIFI.STA_SSID renders the document value ("HomeWiFi"), not blank', sta === 'HomeWiFi', sta);
  const apssid = await evalJs(ws, nid(), fieldVal('WIFI', 'AP_SSID'));
  check('WIFI.AP_SSID renders the document value ("HillGrow")', apssid === 'HillGrow', apssid);
  const stapass = await evalJs(ws, nid(), fieldVal('WIFI', 'STA_PASS'));
  check('WIFI.STA_PASS renders blank (secret withheld by ?secrets=0)', stapass === '', stapass);

  await evalJs(ws, nid(), clickTab('TIME'));
  await sleep(120);
  const tz = await evalJs(ws, nid(), fieldVal('TIME', 'TZ'));
  check('TIME.TZ renders the document value, not blank', tz === 'CET-1CEST,M3.5.0,M10.5.0/3', tz);
  const ntp = await evalJs(ws, nid(), fieldVal('TIME', 'NTP'));
  check('TIME.NTP renders the document value ("pool.ntp.org")', ntp === 'pool.ntp.org', ntp);

  await evalJs(ws, nid(), clickTab('SYS'));
  await sleep(120);
  const host = await evalJs(ws, nid(), fieldVal('SYS', 'HOSTNAME'));
  check('SYS.HOSTNAME renders the document value ("hillgrow")', host === 'hillgrow', host);

  /* A zone document must keep using the nested accessor -- the FE1 fix must
   * not swap the two the other way round. */
  await evalJs(ws, nid(), "location.hash = '#/config/1'; 'nav'");
  await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  await evalJs(ws, nid(), clickTab('WATER'));
  await sleep(120);
  const target0 = await evalJs(ws, nid(), fieldVal('WATER', 'TARGET'));
  check('zone document still renders through the nested accessor (WATER.TARGET = 61)', target0 === '61', target0);

  await closeTab(ws, target);
}

/* ========== FE2: touching+clearing a secret must not wipe it ========== */
async function fe2() {
  console.log('\n==== FE2: a blanked STA_PASS is dropped from the merge body ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/config/0', 360, 800);
  const ok = await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  check('setup: master config page loaded', ok);

  const absent = await evalJs(ws, nid(), "!('STA_PASS' in (HG.state.cfgDoc[0].WIFI || {}))");
  check('setup: the ?secrets=0 document really OMITS STA_PASS (as hg_json_export_mcfg does)', absent === true, absent);

  /* The scenario: focus the password, type a character, delete it again. */
  await evalJs(ws, nid(), setField('WIFI', 'STA_PASS', 'x'));
  await evalJs(ws, nid(), setField('WIFI', 'STA_PASS', ''));
  const dirty = await evalJs(ws, nid(), "HG.state.cfgDirty[0]['WIFI|-1|STA_PASS']");
  check('setup: the blank really is recorded as a dirty field', dirty === '', dirty);

  const body1 = JSON.parse(await evalJs(ws, nid(),
    "JSON.stringify(HG.buildCfgMergeBody(0, HG.state.schema, HG.state.cfgDoc[0], HG.state.cfgDirty[0]))"));
  check('merge body carries NO STA_PASS after a touch-and-clear', !(body1.WIFI && 'STA_PASS' in body1.WIFI), body1);
  check('merge body is completely empty (nothing else changed)', Object.keys(body1).length === 0, body1);

  /* End to end: Save must say "no changes" and must not PUT at all. */
  await evalJs(ws, nid(), "window.__lastPut = HG.mock.lastPut; document.querySelector('form.cfg-form').requestSubmit(); 'saved'");
  await sleep(400);
  const putSeen = await evalJs(ws, nid(), "JSON.stringify(HG.mock.lastPut || null)");
  check('Save issues no PUT at all', putSeen === 'null', putSeen);
  const msg = await evalJs(ws, nid(), "HG.state.cfgMsg");
  check('Save reports "No changes to save"', /no changes/i.test(msg || ''), msg);

  /* Positive control 1: a real new password IS still sent. */
  await evalJs(ws, nid(), setField('WIFI', 'STA_PASS', 'newsecret123'));
  const body2 = JSON.parse(await evalJs(ws, nid(),
    "JSON.stringify(HG.buildCfgMergeBody(0, HG.state.schema, HG.state.cfgDoc[0], HG.state.cfgDirty[0]))"));
  check('a genuinely typed password IS still carried', body2.WIFI && body2.WIFI.STA_PASS === 'newsecret123', body2);

  /* Positive control 2: blanking a NON-secret field is still a real edit. */
  await evalJs(ws, nid(), "HG.state.cfgDirty[0] = {}; 'cleared'");
  await evalJs(ws, nid(), clickTab('TIME'));
  await sleep(120);
  await evalJs(ws, nid(), setField('TIME', 'NTP', ''));
  const body3 = JSON.parse(await evalJs(ws, nid(),
    "JSON.stringify(HG.buildCfgMergeBody(0, HG.state.schema, HG.state.cfgDoc[0], HG.state.cfgDirty[0]))"));
  check('blanking a NON-secret field is still sent (not over-broadly dropped)', body3.TIME && body3.TIME.NTP === '', body3);

  await closeTab(ws, target);
}

/* ========== FE3: logout disarms the previous operator's config edits ========== */
async function fe3() {
  console.log('\n==== FE3: logout clears cfgDirty/cfgDoc, not just HG.drafts ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/config/1', 360, 800);
  const ok = await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  check('setup: zone 1 config page loaded', ok);
  await evalJs(ws, nid(), clickTab('WATER'));
  await sleep(120);
  await evalJs(ws, nid(), setField('WATER', 'TARGET', '95'));
  const armed = await evalJs(ws, nid(), "HG.state.cfgDirty[1]['WATER|0|TARGET']");
  check('setup: operator A armed WATER TARGET = 95 (unsaved)', armed === 95, armed);

  await evalJs(ws, nid(), "document.querySelector('[data-action=\"logout\"]').click(); 'out'");
  await waitFor(ws, nid, "location.hash === '#/login'");
  const after = JSON.parse(await evalJs(ws, nid(), `JSON.stringify({
    hash: location.hash,
    dirtyZones: Object.keys(HG.state.cfgDirty).length,
    docZones: Object.keys(HG.state.cfgDoc).length,
    drafts: Object.keys(HG.drafts).length
  })`));
  check('logout lands on #/login', after.hash === '#/login', after);
  check('logout clears every zone\'s cfgDirty', after.dirtyZones === 0, after);
  check('logout clears every cached cfgDoc', after.docZones === 0, after);
  check('logout still clears HG.drafts (unchanged behaviour)', after.drafts === 0, after);

  /* Operator B logs in and opens the same zone. */
  await evalJs(ws, nid(), `
    (function () {
      var i = document.getElementById('login-password');
      i.value = 'hillgrow1';
      i.dispatchEvent(new Event('input', { bubbles: true }));
      document.querySelector('form.login-card').requestSubmit();
      return 'submitted';
    })()`);
  const back = await waitFor(ws, nid, "location.hash === '#/dashboard'");
  check('operator B is logged in', back);
  await evalJs(ws, nid(), "location.hash = '#/config/1'; 'nav'");
  await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  await evalJs(ws, nid(), clickTab('WATER'));
  await sleep(150);
  const shown = await evalJs(ws, nid(), fieldVal('WATER', 'TARGET'));
  check('operator B sees the DOCUMENT value (61), not A\'s unreviewed 95', shown === '61', shown);
  const body = JSON.parse(await evalJs(ws, nid(),
    "JSON.stringify(HG.buildCfgMergeBody(1, HG.state.schema, HG.state.cfgDoc[1], HG.state.cfgDirty[1] || {}))"));
  check('operator B\'s merge body carries none of A\'s edits', Object.keys(body.cfg).length === 0, body);

  await closeTab(ws, target);
}

/* ========== FE4: a non-numeric #/config/<id> must not storm the master ========== */
async function fe4() {
  console.log('\n==== FE4: "#/config/2x" does not spin an unbounded /api/config loop ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/dashboard', 360, 800);
  const ok = await waitFor(ws, nid, "!!document.querySelector('.master-card')");
  check('setup: dashboard loaded', ok);

  await evalJs(ws, nid(), `
    window.__cfgCalls = 0;
    var orig = HG.mock.handle.bind(HG.mock);
    HG.mock.handle = function (path, opts) {
      if (path.indexOf('/api/config') === 0) window.__cfgCalls++;
      return orig(path, opts);
    };
    'instrumented'
  `);
  await evalJs(ws, nid(), "location.hash = '#/config/2x'; 'nav'");
  await sleep(1800); /* ~1 poll tick, and plenty of time for a storm to show */
  const calls = await evalJs(ws, nid(), 'window.__cfgCalls');
  check('a non-numeric zone id issues ZERO /api/config fetches', calls === 0, calls);
  const routeName = await evalJs(ws, nid(), 'HG.state.route.name');
  check('the invalid hash falls back to the dashboard route', routeName === 'dashboard', routeName);
  const dash = await evalJs(ws, nid(), "!!document.querySelector('.master-card')");
  check('...and the dashboard is what actually renders', dash === true, dash);
  const zoneRoute = await evalJs(ws, nid(), "HG.router.parse.call(HG.router), (function(){ location.hash = '#/zone/abc'; return HG.router.parse().name; })()");
  check('"#/zone/abc" falls back to the dashboard too', zoneRoute === 'dashboard', zoneRoute);

  /* A valid id must still work, and exactly once. */
  await evalJs(ws, nid(), "window.__cfgCalls = 0; location.hash = '#/config/1'; 'nav'");
  await waitFor(ws, nid, "document.querySelectorAll('.cfg-tab').length > 0");
  await sleep(600);
  const validCalls = await evalJs(ws, nid(), 'window.__cfgCalls');
  check('a valid zone id still loads, with exactly one fetch', validCalls === 1, validCalls);

  await closeTab(ws, target);
}

/* ========== FE5: alarms show elapsed time, not the raw uptime stamp ========== */
async function fe5() {
  console.log('\n==== FE5: alarms render (uptime - stamp), not "<stamp>s ago" ====');
  const { ws, nid, target } = await newTab();
  await nav(ws, nid, HOST + '/?mock=1&autologin=1#/alarms', 360, 800);
  const ok = await waitFor(ws, nid, "document.querySelectorAll('.alarm-list li, .event-list li').length > 0");
  check('setup: alarms page loaded', ok);

  /* fixture: master uptime_s 123456, active since_s 900 -> 122556s = 1d 10h.
   * Read the age span's EXACT text -- a substring test on the whole <li> would
   * let "900s ago" satisfy a /0s ago/ style check. */
  const AGE_ACTIVE = "document.querySelector('.alarm-active .muted').textContent";
  const AGE_EVENTS = "JSON.stringify(Array.prototype.map.call(document.querySelectorAll('.event-list li .muted'), function (e) { return e.textContent; }))";
  let activeAge = await evalJs(ws, nid(), AGE_ACTIVE);
  check('active alarm does NOT print the raw stamp ("900s ago")', activeAge !== '900s ago', activeAge);
  check('active alarm prints the elapsed time (exactly "1d 10h ago")', activeAge === '1d 10h ago', activeAge);

  /* The exact case confirmed on the bench: uptime 2741, event stamp 2634. */
  await evalJs(ws, nid(), `
    (function () {
      var orig = HG.mock.handle.bind(HG.mock);
      var st = JSON.parse(JSON.stringify(HG.mock.fixtures.state));
      st.master.uptime_s = 2741;
      var al = { active: [{ key: 'RING_OPEN', text: 'ring open', since_s: 2634 }],
                 events: [{ at_s: 2634, text: 'wire M->Z2' }, { at_s: 3000, text: 'from the future' }] };
      HG.mock.handle = function (path, opts) {
        if (path.indexOf('/api/state') === 0) return Promise.resolve(JSON.parse(JSON.stringify(st)));
        if (path.indexOf('/api/alarms') === 0) return Promise.resolve(JSON.parse(JSON.stringify(al)));
        return orig(path, opts);
      };
      HG.state.snap = JSON.parse(JSON.stringify(st));
      HG.state.alarms = JSON.parse(JSON.stringify(al));
      HG.render();
      return 'installed';
    })()`);
  await sleep(150);
  const benchActive = await evalJs(ws, nid(), AGE_ACTIVE);
  const benchEvents = JSON.parse(await evalJs(ws, nid(), AGE_EVENTS));
  /* 2741 - 2634 = 107 s, formatted as "1m 47s" -- the point is that it is the
   * 107 s elapsed, not the 2634 s stamp (~44 min) the page used to print. */
  check('bench case: uptime 2741 / stamp 2634 renders the 107s elapsed ("1m 47s ago"), not "2634s ago"',
    benchEvents[0] === '1m 47s ago', benchEvents);
  check('active alarm uses the same elapsed rule', benchActive === '1m 47s ago', benchActive);
  check('a stamp newer than the last state sample clamps to exactly "0s ago" (never negative)',
    benchEvents[1] === '0s ago', benchEvents);

  /* No state snapshot yet (a cold load straight onto #/alarms): the page must
   * NOT fall back to printing the stamp as if it were an age. */
  await evalJs(ws, nid(), `
    (function () {
      var orig = HG.mock.handle.bind(HG.mock);
      HG.mock.handle = function (path, opts) {
        if (path.indexOf('/api/state') === 0) return new Promise(function () {});
        return orig(path, opts);
      };
      HG.state.snap = null;
      HG.render();
      return 'nosnap';
    })()`);
  await sleep(150);
  const nosnap = JSON.parse(await evalJs(ws, nid(), AGE_EVENTS));
  check('with no state snapshot the age reads exactly "—", never a raw stamp', nosnap[0] === '—', nosnap);

  await closeTab(ws, target);
}

async function run() {
  await fe1();
  await fe2();
  await fe3();
  await fe4();
  await fe5();
  console.log(`\n${passes}/${passes + failures} checks passed`);
  console.log(failures === 0 ? 'ALL PASS' : `${failures} FAILURE(S)`);
  process.exit(failures === 0 ? 0 : 1);
}
run().catch((e) => { console.error('SCRIPT ERROR', e); process.exit(1); });
