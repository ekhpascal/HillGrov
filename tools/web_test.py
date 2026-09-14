#!/usr/bin/env python3
"""HillGrow SP4 web API bench suite (stdlib only: urllib, json, http.cookiejar,
threading, argparse). Exercises the master's HTTP API end to end: login/
lockout, /api/schema, /api/state, a config round trip on a zone, firmware
uploads (master OTA + zone image + fleet update), a Wi-Fi scan, and an
optional long soak.

Usage:
    python tools/web_test.py 192.168.7.7 --password hillgrow1 \\
        --master-bin master/build/hillgrow_master.bin \\
        --zone-bin zone/build/hillgrow_zone.bin --fleet 2
    python tools/web_test.py 192.168.7.7 --password hillgrow1 --soak 1800
    python tools/web_test.py --selftest   # offline, no network needed

--soak N runs ONLY the two-thread /api/state polling soak for N seconds
(nothing else -- the standard suites deliberately end with a login lockout
that would otherwise sit in front of every 30-minute run). Without --soak,
every other suite runs; uploads only run when --master-bin/--zone-bin are
given, and the fleet-update check only when --fleet ZONE is given.

Every request retries a bounded number of times on a transport-level
failure (connection refused/reset/timeout) -- never on a real HTTP status,
which is a genuine answer, not a dropped packet. The bench AP link is
lossy; every SP4 task report on this rig documents repeated full Wi-Fi
drops, so retries are load-bearing here, not decoration.
"""
import argparse
import http.client
import io
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from http.cookiejar import CookieJar

# ---- bookkeeping -----------------------------------------------------------

def check(results, name, ok, detail=""):
    results.append({"name": name, "ok": bool(ok), "detail": str(detail)})

def skip(results, name, detail):
    results.append({"name": name, "ok": True, "detail": str(detail), "skip": True})

# ---- HTTP client ------------------------------------------------------------

class ApiClient:
    """A minimal cookie-carrying HTTP client for the SP4 web API. request()
    retries a bounded number of times on a transport-level failure and
    returns the real (status, body) for any actual HTTP response -- 4xx/5xx
    included, retried or not: those are answers, not something to hide from
    a caller that needs to assert on them."""

    def __init__(self, ip, timeout=10.0):
        if ":" in ip:
            host, _, port_s = ip.partition(":")
            port = int(port_s)
        else:
            host, port = ip, 80
        self.host, self.port = host, port
        self.base = f"http://{host}" if port == 80 else f"http://{host}:{port}"
        self.timeout = timeout
        self.password = None
        self.cj = CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.cj))

    def request(self, method, path, data=None, headers=None, timeout=None, retries=3, retry_delay=1.5):
        to = timeout if timeout is not None else self.timeout
        last_exc = None
        for attempt in range(max(1, retries)):
            req = urllib.request.Request(self.base + path, data=data, headers=headers or {}, method=method)
            try:
                with self.opener.open(req, timeout=to) as resp:
                    return resp.status, resp.read()
            except urllib.error.HTTPError as e:
                return e.code, (e.read() or b"")   # a real HTTP answer -- never retried
            except (urllib.error.URLError, OSError, TimeoutError) as e:
                last_exc = e
                if attempt + 1 < retries:
                    time.sleep(retry_delay)
        raise RuntimeError(f"{method} {path}: {last_exc}")

    def login(self, password, retries=3, retry_delay=1.5):
        status, _ = self.request("POST", "/api/login", data=json.dumps({"password": password}).encode(),
                                  headers={"Content-Type": "application/json"},
                                  retries=retries, retry_delay=retry_delay)
        if status == 204:
            self.password = password
            return True
        return False

    def _cookie_header(self):
        return "; ".join(f"{c.name}={c.value}" for c in self.cj)

    def post_chunked(self, path, chunk, content_type="application/octet-stream", timeout=10.0):
        """A hand-framed Transfer-Encoding: chunked request. urllib always
        computes Content-Length from a bytes body and has no way to send a
        chunked one -- this is exactly the request shape the server's own
        rule ("chunked -> 400", every body route) exists to reject."""
        conn = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
        try:
            conn.putrequest("POST", path, skip_accept_encoding=True)
            conn.putheader("Content-Type", content_type)
            conn.putheader("Transfer-Encoding", "chunked")
            cookie = self._cookie_header()
            if cookie:
                conn.putheader("Cookie", cookie)
            conn.endheaders()
            conn.send(chunked_body(chunk))
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

def chunked_body(chunk):
    return ("%x\r\n" % len(chunk)).encode() + chunk + b"\r\n0\r\n\r\n"

