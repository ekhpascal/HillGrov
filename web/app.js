/* HillGrow web UI -- vanilla ES2017, no framework, no external resources.
 * Single namespace HG. See .superpowers/sdd/2026-09-10-sp4-web-ui/task-14-brief.md.
 */
(function () {
"use strict";

var HG = window.HG = {};

/* ---------- esc: the only way server/operator text may reach innerHTML ---------- */
var ESC_MAP = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" };
HG.esc = function (s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, function (c) { return ESC_MAP[c]; });
};

/* ---------- state ---------- */
HG.state = {
  auth: null,        /* null = unknown, true = logged in, false = needs login */
  online: true,       /* last /api/state poll succeeded (transport-level) */
  snap: null,          /* last GET /api/state document */
  route: { name: "dashboard" },
  lastRoute: null,      /* hash to return to after a login forced by a 401 */
  loginError: "",
  console: {},           /* zoneId -> { log, history, histIdx, forward } -- drafts live in HG.drafts */

  /* ---- config editor (#/config/N, N=0 is the master's own mcfg) ----
   * schema is fetched once and cached; cfgDoc/cfgDirty are keyed by zone id
   * so switching zones never leaks one zone's edits into another's document.
   * cfgDirty[id] maps "GROUP|idx|KEY" (idx -1 for zone/master-scoped fields)
   * to the field's *typed* edited value (number/bool/string matching the
   * field's JSON wire type) -- reading from this map instead of the DOM is
   * what lets an edited field survive the 2s state poll's rerender without
   * needing a per-field HG.drafts entry. */
  schema: null,
  cfgDoc: {},
  cfgDirty: {},
  cfgUi: { zone: null, group: null, idx: 0 },
  cfgBad: null,          /* { group, idx, key, msg } for the field a 400 named */
  cfgMsg: "",
  cfgSaving: false,
  cfgLoading: null,      /* zone id currently being fetched, or null */
  cfgLoadErr: "",
  cfgLoadFailedId: null, /* zone id a load already failed for -- stops an infinite retry loop, see HG.ensureConfigLoaded */
  cfgSelfHealAttempted: null, /* zone id an automatic "it came online" retry was already tried for, since it was last seen NOT ready */

  alarms: null,           /* last GET /api/alarms document */

  sys: {                  /* #/system page transient UI state */
    wifiScanning: false, wifiScan: null, wifiScanErr: "",
    fwMasterUploading: false, fwMasterPct: 0, fwMasterMsg: "", fwMasterOk: false,
    fwZoneUploading: false, fwZonePct: 0, fwZoneMsg: "", fwZoneOk: false,
    fleetMsg: "", rebootMsg: ""
  }
};

HG.consoleState = function (id) {
  if (!HG.state.console[id]) {
    HG.state.console[id] = { log: [], history: [], histIdx: 0, forward: false };
  }
  return HG.state.console[id];
};

/* ---------- HG.drafts: survives a poll-driven rerender ----------
 * A poll tick replaces #app's innerHTML wholesale; withFocusPreserved keeps
 * focus+selection but NOT the value, so any input that must survive a tick
 * mid-type has to thread its value through here instead of relying on the
 * DOM to remember it. Every such input carries `data-draft` and is rendered
 * with `value="${HG.esc(HG.drafts[key] || '')}"`; the delegated `input`
 * listener below writes back on every keystroke. console-input AND
 * mac-input are keyed per zone (both forms are re-rendered under the same
 * DOM id for every zone) so switching zones doesn't leak one zone's
 * half-typed command/MAC into another zone's input -- an unnoticed MAC
 * draft surviving a zone switch could otherwise get submitted against the
 * wrong zone. */
HG.drafts = {};
HG.consoleDraftKey = function (id) { return "console-input:" + id; };
HG.macDraftKey = function (id) { return "mac-input:" + id; };
HG.draftKey = function (el) {
  if (el.id === "console-input") return HG.consoleDraftKey(HG.state.route.id);
  if (el.id === "mac-input") return HG.macDraftKey(HG.state.route.id);
  return el.id;
};

/* ---------- ApiError ---------- */
function ApiError(status, message) {
  var e = Error.call(this, message);
  this.name = "ApiError";
  this.status = status;
  this.message = message;
  this.stack = e.stack;
}
ApiError.prototype = Object.create(Error.prototype);
ApiError.prototype.constructor = ApiError;
HG.ApiError = ApiError;

/* One representative zone document (matches hg_json_export_cfg's exact shape:
 * gen/hw{HW,shelf[]}/cfg{ZONECFG,shelf[],aux[]}) -- used for every mock zone
 * so the config editor's "10 groups, 4 shelves, 2 aux" shape is exercised
 * without hand-writing the same nested structure four times over. */
function mockZoneDoc(gen, name) {
  var shelf = [];
  for (var i = 0; i < 4; i++) {
    shelf.push({
      SHELF: { CROP: i === 0 ? "Basil" : "", ENABLED: i < 2 ? true : false, PROFILE: 0 },
      LIGHT: { ON: "06:00", OFF: "22:00", WHITE: 80, RED: 40, RAMP_MIN: 10, DLI: 0 },
      WATER: { MODE: "AUTO", TARGET: 61, HYST: 5, SETTLE_MIN: 10, DOSE_S: 20,
               INTERVAL_MIN: 120, MAX_DOSES: 6, DIFF_MAX: 15, WIN_START: "00:00", WIN_END: "00:00" },
      FAN: { MODE: "CYCLE", ON_MIN: 10, PERIOD_MIN: 60 },
      VIB: { MODE: "OFF", INTENSITY: 60, PULSE_S: 5, INTERVAL_MIN: 60, START: "08:00", END: "20:00" }
    });
  }
  var hwShelf = [];
  for (var j = 0; j < 4; j++) {
    hwShelf.push({
      HWSHELF: { LED_W: j, LED_R: j + 4, PUMP: j + 8, FAN: 255, SOIL_A: j, SOIL_B: j + 1, VIB: 255,
                 LED_MAX_W: 100, LED_MAX_R: 100, PUMP_MAX_RUN_S: 30, PUMP_MAX_DAILY_S: 600 },
      CAL: { DRY_A: 2800, DRY_B: 2800, WET_A: 1200, WET_B: 1200, MIN_OK: 300, MAX_OK: 3000 }
    });
  }
  return {
    gen: gen,
    hw: { HW: { SHELVES: 4, PCA_ADDR: 64, PCF_ADDR: 32, SOIL_BACKEND: "INTERNAL", PCF_ACTLOW: 65535, PCA_HZ: 1000 },
          shelf: hwShelf },
    cfg: {
      ZONECFG: { NAME: name || "", LINKLOSS_S: 30 },
      shelf: shelf,
      aux: [{ AUX: { MODE: "OFF", PULSE_S: 5, INTERVAL_MIN: 60, START: "08:00", END: "20:00" } },
            { AUX: { MODE: "OFF", PULSE_S: 5, INTERVAL_MIN: 60, START: "08:00", END: "20:00" } }]
    }
  };
}

function mockParseQuery(path) {
  var q = {};
  var qi = path.indexOf("?");
  if (qi === -1) return q;
  path.slice(qi + 1).split("&").forEach(function (kv) {
    var eq = kv.indexOf("=");
    if (eq === -1) return;
    q[decodeURIComponent(kv.slice(0, eq))] = decodeURIComponent(kv.slice(eq + 1));
  });
  return q;
}

/* ---------- HG.mock: fixtures + fetch-shaped handler, active on ?mock=1 ---------- */
HG.mock = {
  active: /(^|[?&])mock=1(&|$)/.test(location.search),
  /* &autologin=1 is a mock-only screenshot/dev convenience: plain ?mock=1 still
   * boots to the login view (matching real unauthenticated behaviour). */
  loggedIn: /(^|[?&])autologin=1(&|$)/.test(location.search),
  fixtures: {
    state: {
      master: {
        version: "0.4.0", uptime_s: 123456, heap_min_kb: 142,
        time: "2026-09-14 10:30:00", time_src: "NTP",
        wifi: {
          sta: { up: true, ip: "192.168.1.42", ssid: "HomeWiFi", rssi: -52, reason: "" },
          ap: { ssid: "HillGrow", clients: 1, ip: "192.168.7.7" }
        },
        fw: { slot: "ota_0", state: "VALID", other: "NONE", upload_kind: "", upload_pct: 0 },
        fleet: "IDLE",
        alarms: { active: 1, total: 12 },
        http: { cmd_quarantined: 0 },
        defaults: { web: true, ap: false }
      },
      nodes: [
        {
          id: 1, name: "Basil", mac: "c0:5d:89:df:2a:88", health: "ONLINE", fw: "0.1.0",
          gen: 13, hops: 1, link: 7, link_stale: false, cfg_sync: "OK", hb_age_s: 4,
          uptime_s: 36000, heap_kb: 180, reset: 1, faults: "0x0", mode: 0,
          shelves: [
            { pct_a: 62, pct_b: 58, white: 80, red: 40, out: 1, pump_s: 12 },
            { pct_a: 55, pct_b: 50, white: 75, red: 35, out: 0, pump_s: 0 },
            { pct_a: 0, pct_b: 0, white: 0, red: 0, out: 0, pump_s: 0 },
            { pct_a: 0, pct_b: 0, white: 0, red: 0, out: 0, pump_s: 0 }
          ]
        },
        {
          id: 2, name: "", mac: "c0:5d:89:df:2a:d0", health: "OFFLINE", fw: "0.1.0",
          gen: 34, hops: 2, link: 7, link_stale: true, cfg_sync: "FAILED", hb_age_s: 900,
          uptime_s: 0, heap_kb: 0, reset: 1, faults: "0x0", mode: 0,
          shelves: []
        }
      ],
      ring: { state: "OK", size: 2, online: 6, blame: "" }
    },
    /* Exact shape of hg_json_schema()'s output -- see components/hg_json/hg_json_schema.c. */
    schema: {
      groups: [
        { name: "ZONECFG", scope: 0, fields: [
          { key: "NAME", type: "STR16", min: 0, max: 15 },
          { key: "LINKLOSS_S", type: "U16", min: 10, max: 600 } ] },
        { name: "SHELF", scope: 1, fields: [
          { key: "CROP", type: "STR16", min: 0, max: 15 },
          { key: "ENABLED", type: "BOOL", min: 0, max: 1 },
          { key: "PROFILE", type: "U8", min: 0, max: 16 } ] },
        { name: "LIGHT", scope: 1, fields: [
          { key: "ON", type: "HHMM", min: 0, max: 1439 },
          { key: "OFF", type: "HHMM", min: 0, max: 1439 },
          { key: "WHITE", type: "U8", min: 0, max: 100 },
          { key: "RED", type: "U8", min: 0, max: 100 },
          { key: "RAMP_MIN", type: "U8", min: 0, max: 120 },
          { key: "DLI", type: "U16", min: 0, max: 1000 } ] },
        { name: "WATER", scope: 1, fields: [
          { key: "MODE", type: "ENUM", min: 0, max: 1, enums: ["OFF", "AUTO"] },
          { key: "TARGET", type: "U8", min: 0, max: 100 },
          { key: "HYST", type: "U8", min: 1, max: 30 },
          { key: "SETTLE_MIN", type: "U8", min: 1, max: 60 },
          { key: "DOSE_S", type: "U16", min: 1, max: 300 },
          { key: "INTERVAL_MIN", type: "U16", min: 10, max: 1440 },
          { key: "MAX_DOSES", type: "U8", min: 0, max: 24 },
          { key: "DIFF_MAX", type: "U8", min: 5, max: 50 },
          { key: "WIN_START", type: "HHMM", min: 0, max: 1439 },
          { key: "WIN_END", type: "HHMM", min: 0, max: 1439 } ] },
        { name: "FAN", scope: 1, fields: [
          { key: "MODE", type: "ENUM", min: 0, max: 3, enums: ["OFF", "ON", "LIGHT", "CYCLE"] },
          { key: "ON_MIN", type: "U8", min: 0, max: 60 },
          { key: "PERIOD_MIN", type: "U16", min: 0, max: 1440 } ] },
        { name: "VIB", scope: 1, fields: [
          { key: "MODE", type: "ENUM", min: 0, max: 1, enums: ["OFF", "PULSE"] },
          { key: "INTENSITY", type: "U8", min: 20, max: 100 },
          { key: "PULSE_S", type: "U8", min: 1, max: 30 },
          { key: "INTERVAL_MIN", type: "U16", min: 5, max: 1440 },
          { key: "START", type: "HHMM", min: 0, max: 1439 },
          { key: "END", type: "HHMM", min: 0, max: 1439 } ] },
        { name: "AUX", scope: 2, fields: [
          { key: "MODE", type: "ENUM", min: 0, max: 1, enums: ["OFF", "PULSE"] },
          { key: "PULSE_S", type: "U8", min: 1, max: 30 },
          { key: "INTERVAL_MIN", type: "U16", min: 5, max: 1440 },
          { key: "START", type: "HHMM", min: 0, max: 1439 },
          { key: "END", type: "HHMM", min: 0, max: 1439 } ] },
        { name: "HW", scope: 0, fields: [
          { key: "SHELVES", type: "U8", min: 1, max: 4 },
          { key: "PCA_ADDR", type: "U8", min: 0, max: 127 },
          { key: "PCF_ADDR", type: "U8", min: 0, max: 127 },
          { key: "SOIL_BACKEND", type: "ENUM", min: 0, max: 1, enums: ["INTERNAL", "ADS1115"] },
          { key: "PCF_ACTLOW", type: "U16", min: 0, max: 65535 },
          { key: "PCA_HZ", type: "U16", min: 200, max: 1500 } ] },
        { name: "HWSHELF", scope: 1, fields: [
          { key: "LED_W", type: "PIN", min: 0, max: 15 },
          { key: "LED_R", type: "PIN", min: 0, max: 15 },
          { key: "PUMP", type: "PIN", min: 0, max: 15 },
          { key: "FAN", type: "PIN", min: 0, max: 15 },
          { key: "SOIL_A", type: "PIN", min: 0, max: 7 },
          { key: "SOIL_B", type: "PIN", min: 0, max: 7 },
          { key: "VIB", type: "PIN", min: 0, max: 15 },
          { key: "LED_MAX_W", type: "U8", min: 0, max: 100 },
          { key: "LED_MAX_R", type: "U8", min: 0, max: 100 },
          { key: "PUMP_MAX_RUN_S", type: "U16", min: 1, max: 300 },
          { key: "PUMP_MAX_DAILY_S", type: "U16", min: 1, max: 3600 } ] },
        { name: "CAL", scope: 1, fields: [
          { key: "DRY_A", type: "U16", min: 0, max: 3300 },
          { key: "DRY_B", type: "U16", min: 0, max: 3300 },
          { key: "WET_A", type: "U16", min: 0, max: 3300 },
          { key: "WET_B", type: "U16", min: 0, max: 3300 },
          { key: "MIN_OK", type: "U16", min: 0, max: 3300 },
          { key: "MAX_OK", type: "U16", min: 0, max: 3300 } ] }
      ],
      mgroups: [
        { name: "WIFI", fields: [
          { key: "STA_SSID", type: "STR", max: 32, secret: false },
          { key: "STA_PASS", type: "STR", max: 64, secret: true },
          { key: "AP_SSID", type: "STR", max: 32, secret: false },
          { key: "AP_PASS", type: "STR", max: 64, secret: true } ] },
        { name: "TIME", fields: [
          { key: "TZ", type: "STR", max: 47, secret: false },
          { key: "NTP", type: "STR", max: 47, secret: false } ] },
        { name: "SYS", fields: [
          { key: "HOSTNAME", type: "STR", max: 23, secret: false } ] }
      ],
      hw_readonly: true
    },
    alarms: {
      active: [{ key: "Z2_OFFLINE", text: "Zone 2 offline", since_s: 900 }],
      events: [{ at_s: 900, text: "Zone 2 offline" }, { at_s: 4000, text: "Master rebooted" }]
    },
    scan: [
      { ssid: "HomeWiFi", rssi: -52, auth: 3 },
      { ssid: "Neighbour5G", rssi: -81, auth: 3 },
      { ssid: "OpenGuest", rssi: -70, auth: 0 }
    ],
    config: {
      0: { WIFI: { STA_SSID: "HomeWiFi", STA_PASS: "", AP_SSID: "HillGrow", AP_PASS: "" },
           TIME: { TZ: "CET-1CEST,M3.5.0,M10.5.0/3", NTP: "pool.ntp.org" },
           SYS: { HOSTNAME: "hillgrow" } },
      1: mockZoneDoc(13, "Basil"),
      2: mockZoneDoc(34, "")
    }
  },
  /* Mirrors hg_field_write's OUT_OF_RANGE check for the numeric types (the
   * shape the bench's "enter 101 on WATER TARGET" check exercises) closely
   * enough to drive the mock 400/path-highlight check without reimplementing
   * the whole of hg_cfg_validate. */
  mockCheckField: function (gname, key, v) {
    var g = null, gs = this.fixtures.schema.groups;
    for (var i = 0; i < gs.length; i++) if (gs[i].name === gname) { g = gs[i]; break; }
    if (!g) return null;
    var f = null;
    for (var j = 0; j < g.fields.length; j++) if (g.fields[j].key === key) { f = g.fields[j]; break; }
    if (!f) return null;
    if ((f.type === "U8" || f.type === "U16" || f.type === "PIN") && (v < f.min || v > f.max)) return f;
    return null;
  },
  mockValidateCfgPut: function (body) {
    var self = this;
    function scanObj(gname, prefix, obj) {
      for (var key in obj) {
        if (self.mockCheckField(gname, key, obj[key])) return "cfg." + prefix + gname + "." + key;
      }
      return null;
    }
    if (!body.cfg) return null;
    if (body.cfg.ZONECFG) { var p0 = scanObj("ZONECFG", "", body.cfg.ZONECFG); if (p0) return p0; }
    var i, gname, p;
    if (body.cfg.shelf) {
      for (i = 0; i < body.cfg.shelf.length; i++) {
        var el = body.cfg.shelf[i];
        if (!el) continue;
        for (gname in el) { p = scanObj(gname, "shelf[" + i + "].", el[gname]); if (p) return p; }
      }
    }
    if (body.cfg.aux) {
      for (i = 0; i < body.cfg.aux.length; i++) {
        var ael = body.cfg.aux[i];
        if (!ael) continue;
        for (gname in ael) { p = scanObj(gname, "aux[" + i + "].", ael[gname]); if (p) return p; }
      }
    }
    return null;
  },
  handle: function (path, opts) {
    var method = (opts && opts.method) || "GET";
    var qidx = path.indexOf("?");
    var route = qidx === -1 ? path : path.slice(0, qidx);
    var self = this;
    return new Promise(function (resolve) { setTimeout(resolve, 120); }).then(function () {
      if (route === "/api/login") {
        var body = {};
        try { body = JSON.parse(opts.body); } catch (e) { /* ignore */ }
        if (body.password === "hillgrow1") { self.loggedIn = true; return null; }
        throw new ApiError(401, "BAD_PASSWORD");
      }
      if (!self.loggedIn) throw new ApiError(401, "UNAUTHORIZED");
      if (route === "/api/logout") { self.loggedIn = false; return null; }
      if (route === "/api/state") return JSON.parse(JSON.stringify(self.fixtures.state));
      if (route === "/api/schema") return JSON.parse(JSON.stringify(self.fixtures.schema));
      if (route === "/api/alarms") return JSON.parse(JSON.stringify(self.fixtures.alarms));
      if (route === "/api/wifi/scan" && method === "GET") return JSON.parse(JSON.stringify(self.fixtures.scan));
      if (route === "/api/wifi" && method === "POST") {
        var wb = {};
        try { wb = JSON.parse(opts.body); } catch (e) { /* ignore */ }
        if (!wb.sta && !wb.ap) throw new ApiError(400, "INVALID");
        return { ok: true };
      }
      if (route === "/api/password" && method === "POST") {
        var pb = {};
        try { pb = JSON.parse(opts.body); } catch (e) { /* ignore */ }
        if (pb.old !== "hillgrow1") { var e1 = new ApiError(403, "BAD_PASSWORD"); throw e1; }
        return null;
      }
      if (route === "/api/config") {
        var q = mockParseQuery(path);
        var zone = q.zone !== undefined ? Number(q.zone) : 0;
        if (method === "GET") {
          var src = self.fixtures.config[zone] || self.fixtures.config[zone === 0 ? 0 : 1];
          var doc = JSON.parse(JSON.stringify(src));
          if (zone === 0 && q.secrets === "0") { doc.WIFI.STA_PASS = ""; doc.WIFI.AP_PASS = ""; }
          return doc;
        }
        if (method === "PUT") {
          var pbody;
          try { pbody = JSON.parse(opts.body); } catch (e) { var e2 = new ApiError(400, "BAD_JSON"); throw e2; }
          /* Logged (not just captured) so the mock check can grep console
           * output for the exact minimal merge body a Save produced. */
          if (typeof console !== "undefined" && console.log) {
            console.log("MOCK PUT /api/config?zone=" + zone, JSON.stringify(pbody));
          }
          self.lastPut = pbody;
          if (zone === 0) {
            HG.cfgMergeIntoDoc(0, self.fixtures.config[0], pbody);
            return { ok: true };
          }
          var badPath = self.mockValidateCfgPut(pbody);
          if (badPath) {
            var e3 = new ApiError(400, "INVALID_FIELD");
            e3.body = { error: "INVALID_FIELD", path: badPath };
            throw e3;
          }
          /* Apply for real (not just log) so a refetch 3s later reflects the
           * change -- needed for the "value stays at the new value across
           * the 3s window" check to actually mean something against the
           * mock. */
          var applyTarget = self.fixtures.config[zone] || (self.fixtures.config[zone] = JSON.parse(JSON.stringify(self.fixtures.config[1])));
          HG.cfgMergeIntoDoc(zone, applyTarget, pbody);
          return { queued: true, warnings: "" };
        }
      }
      if (route === "/api/fleet" && method === "POST") return { queued: true };
      if (route === "/api/fleet" && method === "DELETE") return { ok: true };
      if (route === "/api/cmd" && method === "POST") return self.cmdReply(String(opts.body || ""));
      throw new ApiError(404, "NOT_FOUND");
    });
  },
  cmdReply: function (line) {
    var up = line.toUpperCase();
    if (up.indexOf("GET ID") === 0) return "OK ID HillGrow-Master mac c0:5d:89:00:00:01";
    if (up.indexOf("SET NODE") === 0) return "OK " + line.slice(4);
    return "OK " + line;
  }
};

/* ---------- HG.api ---------- */
HG.api = {
  request: function (method, path, body, type) {
    var opts = { method: method };
    if (body !== undefined && body !== null) {
      if (type === "text") {
        opts.headers = { "Content-Type": "text/plain" };
        opts.body = body;
      } else {
        opts.headers = { "Content-Type": "application/json" };
        opts.body = JSON.stringify(body);
      }
    }
    return this._fetch(path, opts);
  },
  get: function (path) { return this._fetch(path, { method: "GET" }); },
  post: function (path, body, type) { return this.request("POST", path, body, type); },
  put: function (path, json) { return this.request("PUT", path, json); },
  del: function (path) { return this._fetch(path, { method: "DELETE" }); },
  _fetch: function (path, opts) {
    var run = HG.mock.active ? HG.mock.handle(path, opts) : HG.api._realFetch(path, opts);
    return run.then(function (result) {
      HG.state.auth = true;
      return result;
    }, function (err) {
      if (err instanceof ApiError && err.status === 401) {
        HG.state.auth = false;
        if (location.hash !== "#/login") {
          HG.state.lastRoute = location.hash || "#/dashboard";
          location.hash = "#/login";
        }
      }
      throw err;
    });
  },
  _realFetch: function (path, opts) {
    var o = Object.assign({ credentials: "same-origin" }, opts);
    /* Bound the wait explicitly: a silently wedged AP link (associated but not
     * actually forwarding) can otherwise leave fetch() pending far longer than
     * the OS's own TCP timeout, delaying the offline banner well past the poll
     * cadence. 8s covers a slow upload-adjacent request; the 2s poll itself
     * will retry regardless. */
    var ac = ("AbortController" in window) ? new AbortController() : null;
    var timer = ac ? setTimeout(function () { ac.abort(); }, 8000) : null;
    if (ac) o.signal = ac.signal;
    return fetch(path, o).catch(function () {
      if (timer) clearTimeout(timer);
      throw new ApiError(0, "NETWORK");
    }).then(function (res) {
      if (timer) clearTimeout(timer);
      return res.text().then(function (text) {
        var body = text;
        if (text) { try { body = JSON.parse(text); } catch (e) { body = text; } }
        if (!res.ok) {
          var msg = (body && typeof body === "object" && body.error) ? body.error :
                     (typeof body === "string" && body ? body : ("HTTP " + res.status));
          var err = new ApiError(res.status, msg);
          err.body = body;
          throw err;
        }
        return body;
      });
    });
  }
};

/* ---------- HG.poll: 2s state poll with backoff ---------- */
HG.poll = (function () {
  var backoff = 2000;
  var timer = null;
  var inFlight = false;
  function schedule(ms) {
    if (timer) clearTimeout(timer);
    timer = setTimeout(tick, ms);
  }
  function tick() {
    /* One outstanding request at a time: a stray immediate retick (the
     * visibilitychange handler's schedule(0), or a manual HG.poll.tick())
     * while a fetch is already in flight is a no-op -- the in-flight
     * request's own completion is what reschedules the next tick, so
     * nothing is lost by just returning here. */
    if (inFlight) return;
    if (document.hidden) { schedule(2000); return; }
    /* No point polling while sitting on the login form (nothing to show,
     * and it only hammers the master with 401s) or once we positively know
     * we're logged out. auth===null (cold boot, not yet determined) must
     * still poll -- that first request is how auth gets determined at all.
     * HG.actions.login resumes this explicitly once both conditions clear. */
    if (HG.state.route.name === "login" || HG.state.auth === false) { schedule(2000); return; }
    inFlight = true;
    HG.api.get("/api/state").then(function (snap) {
      HG.state.snap = snap;
      backoff = 2000;
      HG.state.online = true;
    }, function (err) {
      /* A 401 proves the transport succeeded (the master answered) -- only a
       * transport-level failure (network error, timeout) means "offline". */
      HG.state.online = err && err.status === 401 ? true : false;
      backoff = Math.min(backoff * 2, 30000);
    }).then(function () {
      inFlight = false;
      HG.rerender();
      schedule(backoff);
    });
  }
  document.addEventListener("visibilitychange", function () {
    if (!document.hidden) schedule(0);
  });
  return { start: function () { schedule(0); }, tick: tick };
})();

/* ---------- HG.alarmsPoll: 5s /api/alarms poll, gated to #/alarms only ----------
 * Mirrors HG.poll's own shape (document.hidden check, in-flight guard) rather
 * than an unconditional setInterval -- the brief's "don't create a second
 * unguarded loop" -- but gates on the route too, so it costs nothing (beyond
 * one idle 5s timer) while the operator is anywhere else. kick() forces an
 * immediate fetch on first navigating to #/alarms instead of waiting up to
 * 5s for the next tick. */
HG.alarmsPoll = (function () {
  var timer = null, inFlight = false;
  function schedule(ms) {
    if (timer) clearTimeout(timer);
    timer = setTimeout(tick, ms);
  }
  function tick() {
    if (inFlight) return;
    if (document.hidden || HG.state.route.name !== "alarms" || HG.state.auth !== true) {
      schedule(5000);
      return;
    }
    inFlight = true;
    HG.api.get("/api/alarms").then(function (a) {
      HG.state.alarms = a;
    }, function () { /* leave the last-known alarms list showing */ }).then(function () {
      inFlight = false;
      HG.rerender();
      schedule(5000);
    });
  }
  return { start: function () { schedule(0); }, kick: function () { schedule(0); } };
})();

/* ---------- helpers ---------- */
HG.rewriteForZone = function (line, zoneId) {
  var parts = line.split(/\s+/).filter(function (p) { return p.length; });
  if (!parts.length) return line;
  /* Already addressed (operator pre-typed "SET ZONE 2 ..."): don't double it
   * up into "SET ZONE 3 ZONE 2 ...". */
  if (parts.length > 1 && parts[1].toUpperCase() === "ZONE") return line;
  var verb = parts.shift();
  return verb + " ZONE " + zoneId + (parts.length ? " " + parts.join(" ") : "");
};

/* Class names built from server enums are whitelisted, not just
 * lower-cased: an unexpected value must never reach a class/attribute
 * position unescaped (a stray `"` in it could break out of the attribute),
 * and it should render as a neutral fallback rather than a broken class. */
var HEALTH_CLASS = { ONLINE: "online", DEGRADED: "degraded", OFFLINE: "offline", UPDATING: "updating", EMPTY: "empty" };
function healthBadgeClass(h) { return "badge badge-" + (HEALTH_CLASS[h] || "empty"); }
var TIME_SRC_CLASS = { NTP: "ntp", SET: "set", NONE: "none" };
function timeSrcBadgeClass(s) { return "badge badge-src-" + (TIME_SRC_CLASS[s] || "none"); }

function shelfNum(x) { return Number(x) | 0; }

function shelfTotals(shelves) {
  var soilSum = 0, soilN = 0, lightSum = 0, lightN = 0, pumpSum = 0, any = false;
  (shelves || []).forEach(function (s) {
    var a = shelfNum(s.pct_a), b = shelfNum(s.pct_b), w = shelfNum(s.white), r = shelfNum(s.red), p = shelfNum(s.pump_s);
    if (a || b || w || r || p) any = true;
    soilSum += a + b; soilN += 2;
    lightSum += w + r; lightN += 2;
    pumpSum += p;
  });
  return {
    soil: any ? Math.round(soilSum / soilN) + "%" : "—",
    light: any ? Math.round(lightSum / lightN) + "%" : "—",
    pump: any ? pumpSum + "s" : "—"
  };
}

/* ---------- config editor helpers ----------
 * A zone document's shape (hg_json_export_cfg): {gen, hw:{HW,shelf:[{HWSHELF,CAL}x4]},
 * cfg:{ZONECFG,shelf:[{SHELF,LIGHT,WATER,FAN,VIB}x4],aux:[{AUX}x2]}}. HW/HWSHELF/CAL
 * live under "hw" (read-only); everything else, including AUX, lives under "cfg". */
function isHwGroup(name) { return name === "HW" || name === "HWSHELF" || name === "CAL"; }

function cfgFieldOriginal(doc, group, idx, key) {
  if (!doc) return undefined;
  var obj;
  if (group === "ZONECFG") obj = doc.cfg && doc.cfg.ZONECFG;
  else if (group === "HW") obj = doc.hw && doc.hw.HW;
  else if (group === "HWSHELF") obj = doc.hw && doc.hw.shelf && doc.hw.shelf[idx] && doc.hw.shelf[idx].HWSHELF;
  else if (group === "CAL") obj = doc.hw && doc.hw.shelf && doc.hw.shelf[idx] && doc.hw.shelf[idx].CAL;
  else if (group === "AUX") obj = doc.cfg && doc.cfg.aux && doc.cfg.aux[idx] && doc.cfg.aux[idx].AUX;
  else obj = doc.cfg && doc.cfg.shelf && doc.cfg.shelf[idx] && doc.cfg.shelf[idx][group]; /* SHELF/LIGHT/WATER/FAN/VIB */
  return obj ? obj[key] : undefined;
}

function mgroupOriginal(doc, group, key) {
  var obj = doc && doc[group];
  return obj ? obj[key] : undefined;
}

/* Pure-numeric id -- CRITICAL: group/key are schema-controlled text (from
 * /api/schema), so the id (and its <label for=>) must never embed them raw
 * or even HG.esc()'d: an id/for pair is an unquoted-looking DOM identifier
 * a reviewer could easily overlook re-escaping consistently everywhere it's
 * threaded through, and a tampered schema response was confirmed live to
 * break out of the id="..." attribute and inject markup. groupIdx/fieldIdx
 * are this schema's own array indices -- always plain numbers regardless of
 * what the group/field *names* contain. The real names still reach the DOM,
 * safely, via the HG.esc()'d data-cfg-group/data-cfg-key attributes below. */
function cfgFieldId(zoneId, groupIdx, idx, fieldIdx) { return "cfgf-" + zoneId + "-" + groupIdx + "-" + idx + "-" + fieldIdx; }

/* Same tampered-schema threat as the id fix above, applied to min/max/maxlength:
 * f.min/f.max are supposed to be numbers (hg_json_schema.c always emits them via
 * cJSON_AddNumberToObject), but nothing on the client enforces that against a
 * compromised /api/schema response, and both attributes are otherwise
 * interpolated raw. Number() + a finite check turns anything non-numeric into
 * a safe fallback instead of arbitrary attribute text. */
function cfgNum(v, dflt) { var n = Number(v); return isFinite(n) ? n : dflt; }

function cfgGroupScope(groups, name) {
  for (var i = 0; i < groups.length; i++) if (groups[i].name === name) return groups[i].scope;
  return 0;
}

/* Friendly text for the error codes GET /api/config?zone=N can answer with
 * (see http_api_cfg.c's h_config_get): a bare "NO_CACHE"/"ZONE_UNKNOWN" is
 * meaningless to an operator who just clicked a zone link. */
var CFG_LOAD_ERR_TEXT = {
  NO_CACHE: "Zone config not adopted yet — the zone must come online and sync at least once before it can be configured from the web UI.",
  ZONE_UNKNOWN: "Unknown zone.",
  ZONE_NOT_ONLINE: "Zone is offline.",
  LOW_HEAP: "Master is low on memory — try again shortly.",
  BAD_QUERY: "Invalid zone number."
};
function cfgLoadErrText(err) {
  var code = err && err.message;
  return (code && CFG_LOAD_ERR_TEXT[code]) || code || "Failed to load";
}

/* Parses a server error `path` into {group, idx, key} so a 400 can highlight
 * the field it names. Two real shapes reach here (see hg_json.h): the merge's
 * own -2 path, e.g. "cfg.shelf[0].WATER.TARGET" (uppercase, matches the
 * schema's own group/field names), and hg_cfg_validate's -3 path, e.g.
 * "shelf[1].light.off" (lower-case, no "cfg." prefix) -- normalised the same
 * way so either highlights correctly. A zone-0 path is just "WIFI.AP_PASS". */
function cfgParseBadPath(path, errCode) {
  var m = /\[(\d+)\]/.exec(path);
  var idx = m ? Number(m[1]) : -1;
  var clean = String(path).replace(/^cfg\./i, "").replace(/^hw\./i, "");
  var segs = clean.split(".");
  if (segs.length && /^(shelf|aux)\[\d+\]$/i.test(segs[0])) segs.shift();
  var key = segs.pop();
  var group = segs.pop();
  return { group: group ? group.toUpperCase() : null, idx: idx, key: key ? key.toUpperCase() : null,
           msg: errCode || "Invalid value" };
}

/* ---------- views ---------- */
HG.views = {};

HG.views.login = function () {
  var err = HG.state.loginError ? '<p class="form-error">' + HG.esc(HG.state.loginError) + "</p>" : "";
  return '<div class="login-wrap"><form class="login-card" data-action="login">' +
    "<h1>HillGrow</h1>" +
    '<label for="login-password">Password</label>' +
    '<input id="login-password" name="password" type="password" autocomplete="current-password" data-draft value="' +
    HG.esc(HG.drafts["login-password"] || "") + '" required>' +
    err +
    '<button type="submit">Log in</button>' +
    "</form></div>";
};

HG.views.nav = function (current) {
  var items = [["dashboard", "Dashboard", "#/dashboard"], ["alarms", "Alarms", "#/alarms"], ["system", "System", "#/system"]];
  return '<nav class="nav">' + items.map(function (it) {
    var cls = current === it[0] ? ' class="active"' : "";
    return '<a href="' + it[2] + '"' + cls + ">" + it[1] + "</a>";
  }).join("") + "</nav>";
};

HG.views.shell = function (r) {
  var dot = '<span class="dot ' + (HG.state.online ? "online" : "offline") + '"></span>';
  var banner = HG.state.online ? "" : '<div class="banner banner-offline">Master offline — retrying…</div>';
  return '<header class="hdr">' + dot + '<span class="hdr-title">HillGrow</span>' +
    '<button type="button" class="btn-ghost" data-action="logout">Log out</button></header>' +
    HG.views.nav(r.name) +
    '<main id="main">' + banner + HG.views.route(r) + "</main>";
};

HG.views.route = function (r) {
  switch (r.name) {
    case "dashboard": return HG.views.dashboard();
    case "zone": return HG.views.zone(r.id);
    case "alarms": return HG.views.alarms();
    case "system": return HG.views.system();
    case "config": return HG.views.config(r.id);
    default: return HG.views.dashboard();
  }
};

HG.views.dashboard = function () {
  var snap = HG.state.snap;
  if (!snap) return '<p class="loading">Loading…</p>';
  var m = snap.master, ring = snap.ring;
  var ringCls = ring.state === "OK" ? "ok" : (ring.state === "OPEN" ? "open" : "idle");
  var ringBanner = '<div class="ring-banner ring-' + ringCls + '">Ring: ' + HG.esc(ring.state) +
    (ring.blame ? " — " + HG.esc(ring.blame) : "") + "</div>";
  var defBanner = "";
  if (m.defaults && (m.defaults.web || m.defaults.ap)) {
    var which = [];
    if (m.defaults.web) which.push("web");
    if (m.defaults.ap) which.push("Wi-Fi AP");
    defBanner = '<div class="banner banner-warn">Factory default password still in use (' +
      which.join(", ") + ') — change it in <a href="#/system">System</a>.</div>';
  }
  var sta = m.wifi.sta.up
    ? HG.esc(m.wifi.sta.ip) + " · " + HG.esc(m.wifi.sta.ssid) + " · " + m.wifi.sta.rssi + " dBm"
    : "STA down: " + HG.esc(m.wifi.sta.reason || "—");
  var quarantine = (m.http && m.http.cmd_quarantined > 0)
    ? '<p class="warn-line">⚠ ' + m.http.cmd_quarantined + " console slot(s) degraded</p>" : "";
  var master = '<section class="card master-card"><h2>Master <span>v' + HG.esc(m.version) + "</span></h2>" +
    "<dl><dt>Time</dt><dd>" + HG.esc(m.time) + ' <span class="' + timeSrcBadgeClass(m.time_src) + '">' + HG.esc(m.time_src) + "</span></dd>" +
    "<dt>STA</dt><dd>" + sta + "</dd>" +
    "<dt>AP</dt><dd>" + HG.esc(m.wifi.ap.ssid) + " · " + m.wifi.ap.clients + " client(s)</dd>" +
    "<dt>Heap min</dt><dd>" + m.heap_min_kb + " KB</dd></dl>" + quarantine + "</section>";
  var cards = snap.nodes.map(HG.views.nodeCard).join("");
  return ringBanner + defBanner + master + '<div class="node-grid">' + cards + "</div>";
};

HG.views.nodeCard = function (n) {
  var name = n.name ? HG.esc(n.name) : ("Z" + n.id);
  var hcls = healthBadgeClass(n.health);
  var stale = n.link_stale ? '<span class="pill">stale</span>' : "";
  var t = shelfTotals(n.shelves);
  return '<a class="card node-card" href="#/zone/' + n.id + '">' +
    '<div class="node-head"><span class="node-name">' + name + "</span>" +
    '<span class="' + hcls + '">' + HG.esc(n.health) + "</span></div>" +
    '<div class="node-sub">fw ' + HG.esc(n.fw) + " " + stale + " · last seen " + n.hb_age_s + "s ago</div>" +
    '<div class="tiles"><div class="tile"><b>' + t.soil + "</b><span>Soil</span></div>" +
    '<div class="tile"><b>' + t.light + "</b><span>Light</span></div>" +
    '<div class="tile"><b>' + t.pump + "</b><span>Pump</span></div></div></a>";
};

HG.views.zone = function (id) {
  var snap = HG.state.snap;
  if (!snap) return '<p class="loading">Loading…</p>';
  var n = null;
  for (var i = 0; i < snap.nodes.length; i++) { if (snap.nodes[i].id === id) { n = snap.nodes[i]; break; } }
  if (!n) return '<a class="back-link" href="#/dashboard">&larr; Dashboard</a><p class="empty">Zone ' + id + " not found.</p>";
  var name = n.name ? HG.esc(n.name) : ("Z" + n.id);
  var rows = [
    ["MAC", n.mac], ["Health", n.health], ["Firmware", n.fw], ["Config gen", n.gen],
    ["Hops", n.hops], ["Link", n.link], ["Link stale", n.link_stale ? "yes" : "no"],
    ["Config sync", n.cfg_sync], ["Last heard", n.hb_age_s + "s ago"], ["Uptime", n.uptime_s + "s"],
    ["Heap", n.heap_kb + " KB"], ["Resets", n.reset], ["Faults", n.faults], ["Mode", n.mode]
  ];
  var dl = '<dl class="zone-dl">' + rows.map(function (r) {
    return "<dt>" + HG.esc(r[0]) + "</dt><dd>" + HG.esc(String(r[1])) + "</dd>";
  }).join("") + "</dl>";
  var shelves = (n.shelves && n.shelves.length) ? HG.views.shelfTable(n.shelves) : '<p class="empty">No shelf telemetry.</p>';
  return '<a class="back-link" href="#/dashboard">&larr; Dashboard</a>' +
    "<h1>" + name + "</h1>" +
    '<a class="back-link" href="#/config/' + id + '">Configure this zone &rarr;</a>' +
    dl +
    "<h2>Shelves</h2>" + shelves +
    HG.views.replaceBoardForm(id) +
    HG.views.console(id);
};

HG.views.shelfTable = function (shelves) {
  var rows = shelves.map(function (s, i) {
    var a = shelfNum(s.pct_a), b = shelfNum(s.pct_b), w = shelfNum(s.white), r = shelfNum(s.red), p = shelfNum(s.pump_s);
    return "<tr><td>" + i + "</td><td>" + a + "</td><td>" + b + "</td><td>" + w +
      "</td><td>" + r + "</td><td>" + (s.out ? "on" : "off") + "</td><td>" + p + "s</td></tr>";
  }).join("");
  return '<table class="shelf-table"><thead><tr><th>#</th><th>Soil A</th><th>Soil B</th><th>White</th><th>Red</th><th>Out</th><th>Pump</th></tr></thead><tbody>' + rows + "</tbody></table>";
};

HG.views.replaceBoardForm = function (id) {
  /* .form-error MUST be a descendant of <form>: HG.actions.replaceBoard finds
   * it via form.querySelector(".form-error"), which only searches inside the
   * form it was called on. */
  return '<section class="card"><h2>Replace board</h2>' +
    '<form data-action="replace-board" class="inline-form">' +
    '<input id="mac-input" data-draft value="' + HG.esc(HG.drafts[HG.macDraftKey(id)] || "") +
    '" placeholder="aa:bb:cc:dd:ee:ff" required>' +
    '<button type="submit">Set</button>' +
    '<p class="form-error"></p>' +
    "</form></section>";
};

HG.views.console = function (id) {
  var cs = HG.consoleState(id);
  var draft = HG.drafts[HG.consoleDraftKey(id)] || "";
  var log = cs.log.map(function (e) {
    return "&gt; " + HG.esc(e.sent) + "\n" + HG.esc(e.reply) + "\n";
  }).join("\n");
  return '<section class="card console-card"><h2>Console</h2>' +
    '<pre class="console-log" id="console-log">' + log + "</pre>" +
    '<form data-action="console" class="console-form">' +
    '<input id="console-input" autocomplete="off" data-draft placeholder="GET ID" value="' + HG.esc(draft) + '">' +
    '<button type="submit">Send</button></form>' +
    '<label class="forward-label"><input type="checkbox" id="forward-zone"' + (cs.forward ? " checked" : "") +
    "> Forward to zone " + id + "</label></section>";
};

HG.views.alarms = function () {
  var a = HG.state.alarms;
  if (!a) return "<h1>Alarms</h1>" + '<p class="loading">Loading…</p>';
  var active = a.active.length
    ? '<ul class="alarm-list">' + a.active.map(function (x) {
        return '<li class="alarm-active"><b>' + HG.esc(x.key) + "</b>" + HG.esc(x.text) +
          '<span class="muted">' + x.since_s + "s ago</span></li>";
      }).join("") + "</ul>"
    : '<p class="empty">No active alarms.</p>';
  var events = a.events.length
    ? '<ul class="event-list">' + a.events.map(function (x) {
        return "<li>" + '<span class="muted">' + x.at_s + "s ago</span> " + HG.esc(x.text) + "</li>";
      }).join("") + "</ul>"
    : '<p class="empty">No events.</p>';
  return "<h1>Alarms</h1><h2>Active</h2>" + active + "<h2>History</h2>" + events;
};

HG.views.system = function () {
  var snap = HG.state.snap;
  var m = snap && snap.master;
  var sys = HG.state.sys;

  var staStatus = m
    ? (m.wifi.sta.up
        ? HG.esc(m.wifi.sta.ip) + " · " + HG.esc(m.wifi.sta.ssid) + " · " + m.wifi.sta.rssi + " dBm"
        : "STA down: " + HG.esc(m.wifi.sta.reason || "—"))
    : "—";
  var apStatus = m ? (HG.esc(m.wifi.ap.ssid) + " · " + m.wifi.ap.clients + " client(s) · " + HG.esc(m.wifi.ap.ip)) : "—";
  var scanList = "";
  if (sys.wifiScanning) scanList = '<p class="loading">Scanning…</p>';
  else if (sys.wifiScanErr) scanList = '<p class="form-error">' + HG.esc(sys.wifiScanErr) + "</p>";
  else if (sys.wifiScan) {
    /* auth is a raw wifi_auth_mode_t (wifi_mgr.h): 0 == WIFI_AUTH_OPEN, every
     * other value is some flavour of secured -- the UI only needs the binary
     * distinction, not the specific cipher. */
    scanList = sys.wifiScan.length
      ? '<ul class="scan-list">' + sys.wifiScan.map(function (n) {
          return '<li><button type="button" class="scan-item" data-action="wifi-pick" data-ssid="' +
            HG.esc(n.ssid) + '">' + HG.esc(n.ssid) + '<span class="muted">' +
            (n.auth === 0 ? "open" : "secured") + " · " + n.rssi + " dBm</span></button></li>";
        }).join("") + "</ul>"
      : '<p class="empty">No networks found.</p>';
  }
  var wifiCard = '<section class="card"><h2>Wi-Fi</h2>' +
    "<dl><dt>STA</dt><dd>" + staStatus + "</dd><dt>AP</dt><dd>" + apStatus + "</dd></dl>" +
    '<button type="button" data-action="wifi-scan"' + (sys.wifiScanning ? " disabled" : "") + ">Scan</button>" +
    scanList +
    '<form data-action="wifi-join" class="inline-form">' +
    '<input id="sys-sta-ssid" data-draft placeholder="SSID" value="' + HG.esc(HG.drafts["sys-sta-ssid"] || "") + '">' +
    '<input id="sys-sta-pass" type="password" data-draft placeholder="Password" value="' +
    HG.esc(HG.drafts["sys-sta-pass"] || "") + '">' +
    '<button type="submit">Join</button><p class="form-error"></p></form>' +
    '<form data-action="wifi-ap" class="inline-form">' +
    '<input id="sys-ap-ssid" data-draft placeholder="AP SSID" value="' + HG.esc(HG.drafts["sys-ap-ssid"] || "") + '">' +
    '<input id="sys-ap-pass" type="password" data-draft placeholder="AP password (8+ chars)" value="' +
    HG.esc(HG.drafts["sys-ap-pass"] || "") + '">' +
    '<button type="submit">Set AP</button><p class="form-error"></p></form>' +
    "</section>";

  var timeCard = '<section class="card"><h2>Time</h2>' +
    "<dl><dt>Now</dt><dd>" + (m ? HG.esc(m.time) : "—") + "</dd><dt>Source</dt><dd>" +
    (m ? HG.esc(m.time_src) : "—") + "</dd></dl>" +
    '<form data-action="tz-set" class="inline-form">' +
    '<input id="sys-tz-input" data-draft placeholder="CET-1CEST,M3.5.0,M10.5.0/3" value="' +
    HG.esc(HG.drafts["sys-tz-input"] || "") + '">' +
    '<button type="submit">Set TZ</button><p class="form-error"></p></form></section>';

  var pwCard = '<section class="card"><h2>Web password</h2>' +
    '<form data-action="pw-change" class="inline-form">' +
    '<input id="sys-pw-old" type="password" data-draft placeholder="Current password" value="' +
    HG.esc(HG.drafts["sys-pw-old"] || "") + '">' +
    '<input id="sys-pw-new" type="password" data-draft placeholder="New password" value="' +
    HG.esc(HG.drafts["sys-pw-new"] || "") + '">' +
    '<button type="submit">Change</button><p class="form-error"></p></form></section>';

  var fwMasterBar = sys.fwMasterUploading
    ? '<div class="progress"><div class="progress-bar" style="width:' + sys.fwMasterPct + '%"></div></div>' : "";
  var fwMasterMsg = sys.fwMasterMsg
    ? '<p class="' + (sys.fwMasterOk ? "cfg-msg" : "form-error") + '">' + HG.esc(sys.fwMasterMsg) + "</p>" : "";
  var rebootBtn = sys.fwMasterOk ? '<button type="button" data-action="reboot-now">Reboot now</button>' : "";
  var fwZoneBar = sys.fwZoneUploading
    ? '<div class="progress"><div class="progress-bar" style="width:' + sys.fwZonePct + '%"></div></div>' : "";
  var fwZoneMsg = sys.fwZoneMsg
    ? '<p class="' + (sys.fwZoneOk ? "cfg-msg" : "form-error") + '">' + HG.esc(sys.fwZoneMsg) + "</p>" : "";
  var fwCard = '<section class="card"><h2>Firmware</h2>' +
    '<label class="btn-ghost">Upload master image<input type="file" id="sys-fw-master-input" accept=".bin" hidden' +
    (sys.fwMasterUploading ? " disabled" : "") + "></label>" +
    fwMasterBar + fwMasterMsg + rebootBtn +
    '<label class="btn-ghost">Upload zone image<input type="file" id="sys-fw-zone-input" accept=".bin" hidden' +
    (sys.fwZoneUploading ? " disabled" : "") + "></label>" +
    fwZoneBar + fwZoneMsg +
    "</section>";

  var nodes = (snap && snap.nodes) || [];
  /* fleet_status() (node_mgr_fleet.c / fleet_seq.c): "IDLE" is the exact
   * token when !s->active -- anything else ("<zone> PRECHECK|UPDATING|
   * WAIT_HB") means a sequence is already running, so Update/Update-all
   * would only race POST /api/fleet into a 409 FLEET_BUSY; Abort is the
   * only action that makes sense while a sequence is active. */
  var fleetIdle = !m || m.fleet === "IDLE";
  var fleetRows = nodes.map(function (n) {
    var name = n.name ? HG.esc(n.name) : ("Z" + n.id);
    return '<div class="fleet-row"><span>' + name +
      '</span><button type="button" data-action="fleet-update" data-zone="' + n.id + '"' +
      (fleetIdle ? "" : " disabled") + ">Update</button></div>";
  }).join("");
  var fleetLine = m ? HG.esc(m.fleet) : "—";
  var fleetCard = '<section class="card"><h2>Fleet</h2>' +
    "<p>Status: " + fleetLine + "</p>" + fleetRows +
    '<div class="cfg-actions"><button type="button" data-action="fleet-update-all"' +
    (fleetIdle ? "" : " disabled") + ">Update all</button>" +
    '<button type="button" class="btn-ghost" data-action="fleet-abort"' +
    (fleetIdle ? " disabled" : "") + ">Abort</button></div>" +
    (sys.fleetMsg ? '<p class="cfg-msg">' + HG.esc(sys.fleetMsg) + "</p>" : "") +
    "</section>";

  var rebootCard = '<section class="card"><h2>Reboot</h2>' +
    '<button type="button" class="btn-ghost" data-action="reboot-master">Reboot master</button>' +
    (sys.rebootMsg ? '<p class="cfg-msg">' + HG.esc(sys.rebootMsg) + "</p>" : "") +
    "</section>";

  return "<h1>System</h1>" +
    '<p><a href="#/config/0">Master config (Wi-Fi/time/hostname raw fields) &rarr;</a></p>' +
    wifiCard + timeCard + pwCard + fwCard + fleetCard + rebootCard;
};

HG.views.cfgIdxSelector = function (count, cur) {
  var btns = "";
  for (var i = 0; i < count; i++) {
    btns += '<button type="button" class="cfg-idx-btn' + (i === cur ? " active" : "") +
      '" data-action="cfg-idx" data-idx="' + i + '">' + i + "</button>";
  }
  return '<div class="cfg-idx-selector">' + btns + "</div>";
};

/* The two documents have DIFFERENT shapes and therefore need different
 * accessors: a zone document is nested ({gen,hw:{...},cfg:{...}}) and is read
 * by cfgFieldOriginal; the master's mcfg (zone 0, hg_json_export_mcfg) is FLAT
 * ({WIFI:{...},TIME:{...},SYS:{...}}) and is read by mgroupOriginal. Falling
 * through to cfgFieldOriginal for zone 0 makes every group miss (there is no
 * doc.cfg at all on a master document) and renders every master field blank --
 * with the dashboard simultaneously showing the STA associated, so the page
 * reads as "master unconfigured" and any edit made there is made blind. The
 * SAVE path (HG.buildCfgMergeBody) already picks the accessor on id === 0;
 * this is the same choice on the RENDER path. */
HG.cfgFieldValue = function (zoneId, doc, group, idx, key) {
  var dirty = HG.state.cfgDirty[zoneId] || {};
  var dk = group + "|" + idx + "|" + key;
  if (Object.prototype.hasOwnProperty.call(dirty, dk)) return dirty[dk];
  if (zoneId === 0) return mgroupOriginal(doc, group, key);
  return cfgFieldOriginal(doc, group, idx, key);
};

HG.views.cfgField = function (zoneId, group, idx, f, value, disabled, groupIdx, fieldIdx) {
  var id = cfgFieldId(zoneId, groupIdx, idx, fieldIdx);
  var bad = HG.state.cfgBad && HG.state.cfgBad.group === group &&
    (HG.state.cfgBad.idx === idx || HG.state.cfgBad.idx === -1) && HG.state.cfgBad.key === f.key;
  var badCls = bad ? " bad" : "";
  var common = ' data-cfg-group="' + HG.esc(group) + '" data-cfg-idx="' + idx +
    '" data-cfg-key="' + HG.esc(f.key) + '" data-cfg-type="' + HG.esc(f.type) + '"';
  var dis = disabled ? " disabled" : "";
  var input;
  switch (f.type) {
    case "BOOL":
      input = '<input type="checkbox" id="' + id + '" class="' + badCls.trim() + '"' +
        (value ? " checked" : "") + dis + common + ">";
      break;
    case "HHMM":
      input = '<input type="time" id="' + id + '" class="' + badCls.trim() + '" value="' +
        HG.esc(value || "00:00") + '"' + dis + common + ">";
      break;
    case "ENUM":
      var opts = (f.enums || []).map(function (name) {
        return '<option value="' + HG.esc(name) + '"' + (name === value ? " selected" : "") + ">" + HG.esc(name) + "</option>";
      }).join("");
      input = '<select id="' + id + '" class="' + badCls.trim() + '"' + dis + common + ">" + opts + "</select>";
      break;
    case "STR16": case "STR":
      var typ = (group === "WIFI" && (f.key === "STA_PASS" || f.key === "AP_PASS")) ? "password" : "text";
      input = '<input type="' + typ + '" id="' + id + '" class="' + badCls.trim() + '" maxlength="' + cfgNum(f.max, 255) +
        '" value="' + HG.esc(value == null ? "" : value) + '"' + dis + common + ">";
      break;
    default: /* U8, U16, PIN */
      input = '<input type="number" id="' + id + '" class="' + badCls.trim() + '" min="' + cfgNum(f.min, 0) + '" max="' + cfgNum(f.max, 65535) +
        '" value="' + HG.esc(value == null ? 0 : value) + '"' + dis + common + ">";
  }
  var err = bad ? '<p class="form-error">' + HG.esc(HG.state.cfgBad.msg) + "</p>" : "";
  return '<div class="cfg-field"><label for="' + id + '">' + HG.esc(f.key) + "</label>" + input + err + "</div>";
};

HG.views.configMaster = function (schema) {
  var doc = HG.state.cfgDoc[0];
  if (!doc) return "<h1>Config — Master</h1>" + '<p class="loading">Loading…</p>';
  var ui = HG.state.cfgUi;
  var groups = schema.mgroups;
  var tabs = '<div class="cfg-tabs">' + groups.map(function (g) {
    return '<button type="button" class="cfg-tab' + (g.name === ui.group ? " active" : "") +
      '" data-action="cfg-tab" data-group="' + HG.esc(g.name) + '">' + HG.esc(g.name) + "</button>";
  }).join("") + "</div>";
  var activeIdx = 0, active = groups[0];
  for (var gi0 = 0; gi0 < groups.length; gi0++) {
    if (groups[gi0].name === ui.group) { active = groups[gi0]; activeIdx = gi0; break; }
  }
  var fields = active.fields.map(function (f, fi) {
    return HG.views.cfgField(0, active.name, -1, f, HG.cfgFieldValue(0, doc, active.name, -1, f.key), false, activeIdx, fi);
  }).join("");
  var exportHref = "data:application/json," + encodeURIComponent(JSON.stringify(doc));
  var msg = HG.state.cfgMsg ? '<p class="cfg-msg">' + HG.esc(HG.state.cfgMsg) + "</p>" : "";
  return "<h1>Config — Master</h1>" + tabs +
    '<form class="cfg-form" data-action="cfg-form" novalidate><div class="cfg-fields">' + fields + "</div>" +
    '<div class="cfg-actions"><button type="submit"' + (HG.state.cfgSaving ? " disabled" : "") + ">" +
    (HG.state.cfgSaving ? "Saving…" : "Save") + "</button>" +
    '<a class="btn-ghost" download="hillgrow-master.json" href="' + exportHref + '">Export</a>' +
    '<label class="btn-ghost cfg-import-label">Import<input type="file" id="cfg-import-input" ' +
    'accept="application/json" hidden></label></div>' + msg + "</form>";
};

HG.views.config = function (id) {
  var schema = HG.state.schema;
  if (!schema) return "<h1>Config</h1>" + '<p class="loading">Loading…</p>';
  if (id === 0) return HG.views.configMaster(schema);
  var doc = HG.state.cfgDoc[id];
  if (!doc) {
    /* Retry (fix round 2 (b)): clears the failed-load guard and re-fetches
     * without needing to leave and re-enter the page. */
    var err = HG.state.cfgLoadErr
      ? '<p class="form-error">' + HG.esc(HG.state.cfgLoadErr) + '</p><button type="button" data-action="cfg-retry" data-zone="' + id + '">Retry</button>'
      : '<p class="loading">Loading…</p>';
    return '<a class="back-link" href="#/dashboard">&larr; Dashboard</a><h1>Config — Zone ' + id + "</h1>" + err;
  }
  var ui = HG.state.cfgUi;
  var groups = schema.groups;
  var tabs = '<div class="cfg-tabs">' + groups.map(function (g) {
    return '<button type="button" class="cfg-tab' + (g.name === ui.group ? " active" : "") +
      '" data-action="cfg-tab" data-group="' + HG.esc(g.name) + '">' + HG.esc(g.name) + "</button>";
  }).join("") + "</div>";
  var activeIdx = 0, active = groups[0];
  for (var gi1 = 0; gi1 < groups.length; gi1++) {
    if (groups[gi1].name === ui.group) { active = groups[gi1]; activeIdx = gi1; break; }
  }
  var scope = active.scope;
  var selector = "";
  if (scope === 1) selector = HG.views.cfgIdxSelector(4, ui.idx);
  else if (scope === 2) selector = HG.views.cfgIdxSelector(2, ui.idx);
  var idx = scope === 0 ? -1 : ui.idx;
  var hwNote = isHwGroup(active.name)
    ? '<p class="cfg-hw-note">hardware plane — set at the zone console</p>' : "";
  var fields = active.fields.map(function (f, fi) {
    return HG.views.cfgField(id, active.name, idx, f, HG.cfgFieldValue(id, doc, active.name, idx, f.key), isHwGroup(active.name), activeIdx, fi);
  }).join("");
  var exportHref = "data:application/json," + encodeURIComponent(JSON.stringify(doc));
  var msg = HG.state.cfgMsg ? '<p class="cfg-msg">' + HG.esc(HG.state.cfgMsg) + "</p>" : "";
  return '<a class="back-link" href="#/dashboard">&larr; Dashboard</a>' +
    "<h1>Config — Zone " + id + " (gen " + doc.gen + ")</h1>" +
    tabs + selector +
    '<form class="cfg-form" data-action="cfg-form" novalidate><div class="cfg-fields">' + hwNote + fields + "</div>" +
    '<div class="cfg-actions"><button type="submit"' + (HG.state.cfgSaving ? " disabled" : "") + ">" +
    (HG.state.cfgSaving ? "Saving…" : "Save") + "</button>" +
    '<a class="btn-ghost" download="hillgrow-zone' + id + '.json" href="' + exportHref + '">Export</a>' +
    '<label class="btn-ghost cfg-import-label">Import<input type="file" id="cfg-import-input" ' +
    'accept="application/json" hidden></label></div>' + msg + "</form>";
};

/* ---------- router ---------- */
HG.router = {
  parse: function () {
    var h = location.hash.replace(/^#\/?/, "");
    if (!h) h = "dashboard";
    var parts = h.split("/");
    switch (parts[0]) {
      case "login": return { name: "login" };
      case "zone": return { name: "zone", id: Number(parts[1]) };
      case "config": return { name: "config", id: Number(parts[1] || 0) };
      case "alarms": return { name: "alarms" };
      case "system": return { name: "system" };
      case "dashboard": return { name: "dashboard" };
      default: return { name: "dashboard" };
    }
  }
};

/* ---------- render (with focus preservation across in-place polls) ---------- */
function withFocusPreserved(fn) {
  var el = document.activeElement;
  var id = null, start = null, end = null;
  var app = document.getElementById("app");
  if (el && el.id && app && app.contains(el)) {
    id = el.id;
    if ("selectionStart" in el) { try { start = el.selectionStart; end = el.selectionEnd; } catch (e) { /* not applicable to this input type */ } }
  }
  fn();
  if (id) {
    var ne = document.getElementById(id);
    if (ne) {
      ne.focus();
      if (start != null && "setSelectionRange" in ne) { try { ne.setSelectionRange(start, end); } catch (e) { /* ignore */ } }
    }
  }
}

/* Is zone `id` currently reported healthy enough for its config to be worth
 * re-fetching -- read straight from the already-polling /api/state snapshot,
 * no extra request needed. Used only by the self-heal retry below. */
function cfgZoneReady(id) {
  var snap = HG.state.snap;
  if (!snap || !snap.nodes) return false;
  for (var i = 0; i < snap.nodes.length; i++) {
    if (snap.nodes[i].id === id) return snap.nodes[i].health === "ONLINE" && snap.nodes[i].cfg_sync === "OK";
  }
  return false;
}

/* Fetches schema (once, cached) + the zone/master document for #/config/N,
 * called from HG.render on every route parse -- cheap once both are cached
 * (two object-existence checks), which is what lets the config route sit
 * under the same 2s state poll as everything else without refetching on
 * every tick. Re-entering a *different* zone resets that zone's UI/dirty
 * state so a leftover .bad highlight or mid-shelf tab from zone A can never
 * bleed into zone B.
 *
 * cfgLoadFailedId guards against a real bug found while adding the
 * friendly-error-text minor (fix round 1): a failed fetch used to leave
 * cfgDoc[id] unset, so the very next HG.render() (the 2s state poll, if
 * nothing else) saw needDoc still true and started ANOTHER fetch -- which
 * fails again, renders again, fetches again, forever, as fast as the
 * network round-trip allows, for as long as the operator sits on a zone
 * that's genuinely not adopted yet. Recording which id's load already
 * failed turns that into "try once per zone visit" -- cleared by the zone
 * actually changing (the reset block above), by HG.render() noticing the
 * route left #/config/N entirely (fix round 2 (a) -- so leaving for the
 * dashboard and coming back to the SAME zone retries once, not never), or
 * by the "Retry" button (fix round 2 (b), HG.actions.cfgRetry).
 *
 * Fix round 2 (c): while a load is in its failed state, also self-heal
 * automatically the moment the already-polling /api/state snapshot shows
 * this zone transition to ONLINE + cfg_sync OK -- an operator who leaves
 * the tab open while the zone comes back shouldn't have to leave and
 * re-enter, or hit Retry, just to see that. cfgSelfHealAttempted keeps
 * this to exactly one automatic retry per "became ready" transition (reset
 * only when the zone is next observed NOT ready), which is what keeps the
 * storm guard intact even if that one retry itself fails again (e.g. a
 * concurrent LOW_HEAP) -- it does not degrade back into a tight loop just
 * because the poll keeps reporting the same already-tried "ready" state. */
HG.ensureConfigLoaded = function (id) {
  var ui = HG.state.cfgUi;
  if (ui.zone !== id) {
    HG.state.cfgUi = { zone: id, group: null, idx: 0 };
    HG.state.cfgDirty[id] = HG.state.cfgDirty[id] || {};
    HG.state.cfgBad = null;
    HG.state.cfgMsg = "";
    HG.state.cfgLoadErr = "";
    HG.state.cfgLoadFailedId = null;
    HG.state.cfgSelfHealAttempted = null;
  }
  if (HG.state.cfgLoading === id) return;
  if (HG.state.cfgLoadFailedId === id) {
    var ready = id > 0 && cfgZoneReady(id);
    if (ready && HG.state.cfgSelfHealAttempted !== id) {
      HG.state.cfgSelfHealAttempted = id;
      HG.state.cfgLoadFailedId = null;
    } else {
      if (!ready) HG.state.cfgSelfHealAttempted = null; /* allow one more attempt next time it becomes ready */
      return;
    }
  }
  var needSchema = !HG.state.schema;
  var needDoc = !HG.state.cfgDoc[id];
  if (!needSchema && !needDoc) {
    if (!HG.state.cfgUi.group) {
      var groups = id === 0 ? HG.state.schema.mgroups : HG.state.schema.groups;
      HG.state.cfgUi.group = groups[0].name;
    }
    return;
  }
  HG.state.cfgLoadErr = "";
  HG.state.cfgLoading = id;
  var p = needSchema ? HG.api.get("/api/schema").then(function (s) { HG.state.schema = s; }) : Promise.resolve();
  p.then(function () {
    var q = id === 0 ? "/api/config?zone=0&secrets=0" : "/api/config?zone=" + id;
    return HG.api.get(q);
  }).then(function (doc) {
    HG.state.cfgDoc[id] = doc;
    HG.state.cfgLoading = null;
    var groups = id === 0 ? HG.state.schema.mgroups : HG.state.schema.groups;
    if (!HG.state.cfgUi.group) HG.state.cfgUi.group = groups[0].name;
    HG.render();
  }, function (err) {
    HG.state.cfgLoading = null;
    HG.state.cfgLoadFailedId = id;
    HG.state.cfgLoadErr = cfgLoadErrText(err);
    HG.render();
  });
};

/* Reuses the same zone-0 document the config editor caches (fetched
 * ?secrets=0) purely to pre-fill the Time card's TZ field on first visit --
 * no schema fetch needed here, System doesn't render a schema-driven form. */
HG.ensureSysLoaded = function () {
  /* cfgDoc[0] may already be cached from a previous #/config/0 visit -- the
   * TZ prefill must still happen in that case, not only on a fresh fetch. */
  if (HG.state.cfgDoc[0]) {
    if (HG.drafts["sys-tz-input"] == null) HG.drafts["sys-tz-input"] = HG.state.cfgDoc[0].TIME.TZ;
    return;
  }
  if (HG.state.cfgLoading === 0) return;
  HG.state.cfgLoading = 0;
  HG.api.get("/api/config?zone=0&secrets=0").then(function (doc) {
    HG.state.cfgDoc[0] = doc;
    HG.state.cfgLoading = null;
    if (HG.drafts["sys-tz-input"] == null) HG.drafts["sys-tz-input"] = doc.TIME.TZ;
    HG.render();
  }, function () { HG.state.cfgLoading = null; });
};

HG.render = function () {
  withFocusPreserved(function () {
    var r = HG.router.parse();
    var enteringAlarms = r.name === "alarms" && HG.state.route.name !== "alarms";
    /* Fix round 2 (a): leaving #/config/N entirely (not just switching to a
     * different zone -- that's already handled inside HG.ensureConfigLoaded
     * itself) clears the failed-load guard, so navigating away and back to
     * the SAME zone retries once instead of the error persisting forever
     * for the rest of the session. */
    if (HG.state.route.name === "config" && r.name !== "config") {
      HG.state.cfgLoadFailedId = null;
      HG.state.cfgSelfHealAttempted = null;
    }
    HG.state.route = r;
    if (r.name === "config") HG.ensureConfigLoaded(r.id);
    else if (r.name === "system") HG.ensureSysLoaded();
    else if (enteringAlarms) HG.alarmsPoll.kick();
    document.body.classList.toggle("is-login", r.name === "login");
    var html = r.name === "login" ? HG.views.login() : HG.views.shell(r);
    document.getElementById("app").innerHTML = html;
  });
};

/* True while the operator is actively editing a field inside #/config/N or
 * #/system -- the two routes whose form re-renders HG.rerender is allowed to
 * skip entirely on a poll tick (see HG.rerender below for why). Any other
 * route (dashboard, zone, alarms, login) is unaffected: their own editable
 * fields (console-input, mac-input, all plain <input type=text>) already
 * have their caret restored correctly by withFocusPreserved's own
 * selectionStart/setSelectionRange path, and their pages show live
 * telemetry that must keep updating every tick regardless of focus. */
function pollSkipsRebuild() {
  var name = HG.state.route.name;
  if (name !== "config" && name !== "system") return false;
  var el = document.activeElement;
  var app = document.getElementById("app");
  if (!el || !app || !app.contains(el)) return false;
  var tag = el.tagName;
  return tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA";
}

/* Poll-driven rerender path -- HG.poll.tick() and HG.alarmsPoll.tick() call
 * this, never HG.render() directly, specifically so this gate applies to
 * every poll-triggered rebuild without touching any of the many
 * action-driven call sites that call HG.render() directly (navigation, Save,
 * tab switch, login, etc. -- all of which must keep rebuilding unconditionally,
 * per the brief).
 *
 * THE BUG (root cause): withFocusPreserved (above) can restore focus AND
 * text selection across a full #app innerHTML rebuild, but <input
 * type=number> -- the U8/U16/PIN config fields (e.g. WATER TARGET) -- does
 * NOT support selectionStart/selectionEnd/setSelectionRange at all (Chrome
 * throws reading them; withFocusPreserved's own try/catch swallows that and
 * leaves start/end null). So every rebuild silently resets that field's
 * caret to position 0. A fast desktop typist never notices -- the next
 * keystroke re-focuses/re-selects faster than a human perceives the jump --
 * but on a real phone, typing is slow enough that the 2s poll's rebuild
 * lands mid-word: each new digit is inserted at position 0 instead of after
 * the previous one, so the value can never be correctly edited. The field's
 * *value* was never at risk (HG.cfgDirty / HG.drafts already survive a
 * rebuild) -- only the caret was.
 *
 * THE FIX: #/config/N and #/system show no live telemetry inside their own
 * forms (unlike the dashboard/zone pages), so there is nothing on either
 * page that a poll tick needs to update while a field is focused -- the
 * simplest robust fix is to not rebuild #app for that tick at all. The
 * header's online/offline dot is still updated directly (a one-element
 * class swap, no rebuild) since HG.state.online can flip on any tick
 * regardless of what's focused. */
HG.rerender = function () {
  if (pollSkipsRebuild()) {
    var dot = document.querySelector(".hdr .dot");
    if (dot) dot.className = "dot " + (HG.state.online ? "online" : "offline");
    return;
  }
  HG.render();
};

/* ---------- config editor: merge-body building + save/import pipeline ---------- */

/* Builds the MINIMAL PUT body: only fields present in `dirty` whose typed
 * value actually differs from the document's own current value. zone 0's
 * password fields need no special-casing to honour "blank = unchanged" --
 * cfgDoc[0] is always fetched with ?secrets=0, so an untouched password
 * field's original value is always "", and leaving the input blank makes
 * dirty's value equal that same "" and therefore fall out via the plain
 * equality check below. */
HG.buildCfgMergeBody = function (id, schema, doc, dirty) {
  if (id === 0) {
    var out = {};
    Object.keys(dirty).forEach(function (k) {
      var parts = k.split("|");
      var group = parts[0], key = parts[2];
      var val = dirty[k];
      if (val === mgroupOriginal(doc, group, key)) return;
      out[group] = out[group] || {};
      out[group][key] = val;
    });
    return out;
  }
  var zonecfg = null, shelfMap = {}, auxMap = {}, shelfMax = -1, auxMax = -1;
  Object.keys(dirty).forEach(function (k) {
    var parts = k.split("|");
    var group = parts[0], idx = Number(parts[1]), key = parts[2];
    var val = dirty[k];
    if (val === cfgFieldOriginal(doc, group, idx, key)) return;
    if (group === "ZONECFG") { zonecfg = zonecfg || {}; zonecfg[key] = val; return; }
    var scope = cfgGroupScope(schema.groups, group);
    if (scope === 1) {
      shelfMap[idx] = shelfMap[idx] || {};
      shelfMap[idx][group] = shelfMap[idx][group] || {};
      shelfMap[idx][group][key] = val;
      if (idx > shelfMax) shelfMax = idx;
    } else if (scope === 2) {
      auxMap[idx] = auxMap[idx] || {};
      auxMap[idx].AUX = auxMap[idx].AUX || {};
      auxMap[idx].AUX[key] = val;
      if (idx > auxMax) auxMax = idx;
    }
  });
  var cfg = {};
  if (zonecfg) cfg.ZONECFG = zonecfg;
  if (shelfMax >= 0) {
    var sarr = [];
    for (var i = 0; i <= shelfMax; i++) sarr.push(shelfMap[i] || null);
    cfg.shelf = sarr;
  }
  if (auxMax >= 0) {
    var aarr = [];
    for (var j = 0; j <= auxMax; j++) aarr.push(auxMap[j] || null);
    cfg.aux = aarr;
  }
  return { cfg: cfg };
};

/* Points the editor at the group/idx a 400's `path` named (switching tabs if
 * necessary) BEFORE the next render, so the offending field is actually on
 * screen to receive its .bad class -- a field on an inactive tab can't be
 * highlighted, it doesn't exist in the DOM yet. */
HG.cfgFocusPath = function (id, path, errCode) {
  var bad = cfgParseBadPath(path, errCode);
  HG.state.cfgBad = bad;
  if (!bad.group) return;
  var schema = HG.state.schema;
  var groups = id === 0 ? schema.mgroups : schema.groups;
  for (var i = 0; i < groups.length; i++) {
    if (groups[i].name === bad.group) {
      HG.state.cfgUi.group = bad.group;
      if (bad.idx >= 0) HG.state.cfgUi.idx = bad.idx;
      return;
    }
  }
};

/* Shared PUT pipeline for both Save and Import: applies `body`, clears dirty
 * state on success, and re-fetches the document (immediately for zone 0's
 * synchronous 200, after 3s for a zone's async 202 -- the brief's "queued"
 * contract, giving the zone time to actually apply and report back before
 * the re-fetch would just show the pre-change values again). */
/* Applies the exact body just PUT onto the cached document in place
 * (IMPORTANT 3, fix round 1): without this, a saved field keeps showing its
 * pre-edit value for the whole 3s window before the authoritative refetch
 * lands, and re-typing that same pre-edit value in the meantime reads as
 * "no changes" (HG.buildCfgMergeBody diffs against cfgDoc, not against
 * what's on screen). Mirrors the exact shape HG.buildCfgMergeBody produces
 * -- shelf/aux array entries may be `null` (padding for a skipped index)
 * and must be skipped, not written; the later refetch remains the
 * authoritative source of truth (this is a same-tick UI optimisation only,
 * not a substitute for it -- e.g. it can't know about server-side
 * side-effects or a rejected-after-the-fact write). */
HG.cfgMergeIntoDoc = function (id, doc, body) {
  if (!doc || !body) return;
  if (id === 0) {
    Object.keys(body).forEach(function (g) {
      doc[g] = doc[g] || {};
      var vals = body[g];
      Object.keys(vals).forEach(function (k) { doc[g][k] = vals[k]; });
    });
    return;
  }
  var cfg = body.cfg;
  if (!cfg || !doc.cfg) return;
  if (cfg.ZONECFG) {
    doc.cfg.ZONECFG = doc.cfg.ZONECFG || {};
    Object.keys(cfg.ZONECFG).forEach(function (k) { doc.cfg.ZONECFG[k] = cfg.ZONECFG[k]; });
  }
  ["shelf", "aux"].forEach(function (arrName) {
    var arr = cfg[arrName], dstArr = doc.cfg[arrName];
    if (!arr || !dstArr) return;
    arr.forEach(function (el, i) {
      if (!el || !dstArr[i]) return;
      Object.keys(el).forEach(function (g) {
        dstArr[i][g] = dstArr[i][g] || {};
        var vals = el[g];
        Object.keys(vals).forEach(function (k) { dstArr[i][g][k] = vals[k]; });
      });
    });
  });
};

HG.cfgApplyPut = function (id, body) {
  HG.state.cfgSaving = true;
  HG.state.cfgMsg = "";
  HG.state.cfgBad = null;
  HG.render();
  return HG.api.put("/api/config?zone=" + id, body).then(function (resp) {
    HG.state.cfgSaving = false;
    HG.state.cfgDirty[id] = {};
    HG.cfgMergeIntoDoc(id, HG.state.cfgDoc[id], body);
    var refetch = function () {
      var q = id === 0 ? "/api/config?zone=0&secrets=0" : "/api/config?zone=" + id;
      HG.api.get(q).then(function (doc2) { HG.state.cfgDoc[id] = doc2; HG.render(); }, noop);
    };
    if (id === 0) {
      HG.state.cfgMsg = "Saved.";
      refetch();
    } else {
      HG.state.cfgMsg = "Queued, pushing to zone" + (resp && resp.warnings ? " (" + resp.warnings + ")" : "");
      setTimeout(refetch, 3000);
    }
    HG.render();
  }, function (err) {
    HG.state.cfgSaving = false;
    if (err.status === 400 && err.body && err.body.path) {
      HG.cfgFocusPath(id, err.body.path, err.body.error);
    } else if (err.status === 409) {
      HG.state.cfgMsg = "Zone busy, retry";
    } else {
      HG.state.cfgMsg = (err && err.message) || "Save failed";
    }
    HG.render();
  });
};

/* ---------- actions ---------- */
HG.actions = {
  login: function (form) {
    HG.state.loginError = "";
    var pwd = form.querySelector("#login-password").value;
    HG.api.post("/api/login", { password: pwd }, "json").then(function () {
      HG.state.auth = true;
      HG.drafts["login-password"] = "";
      var dest = HG.state.lastRoute || "#/dashboard";
      HG.state.lastRoute = null;
      location.hash = dest;
      HG.render();
      HG.poll.tick();   /* route/auth just cleared the poll's login-page gate */
    }, function (err) {
      var msg = "Login failed";
      if (err.status === 401) msg = "Wrong password";
      else if (err.status === 429) msg = "Too many attempts — locked out, try again shortly";
      HG.state.loginError = msg;
      HG.render();
    });
  },
  logout: function () {
    HG.api.post("/api/logout", null, "text").then(noop, noop).then(function () {
      HG.state.auth = false;
      /* No typed password (or any other draft) should linger in the DOM
       * past a deliberate logout -- a failed *login attempt* keeping its
       * typed password is fine (the operator is about to retry), but this
       * is a different person potentially about to sit down at the console. */
      HG.drafts = {};
      location.hash = "#/login";
      HG.render();
    });
  },
  consoleSend: function () {
    var id = HG.state.route.id;
    var cs = HG.consoleState(id);
    var draftKey = HG.consoleDraftKey(id);
    var input = document.getElementById("console-input");
    var line = (input.value || "").trim();
    if (!line) return;
    var sent = cs.forward ? HG.rewriteForZone(line, id) : line;
    cs.history.push(line);
    cs.histIdx = cs.history.length;
    HG.drafts[draftKey] = "";
    var entry = { sent: sent, reply: "…" };
    cs.log.push(entry);
    if (cs.log.length > 20) cs.log.shift();
    HG.render();
    HG.api.post("/api/cmd", sent, "text").then(function (reply) {
      entry.reply = reply || "";
    }, function (err) {
      entry.reply = (err && err.message) || "ERR";
    }).then(function () { HG.render(); });
  },
  consoleKey: function (e) {
    var id = HG.state.route.id;
    var cs = HG.consoleState(id);
    var draftKey = HG.consoleDraftKey(id);
    if (e.key === "ArrowUp") {
      e.preventDefault();
      if (cs.histIdx > 0) cs.histIdx--;
      if (cs.history[cs.histIdx] != null) { HG.drafts[draftKey] = cs.history[cs.histIdx]; e.target.value = HG.drafts[draftKey]; }
    } else if (e.key === "ArrowDown") {
      e.preventDefault();
      if (cs.histIdx < cs.history.length) cs.histIdx++;
      HG.drafts[draftKey] = cs.history[cs.histIdx] || "";
      e.target.value = HG.drafts[draftKey];
    }
  },
  replaceBoard: function (form) {
    var id = HG.state.route.id;
    var macInput = form.querySelector("#mac-input");
    var mac = macInput.value.trim();
    var out = form.querySelector(".form-error");
    if (!/^([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}$/.test(mac)) {
      out.textContent = "Enter a MAC like aa:bb:cc:dd:ee:ff";
      return;
    }
    out.textContent = "…";
    HG.api.post("/api/cmd", "SET NODE " + id + " MAC " + mac, "text").then(function (reply) {
      out.textContent = reply;
      HG.drafts[HG.macDraftKey(id)] = "";
      macInput.value = "";
    }, function (err) {
      out.textContent = (err && err.message) || "Failed";
    });
  },

  /* ---- config editor ---- */
  cfgTab: function (group) {
    var schema = HG.state.schema, id = HG.state.route.id;
    var groups = id === 0 ? schema.mgroups : schema.groups;
    HG.state.cfgUi.group = group;
    for (var i = 0; i < groups.length; i++) {
      if (groups[i].name === group && typeof groups[i].scope === "number") {
        var max = groups[i].scope === 2 ? 1 : groups[i].scope === 1 ? 3 : 0;
        if (HG.state.cfgUi.idx > max) HG.state.cfgUi.idx = 0;
      }
    }
    HG.state.cfgBad = null;
    HG.render();
  },
  cfgIdx: function (idx) {
    HG.state.cfgUi.idx = idx;
    HG.render();
  },
  /* Fix round 2 (b): the load-error view's own "Retry" button. Clears the
   * failed-load guard first -- HG.ensureConfigLoaded's own guard would
   * otherwise just see cfgLoadFailedId still equal to this zone and no-op. */
  cfgRetry: function (zone) {
    HG.state.cfgLoadFailedId = null;
    HG.state.cfgSelfHealAttempted = null;
    HG.ensureConfigLoaded(zone);
    HG.render();
  },
  /* Writes straight into HG.state.cfgDirty rather than re-rendering: a
   * config field's displayed value is read back from cfgDirty on the next
   * render (see HG.cfgFieldValue), which is what lets it survive the 2s
   * state poll's rerender without a per-field HG.drafts entry -- but it
   * means this handler must NOT call HG.render() itself, or every keystroke
   * would force a full #app rebuild. */
  cfgFieldInput: function (el) {
    var group = el.dataset.cfgGroup, idx = Number(el.dataset.cfgIdx), key = el.dataset.cfgKey, type = el.dataset.cfgType;
    var val;
    if (type === "BOOL") val = el.checked;
    else if (type === "U8" || type === "U16" || type === "PIN") val = Number(el.value);
    else val = el.value;
    var zone = HG.state.route.id;
    HG.state.cfgDirty[zone] = HG.state.cfgDirty[zone] || {};
    HG.state.cfgDirty[zone][group + "|" + idx + "|" + key] = val;
  },
  cfgSave: function () {
    var id = HG.state.route.id;
    var doc = HG.state.cfgDoc[id];
    var schema = HG.state.schema;
    if (!doc || !schema) return;
    var dirty = HG.state.cfgDirty[id] || {};
    var body = HG.buildCfgMergeBody(id, schema, doc, dirty);
    var empty = id === 0 ? Object.keys(body).length === 0 : Object.keys(body.cfg).length === 0;
    if (empty) { HG.state.cfgMsg = "No changes to save"; HG.render(); return; }
    HG.cfgApplyPut(id, body);
  },
  /* Import re-uses the exact shape GET returned (a zone's {hw,cfg,gen} or the
   * master's {WIFI,TIME,SYS}) as the PUT body directly -- for a zone, "hw" is
   * accepted and turned into read-only warnings rather than rejected, so the
   * round trip of an exported zone document needs no filtering at all. Only
   * zone 0 needs one: an export taken with ?secrets=0 carries "" for both
   * passwords, and PUTting "" for AP_PASS would fail hg_mcfg_validate's
   * 8-63-char rule (an *unconfigured* STA_PASS is fine empty; AP_PASS never
   * is) -- so an empty password key is dropped, matching "blank = unchanged". */
  cfgImportFile: function (file) {
    var id = HG.state.route.id;
    var reader = new FileReader();
    reader.onload = function () {
      var parsed;
      try { parsed = JSON.parse(String(reader.result)); } catch (e) {
        HG.state.cfgMsg = "Invalid JSON file";
        HG.render();
        return;
      }
      var body = parsed;
      if (id === 0 && parsed && parsed.WIFI) {
        body = Object.assign({}, parsed);
        body.WIFI = Object.assign({}, parsed.WIFI);
        if (body.WIFI.STA_PASS === "") delete body.WIFI.STA_PASS;
        if (body.WIFI.AP_PASS === "") delete body.WIFI.AP_PASS;
      }
      HG.cfgApplyPut(id, body);
    };
    reader.readAsText(file);
  },

  /* ---- system: wifi ---- */
  wifiScan: function () {
    HG.state.sys.wifiScanning = true;
    HG.state.sys.wifiScanErr = "";
    HG.render();
    HG.api.get("/api/wifi/scan").then(function (list) {
      HG.state.sys.wifiScanning = false;
      HG.state.sys.wifiScan = list;
      HG.render();
    }, function (err) {
      HG.state.sys.wifiScanning = false;
      HG.state.sys.wifiScanErr = (err && err.message) || "Scan failed";
      HG.render();
    });
  },
  wifiPick: function (ssid) {
    HG.drafts["sys-sta-ssid"] = ssid;
    HG.render();
  },
  wifiJoin: function (form) {
    var ssid = form.querySelector("#sys-sta-ssid").value.trim();
    var pass = form.querySelector("#sys-sta-pass").value;
    var out = form.querySelector(".form-error");
    if (!ssid) { out.textContent = "Enter an SSID"; return; }
    out.textContent = "…";
    HG.api.post("/api/wifi", { sta: { ssid: ssid, pass: pass } }, "json").then(function () {
      out.textContent = "Saved — joining…";
    }, function (err) {
      out.textContent = (err && err.message) || "Failed";
    });
  },
  apSet: function (form) {
    var ssid = form.querySelector("#sys-ap-ssid").value.trim();
    var pass = form.querySelector("#sys-ap-pass").value;
    var out = form.querySelector(".form-error");
    if (!ssid || pass.length < 8) { out.textContent = "AP SSID required, password 8+ chars"; return; }
    out.textContent = "…";
    HG.api.post("/api/wifi", { ap: { ssid: ssid, pass: pass } }, "json").then(function () {
      out.textContent = "Saved.";
    }, function (err) {
      out.textContent = (err && err.message) || "Failed";
    });
  },

  /* ---- system: time ---- */
  tzSet: function (form) {
    var tz = form.querySelector("#sys-tz-input").value.trim();
    var out = form.querySelector(".form-error");
    if (!tz || /\s/.test(tz)) { out.textContent = "Enter a POSIX TZ with no spaces"; return; }
    out.textContent = "…";
    HG.api.post("/api/cmd", "SET TZ " + tz, "text").then(function (reply) {
      out.textContent = reply || "OK";
      if (HG.state.cfgDoc[0]) HG.state.cfgDoc[0].TIME.TZ = tz;
    }, function (err) {
      out.textContent = (err && err.message) || "Failed";
    });
  },

  /* ---- system: password ---- */
  pwChange: function (form) {
    var oldPw = form.querySelector("#sys-pw-old").value;
    var newPw = form.querySelector("#sys-pw-new").value;
    var out = form.querySelector(".form-error");
    out.textContent = "…";
    HG.api.post("/api/password", { old: oldPw, new: newPw }, "json").then(function () {
      out.textContent = "Password changed.";
      HG.drafts["sys-pw-old"] = "";
      HG.drafts["sys-pw-new"] = "";
      form.querySelector("#sys-pw-old").value = "";
      form.querySelector("#sys-pw-new").value = "";
    }, function (err) {
      var msg = "Failed";
      if (err.status === 403) msg = "Old password is wrong";
      else if (err.status === 400) msg = "New password invalid";
      out.textContent = msg;
    });
  },

  /* ---- system: firmware upload (XHR for upload.onprogress; HG.api wraps
   * fetch(), which has no upload-progress event) ---- */
  fwUpload: function (kind, file) {
    var st = HG.state.sys;
    var prefix = kind === "master" ? "fwMaster" : "fwZone";
    st[prefix + "Uploading"] = true;
    st[prefix + "Pct"] = 0;
    st[prefix + "Msg"] = "";
    st[prefix + "Ok"] = false;
    HG.render();
    var xhr = new XMLHttpRequest();
    xhr.open("POST", "/api/fw/" + kind, true);
    xhr.setRequestHeader("Content-Type", "application/octet-stream");
    xhr.upload.onprogress = function (e) {
      if (e.lengthComputable) {
        st[prefix + "Pct"] = Math.round(e.loaded * 100 / e.total);
        HG.render();
      }
    };
    xhr.onload = function () {
      st[prefix + "Uploading"] = false;
      var ok = xhr.status >= 200 && xhr.status < 300;
      var body = null;
      /* 4xx/5xx from the upload routes may arrive with NO body (the server
       * drains-then-closes on a refused/failed upload) -- JSON.parse("")
       * throws, which is exactly why this is wrapped rather than assumed. */
      try { body = JSON.parse(xhr.responseText); } catch (e) { /* no body */ }
      if (ok) {
        st[prefix + "Ok"] = true;
        st[prefix + "Msg"] = kind === "master"
          ? "Uploaded" + (body && body.version ? " v" + body.version : "") + " to " + ((body && body.slot) || "?") + "."
          : "Uploaded (" + ((body && body.len) || "?") + " bytes).";
      } else {
        st[prefix + "Msg"] = (body && body.error) || ("HTTP " + xhr.status);
      }
      HG.render();
    };
    xhr.onerror = function () {
      st[prefix + "Uploading"] = false;
      st[prefix + "Msg"] = "Upload failed (network error)";
      HG.render();
    };
    xhr.send(file);
  },

  /* ---- system: reboot ---- */
  rebootMaster: function (msgField) {
    /* Optimistic: REBOOT CONFIRM makes the master reboot immediately, which
     * routinely cuts the TCP connection before this fetch's response is
     * read back (bench-confirmed: the reboot lands and GET VERSION shows
     * the new slot PENDING even when the request's own .then() never fires)
     * -- so "Rebooting..." has to be shown up front rather than waiting on
     * a response that may never arrive. */
    HG.state.sys[msgField] = "Rebooting…";
    HG.render();
    HG.api.post("/api/cmd", "REBOOT CONFIRM", "text").then(noop, function (err) {
      /* A genuine rejection (e.g. a 401 before the reboot could even be
       * dispatched) still overrides the optimistic message. */
      HG.state.sys[msgField] = (err && err.message) || "Reboot failed";
      HG.render();
    });
  },

  /* ---- system: fleet ---- */
  fleetUpdate: function (zone) {
    HG.api.post("/api/fleet", { zone: zone }, "json").then(function () {
      HG.state.sys.fleetMsg = "Update queued for zone " + zone + ".";
      HG.render();
    }, function (err) {
      HG.state.sys.fleetMsg = (err && err.message) || "Failed";
      HG.render();
    });
  },
  fleetUpdateAll: function () {
    HG.api.post("/api/fleet", { all: true }, "json").then(function () {
      HG.state.sys.fleetMsg = "Fleet update queued.";
      HG.render();
    }, function (err) {
      HG.state.sys.fleetMsg = (err && err.message) || "Failed";
      HG.render();
    });
  },
  fleetAbort: function () {
    HG.api.del("/api/fleet").then(function () {
      HG.state.sys.fleetMsg = "Fleet update aborted.";
      HG.render();
    }, function (err) {
      HG.state.sys.fleetMsg = (err && err.message) || "Failed";
      HG.render();
    });
  }
};
function noop() {}

/* ---------- event delegation (bound once) ---------- */
HG.bindEvents = function () {
  document.addEventListener("submit", function (e) {
    var f = e.target.closest && e.target.closest("form[data-action]");
    if (!f) return;
    e.preventDefault();
    var action = f.dataset.action;
    if (action === "login") HG.actions.login(f);
    else if (action === "console") HG.actions.consoleSend(f);
    else if (action === "replace-board") HG.actions.replaceBoard(f);
    else if (action === "cfg-form") HG.actions.cfgSave();
    else if (action === "wifi-join") HG.actions.wifiJoin(f);
    else if (action === "wifi-ap") HG.actions.apSet(f);
    else if (action === "tz-set") HG.actions.tzSet(f);
    else if (action === "pw-change") HG.actions.pwChange(f);
  });
  document.addEventListener("click", function (e) {
    var t = e.target;
    var b = t.closest && t.closest('[data-action="logout"]');
    if (b) { e.preventDefault(); HG.actions.logout(); return; }
    var tab = t.closest && t.closest('[data-action="cfg-tab"]');
    if (tab) { HG.actions.cfgTab(tab.dataset.group); return; }
    var idxBtn = t.closest && t.closest('[data-action="cfg-idx"]');
    if (idxBtn) { HG.actions.cfgIdx(Number(idxBtn.dataset.idx)); return; }
    var retryBtn = t.closest && t.closest('[data-action="cfg-retry"]');
    if (retryBtn) { HG.actions.cfgRetry(Number(retryBtn.dataset.zone)); return; }
    var scan = t.closest && t.closest('[data-action="wifi-scan"]');
    if (scan) { HG.actions.wifiScan(); return; }
    var pick = t.closest && t.closest('[data-action="wifi-pick"]');
    if (pick) { HG.actions.wifiPick(pick.dataset.ssid); return; }
    var rebootNow = t.closest && t.closest('[data-action="reboot-now"]');
    if (rebootNow) { HG.actions.rebootMaster("fwMasterMsg"); return; }
    var rebootMaster = t.closest && t.closest('[data-action="reboot-master"]');
    if (rebootMaster) { HG.actions.rebootMaster("rebootMsg"); return; }
    var fUpd = t.closest && t.closest('[data-action="fleet-update"]');
    if (fUpd) { HG.actions.fleetUpdate(Number(fUpd.dataset.zone)); return; }
    var fAll = t.closest && t.closest('[data-action="fleet-update-all"]');
    if (fAll) { HG.actions.fleetUpdateAll(); return; }
    var fAbort = t.closest && t.closest('[data-action="fleet-abort"]');
    if (fAbort) { HG.actions.fleetAbort(); return; }
  });
  document.addEventListener("keydown", function (e) {
    if (e.target && e.target.id === "console-input") HG.actions.consoleKey(e);
  });
  document.addEventListener("input", function (e) {
    var t = e.target;
    if (!t || !t.hasAttribute) return;
    if (t.hasAttribute("data-cfg-group")) { HG.actions.cfgFieldInput(t); return; }
    if (t.hasAttribute("data-draft")) HG.drafts[HG.draftKey(t)] = t.value;
  });
  document.addEventListener("change", function (e) {
    var t = e.target;
    if (!t) return;
    if (t.hasAttribute && t.hasAttribute("data-cfg-group")) { HG.actions.cfgFieldInput(t); return; }
    if (t.id === "forward-zone") {
      HG.consoleState(HG.state.route.id).forward = t.checked;
      return;
    }
    if (t.id === "cfg-import-input") {
      if (t.files && t.files[0]) HG.actions.cfgImportFile(t.files[0]);
      return;
    }
    if (t.id === "sys-fw-master-input") {
      if (t.files && t.files[0]) HG.actions.fwUpload("master", t.files[0]);
      return;
    }
    if (t.id === "sys-fw-zone-input") {
      if (t.files && t.files[0]) HG.actions.fwUpload("zone", t.files[0]);
      return;
    }
  });
  window.addEventListener("hashchange", HG.render);
};

/* ---------- bootstrap ---------- */
document.addEventListener("DOMContentLoaded", function () {
  HG.bindEvents();
  HG.render();
  HG.poll.start();
  HG.alarmsPoll.start();
});

})();
