#!/usr/bin/env python3
"""HillGrow serial CLI smoke/regression harness.

Drives one board's console over UART and checks it against the live command
grammar (cmd_core/cmd_common/zone_cmds); the regression gate used from SP2
onward. "smoke" (default, only suite) auto-skips zone-only checks on role
MASTER; --role is asserted against GET ID, not a mode switch. Exit code is
the number of failed checks.

Usage:
    C:\\Python311\\python tools/uart_test.py COM5 --role ZONE --allow-reboot
    C:\\Python311\\python tools/uart_test.py --http 192.168.7.7 --password hillgrow1 --role MASTER
    C:\\Python311\\python tools/uart_test.py --selftest   # offline, no hardware/pyserial
"""
import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request
from http.cookiejar import CookieJar

LOG_RE = re.compile(r'^[IWEDV] \(\d+\)')
# Adjacent two-word nouns only ("SET FW ROLLBACK"); extend when a split-shape
# row ("SET NODE <z> NAME <v>") lands in the master/ring tables.
ROW_RE = re.compile(r'^\+ (SET|GET) ([A-Z][A-Z0-9_]*)(?: ([A-Z][A-Z0-9_]*))?(?=[ \[]|$)')
VERSION_RE = re.compile(r'\d+\.\d+\.\d+')
MASK_TOK_RE = re.compile(r'^[A-Z_]+=[01]$')

class Reply:
    """One Node.send() result: ok/err/timeout, the OK line's payload text,
    continuation lines ("  " stripped) or raw HELP "+" rows, NOTIFY lines."""
    def __init__(self, ok, err=None, text="", lines=None, notify=None, timeout=False):
        self.ok, self.err, self.text = ok, err, text
        self.lines = lines or []
        self.notify = notify or []
        self.timeout = timeout

class Node:
    """Drives one board over an injected transport (write_line/read_line).
    Real hardware: SerialTransport via open_node(); HTTP: HttpTransport via
    open_http_node(); --selftest: FakeTransport. is_http lets prologue()/
    run_smoke() adapt to the console-only rows HTTP sessions can't touch
    (they're CMDF_SESSION rows -- cmd_dispatch.c answers ERR NOT_LOCAL for a
    CMD_SRC_HTTP session, see http_cmd.c) without the caller having to know
    the transport's concrete type."""
    def __init__(self, transport, is_http=False):
        self.transport = transport
        self._pending = None
        self.role = None
        self.is_http = is_http

    def adopt(self, transport):
        """Swap in a fresh transport (post-reboot reconnect) so callers keep
        using this Node object; drops any stale pending line."""
        self.transport = transport
        self._pending = None

    def _readline(self, deadline):
        if self._pending is not None:
            line, self._pending = self._pending, None
            return line
        return self.transport.read_line(deadline)

    def send(self, line, timeout=4.0):
        """A normal reply is "OK ..." plus "  "-indented continuations; a
        HELP reply (cmd_help.c) has no OK wrapper, just "+ ..." lines.
        Log lines are dropped anywhere; NOTIFY lines are side-collected."""
        self.transport.write_line(line)
        deadline = time.monotonic() + timeout
        notify, cont = [], []
        status, text = None, ""
        while time.monotonic() < deadline:
            raw = self._readline(deadline)
            if raw is None:
                break
            if LOG_RE.match(raw):
                continue
            if raw.startswith("NOTIFY "):
                notify.append(raw)
                continue
            if status is None:
                if raw == "OK" or raw.startswith("OK "):
                    status, text = "OK", raw[2:].strip()
                    continue
                if raw == "ERR" or raw.startswith("ERR "):
                    return Reply(False, err=raw[3:].strip(), notify=notify)
                if raw.startswith("+"):
                    status = "HELP"
                    cont.append(raw)
                    continue
                continue  # noise before any status line: ignore
            if status == "OK" and raw.startswith("  "):
                cont.append(raw[2:])
                continue
            if status == "HELP" and raw.startswith("+"):
                cont.append(raw)
                continue
            self._pending = raw  # belongs to the next response; stash it
            break
        if status in ("OK", "HELP"):
            return Reply(True, text=text, lines=cont, notify=notify)
        return Reply(False, notify=notify, timeout=True)

    def prologue(self, results=None):
        """SET ECHO/SET LOG/SET NOTIFY are session rows (CMDF_SESSION):
        cmd_dispatch answers ERR NOT_LOCAL for any CMD_SRC_HTTP session, so
        over HTTP there is no console state for them to configure. Rather
        than silently skip them, send each once and assert the NOT_LOCAL
        contract holds -- if results is given, one check per row is recorded
        instead of a bare assert, so a broken contract shows up as a normal
        FAIL rather than an uncaught exception mid-run."""
        if self.is_http:
            for line in ("SET ECHO OFF", "SET LOG WARN", "SET NOTIFY ALL OFF"):
                r = self.send(line)
                ok = (not r.ok) and (not r.timeout) and r.err == "NOT_LOCAL"
                if results is not None:
                    check(results, f"PROLOGUE(http): {line} -> ERR NOT_LOCAL", ok, r.err or r.text)
        else:
            self.send("SET ECHO OFF")
            self.send("SET LOG WARN")
            self.send("SET NOTIFY ALL OFF")
        r = self.send("GET ID")
        parts = r.text.split()
        self.role = parts[1] if r.ok and len(parts) > 1 else None
        return r

    def close(self):
        self.transport.close()