def get_json(api, path, **kw):
    status, body = api.request("GET", path, **kw)
    try:
        doc = json.loads(body) if body else None
    except ValueError:
        doc = None
    return status, doc, body

def post_json(api, path, obj, **kw):
    headers = dict(kw.pop("headers", None) or {})
    headers.setdefault("Content-Type", "application/json")
    status, body = api.request("POST", path, data=json.dumps(obj).encode(), headers=headers, **kw)
    try:
        doc = json.loads(body) if body else None
    except ValueError:
        doc = None
    return status, doc, body

def put_json(api, path, obj, **kw):
    headers = dict(kw.pop("headers", None) or {})
    headers.setdefault("Content-Type", "application/json")
    status, body = api.request("PUT", path, data=json.dumps(obj).encode(), headers=headers, **kw)
    try:
        doc = json.loads(body) if body else None
    except ValueError:
        doc = None
    return status, doc, body

def cmd(api, line, **kw):
    kw.setdefault("timeout", 10.0)
    kw.setdefault("retries", 3)
    kw.setdefault("retry_delay", 1.5)
    status, body = api.request("POST", "/api/cmd", data=line.encode("utf-8", "replace"),
                                headers={"Content-Type": "text/plain"}, **kw)
    return status, body.decode("utf-8", "replace")

def poll_until(predicate, timeout, poll=2.0):
    """Call predicate() every `poll` seconds (a predicate exception counts as
    "not yet") until it returns truthy or `timeout` elapses."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            v = predicate()
        except Exception:
            v = False
        if v:
            return True
        time.sleep(poll)
    return False

# ---- pure JSON-shape checks (shared by the live suites and --selftest) -----

def schema_problems(doc):
    """/api/schema: {"groups":[10x {name,scope,fields}],"mgroups":[...],
    "hw_readonly":true} -- hg_json.h's own doc comment."""
    if not isinstance(doc, dict):
        return ["not a JSON object"]
    p = []
    groups = doc.get("groups")
    if not isinstance(groups, list) or len(groups) != 10:
        p.append(f"expected 10 groups, got {len(groups) if isinstance(groups, list) else groups!r}")
    else:
        for g in groups:
            if not all(k in g for k in ("name", "scope", "fields")):
                p.append(f"group missing name/scope/fields: {g}")
    if doc.get("hw_readonly") is not True:
        p.append(f"hw_readonly is not true: {doc.get('hw_readonly')!r}")
    mgroups = doc.get("mgroups")
    if not isinstance(mgroups, list) or len(mgroups) < 1:
        p.append(f"mgroups missing/empty: {mgroups!r}")
    return p

def state_problems(doc):
    """/api/state: {"master":{...,"time","defaults","http":{"cmd_quarantined"}},
    "nodes":[...],"ring":{"state",...}} -- state_snap.h / task-12-report.md."""
    if not isinstance(doc, dict):
        return ["not a JSON object"]
    p = []
    nodes = doc.get("nodes")
    if not isinstance(nodes, list) or len(nodes) < 1:
        p.append(f"expected >=1 node, got {nodes!r}")
    ring = doc.get("ring") or {}
    if ring.get("state") not in ("OK", "OPEN", "IDLE"):
        p.append(f"ring.state not in OK/OPEN/IDLE: {ring.get('state')!r}")
    master = doc.get("master") or {}
    if not isinstance(master.get("time"), str):
        p.append("master.time missing")
    if "defaults" not in master:
        p.append("master.defaults missing")
    if "cmd_quarantined" not in (master.get("http") or {}):
        p.append("master.http.cmd_quarantined missing")
    return p

def config_problems(doc, zone):
    """zone 0: {"WIFI":{},"TIME":{},"SYS":{}}; zone 1..8:
    {"gen":N,"hw":{...},"cfg":{...}} -- hg_json.h's own doc comment."""
    if not isinstance(doc, dict):
        return ["not a JSON object"]
    p = []
    if zone == 0:
        for g in ("WIFI", "TIME", "SYS"):
            if g not in doc:
                p.append(f"zone 0 document missing {g}")
    else:
        if "gen" not in doc:
            p.append("missing gen")
        if "hw" not in doc or "cfg" not in doc:
            p.append("missing hw/cfg")
    return p

def parse_heap_min(text):
    """cmd_common.c h_status: '  Heap min : %u' (bytes)."""
    m = re.search(r"Heap min\s*:\s*(\d+)", text)
    return int(m.group(1)) if m else None

# ---- suites -----------------------------------------------------------------

