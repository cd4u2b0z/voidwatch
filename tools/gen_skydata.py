#!/usr/bin/env python3
"""Generator for src/skydata.c.

Reads from tools/data/:
  hyg_v36_1.csv          HYG v3.6.1 (CSV, decompressed). HIP, RA, Dec,
                         Mag, spectral class, IAU proper name, Bayer.
  stellarium_modern.json Stellarium "modern" skyculture index.json:
                         constellations + their polylines as HIP IDs.

Emits:
  src/skydata.c          Baked-in star + constellation tables.

Run from repo root:
  python3 tools/gen_skydata.py

The generator filters HYG to mag <= MAG_CUTOFF (default 6.5 — naked-eye
limit, matches the Yale Bright Star Catalog footprint). Constellation
line endpoints whose HIP isn't in the filtered set are dropped.

Sources:
  - HYG Database (CC BY-SA 2.5, https://www.astronexus.com/hyg)
  - Stellarium "modern" skyculture (GPL-2.0)
  - Underlying: Hipparcos / Yale BSC5 / IAU named-star list

The generated src/skydata.c is committed; end users build voidwatch
without Python or network access. This script only re-runs when the
catalogs are updated.
"""
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "tools" / "data"
OUT  = ROOT / "src" / "skydata.c"

HYG_PATH = DATA / "hyg_v36_1.csv"
STC_PATH = DATA / "stellarium_modern.json"

MAG_CUTOFF = 6.5

def fail(msg):
    sys.stderr.write(f"gen_skydata: {msg}\n")
    sys.exit(2)


def parse_hyg(path):
    """Yield filtered HYG rows. We keep mag <= MAG_CUTOFF entries that
    have a valid HIP and a parseable RA/Dec/mag. The Sun is excluded
    (HYG includes it as id=0 but it's already in the ephemeris)."""
    if not path.exists():
        fail(f"missing {path} — see the docstring for the URL")
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                mag = float(row.get("mag") or "99")
            except ValueError:
                continue
            if mag > MAG_CUTOFF:
                continue
            hip_raw = row.get("hip") or ""
            if not hip_raw.strip():
                continue
            try:
                hip = int(hip_raw)
            except ValueError:
                continue
            try:
                ra_h    = float(row.get("ra")  or "0")  # already hours
                dec_deg = float(row.get("dec") or "0")
            except ValueError:
                continue

            spect = (row.get("spect") or "").strip()
            cls = "?"
            for c in spect:
                if c in "OBAFGKM":
                    cls = c
                    break

            proper = (row.get("proper") or "").strip()
            bayer  = (row.get("bayer")  or "").strip()
            # IAU proper name wins; fall back to Bayer; else unnamed.
            name = proper or bayer or ""

            yield {
                "hip":      hip,
                "ra_h":     ra_h,
                "dec_deg":  dec_deg,
                "mag":      mag,
                "spectral": cls,
                "name":     name,
            }


def parse_stellarium(path):
    """Yield (constellation_id, [hip, ...]) polylines."""
    if not path.exists():
        fail(f"missing {path} — see the docstring for the URL")
    data = json.loads(path.read_text())
    for con in data.get("constellations", []):
        cid = con.get("id", "?")
        for polyline in con.get("lines", []):
            if isinstance(polyline, list) and len(polyline) >= 2:
                yield cid, polyline


def c_string_escape(s):
    """Minimal C string escape — names contain only ASCII Latin letters,
    digits, spaces, hyphens, and apostrophes in the IAU list. Be safe
    with backslash + quote anyway."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main():
    print(f"reading {HYG_PATH.name}", file=sys.stderr)
    stars = list(parse_hyg(HYG_PATH))
    print(f"  {len(stars)} stars at mag <= {MAG_CUTOFF}", file=sys.stderr)

    # Stable index per HIP. Keep the first occurrence; HYG sometimes has
    # multiple rows per HIP (binary components).
    hip_to_idx = {}
    deduped = []
    for s in stars:
        if s["hip"] in hip_to_idx:
            continue
        hip_to_idx[s["hip"]] = len(deduped)
        deduped.append(s)
    stars = deduped

    print(f"reading {STC_PATH.name}", file=sys.stderr)
    line_segs = []
    dropped_endpoints = 0
    for cid, polyline in parse_stellarium(STC_PATH):
        for i in range(len(polyline) - 1):
            a_hip, b_hip = polyline[i], polyline[i + 1]
            ai = hip_to_idx.get(a_hip)
            bi = hip_to_idx.get(b_hip)
            if ai is None or bi is None:
                dropped_endpoints += 1
                continue
            line_segs.append((ai, bi))
    print(f"  {len(line_segs)} line segments, "
          f"{dropped_endpoints} dropped (endpoint dimmer than mag {MAG_CUTOFF})",
          file=sys.stderr)

    print(f"writing {OUT}", file=sys.stderr)
    with OUT.open("w", encoding="utf-8") as f:
        f.write("#include <stddef.h>\n")
        f.write('\n')
        f.write('#include "skydata.h"\n')
        f.write("\n")
        f.write("/* AUTO-GENERATED by tools/gen_skydata.py — do not edit by hand. *\n")
        f.write(" *\n")
        f.write(" * Sources:\n")
        f.write(" *   HYG Database v3.6.1 (CC BY-SA 2.5)\n")
        f.write(" *     https://github.com/astronexus/HYG-Database\n")
        f.write(" *   Stellarium 'modern' skyculture (GPL-2.0)\n")
        f.write(" *     https://github.com/Stellarium/stellarium\n")
        f.write(" *\n")
        f.write(f" * Filter: V magnitude <= {MAG_CUTOFF} (naked-eye limit; "
                "matches BSC5 footprint).\n")
        f.write(" * See CITATIONS.md for full credits.\n")
        f.write(" */\n")
        f.write("\n")
        f.write("const SkyStar sky_stars[] = {\n")
        for s in stars:
            name = f'"{c_string_escape(s["name"])}"' if s["name"] else "NULL"
            f.write(f'    {{ {name}, '
                    f'{s["ra_h"]:.6f}, '
                    f'{s["dec_deg"]:+.4f}, '
                    f'{s["mag"]:+.2f}f, '
                    f'\'{s["spectral"]}\' }},\n')
        f.write("};\n")
        f.write("const int sky_stars_count =\n")
        f.write("    (int)(sizeof sky_stars / sizeof sky_stars[0]);\n")
        f.write("\n")
        f.write("const SkyLine sky_lines[] = {\n")
        for a, b in line_segs:
            f.write(f"    {{ {a}, {b} }},\n")
        f.write("};\n")
        f.write("const int sky_lines_count =\n")
        f.write("    (int)(sizeof sky_lines / sizeof sky_lines[0]);\n")

    # Sanity: SkyLine.a/b are short — must fit in 32767.
    if len(stars) > 32767:
        fail(f"star count {len(stars)} exceeds short range; "
             "widen SkyLine fields in skydata.h")

    print(f"done: {len(stars)} stars, {len(line_segs)} segments", file=sys.stderr)


if __name__ == "__main__":
    main()
