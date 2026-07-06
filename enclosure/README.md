# T-Beam Supreme enclosure

Parametric 3D printable enclosure for the LilyGo T-Beam Supreme,
modelled in Python with [CadQuery](https://cadquery.readthedocs.io/).

Handheld, portrait orientation: LoRa antenna up, screen facing the
user, the three buttons (RST, PWR, BOOT) under the fingers on the
lower left edge, USB-C on the right side of the bottom edge. The 18650
battery sits under a contoured grip hump on the back with a snap
closed hatch, so cells swap without tools.

## Layout

- `src/tbeam_case/params.py` all dimensions, with provenance notes
  (official LilyGo DXF / STEP / schematic vs estimated)
- `src/tbeam_case/board.py` reference keep-out model of the assembled
  board that the case parts are designed around
- `src/tbeam_case/cli.py` build and fetch commands
- `build/` exported STEP / STL output (not committed)
- `reference/` downloaded LilyGo CAD files (not committed)

## Usage

Everything runs through [uv](https://docs.astral.sh/uv/); the venv is
created automatically in `enclosure/.venv` on first run.

```sh
cd enclosure
uv run tbeam-case build          # export STEP + STL into build/
uv run tbeam-case fetch          # download LilyGo reference CAD
```

Or without a checkout-local venv, via uvx:

```sh
uvx --from ./enclosure tbeam-case build
```

uvx caches the built wheel; after editing the source, add
`--reinstall` (or just use `uv run`, which always sees the working
tree). Output goes to `build/` and `reference/` under the current
working directory for both invocations.

## Reference sources

Official mechanical files are published in
[Xinyuan-LilyGO/LilyGo-LoRa-Series](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series)
under `dimensions/` (board DXF, full assembly STEP, LilyGo's own shell
as STLs) and `schematic/T-Beam-S3-Supreme/`. `tbeam-case fetch` pulls
them into `reference/`.