def suite_schema(api, results):
    status, doc, body = get_json(api, "/api/schema", retries=3, retry_delay=1.5)
    check(results, "SCHEMA: GET /api/schema -> 200", status == 200, status)
    if status != 200 or doc is None:
        check(results, "SCHEMA: valid JSON body", doc is not None, body[:200])
        return
    problems = schema_problems(doc)
    check(results, "SCHEMA: shape (10 groups, hw_readonly, mgroups)", not problems, problems)

def suite_state(api, results):
    status, doc, body = get_json(api, "/api/state", retries=3, retry_delay=1.5)
    check(results, "STATE: GET /api/state -> 200", status == 200, status)
    if status != 200 or doc is None:
        check(results, "STATE: valid JSON body", doc is not None, body[:200])
        return
    problems = state_problems(doc)
    check(results, "STATE: shape (nodes, ring.state, master.time/defaults/http)", not problems, problems)

def suite_config(api, results, zone=2):
    status, doc, body = get_json(api, f"/api/config?zone={zone}", retries=3, retry_delay=1.5)
    check(results, f"CONFIG: GET zone {zone} -> 200", status == 200, status)
    if status != 200 or doc is None:
        return
    check(results, f"CONFIG: zone {zone} document shape (gen/hw/cfg)",
          not config_problems(doc, zone), doc if doc is not None else body[:200])

    def put_target(v):
        return put_json(api, f"/api/config?zone={zone}", {"cfg": {"shelf": [{"WATER": {"TARGET": v}}]}},
                         retries=3, retry_delay=1.5)

    def node_gen():
        s, d, _ = get_json(api, "/api/state", timeout=5.0, retries=2)
        if s != 200 or d is None:
            return None
        node = next((n for n in d.get("nodes", []) if n.get("id") == zone), None)
        return node

    def make_synced(gen_before):
        # cfg_sync == "OK" alone is not enough: it is already "OK" from
        # BEFORE this PUT in the steady state, so polling it right after
        # queuing a new write can return true on the very first tick,
        # before the new generation has even been pushed -- a genuine
        # false-positive race found on the bench (this run). The node's own
        # "gen" (node_mgr's cfg generation counter, task-12-report.md's own
        # bench transcript shows it incrementing per push) must also have
        # advanced past the pre-PUT value before "OK" means anything.
        def synced():
            node = node_gen()
            if not node:
                return False
            if gen_before is not None and node.get("gen") == gen_before:
                return False
            return node.get("cfg_sync") == "OK"
        return synced

    node0 = node_gen()
    gen_before = node0.get("gen") if node0 else None

    status, _, body = put_target(55)
    check(results, "CONFIG: PUT WATER TARGET 55 -> 202", status == 202, (status, body[:200]))
    check(results, f"CONFIG: node {zone} gen advances + cfg_sync == OK within 10s",
          poll_until(make_synced(gen_before), 10.0, 1.0), "")

    status, text = cmd(api, f"GET ZONE {zone} WATER 1", retries=3, retry_delay=1.5)
    check(results, "CONFIG: GET ZONE WATER 1 shows Target : 55",
          status == 200 and "Target : 55" in text, text[:200])

    node1 = node_gen()
    gen_mid = node1.get("gen") if node1 else None
    status, _, body = put_target(61)
    check(results, "CONFIG: restore TARGET 61 -> 202", status == 202, (status, body[:200]))
    poll_until(make_synced(gen_mid), 10.0, 1.0)
    status, text = cmd(api, f"GET ZONE {zone} WATER 1", retries=3, retry_delay=1.5)
    check(results, "CONFIG: restore confirmed Target : 61",
          status == 200 and "Target : 61" in text, text[:200])

    status, doc2, body = put_target(101)
    path_ok = bool(doc2 and "TARGET" in str(doc2.get("path", "")))
    check(results, "CONFIG: PUT TARGET 101 -> 400 with path", status == 400 and path_ok, (status, body[:200]))

    status, doc2, body = put_json(api, f"/api/config?zone={zone}", {"hw": {"HW": {"SOIL_MIN_OK_MV": 123}}},
                                   retries=3, retry_delay=1.5)
    warn_ok = bool(doc2 and doc2.get("warnings"))
    check(results, "CONFIG: PUT hw key -> 202 with a warning", status == 202 and warn_ok, (status, body[:200]))

