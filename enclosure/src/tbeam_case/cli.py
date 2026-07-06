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

    return 0


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

    fetch = sub.add_parser("fetch", help="download LilyGo reference CAD files")
    fetch.add_argument("--out", type=Path, default=Path("reference"))

    args = parser.parse_args(argv)
    if args.command == "fetch":
        return cmd_fetch(args.out)
    if args.command == "build":
        return cmd_build(args.out)
    # No subcommand behaves as build with defaults
    return cmd_build(Path("build"))


if __name__ == "__main__":
    raise SystemExit(main())
