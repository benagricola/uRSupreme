# T-Beam Supreme enclosure

Parametric 3D printable enclosure for the LilyGo T-Beam Supreme,
modelled in Python with [CadQuery](https://cadquery.readthedocs.io/).

Handheld, portrait orientation: LoRa antenna up, screen facing the
user, the three buttons (RST, PWR, BOOT) under the fingers on the
lower left edge, USB-C on the right side of the bottom edge. The 18650
battery sits under a grip hump on the back with a lift-out hatch, so
cells swap without tools.

## Parts and closure

- **front_shell**: screen window in a recessed valley between two
  side rails (the rails clear the GPIO header rows), compliant
  cantilever button flexures in the left wall, USB-C and microSD
  openings, SMA hole, board pedestals and M3 bosses.
- **back_shell**: shallow pan plus the battery grip hump, board clamp
  posts, hook tabs, M3 counterbores, hatch opening with ledge.
- **battery_hatch**: lift-out lid, two fixed tabs at the antenna end,
  snap bumps at the sides, thumb scoop to pry it out.

Closure: hook the back shell tabs into the front shell windows at the
USB end, close, then two M3x10 SHCS at the antenna end into thread
forming pilot holes. Only fastener size used: M3x10.

Assembly: board goes into the front shell antenna first (the board
mounted SMA barrel passes through its wall hole), then the USB end
swings down onto the locating pins. No screws touch the board.

## Layout

- `src/tbeam_case/params.py` board dimensions, with provenance notes
  (official LilyGo DXF / STEP / schematic vs estimated)
- `src/tbeam_case/board.py` reference keep-out model of the assembled
  board that the case parts are designed around
- `src/tbeam_case/case.py` the three printable parts
- `src/tbeam_case/render.py` offscreen VTK renders
- `build/` exported STEP / STL / PNG output (not committed)
- `reference/` downloaded LilyGo CAD files (not committed)

## Usage

Everything runs through [uv](https://docs.astral.sh/uv/); the venv is
created automatically in `enclosure/.venv` on first run.

```sh
cd enclosure
uv run tbeam-case build          # export STEP + STL into build/
uv run tbeam-case check          # boolean interference checks
uv run tbeam-case render         # PNG views into build/renders/
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

Rendering needs headless OpenGL: `apt-get install libosmesa6`.

## Reference sources

Official mechanical files are published in
[Xinyuan-LilyGO/LilyGo-LoRa-Series](https://github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series)
under `dimensions/` (board DXF, full assembly STEP, LilyGo's own shell
as STLs) and `schematic/T-Beam-S3-Supreme/`. `tbeam-case fetch` pulls
them into `reference/`; extract `T-Beam-Supreme.zip` to
`reference/step/` to enable the STEP overlay renders
(`board_vs_step*.png`), which draw LilyGo's board model inside the
case for visual verification.

## Verify on hardware before a tight fit

Tagged ESTIMATED in `params.py`: the OLED active area position (the
window may need to shift), the GPIO header socket height, the corner
screw stack height the front stubs land on, the 18650 holder
envelope, and the M.2 stack height. The SMA barrel position is
confirmed from LilyGo's STEP (x 6.7, z 2.5 above PCB top).
