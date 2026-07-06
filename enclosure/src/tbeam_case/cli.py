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


def cmd_check(out_dir: Path) -> int:
    """Boolean interference checks between shells and the board."""
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

    check = sub.add_parser("check", help="boolean interference checks")
    check.add_argument("--out", type=Path, default=Path("build"))

    fetch = sub.add_parser("fetch", help="download LilyGo reference CAD files")
    fetch.add_argument("--out", type=Path, default=Path("reference"))

    args = parser.parse_args(argv)
    if args.command == "fetch":
        return cmd_fetch(args.out)
    if args.command == "render":
        return cmd_render(args.out)
    if args.command == "check":
        return cmd_check(args.out)
    if args.command == "build":
        return cmd_build(args.out)
    # No subcommand behaves as build with defaults
    return cmd_build(Path("build"))


if __name__ == "__main__":
    raise SystemExit(main())
