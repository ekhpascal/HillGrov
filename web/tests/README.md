# Browser regression suites for the master web UI

Two headless-Chrome suites kept because each one pins a defect that reached the
owner on real hardware. They are **not** wired into the host-test gate — they
need a browser, so they stay manual. Run them when you change `web/app.js`.

## What they cover

| Suite | Pins |
|---|---|
| `t17_caret.js` | The 2 s poll re-render must not move the caret in a focused form field. Found on an Android phone at Task 17: typing `1` then `2` produced `21`, because headless drivers type faster than a poll tick and never saw it. Fixed in a8f1ff4. |
| `verify_fe_fixes.js` | The five defects the SP4 final whole-branch review found: the master config page rendering blank, a blanked secret wiping the stored Wi-Fi password, logout leaving the previous operator's config edits armed, a non-numeric route id causing a request storm, and alarm ages printed from absolute uptime stamps. |

## Running them

No npm dependencies — they drive Chrome over the raw CDP WebSocket using Node's
built-in `fetch` and `WebSocket` (Node 22+).

```sh
# 1. serve the web directory
cd web && python -m http.server 8123

# 2. start headless Chrome with the debug port open
chrome --headless=new --remote-debugging-port=9788 --user-data-dir=/tmp/hgtest

# 3. run a suite
HOST=http://127.0.0.1:8123 CDP_PORT=9788 node tests/verify_fe_fixes.js
```

Both suites exercise the UI in mock mode (`?mock=1`), which serves fixtures from
`HG.mock` inside `app.js` rather than talking to a master.

## The trap these suites exist to avoid

`verify_fe_fixes.js` only turns red because two **mock fidelity** bugs were fixed
alongside it: the mock used to return secret fields that the real
`hg_json_export_mcfg` omits at `?secrets=0`, and it answered an unparsable
`?zone=` value instead of the 400 the real `query_int` returns. Against the older,
more forgiving mock, a credential-destroying bug and an unbounded request storm
both looked like correct behaviour, and four suites stayed green over them.

So: when you add a fixture here, pin it to what the firmware actually *refuses*,
not just what it returns on the happy path. A mock kinder than the server is not
a test — it is a second implementation, and it will agree with your bug.