def suite_uploads(api, results, master_bin, zone_bin, fleet_zone):
    """Every major step is its own try/except: on this rig a connection can
    fail outright (WinError 10060, the whole PC Wi-Fi association dropped)
    at any point, and one step's transport failure must not swallow the
    independent checks after it (zone upload and the fleet update do not
    depend on the master OTA cycle having completed this run)."""
    if not master_bin and not zone_bin:
        skip(results, "UPLOADS", "skip: no --master-bin/--zone-bin given")
        return

    def step(name, fn):
        try:
            fn()
        except Exception as e:
            check(results, f"UPLOADS: {name} raised {type(e).__name__}", False, str(e))

    zone_bytes = open(zone_bin, "rb").read() if zone_bin else None
    master_bytes = open(master_bin, "rb").read() if master_bin else None

    def do_mismatch():
        status, body = api.request("POST", "/api/fw/master", data=zone_bytes,
                                    headers={"Content-Type": "application/octet-stream"},
                                    timeout=60.0, retries=4, retry_delay=4.0)
        # a 4xx decided before the size guard passes may arrive with NO body
        # at all (http_upload.c) -- the status code is the truth here.
        check(results, "UPLOADS: zone image -> /api/fw/master -> 422 IMAGE_MISMATCH", status == 422, status)

    if zone_bytes is not None:
        step("zone-into-master mismatch check", do_mismatch)

    def do_chunked():
        status, body = api.post_chunked("/api/fw/master", b"not a real image" * 4, timeout=10.0)
        check(results, "UPLOADS: chunked body -> /api/fw/master -> 400", status == 400, status)

    step("chunked-body check", do_chunked)

    master_ok = [False]
    master_slot = [None]

    def do_master_upload():
        status, body = api.request("POST", "/api/fw/master", data=master_bytes,
                                    headers={"Content-Type": "application/octet-stream"},
                                    timeout=180.0, retries=4, retry_delay=5.0)
        try:
            doc = json.loads(body) if body else None
        except ValueError:
            doc = None
        ok = status == 200 and bool(doc and doc.get("ok") and doc.get("slot") and doc.get("version"))
        check(results, "UPLOADS: master image -> /api/fw/master -> 200 ok/slot/version",
              ok, (status, body[:200]))
        if ok:
            master_ok[0] = True
            master_slot[0] = doc["slot"]

    if master_bytes is not None:
        step("master image upload", do_master_upload)

    def do_reboot_and_trial():
        slot = master_slot[0]
        try:
            api.request("POST", "/api/cmd", data=b"REBOOT CONFIRM",
                        headers={"Content-Type": "text/plain"}, timeout=5.0, retries=1)
        except Exception:
            pass  # the board drops the connection mid-reboot -- expected

        def reachable():
            s, _ = api.request("GET", "/api/state", timeout=4.0, retries=1)
            if s == 200:
                return True
            if s == 401 and api.password:
                # a clockless boot never persists a session (http_auth's
                # documented frozen-base rule) -- log back in and keep polling.
                api.login(api.password, retries=1)
            return False

        reconnected = poll_until(reachable, 90.0, 2.0)
        check(results, "UPLOADS: reconnect after master reboot <= 90s", reconnected, "")
        if not reconnected:
            return

        def version_shows(want_state):
            s, t = cmd(api, "GET VERSION", timeout=5.0, retries=3, retry_delay=2.0)
            return s == 200 and slot in t and want_state in t

        pending = poll_until(lambda: version_shows("PENDING"), 40.0, 2.0)
        check(results, f"UPLOADS: GET VERSION shows {slot} PENDING", pending, "")
        valid = poll_until(lambda: version_shows("VALID"), 90.0, 3.0)
        check(results, f"UPLOADS: GET VERSION {slot} reaches VALID (trial pass)", valid, "")

    if master_ok[0]:
        step("reboot + trial pass", do_reboot_and_trial)

    def do_zone_upload():
        status, body = api.request("POST", "/api/fw/zone", data=zone_bytes,
                                    headers={"Content-Type": "application/octet-stream"},
                                    timeout=60.0, retries=4, retry_delay=4.0)
        try:
            doc = json.loads(body) if body else None
        except ValueError:
            doc = None
        ok = status == 200 and bool(doc and doc.get("ok") and "len" in doc)
        check(results, "UPLOADS: zone image -> /api/fw/zone -> 200 ok/len", ok, (status, body[:200]))

        status, text = cmd(api, "GET FW ZONE", retries=4, retry_delay=2.0)
        # GET FW ZONE reports the fleet sequencer's own state (IDLE/<z> PHASE),
        # not the stored image (task-13-report.md concern #6) -- this only
        # proves the command channel/zone_fw partition are alive post-upload.
        check(results, "UPLOADS: GET FW ZONE answers OK after the zone upload", status == 200, text[:200])

    if zone_bytes is not None:
        step("zone image upload", do_zone_upload)

    def do_fleet():
        status, doc, body = post_json(api, "/api/fleet", {"zone": fleet_zone}, retries=4, retry_delay=2.0)
        check(results, f"UPLOADS: POST /api/fleet zone {fleet_zone} -> 202", status == 202, (status, body[:200]))

        def fleet_done():
            s, d, _ = get_json(api, "/api/state", timeout=5.0, retries=2)
            if s != 200 or d is None:
                return False
            line = (d.get("master") or {}).get("fleet", "")
            return line == "IDLE" or "DONE" in line

        done = poll_until(fleet_done, 60.0, 2.0)
        check(results, "UPLOADS: state.fleet reaches DONE/IDLE within 60s", done, "")

    if fleet_zone:
        step("fleet update", do_fleet)

