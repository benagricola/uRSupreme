# Web app source

The web app the device serves is built from these source files, gzipped
into `../SPAEmbedded.h` by `embed_spa()` in `extra_script.py` on every
`pio run`. The sources are the source of truth; `SPAEmbedded.h` is
generated. Do not edit `SPAEmbedded.h` by hand.

| Source | Role |
|---|---|
| `index.html` | shell: `<head>`, one `<!--SPA:name-->` placeholder per markup fragment, and the inline `<script>__APP_JS__</script>` |
| `app/*.js` | the app logic, split by feature |
| `html/*.html` | the page markup, split by feature |
| `styles.css` | styles (served separately, cache-busted by content hash) |
| `alpine.min.js`, `leaflet*`, `protomaps-leaflet.js` | vendored libraries (served separately) |

At build time `embed_spa()` concatenates `app/*.js` in filename order
into the inline `<script>`, splices each `html/*.html` into the shell at
its placeholder, then substitutes the `API` table and the styles version.
The served document is one classic page identical to a single combined
file; the split exists only to keep editing sane.

## Editing existing code

Just edit the relevant `app/*.js` or `html/*.html` file. Any `pio run`
regenerates `SPAEmbedded.h` (the files' mtimes are tracked).

## Adding a JavaScript module

Drop a new `app/NN-name.js`. It is included automatically, concatenated
in filename order, so the numeric prefix sets its position. No other edit
is needed.

- All modules share one script scope (a global `state` object plus free
  functions and classes that the markup binds to). Define functions and
  classes; do not put order-dependent top-level statements in a new
  module.
- `app/00-core.js` must stay first (it holds the `API` table and the
  Alpine stores) and `app/99-misc.js` must stay last (it calls `boot()`).
  Name new modules in the `10`-`90` range.

## Adding a markup fragment

Two steps, because markup goes at a specific place in the page:

1. Create `html/NN-name.html` with the markup.
2. Add `<!--SPA:NN-name-->` where it should render: in `index.html`, or
   inside another fragment to nest it.

The build fails if a placeholder has no matching file, or a fragment file
has no placeholder, so a forgotten step is caught, not silently dropped.

## Rules the build enforces

- Never write a literal `/api/...` path. Use the generated `API` table
  (`API.NAME`, from `../api_routes.def`). `tools/check_api_parity.py`
  scans `index.html`, `app/*.js`, and `html/*.html` and fails the build
  on a bypassing literal or an unknown `API.NAME`.
- No em dashes anywhere (CI-enforced tree-wide). The vendored libraries
  are excluded from that check.
