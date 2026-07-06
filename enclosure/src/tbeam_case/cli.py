"""Command line entry point.

  tbeam-case build [--out DIR]   export STEP and STL for every part
  tbeam-case fetch [--out DIR]   download LilyGo reference CAD files
"""

import argparse
import sys
import urllib.request
from pathlib import Path

REFERENCE_FILES = {
    # Official LilyGo mechanical files, Xinyuan-LilyGO/LilyGo-LoRa-Series
    "T-BEAM-SUPREME-V3.0.DXF": (
        "https://raw.githubusercontent.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/"
        "master/dimensions/T-BEAM-SUPREME-V3.0.DXF"
    ),
    "T-Beam-Supreme.zip": (
        "https://raw.githubusercontent.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/"
        "master/dimensions/T-Beam-Supreme.zip"
    ),
    "T-Beam-Supreme-Shell.7z": (
        "https://raw.githubusercontent.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/"
        "master/dimensions/T-Beam-Supreme-Shell.7z"
    ),
    "T-Beam-Supreme-Bracket.zip": (
        "https://raw.githubusercontent.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/"
        "master/dimensions/T-Beam-Supreme-Bracket.zip"
    ),
}


BOARD_PARTS = (
    "pcb", "oled", "pmma", "headers", "m2_core", "sd_slot", "usb_c",
    "buttons", "sma", "battery_holder",
)


def _export_stls(out_dir: Path) -> None:
    """Tessellate every board part and case part to its own STL."""
    import cadquery as cq

    from . import board, case

    out_dir.mkdir(parents=True, exist_ok=True)
    for part in BOARD_PARTS:
        path = out_dir / f"board_{part}.stl"
        cq.exporters.export(getattr(board, part)(), str(path))
        print(f"wrote {path}")
    for name, solid in case.parts().items():
        for ext in ("stl", "step"):
            path = out_dir / f"{name}.{ext}"
            cq.exporters.export(solid, str(path))
            print(f"wrote {path}")


def cmd_build(out_dir: Path) -> int:
    import cadquery as cq

    from . import board

    out_dir.mkdir(parents=True, exist_ok=True)

    asm = board.assembly()
    step_path = out_dir / "board_reference.step"
    asm.export(str(step_path))
    print(f"wrote {step_path}")

    keepout = board.keepout()
    stl_path = out_dir / "board_reference.stl"
    cq.exporters.export(keepout, str(stl_path))
    print(f"wrote {stl_path}")

    _export_stls(out_dir)
    return 0


REFERENCE_STEP = Path("reference/step/3d_file-1105.stp")


def _export_step_overlay(out_dir: Path) -> None:
    """Filter LilyGo's assembly STEP down to board level solids.

    Excluded: the mounting bracket (spans past both board ends), one
    block the import misplaces under the USB end, and the etched
    logo text (zero volume).
    """
    dest = out_dir / "lilygo_step_overlay.stl"
    if dest.exists() or not REFERENCE_STEP.exists():
        return
    import cadquery as cq

    raw = cq.importers.importStep(str(REFERENCE_STEP))
    keep = []
    for s in raw.solids().vals():
        b = s.BoundingBox()
        if s.Volume() < 1.0 or b.ylen > 101.0 or b.zmin < -9.0:
            continue
        keep.append(s)
    comp = cq.Compound.makeCompound(keep)
    cq.exporters.export(cq.Workplane(obj=comp), str(dest))
    print(f"wrote {dest}")


def cmd_render(out_dir: Path) -> int:
    from . import render

    stl_dir = out_dir
    _export_stls(stl_dir)
    _export_step_overlay(stl_dir)
    pngs = render.render_all(stl_dir, out_dir / "renders")
    for png in pngs:
        print(f"wrote {png}")
    return 0


def cmd_animate(out_dir: Path) -> int:
    from . import render

    _export_stls(out_dir)
    for path in render.animate(out_dir, out_dir / "renders" / "assembly.gif"):
        print(f"wrote {path}")
    return 0


# Print orientation per part: sign of the global Z axis that points
# UP on the printer (front prints face down, so global -Z is up).
PRINT_UP = {"front_shell": -1.0, "back_shell": 1.0, "battery_hatch": 1.0}

# Accepted micro overhang area (mm^2): hook window bridges in the
# front, hook tab noses in the back, hatch fixed tab arms. All under
# 1.5 mm wide and bridgeable; a slicer will not generate supports.
OVERHANG_BUDGET = {"front_shell": 20.0, "back_shell": 15.0, "battery_hatch": 20.0}