def suite_wifi(api, results):
    status, doc, body = None, None, b""
    for _ in range(3):
        status, doc, body = get_json(api, "/api/wifi/scan", timeout=20.0, retries=5, retry_delay=3.0)
        if status != 409:   # a concurrent net_ops apply -- not expected here, retry once or twice
            break
        time.sleep(1.5)
    check(results, "WIFI: GET /api/wifi/scan -> 200", status == 200, status)
    if status != 200 or doc is None:
        return
    check(results, "WIFI: >= 1 network found", isinstance(doc, list) and len(doc) >= 1,
          len(doc) if isinstance(doc, list) else doc)
    if isinstance(doc, list) and doc:
        check(results, "WIFI: entries carry ssid/rssi/auth",
              all(k in doc[0] for k in ("ssid", "rssi", "auth")), doc[0])

def login_raw(api, password):
    return api.request("POST", "/api/login", data=json.dumps({"password": password}).encode(),
                        headers={"Content-Type": "application/json"}, retries=3, retry_delay=1.0)

def suite_login(ip, password, results, timeout=10.0):
    """Runs LAST in the standard suite order: this deliberately locks the web
    password out for ~60s (5 wrong + 1 confirm), and nothing after it needs a
    fresh login."""
    probe = ApiClient(ip, timeout=timeout)
    status, _ = probe.request("GET", "/api/state", retries=3, retry_delay=1.0)
    check(results, "LOGIN: /api/state without a cookie -> 401", status == 401, status)

    fresh = ApiClient(ip, timeout=timeout)
    wrong = password + "-wrong"
    for i in range(5):
        status, _ = login_raw(fresh, wrong)
        check(results, f"LOGIN: wrong password attempt {i + 1}/5 -> 401", status == 401, status)
    status, _ = login_raw(fresh, wrong)
    check(results, "LOGIN: 6th wrong password -> 429 LOCKED", status == 429, status)

    status, _ = login_raw(fresh, password)
    check(results, "LOGIN: correct password right after lockout -> 429 or 204", status in (429, 204), status)
    if status == 429:
        # the lockout persists ~60s (http_auth's frozen-base-aware release,
        # task-11-report.md) -- wait it out so the release itself is proven
        # rather than just the transition into lockout.
        time.sleep(62)
        status, _ = login_raw(fresh, password)
        check(results, "LOGIN: correct password after the ~60s lockout window -> 204", status == 204, status)
    check(results, "LOGIN: session cookie set on the eventual success",
          any(c.name == "hg_sess" for c in fresh.cj), [c.name for c in fresh.cj])

def get_heap_min(api, **kw):
    kw.setdefault("timeout", 8.0)
    kw.setdefault("retries", 5)
    kw.setdefault("retry_delay", 2.0)
    status, text = cmd(api, "GET STATUS", **kw)
    return parse_heap_min(text) if status == 200 else None

def soak_worker(ip, password, seconds, out, idx, stop_evt):
    api = ApiClient(ip)
    if not api.login(password, retries=5, retry_delay=2.0):
        out[idx] = {"ok": 0, "non200": 0, "errors": 1}
        return
    ok = non200 = errors = 0
    end = time.monotonic() + seconds
    while time.monotonic() < end and not stop_evt.is_set():
        t0 = time.monotonic()
        try:
            status, _ = api.request("GET", "/api/state", timeout=6.0, retries=1)
            if status == 200:
                ok += 1
            else:
                non200 += 1
        except Exception:
            errors += 1
        time.sleep(max(0.0, 2.0 - (time.monotonic() - t0)))
    out[idx] = {"ok": ok, "non200": non200, "errors": errors}

