# Engineering rules for this repository

These rules are binding for every agent and every session working on this
repo. They exist because a full audit of the fork's first 377 commits
(2026-06) traced its most expensive defects to a small set of recurring
process failures. Each rule names the failure it prevents.

## 1. Upstream parity comes from upstream code, not reasoning

This firmware ports Reticulum (RNS), LXMF and RNode behaviour. Upstream
is the source of truth.

- Before setting any default, constant, timeout, retry count, or
  mechanism that exists upstream, read the upstream code and cite the
  upstream file and line in the commit message. The reference repos
  (Reticulum, LXMF, RNode firmware) may already exist as sibling
  checkouts of this repo; if not, check them out from upstream.
- If you must diverge (hardware limits, memory, missing primitive),
  add or update an entry in `DIVERGENCES.md` in the same commit and
  mark the code comment with `DIVERGES:` and the reason. Keep the
  divergence minimal.
- A plausible rationale is not verification. The fork's worst defect
  (TCP `MODE_GATEWAY`, 28 days of LoRa starvation) shipped with a
  confident, wrong rationale written from upstream *docs* while
  upstream *code* said `MODE_FULL`.

## 2. Verification claims require evidence

- Write "verified on hardware" only with the actual output pasted into
  the commit message (counter values, test output, timings).
- Never claim verification through an endpoint, test, or tool that does
  not exist in the tree at the commit you are writing.
- Intended behaviour is described in the future tense ("the button will
  now..."), observed behaviour in the past tense with evidence. Do not
  dress the former as the latter.
- When diagnosing: read the existing counters first (`/api/info` radio
  stats, `/api/diag/*`), add the missing counter second, change code
  third. Every long debugging saga in this repo ended within hours of
  the right counter being read; several burned weeks before it.

## 3. Commit and branch discipline

- Work in your own git worktree on your own branch. Never commit to
  master directly; never push or merge without the user's verification.
- One commit per logical fix or feature. Every commit builds standalone
  (`pio run -e ttgo-t-beam-supreme -e ttgo-t-beam-supreme-lr1121`) and
  never references files committed later.
- Write the commit message from the staged diff, then check: does the
  diff contain anything the message does not say? Undisclosed payloads
  (silent endpoint renames, removed fallbacks, deleted patches) caused
  several of the fork's longest-lived regressions.
- When the user gives feedback on a commit, rewrite that commit
  (`git commit --fixup <hash>`, then
  `GIT_SEQUENCE_EDITOR=true git rebase -i --autosquash <hash>~1`)
  instead of stacking a correction commit. Local history rewrites
  before push are encouraged. The final history must represent the work
  delivered, not the feedback churn that produced it.
- Editing `src/Web/spa/*` requires regenerating `src/Web/SPAEmbedded.h`
  in the same commit (any `pio run` does it, or mirror the
  `embed_spa()` logic in `extra_script.py`).

## 4. Device constraints

- No flash, SD, or other blocking I/O on the main loop. Persistence is
  event-driven or runs off-loop; anything periodic must be measured
  against the loop-timing diag before it lands.
- Any new table, ring, queue, or persistence tier states its bound in
  the commit message. Unbounded growth on a 8 MB-flash device is a
  defect, not a TODO.
- Any change to an on-disk schema, path, or default capacity ships a
  migration in the same commit, or an explicit statement of accepted
  data loss.
- WebSocket and broadcast buffers are PSRAM-backed; never introduce
  internal-SRAM alloc churn on hot paths (the WiFi MAC needs that
  region for esf_buf descriptors).
- Prefer the WebSocket frame over HTTP polling for live data; polling
  taxes the SRAM-constrained device and perturbs measurements.

## 5. User-facing copy

- Non-technical and minimal. State the outcome and the numbers, not the
  mechanism ("Message too long. 1200/1024", never an explanation of
  UTF-8). Mechanism explanations live in code comments.
- No em dashes anywhere in user-facing copy. Short sentences. It is a
  "web app", not an "SPA", in user-facing text.
- Surface failures: any user-triggered action that can fail shows a
  toast or status with the server's message. A silent 404 hid a broken
  button for 19 days.

## 6. Code structure

- DRY is a first-class concern: extract duplicates into shared helpers
  or `Common/`; never ship parallel implementations of the same logic.
  Use a library already in `lib_deps` before writing your own.
- Repeat-use SPA icons are defined once as CSS-mask `::before` classes;
  only genuine one-offs stay inline.
- Put state at the layer that reads it; `Web/` consumes data, it does
  not produce it. Cross-cutting primitives go in `Common/`.
- No work-history in code comments: no task IDs, commit references, or
  "phase N" markers. Comments describe current behaviour and why.
- Python tooling installs into the repo's `.venv` via `uv`, never
  globally.

## 7. Testing

- The API regression suite lives in `tools/kiss-test/test_api/` and
  runs against a live device. Run it before presenting work that
  touches the web API.
- Keep all rig devices flashed to the same build when testing; version
  skew between the two radios produces misleading results.
- CI must stay green on every branch push: both Supreme env builds and
  `tools/check_api_parity.py` (SPA calls vs firmware routes).
