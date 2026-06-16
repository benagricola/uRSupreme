#!/usr/bin/env python3
"""Generate offline map tiles for the device SD card.

The web app's location map reads tiles from the SD card in the standard
slippy layout <maps_dir>/{z}/{x}/{y}.png (default /maps, set in the Map
settings). This tool fills that layout two ways:

  unpack  Explode a .mbtiles file into the layout. This is the clean path
          for fair use: obtain an .mbtiles for your area from a source
          whose terms permit offline use, then unpack it onto the card.

  fetch   Download tiles for a region from a tile server you are allowed
          to bulk-download from: your own server, or a provider whose
          terms permit it. There is NO default source, on purpose: bulk
          downloading the public OpenStreetMap tile server is against its
          tile usage policy (https://operations.osmfoundation.org/policies/tiles/).

The whole world fits in very few tiles at low zoom (z0..6 is ~5500 tiles);
detail over a small area is cheap, detail over a large area is not, so
`fetch` prints the tile count and a size estimate first and refuses an
oversized job unless you pass --yes.

Pure standard library (urllib + sqlite3); no install needed.

Examples:
  # a low-zoom world base from your own tile server
  tools/maptiles.py fetch --source 'http://my-tiles/{z}/{x}/{y}.png' \\
      --bbox -180,-85,180,85 --min-zoom 0 --max-zoom 6 --out /media/sd/maps

  # 20 km around a coordinate, up to street level
  tools/maptiles.py fetch --source 'http://my-tiles/{z}/{x}/{y}.png' \\
      --center 51.5074,-0.1278 --radius-km 20 --min-zoom 0 --max-zoom 16 \\
      --out /media/sd/maps

  # explode an mbtiles you already have
  tools/maptiles.py unpack --mbtiles area.mbtiles --out /media/sd/maps
"""
import argparse
import math
import os
import sqlite3
import sys
import time
import urllib.request

DEFAULT_UA = "microReticulum-maptiles/1.0 (offline tile cache for a personal device)"


def deg2num(lat, lon, z):
    """Slippy/XYZ tile index for a lat/lon at zoom z."""
    n = 2 ** z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


def bbox_from_radius(lat, lon, km):
    """A W,S,E,N box that covers `km` around a point (rough, good enough)."""
    dlat = km / 111.32
    dlon = km / (111.32 * max(0.01, math.cos(math.radians(lat))))
    return (lon - dlon, lat - dlat, lon + dlon, lat + dlat)


def tile_range(bbox, z):
    """Inclusive x/y tile range covering bbox (W,S,E,N) at zoom z."""
    w, s, e, n = bbox
    x0, y0 = deg2num(n, w, z)   # NW corner -> top-left tile
    x1, y1 = deg2num(s, e, z)   # SE corner -> bottom-right tile
    return min(x0, x1), max(x0, x1), min(y0, y1), max(y0, y1)


def count_tiles(bbox, zmin, zmax):
    per = {}
    for z in range(zmin, zmax + 1):
        xa, xb, ya, yb = tile_range(bbox, z)
        per[z] = (xb - xa + 1) * (yb - ya + 1)
    return sum(per.values()), per


def fetch_one(url, path, ua, retries=3):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": ua})
            with urllib.request.urlopen(req, timeout=20) as r:
                data = r.read()
            if not data:
                raise IOError("empty response")
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
            return True
        except Exception as e:
            if attempt == retries - 1:
                sys.stderr.write(f"  fail {url}: {e}\n")
                return False
            time.sleep(1.5 * (attempt + 1))
    return False