def suite_soak(ip, password, seconds, results, timeout=10.0):
    """Two threads poll /api/state every 2s for `seconds`. Hard requirement:
    zero non-200 responses. Soft allowance: <=1% transport-level errors (the
    bench AP link is lossy), reported either way. GET STATUS heap-min is read
    before the threads start and after they finish; the SP4 bar (by ruling,
    given task-13's own measured ~85-87 KB fresh-boot baseline on this rig)
    is drift <=8 KB and both readings >=64 KB."""
    api0 = ApiClient(ip, timeout=timeout)
    if not api0.login(password, retries=5, retry_delay=2.0):
        check(results, "SOAK: initial login", False, "")
        return
    heap_before = get_heap_min(api0)
    check(results, "SOAK: GET STATUS heap-min before", heap_before is not None, heap_before)

    stop_evt = threading.Event()
    out = [None, None]
    threads = [threading.Thread(target=soak_worker, args=(ip, password, seconds, out, i, stop_evt), daemon=True)
               for i in range(2)]
    t_start = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join(seconds + 60)
    elapsed = time.monotonic() - t_start
    stop_evt.set()

    total_ok = sum((s or {}).get("ok", 0) for s in out)
    total_non200 = sum((s or {}).get("non200", 0) for s in out)
    total_errors = sum((s or {}).get("errors", 0) for s in out)
    total = total_ok + total_non200 + total_errors
    err_pct = (100.0 * total_errors / total) if total else 100.0

    check(results, "SOAK: zero non-200 /api/state responses", total_non200 == 0, total_non200)
    check(results, f"SOAK: transport errors <= 1% ({total_errors}/{total} = {err_pct:.2f}%)",
          total > 0 and err_pct <= 1.0, total_errors)

    heap_after = get_heap_min(api0)
    check(results, "SOAK: GET STATUS heap-min after", heap_after is not None, heap_after)
    drift = None
    if heap_before is not None and heap_after is not None:
        drift = abs(heap_before - heap_after)
        check(results, "SOAK: heap-min drift <= 8 KB", drift <= 8 * 1024, drift)
        check(results, "SOAK: heap-min >= 64 KB before and after",
              heap_before >= 64 * 1024 and heap_after >= 64 * 1024, (heap_before, heap_after))

    print(f"SOAK summary: {elapsed:.1f}s elapsed, 2 pollers, {total} requests "
          f"({total_ok} ok / {total_non200} non-200 / {total_errors} transport errors, "
          f"{err_pct:.2f}% error rate); heap_min before={heap_before} after={heap_after} drift={drift}")

# ---- offline self-test -------------------------------------------------------

FIXTURE_SCHEMA = {
    "groups": [{"name": n, "scope": 0, "fields": [{"key": "X", "type": "U8", "min": 0, "max": 1}]}
               for n in ("ZONECFG", "SHELF", "LIGHT", "WATER", "FAN", "VIB", "AUX", "HW", "HWSHELF", "CAL")],
    "mgroups": [{"name": n, "fields": [{"key": "X", "type": "STR16", "max": 32, "secret": False}]}
                for n in ("WIFI", "TIME", "SYS")],
    "hw_readonly": True,
}

# Captured verbatim from task-12-report.md's bench transcript.
FIXTURE_STATE = {
    "master": {
        "version": "0.1.0", "uptime_s": 208, "heap_min_kb": 83,
        "time": "1970-01-01 00:03:28", "time_src": "NONE",
        "wifi": {"sta": {"up": False, "ip": "", "ssid": "", "rssi": 0, "reason": ""},
                 "ap": {"ssid": "HillGrow", "clients": 1, "ip": "192.168.7.7"}},
        "fw": {"slot": "ota_0", "state": "VALID", "other": "NONE", "upload_kind": "", "upload_pct": 0},
        "fleet": "IDLE",
        "alarms": {"active": 0, "total": 155},
        "http": {"cmd_quarantined": 0},
        "defaults": {"web": False, "ap": False},
    },
    "nodes": [
        {"id": 1, "name": "", "mac": "c0:5d:89:df:2a:88", "health": "ONLINE", "fw": "0.1.0",
         "gen": 3, "hops": 0, "link": 7, "link_stale": False, "cfg_sync": "OK", "hb_age_s": 0,
         "uptime_s": 2630, "heap_kb": 233, "reset": 1, "faults": "0x0", "mode": 0, "shelves": []},
    ],
    "ring": {"state": "OK", "size": 2, "online": 6, "blame": ""},
}

FIXTURE_CONFIG0 = {
    "WIFI": {"STA_SSID": "", "STA_PASS": "", "AP_SSID": "HillGrow", "AP_PASS": "hillgrow1"},
    "TIME": {"TZ": "CET-1CEST,M3.5.0,M10.5.0/3", "NTP": "pool.ntp.org"},
    "SYS": {"HOSTNAME": "hillgrow"},
}

