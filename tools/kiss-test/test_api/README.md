# API regression suite

Black-box tests for the firmware HTTP + SSE surface. Used as a
behavioural freeze before the WebServer-stack migration (synchronous
`WebServer.h` → `ESPAsyncWebServer`) so we can validate the swap diff-
style: every test passing pre-migration must pass post-migration.

## Running

Both bench devices need to be reachable on the LAN; the test fixtures
use the kiss-tester / lr-tester credentials cached in `../project_tester_accounts`.

```bash
cd microReticulum_Firmware/tools/kiss-test
pytest test_api/                    # whole suite
pytest test_api/ -v                 # verbose
pytest test_api/test_sse.py -v      # one file
pytest test_api/ -k "battery"       # by substring
```

The suite logs in fresh tokens at session start and caches them in
`.token` / `.lr-token`, so subsequent re-runs (or the `bidir_*` scripts)
inherit the same auth state.

## What's covered

- `test_auth.py` — login / 401 paths, bad-token handling
- `test_info.py` — `/api/info` lightweight surface; asserts the
  storage / outbound_caps / battery-detail blocks **don't** leak in
- `test_system_status.py` — `/api/system_status` detailed surface;
  asserts radio / wifi / transport / battery-summary **don't** appear
- `test_identity.py` — identity CRUD + settings round-trips
- `test_inbox_outbox.py` — state / messages / outbox shape
- `test_attachments.py` — upload (X-Total-Length header, bound checks)
  + send-with-staging-id + download 404
- `test_sse.py` — `/events` connects, emits, announces propagate
- `test_misc.py` — storage migration absent-path, inbox/sensor config
  round-trips, paths lookup + estimate, radio shape

The full bidirectional attachment send-receive-verify path is covered
by the existing `bidir_attachment_test.py` at the parent directory;
this suite assumes that script passes and focuses on contract checks.

## Migration workflow

1. Run `pytest test_api/` against the current `WebServer.h`-based
   firmware. Save the run as the baseline (all pass).
2. Land the ESPAsyncWebServer port; re-run the suite. **All tests
   must still pass** before merging — that's the migration's
   correctness gate.
3. Path-consistency renames (e.g. `/api/system_status` →
   `/api/system/status`) update both server + SPA + this suite in
   the same commit.
4. After the REST surface is stable on the new server, the WebSocket
   counterpart lands as additional tests in `test_ws.py` (not yet
   written).