class SerialTransport:
    """Real pyserial transport; import is lazy so --selftest needs no pyserial."""
    def __init__(self, port, baud=115200):
        import serial
        self.ser = serial.Serial(port, baud, timeout=0.2)

    def write_line(self, line):
        self.ser.write((line + "\r\n").encode("utf-8", "replace"))

    def read_line(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        self.ser.timeout = min(0.2, remaining)
        raw = self.ser.readline()
        if not raw:
            return None
        return raw.decode("utf-8", "replace").rstrip("\r\n")

    def close(self):
        self.ser.close()

def open_node(port, baud=115200):
    return Node(SerialTransport(port, baud))

class HttpTransport:
    """HTTP transport for the same Node/Reply parser: POST /api/cmd's reply
    is the CLI's own reply text verbatim (http_cmd.c's h_cmd), so splitting
    it into lines and handing them back one at a time from read_line() needs
    no HTTP-specific parsing anywhere else in this file -- Node.send()'s
    OK/ERR/continuation logic is identical to the serial path. Logs in once
    at construction (POST /api/login); the cookie jar then carries hg_sess
    for the life of the run. The bench AP link is lossy (every SP4 task
    report documents repeated full Wi-Fi drops), so every request retries a
    bounded number of times on a transport-level failure -- never on an HTTP
    error response, which is a real answer, not a dropped packet."""
    def __init__(self, ip, password, timeout=8.0, retries=6, retry_delay=2.0):
        self.base = f"http://{ip}"
        self.timeout = timeout
        self.retries = retries
        self.retry_delay = retry_delay
        self.cj = CookieJar()
        self.opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.cj))
        self._lines = []
        self._login(password)

    def _request(self, path, data, headers):
        last_exc = None
        for attempt in range(self.retries):
            req = urllib.request.Request(self.base + path, data=data, headers=headers, method="POST")
            try:
                with self.opener.open(req, timeout=self.timeout) as resp:
                    return resp.status, resp.read()
            except urllib.error.HTTPError as e:
                return e.code, e.read()   # a real HTTP answer -- never retried
            except (urllib.error.URLError, OSError) as e:
                last_exc = e
                if attempt + 1 < self.retries:
                    time.sleep(self.retry_delay)
        raise RuntimeError(f"POST {path} failed after {self.retries} attempts: {last_exc}")

    def _login(self, password):
        body = json.dumps({"password": password}).encode("utf-8")
        status, resp_body = self._request("/api/login", body, {"Content-Type": "application/json"})
        if status != 204:
            raise RuntimeError(f"HTTP login failed: {status} {resp_body!r}")

    def write_line(self, line):
        status, body = self._request("/api/cmd", line.encode("utf-8", "replace"),
                                      {"Content-Type": "text/plain"})
        self._lines = body.decode("utf-8", "replace").splitlines()

    def read_line(self, deadline):
        return self._lines.pop(0) if self._lines else None

    def close(self):
        pass

