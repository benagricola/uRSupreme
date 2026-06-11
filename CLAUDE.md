# Engineering rules for this repository

These rules are binding for every agent and every session working on this
repo. They exist because a full audit of the fork's first 377 commits
(2026-06) traced its most expensive defects to a small set of recurring
process failures. Each rule names the failure it prevents.

A subset is enforced by CI (see Enforcement at the end). The rest are
review-time: they hold only if you apply them, so do.

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
  divergence minimal. CI enforces the same-commit pairing: a `src/`
  diff that touches a `DIVERGES:` marker without touching
  `DIVERGENCES.md` fails the build (`tools/check_diverges_ledger.py`).
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
- When the user gives feedback on a commit, fold it into that commit
  rather than stacking a correction commit: amend if it is the tip,
  otherwise a fixup squashed before push. The rule is the outcome, a
  history with no feedback-churn commits, not one specific command
  (interactive rebase is unavailable in some agent harnesses, so do not
  hard-depend on it). Local history rewrites before push are
  encouraged. The final history must represent the work delivered, not
  the feedback churn that produced it.
- Do not add a `Co-Authored-By` line or any AI-authorship trailer to
  commits or pull requests.
- Editing `src/Web/spa/*` requires regenerating `src/Web/SPAEmbedded.h`
  in the same commit (any `pio run` does it, or mirror the
  `embed_spa()` logic in `extra_script.py`).

## 4. Device constraints

- No long or unbounded blocking I/O on the main loop. Persistence and
  any multi-tick op (a FAT scan, a ring drain, a multi-sector write)
  run off-loop on a dedicated task. A brief, bounded op (a single stat)
  may run on-loop only after it is measured against the loop-timing
  diag; if it can stall for more than a tick, it does not belong there.
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
- No em dashes in any authored file: copy, comments, and docs alike.
  Mirrored and vendored trees (`Console/`, `MIRROR.md`, generated and
  vendored files) are excluded; do not rewrite imported upstream
  content to satisfy a house style rule. CI enforces this
  (`tools/check_copy_and_config.py`). Short sentences. It is a "web
  app", not an "SPA", in user-facing text.
- Surface failures: any user-triggered action that can fail shows a
  toast or status with the server's message. A silent 404 hid a broken
  button for 19 days.

## 6. Code structure

- DRY is a first-class concern: extract duplicates into shared helpers
  or `Common/`; never ship parallel implementations of the same logic.
  Use a library already in `lib_deps` before writing your own. The one
  exception is upstream parity (rule 1): where a faithful port must
  mirror an upstream structure, keep the upstream shape even if it
  reads as duplication. Parity wins over DRY.
- Repeat-use SPA icons are defined once as CSS-mask `::before` classes;
  only genuine one-offs stay inline.
- Put state at the layer that reads it; `Web/` consumes data, it does
  not produce it. Cross-cutting primitives go in `Common/`.
- New web API routes are declared in `src/Web/api_routes.def` and
  referenced via `ApiRoutes::` constants (C++) and the generated `API`
  table (SPA). Never write an `/api` path literal; the parity check
  fails the build on one.
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
- CI must stay green on every branch push: the builds and the guardrail
  checks (see Enforcement). A red guardrail is a stop, not a warning.

## 8. Enforcement

Which rules are mechanical and which are not, so nobody mistakes
aspiration for a guarantee.

- Enforced by CI on every push (the `guardrails` job plus the builds):
  both Supreme env builds; API route parity
  (`tools/check_api_parity.py`); no absolute paths in `platformio.ini`
  and the em-dash ban over authored paths
  (`tools/check_copy_and_config.py`); and the
  `DIVERGES:`-moves-with-`DIVERGENCES.md` gate
  (`tools/check_diverges_ledger.py`).
- Review-time only, so they hold only if you apply them: the upstream
  file:line citation (rule 1), hardware evidence for verification
  claims (rule 2), commit-message-matches-diff (rule 3), bounding every
  ring/queue and keeping blocking I/O off the loop (rule 4), DRY and
  state placement (rule 6), and running the API regression suite before
  presenting web changes (rule 7). These are the expensive,
  silent-failure rules: the cost of skipping one is paid weeks later,
  not at the next push.