def _overhang_area(solid, up_sign: float) -> float:
    """Total downward facing facet area steeper than 45 degrees,
    excluding the bed plane."""
    import math

    verts, tris = solid.val().tessellate(0.2)
    pts = [(v.x, v.y, v.z) for v in verts]
    z_bed = min(pt[2] * up_sign for pt in pts)
    area = 0.0
    for a, b, c in tris:
        pa, pb, pc = pts[a], pts[b], pts[c]
        ux = [pb[i] - pa[i] for i in range(3)]
        vx = [pc[i] - pa[i] for i in range(3)]
        n = (
            ux[1] * vx[2] - ux[2] * vx[1],
            ux[2] * vx[0] - ux[0] * vx[2],
            ux[0] * vx[1] - ux[1] * vx[0],
        )
        mag = math.sqrt(n[0] ** 2 + n[1] ** 2 + n[2] ** 2)
        if mag < 1e-9:
            continue
        nz = n[2] * up_sign / mag
        # facing down more steeply than 45 degrees (with tolerance)
        if nz < -0.723:
            zc = (pa[2] + pb[2] + pc[2]) / 3 * up_sign
            if zc > z_bed + 0.3:
                area += mag / 2
    return area


def cmd_check(out_dir: Path) -> int:
    """Boolean interference checks plus a print overhang audit."""
    from . import board, case

    keepout = board.keepout()
    parts = case.parts()
    failures = 0
    pairs = [
        ("front_shell", "board", parts["front_shell"], keepout),
        ("back_shell", "board", parts["back_shell"], keepout),
        ("battery_hatch", "board", parts["battery_hatch"], keepout),
        ("front_shell", "back_shell", parts["front_shell"], parts["back_shell"]),
        ("battery_hatch", "back_shell", parts["battery_hatch"], parts["back_shell"]),
    ]
    for a_name, b_name, a, b in pairs:
        common = a.intersect(b)
        vol = sum(s.Volume() for s in common.solids().vals()) if common.solids().vals() else 0.0
        status = "OK" if vol < 0.05 else "INTERFERENCE"
        if vol >= 0.05:
            failures += 1
        print(f"{status}: {a_name} vs {b_name}: {vol:.3f} mm^3")

    for name, solid in parts.items():
        area = _overhang_area(solid, PRINT_UP[name])
        budget = OVERHANG_BUDGET[name]
        status = "OK" if area <= budget else "OVERHANG"
        if area > budget:
            failures += 1
        print(f"{status}: {name} unsupported >45deg area: {area:.1f} mm^2"
              f" (budget {budget:.0f})")
    return 1 if failures else 0


def cmd_fetch(out_dir: Path) -> int:
    out_dir.mkdir(parents=True, exist_ok=True)
    failed = 0
    for name, url in REFERENCE_FILES.items():
        dest = out_dir / name
        if dest.exists():
            print(f"exists  {dest}")
            continue
        try:
            urllib.request.urlretrieve(url, dest)
            print(f"fetched {dest}")
        except OSError as exc:
            print(f"FAILED  {name}: {exc}", file=sys.stderr)
            failed += 1
    if failed == 0:
        print("Note: the .zip and .7z archives need manual extraction.")
    return 1 if failed else 0


def main(argv: list[str] | None = None) -> int:
    # Output defaults are relative to the working directory so the
    # command behaves the same under uv run and uvx.
    parser = argparse.ArgumentParser(prog="tbeam-case")
    sub = parser.add_subparsers(dest="command")

    build = sub.add_parser("build", help="export STEP and STL for every part")
    build.add_argument("--out", type=Path, default=Path("build"))

    render = sub.add_parser("render", help="render PNG views of the assembly")
    render.add_argument("--out", type=Path, default=Path("build"))

    animate = sub.add_parser("animate", help="rotating exploding assembly GIF")
    animate.add_argument("--out", type=Path, default=Path("build"))

    check = sub.add_parser("check", help="boolean interference checks")
    check.add_argument("--out", type=Path, default=Path("build"))

    fetch = sub.add_parser("fetch", help="download LilyGo reference CAD files")
    fetch.add_argument("--out", type=Path, default=Path("reference"))

    args = parser.parse_args(argv)
    if args.command == "fetch":
        return cmd_fetch(args.out)
    if args.command == "render":
        return cmd_render(args.out)
    if args.command == "animate":
        return cmd_animate(args.out)
    if args.command == "check":
        return cmd_check(args.out)
    if args.command == "build":
        return cmd_build(args.out)
    # No subcommand behaves as build with defaults
    return cmd_build(Path("build"))


if __name__ == "__main__":
    raise SystemExit(main())
