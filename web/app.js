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
  console: {}            /* zoneId -> { log, history, histIdx, forward } -- drafts live in HG.drafts */
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
    schema: { groups: ["ZONECFG", "SHELF", "LIGHT", "WATER", "FAN", "VIB", "AUX", "HW", "HWSHELF", "CAL"], mgroups: ["WIFI", "TIME", "SYS"], hw_readonly: true },
    alarms: { active: [{ key: "Z2_OFFLINE", text: "Zone 2 offline", since_s: 900 }], events: [{ at_s: 900, text: "Zone 2 offline" }] },
    config: { 0: { WIFI: { STA_SSID: "HomeWiFi", STA_PASS: "", AP_SSID: "HillGrow", AP_PASS: "hillgrow1" }, TIME: { TZ: "CET-1CEST,M3.5.0,M10.5.0/3", NTP: "pool.ntp.org" }, SYS: { HOSTNAME: "hillgrow" } } }
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
      if (route === "/api/config") return JSON.parse(JSON.stringify(self.fixtures.config[0]));
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
    "<h1>" + name + "</h1>" + dl +
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
  var snap = HG.state.snap;
  var summary = snap ? ("Active: " + snap.master.alarms.active + " · Total: " + snap.master.alarms.total) : "";
  return "<h1>Alarms</h1><p>" + summary + '</p><p class="empty">Full alarm log lands in part 2.</p>';
};
HG.views.system = function () {
  return '<h1>System</h1><p class="empty">Wi-Fi, time, password and firmware tools land in part 2.</p>';
};
HG.views.config = function (id) {
  return "<h1>Config — Zone " + id + '</h1><p class="empty">Schema-driven editor lands in part 2.</p>';
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

HG.render = function () {
  withFocusPreserved(function () {
    var r = HG.router.parse();
    HG.state.route = r;
    document.body.classList.toggle("is-login", r.name === "login");
    var html = r.name === "login" ? HG.views.login() : HG.views.shell(r);
    document.getElementById("app").innerHTML = html;
  });
};
HG.rerender = HG.render;

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
  });
  document.addEventListener("click", function (e) {
    var b = e.target.closest && e.target.closest('[data-action="logout"]');
    if (b) { e.preventDefault(); HG.actions.logout(); }
  });
  document.addEventListener("keydown", function (e) {
    if (e.target && e.target.id === "console-input") HG.actions.consoleKey(e);
  });
  document.addEventListener("input", function (e) {
    if (!e.target || !e.target.hasAttribute || !e.target.hasAttribute("data-draft")) return;
    HG.drafts[HG.draftKey(e.target)] = e.target.value;
  });
  document.addEventListener("change", function (e) {
    if (e.target && e.target.id === "forward-zone") {
      var id = HG.state.route.id;
      HG.consoleState(id).forward = e.target.checked;
    }
  });
  window.addEventListener("hashchange", HG.render);
};

/* ---------- bootstrap ---------- */
document.addEventListener("DOMContentLoaded", function () {
  HG.bindEvents();
  HG.render();
  HG.poll.start();
});

})();