def open_http_node(ip, password, timeout=8.0):
    return Node(HttpTransport(ip, password, timeout=timeout), is_http=True)

def parse_help_rows(text):
    """Extract 'SET NOUN[ NOUN2]' / 'GET NOUN[ NOUN2]' prefixes from a block
    of cmd_help.c '+ ...' usage lines, one prefix per matched row."""
    out = []
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        parts = [m.group(1), m.group(2)] + ([m.group(3)] if m.group(3) else [])
        out.append(" ".join(parts))
    return out

# ---- suites (each appends {"name","ok","detail"} dicts to results) --------

def check(results, name, ok, detail=""):
    results.append({"name": name, "ok": bool(ok), "detail": str(detail)})

def skip(results, name, detail):
    """A check that never fails and prints as SKIP rather than PASS/FAIL --
    used for suites that need a real console (session rows) and cannot run
    over HTTP at all, so their absence is visible rather than silently
    dropped from the report."""
    results.append({"name": name, "ok": True, "detail": str(detail), "skip": True})

def suite_id_version(node, role_arg, results):
    r = node.send("GET ID")
    ok = r.ok and (role_arg is None or role_arg.upper() in r.text.split())
    check(results, "ID/VERSION: GET ID role", ok, r.text)
    r2 = node.send("GET VERSION")
    check(results, "ID/VERSION: GET VERSION M.m.p", r2.ok and bool(VERSION_RE.search(r2.text)), r2.text)

def suite_help(node, results):
    r = node.send("HELP")
    check(results, "HELP: bare non-empty", r.ok and len(r.lines) > 0, len(r.lines))
    rows = set()
    for q in ("SET HELP", "GET HELP"):
        rr = node.send(q)
        rows.update(parse_help_rows("\n".join(rr.lines)))
    for prefix in sorted(rows):
        rr = node.send(prefix + " HELP")
        seen = "\n".join(rr.lines)
        check(results, f"HELP self-check: {prefix}", rr.ok and prefix in seen, seen[:60])

def suite_errors(node, role, results):
    r = node.send("FOO")
    check(results, "ERRORS: UNKNOWN_CMD", not r.ok and r.err == "UNKNOWN_CMD", r.err)
    if role != "ZONE":
        return
    r = node.send("SET WATER 1 TARGET 101")
    check(results, "ERRORS: OUT_OF_RANGE", not r.ok and r.err == "OUT_OF_RANGE", r.err)
    r = node.send("SET SHELVES 1")  # CMDF_UNLOCK row; session not DEBUG-unlocked
    check(results, "ERRORS: LOCKED", not r.ok and r.err == "LOCKED", r.err)

def suite_config(node, role, results):
    if role != "ZONE":
        return
    node.send("SET WATER 1 TARGET 61")
    r = node.send("GET WATER 1")
    check(results, "CONFIG: WATER TARGET round-trip", r.ok and "Target : 61" in r.lines, r.lines)
    r = node.send("SET SHELF 1 CROP Basil")
    check(results, "CONFIG: SHELF CROP echo preserved", r.ok and "Basil" in r.text, r.text)

def suite_persist(node, role, args, results, reconnect=None):
    if role != "ZONE":
        return
    r = node.send("SAVE")
    check(results, "PERSIST: SAVE", r.ok, r.text)
    if not args.allow_reboot:
        return
    node.send("REBOOT CONFIRM", timeout=1.0)
    node.close()  # Windows opens COM ports exclusively: close before reopen
    time.sleep(3.0)
    node2 = (reconnect or (lambda: open_node(args.port, args.baud)))()
    node.adopt(node2.transport)  # later suites keep using the caller's node
    node.prologue()
    r = node.send("GET WATER 1")
    check(results, "PERSIST: WATER TARGET survives reboot", r.ok and "Target : 61" in r.lines, r.lines)

def suite_session(node, results):
    r = node.send("SET LOG DEBUG")
    check(results, "SESSION: LOG DEBUG echo", r.ok and "DEBUG" in r.text, r.text)
    r = node.send("GET NOTIFY")
    # notify_mask_str() (cmd_common.c): space-joined "NAME=0|1" tokens.
    toks = r.text.split()
    mask_ok = (r.ok and toks[:1] == ["NOTIFY"] and len(toks) > 1
               and all(MASK_TOK_RE.match(t) for t in toks[1:])
               and any(t.startswith("BOOT=") for t in toks[1:]))
    check(results, "SESSION: NOTIFY mask shape", mask_ok, r.text)