FIXTURE_CONFIGN = {
    "gen": 34,
    "hw": {"HW": {}, "shelf": [{"HWSHELF": {}, "CAL": {}}]},
    "cfg": {"ZONECFG": {}, "shelf": [{"SHELF": {}, "LIGHT": {}, "WATER": {"MODE": "AUTO", "TARGET": 61},
                                       "FAN": {}, "VIB": {}}],
            "aux": [{"AUX": {}}]},
}

class _FakeHTTPResponse:
    def __init__(self, status, body):
        self.status, self._body = status, body
    def read(self):
        return self._body
    def __enter__(self):
        return self
    def __exit__(self, *a):
        return False

class _FlakyOpener:
    """Offline stand-in for ApiClient.opener: raises a transport error the
    first `fail_times` calls, then returns a canned response."""
    def __init__(self, fail_times, status=200, body=b""):
        self.fail_times, self.status, self.body, self.calls = fail_times, status, body, 0
    def open(self, req, timeout=None):
        self.calls += 1
        if self.calls <= self.fail_times:
            raise urllib.error.URLError("simulated drop")
        return _FakeHTTPResponse(self.status, self.body)

class _Fail401Opener:
    """Offline stand-in that always answers a real HTTP 401 the way urllib
    itself would (as an HTTPError, not a returned status) -- proves such a
    response is returned as-is, never retried."""
    def __init__(self):
        self.calls = 0
    def open(self, req, timeout=None):
        self.calls += 1
        raise urllib.error.HTTPError(req.full_url, 401, "Unauthorized", {},
                                      io.BytesIO(b'{"error":"UNAUTHORIZED"}'))

def selftest():
    fails = []

    def expect(name, cond):
        if not cond:
            fails.append(name)

    # (a) check()/skip() bookkeeping
    r = []
    check(r, "x", True, "d")
    check(r, "y", False, "d2")
    skip(r, "z", "skip reason")
    expect("a: check ok/detail", r[0] == {"name": "x", "ok": True, "detail": "d"})
    expect("a: check fail recorded", r[1]["ok"] is False)
    expect("a: skip flagged and ok", r[2]["ok"] is True and r[2].get("skip") is True)

    # (b) chunked_body framing (the exact bytes post_chunked sends on the wire)
    expect("b: chunked framing", chunked_body(b"hello") == b"5\r\nhello\r\n0\r\n\r\n")

    # (c) parse_heap_min against the real GET STATUS reply shape (cmd_common.c)
    expect("c: heap-min parsed", parse_heap_min("OK STATUS\n  Uptime : 39 s\n  Heap min : 87508\n") == 87508)
    expect("c: heap-min missing -> None", parse_heap_min("OK STATUS\n  Uptime : 1 s\n") is None)

    # (d) schema shape, against the real captured document and two mutations
    expect("d: real schema fixture has no problems", schema_problems(FIXTURE_SCHEMA) == [])
    bad = dict(FIXTURE_SCHEMA); bad["groups"] = FIXTURE_SCHEMA["groups"][:9]
    expect("d: 9 groups flagged", schema_problems(bad) != [])
    bad2 = dict(FIXTURE_SCHEMA); bad2["hw_readonly"] = False
    expect("d: hw_readonly=false flagged", schema_problems(bad2) != [])

    # (e) state shape, against the real captured document and three mutations
    expect("e: real state fixture has no problems", state_problems(FIXTURE_STATE) == [])
    bad3 = json.loads(json.dumps(FIXTURE_STATE)); bad3["nodes"] = []
    expect("e: zero nodes flagged", state_problems(bad3) != [])
    bad4 = json.loads(json.dumps(FIXTURE_STATE)); bad4["ring"]["state"] = "WEIRD"
    expect("e: bad ring.state flagged", state_problems(bad4) != [])
    bad5 = json.loads(json.dumps(FIXTURE_STATE)); del bad5["master"]["http"]
    expect("e: missing master.http flagged", state_problems(bad5) != [])

    # (f) config shape, zone 0 and zone N, against the real captured documents
    expect("f: zone0 fixture ok", config_problems(FIXTURE_CONFIG0, 0) == [])
    expect("f: zoneN fixture ok", config_problems(FIXTURE_CONFIGN, 2) == [])
    bad6 = dict(FIXTURE_CONFIG0); del bad6["TIME"]
    expect("f: zone0 missing TIME flagged", config_problems(bad6, 0) != [])
    bad7 = dict(FIXTURE_CONFIGN); del bad7["gen"]
    expect("f: zoneN missing gen flagged", config_problems(bad7, 2) != [])

    # (g) ApiClient.request: retries a transport failure, never a real HTTP status
    api = ApiClient("127.0.0.1")
    api.opener = _FlakyOpener(fail_times=2, status=200, body=b"ok")
    status, body = api.request("GET", "/x", retries=5, retry_delay=0)
    expect("g: retried past two transport failures", status == 200 and body == b"ok" and api.opener.calls == 3)

    api2 = ApiClient("127.0.0.1")
    api2.opener = _Fail401Opener()
    status2, body2 = api2.request("GET", "/x", retries=5, retry_delay=0)
    expect("g: a real 401 is returned as-is, never retried",
           status2 == 401 and api2.opener.calls == 1 and b"UNAUTHORIZED" in body2)

    if fails:
        print("SELFTEST FAIL:", ", ".join(fails))
        return 1
    print("SELFTEST OK (7 assertion groups)")
    return 0