def do_fetch(a):
    if a.center:
        lat, lon = (float(v) for v in a.center.split(","))
        bbox = bbox_from_radius(lat, lon, a.radius_km)
    elif a.bbox:
        bbox = tuple(float(v) for v in a.bbox.split(","))
    else:
        sys.exit("fetch needs --center LAT,LON --radius-km KM, or --bbox W,S,E,N")
    if not a.source or "{z}" not in a.source:
        sys.exit("fetch needs --source with a {z}/{x}/{y} URL template")

    total, per = count_tiles(bbox, a.min_zoom, a.max_zoom)
    print(f"region W,S,E,N = {tuple(round(v, 4) for v in bbox)}")
    for z in sorted(per):
        print(f"  z{z:<2} {per[z]} tiles")
    print(f"total {total} tiles, about {total * a.avg_kb / 1024.0:.1f} MB "
          f"at {a.avg_kb} KB/tile")
    if "openstreetmap.org" in a.source:
        print("WARNING: the public OpenStreetMap tile server forbids bulk "
              "downloads. Use your own server or a provider that permits it.")
    if total > a.max_tiles and not a.yes:
        sys.exit(f"refusing {total} tiles (over --max-tiles {a.max_tiles}); "
                 f"narrow the area/zoom or pass --yes")
    if a.dry_run:
        return

    delay = 1.0 / a.rate if a.rate > 0 else 0.0
    done = skipped = failed = 0
    for z in range(a.min_zoom, a.max_zoom + 1):
        xa, xb, ya, yb = tile_range(bbox, z)
        for x in range(xa, xb + 1):
            for y in range(ya, yb + 1):
                path = os.path.join(a.out, str(z), str(x), f"{y}.png")
                if os.path.exists(path) and os.path.getsize(path) > 0:
                    skipped += 1
                    continue
                url = (a.source.replace("{z}", str(z))
                               .replace("{x}", str(x))
                               .replace("{y}", str(y)))
                if fetch_one(url, path, a.user_agent):
                    done += 1
                else:
                    failed += 1
                if delay:
                    time.sleep(delay)
                if (done + failed) % 100 == 0 and (done + failed):
                    print(f"  {done} got, {skipped} skipped, {failed} failed",
                          flush=True)
    print(f"done: {done} downloaded, {skipped} already present, {failed} failed "
          f"-> {a.out}")


def do_unpack(a):
    if not os.path.exists(a.mbtiles):
        sys.exit(f"no such file: {a.mbtiles}")
    db = sqlite3.connect(a.mbtiles)
    cur = db.cursor()
    try:
        total = cur.execute("SELECT count(*) FROM tiles").fetchone()[0]
    except sqlite3.Error as e:
        sys.exit(f"not a valid mbtiles file ({e})")
    print(f"{total} tiles in {a.mbtiles}")
    n = warned = 0
    for z, col, row, data in cur.execute(
            "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles"):
        # mbtiles rows are TMS (origin bottom-left); slippy/XYZ is top-left.
        y = (2 ** z - 1) - row
        if not warned and data[:3] == b"\xff\xd8\xff":
            print("WARNING: tiles look like JPEG; the device serves them as "
                  "image/png, which some browsers reject. A PNG mbtiles is safest.")
            warned = 1
        path = os.path.join(a.out, str(z), str(col), f"{y}.png")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        n += 1
        if n % 1000 == 0:
            print(f"  {n}/{total}", flush=True)
    db.close()
    print(f"unpacked {n} tiles -> {a.out}")


def main():
    p = argparse.ArgumentParser(
        description="Generate offline map tiles for the device SD card.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    f = sub.add_parser("fetch", help="download a region from a tile server")
    f.add_argument("--source", required=True,
                   help="tile URL template with {z}/{x}/{y} (no default; not OSM public)")
    f.add_argument("--out", required=True, help="output maps dir (the SD /maps)")
    f.add_argument("--center", help="LAT,LON for a radius job")
    f.add_argument("--radius-km", type=float, default=10.0)
    f.add_argument("--bbox", help="W,S,E,N for a box job")
    f.add_argument("--min-zoom", type=int, default=0)
    f.add_argument("--max-zoom", type=int, default=14)
    f.add_argument("--rate", type=float, default=2.0,
                   help="max tiles/second (default 2; be polite)")
    f.add_argument("--avg-kb", type=float, default=18.0, help="size estimate per tile")
    f.add_argument("--max-tiles", type=int, default=50000,
                   help="refuse jobs bigger than this without --yes")
    f.add_argument("--user-agent", default=DEFAULT_UA)
    f.add_argument("--dry-run", action="store_true", help="count + estimate only")
    f.add_argument("--yes", action="store_true", help="proceed past --max-tiles")
    f.set_defaults(func=do_fetch)

    u = sub.add_parser("unpack", help="explode an .mbtiles into the layout")
    u.add_argument("--mbtiles", required=True)
    u.add_argument("--out", required=True, help="output maps dir (the SD /maps)")
    u.set_defaults(func=do_unpack)

    a = p.parse_args()
    a.func(a)


if __name__ == "__main__":
    main()