def run_smoke(node, args, results):
    node.prologue(results)
    role = node.role
    suite_id_version(node, args.role, results)
    suite_help(node, results)
    suite_errors(node, role, results)
    suite_config(node, role, results)
    suite_persist(node, role, args, results)
    if node.is_http:
        # LOG level and NOTIFY filters are console-only (CMDF_SESSION rows,
        # ERR NOT_LOCAL over HTTP -- see prologue()); nothing here would
        # exercise anything but that same refusal a second time.
        skip(results, "SESSION: LOG DEBUG / NOTIFY mask", "skip(http): console-only, NOT_LOCAL over HTTP")
    else:
        suite_session(node, results)

# ---- selftest: offline parser core check, no hardware/pyserial needed -----

class FakeTransport:
    """Offline replay for --selftest: write_line() is a no-op; read_line()
    pops the next canned line regardless of deadline (fully scripted).
    on_close, if given, fires from close() -- lets selftest observe ordering."""
    def __init__(self, lines, on_close=None):
        self.lines = list(lines)
        self.closed = False
        self._on_close = on_close

    def write_line(self, line):
        pass

    def read_line(self, deadline):
        return self.lines.pop(0) if self.lines else None

    def close(self):
        self.closed = True
        if self._on_close:
            self._on_close()

# Real cmd_help.c render_usage() format: "+ " SET/GET noun1[ noun2] args
# (each " <name>"/" [<name>]") [unlock]; GET rows show only n_key args.
HELP_BLOCK = (
    "+ SET WATER <shelf 1-4> <key> <value>\n"
    "+ GET WATER <shelf 1-4>\n"
    "+ SET HW <key> <value> [unlock]\n"
    "+ SET FW ROLLBACK <CONFIRM> [unlock]\n"
    "+ GET ID\n"
)