# ---- main ---------------------------------------------------------------

def run_standard(args, results):
    api = ApiClient(args.ip, timeout=args.timeout)
    # The bench AP link drops the PC's whole Wi-Fi association outright
    # every few minutes (every SP4 task report on this rig documents it) --
    # that is an OS-level disconnect no amount of per-request retrying can
    # paper over, so the initial login gets its own generous retry budget
    # and a clean failure (not a crash) if the link is down for longer than
    # that. Everything past this point can also fail the same way; each
    # suite already runs inside its own try/except below.
    try:
        ok = api.login(args.password, retries=6, retry_delay=3.0)
    except Exception as e:
        check(results, "SETUP: initial login", False, f"{type(e).__name__}: {e}")
        return
    check(results, "SETUP: initial login", ok, "" if ok else "could not log in with --password")
    if not ok:
        return

    wanted = {s.strip().upper() for s in args.only.split(",")} if args.only else None
    for name, fn, fnargs in (
        ("SCHEMA", suite_schema, (api, results)),
        ("STATE", suite_state, (api, results)),
        ("CONFIG", suite_config, (api, results, args.config_zone)),
        ("UPLOADS", suite_uploads, (api, results, args.master_bin, args.zone_bin, args.fleet)),
        ("WIFI", suite_wifi, (api, results)),
        ("LOGIN", suite_login, (args.ip, args.password, results, args.timeout)),
    ):
        if wanted is not None and name not in wanted:
            continue
        try:
            fn(*fnargs)
        except Exception as e:
            check(results, f"{name}: suite raised {type(e).__name__}", False, str(e))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ip", nargs="?", help="master's HTTP IP, e.g. 192.168.7.7 (omit with --selftest)")
    ap.add_argument("--password", help="web UI password")
    ap.add_argument("--master-bin", help="path to hillgrow_master.bin for the uploads suite")
    ap.add_argument("--zone-bin", help="path to hillgrow_zone.bin for the uploads suite")
    ap.add_argument("--fleet", type=int, metavar="ZONE", help="zone id for the fleet-update check")
    ap.add_argument("--config-zone", type=int, default=2, help="zone id for the config suite (default 2)")
    ap.add_argument("--only", metavar="SUITE[,SUITE...]",
                     help="run only these standard suites (schema,state,config,uploads,wifi,login); "
                          "default: all. Ignored with --soak.")
    ap.add_argument("--soak", type=int, default=0, metavar="SECONDS",
                     help="run ONLY the soak suite for this many seconds instead of the standard suites")
    ap.add_argument("--timeout", type=float, default=10.0, help="default per-request timeout, seconds")
    ap.add_argument("--json", metavar="FILE", help="dump results as JSON to FILE")
    ap.add_argument("--selftest", action="store_true", help="offline shape/retry self-check; no network needed")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())
    if not args.ip:
        ap.error("ip is required unless --selftest")
    if not args.password:
        ap.error("--password is required")

    results = []
    if args.soak > 0:
        try:
            suite_soak(args.ip, args.password, args.soak, results, timeout=args.timeout)
        except Exception as e:
            check(results, f"SOAK: suite raised {type(e).__name__}", False, str(e))
    else:
        run_standard(args, results)

    failures = [r for r in results if not r["ok"]]
    for r in results:
        label = "SKIP" if r.get("skip") else ("PASS" if r["ok"] else "FAIL")
        print(label, r["name"], "--", r["detail"])
    print(f"{len(results) - len(failures)}/{len(results)} passed")

    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)

    sys.exit(len(failures))

if __name__ == "__main__":
    main()