def selftest():
    fails = []

    def expect(name, cond):
        if not cond:
            fails.append(name)

    # (a) OK line + two-space continuations collected
    n = Node(FakeTransport(["OK WATER 1", "  Target : 61", "  Hyst : 5", "GARBAGE"]))
    r = n.send("GET WATER 1", timeout=0.2)
    expect("a: ok", r.ok)
    expect("a: continuations", r.lines == ["Target : 61", "Hyst : 5"])
    # (b) ERR line terminates send() -- no continuations collected after it
    n = Node(FakeTransport(["ERR OUT_OF_RANGE", "  should not be collected"]))
    r = n.send("SET WATER 1 TARGET 101", timeout=0.2)
    expect("b: err", (not r.ok) and not r.timeout and r.err == "OUT_OF_RANGE")
    expect("b: no continuations after err", r.lines == [])
    # (c) log lines matching ^[IWEDV] (n) dropped mid-response
    n = Node(FakeTransport(["OK WATER 1", "I (1234) wifi: connected", "  Target : 61"]))
    r = n.send("GET WATER 1", timeout=0.2)
    expect("c: log dropped", r.lines == ["Target : 61"])
    # (d) NOTIFY lines side-collected, never treated as the reply
    n = Node(FakeTransport(["NOTIFY WATER 1 shelf=1 dose", "OK WATER 1", "  Target : 61"]))
    r = n.send("GET WATER 1", timeout=0.2)
    expect("d: notify side-collected", r.notify == ["NOTIFY WATER 1 shelf=1 dose"])
    expect("d: reply still parsed", r.lines == ["Target : 61"])
    # (e) timeout returns a distinguishable failure (not an ERR, not ok)
    n = Node(FakeTransport([]))
    r = n.send("GET ID", timeout=0.05)
    expect("e: timeout distinguishable", r.timeout and not r.ok and r.err is None)
    # (f) HELP row-parsing regex against the real cmd_help.c output format
    rows = parse_help_rows(HELP_BLOCK)
    expect("f: help rows", rows == ["SET WATER", "GET WATER", "SET HW", "SET FW ROLLBACK", "GET ID"])
    # (g) PERSIST: close() before reopen (Windows COM ports are exclusive).
    # An on_close hook and an injected reconnect() record ordering offline.
    events = []

    def fake_reconnect():
        events.append("reopen")
        return Node(FakeTransport([]))

    p_node = Node(FakeTransport(["OK SAVE"], on_close=lambda: events.append("close")))
    old_transport = p_node.transport
    p_args = argparse.Namespace(allow_reboot=True, port="FAKE", baud=115200)
    orig_sleep, time.sleep = time.sleep, lambda s: None
    try:
        suite_persist(p_node, "ZONE", p_args, [], reconnect=fake_reconnect)
    finally:
        time.sleep = orig_sleep
    expect("g: close before reopen", events == ["close", "reopen"])
    expect("g: old transport closed", old_transport.closed)
    expect("g: reconnect adopted", p_node.transport is not old_transport
           and not p_node.transport.closed)

    # (h) prologue() over HTTP: session rows (SET ECHO/LOG/NOTIFY) asserted
    # NOT_LOCAL rather than sent as if they configured real console state.
    n = Node(FakeTransport(["ERR NOT_LOCAL", "ERR NOT_LOCAL", "ERR NOT_LOCAL",
                            "OK ID MASTER c0:5d:89:00:00:01 0 master"]), is_http=True)
    hresults = []
    n.prologue(hresults)
    expect("h: three NOT_LOCAL checks recorded", len(hresults) == 3)
    expect("h: all three passed", all(r["ok"] for r in hresults))
    expect("h: role parsed after the session-row checks", n.role == "MASTER")
    # a session row that does NOT answer NOT_LOCAL must fail loudly, not pass
    n2 = Node(FakeTransport(["OK", "ERR NOT_LOCAL", "ERR NOT_LOCAL", "OK ID MASTER x 0 master"]), is_http=True)
    hresults2 = []
    n2.prologue(hresults2)
    expect("h: a wrongly-accepted session row is reported FAIL, not swallowed", not hresults2[0]["ok"])

    # (i) skip(): marks a result SKIP without counting as a failure
    sresults = []
    skip(sresults, "SESSION: test", "skip(http): console-only")
    expect("i: skip ok=True", sresults[0]["ok"] is True)
    expect("i: skip flagged", sresults[0].get("skip") is True)
    expect("i: skip detail kept", sresults[0]["detail"] == "skip(http): console-only")

    if fails:
        print("SELFTEST FAIL:", ", ".join(fails))
        return 1
    print("SELFTEST OK (9 assertion groups)")
    return 0

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM5 (omit with --selftest or --http)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", metavar="IP", help="drive /api/cmd over HTTP instead of a serial port")
    ap.add_argument("--password", help="web UI password (required with --http)")
    ap.add_argument("--role", choices=["MASTER", "ZONE"], help="asserted against GET ID, not a mode switch")
    ap.add_argument("--suite", default="smoke", choices=["smoke"])
    ap.add_argument("--allow-reboot", action="store_true", help="run PERSIST's REBOOT CONFIRM step")
    ap.add_argument("--json", metavar="FILE", help="dump results as JSON to FILE")
    ap.add_argument("--selftest", action="store_true", help="offline parser self-check; no port/network needed")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest())

    results = []
    if args.http:
        if not args.password:
            ap.error("--password is required with --http")
        # The bench AP link drops the PC's whole Wi-Fi association outright
        # every few minutes (every SP4 task report on this rig documents
        # it) -- an OS-level disconnect HttpTransport's own per-request
        # retries cannot paper over. Report that cleanly (one FAIL, proper
        # exit code) rather than a raw traceback, so a flaky run is just
        # another failing check instead of a crash.
        try:
            node = open_http_node(args.http, args.password)
        except Exception as e:
            check(results, "SETUP: HTTP login", False, f"{type(e).__name__}: {e}")
            node = None
        if node is not None:
            try:
                run_smoke(node, args, results)
            except Exception as e:
                check(results, f"RUN: suite raised {type(e).__name__}", False, str(e))
    else:
        if not args.port:
            ap.error("port is required unless --selftest or --http")
        node = open_node(args.port, args.baud)
        run_smoke(node, args, results)

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
