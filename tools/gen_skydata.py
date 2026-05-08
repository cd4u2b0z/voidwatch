#!/usr/bin/env python3
"""Generator for src/skydata.c.

Reads from tools/data/:
  hyg_v36_1.csv             HYG v3.6.1 (CSV, decompressed). HIP, RA, Dec,
                            Mag, spectral class, IAU proper name, Bayer.
  d3celestial_lines.json    d3-celestial constellation-line GeoJSON
                            (Olaf Frohn, BSD-3-Clause). MultiLineString
                            features with [ra_deg, dec_deg] coordinates,
                            one feature per IAU constellation.

Emits:
  src/skydata.c             Baked-in star + constellation tables.

Run from repo root:
  python3 tools/gen_skydata.py

The generator filters HYG to mag <= MAG_CUTOFF (default 6.5 — naked-eye
limit, matches the Yale Bright Star Catalog footprint). Each
constellation-line endpoint is matched to the nearest star in the
filtered table by angular distance; if no match within MATCH_TOL_DEG
exists, the segment is dropped.

Sources:
  - HYG Database (CC BY-SA 2.5, https://www.astronexus.com/hyg)
  - d3-celestial constellation lines (BSD-3-Clause, © 2015 Olaf Frohn,
    https://github.com/ofrohn/d3-celestial)
  - Underlying: Hipparcos / Yale BSC5 / IAU named-star list

Why d3-celestial: Stellarium's `modern_iau` skyculture is the obvious
reference set but it ships GPL-2.0, which would contaminate voidwatch's
binary. d3-celestial encodes the same Western IAU 88-figure conventions
under BSD-3-Clause. We swap sources, keep the same constellation
geometry, and the voidwatch binary stays free of GPL.

The generated src/skydata.c is committed; end users build voidwatch
without Python or network access. This script only re-runs when the
catalogs are updated.

To refresh upstream caches:
    curl -L -o tools/data/hyg_v36_1.csv.gz \\
        https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/v3/hyg_v36_1.csv.gz
    gunzip -kf tools/data/hyg_v36_1.csv.gz
    curl -L -o tools/data/d3celestial_lines.json \\
        https://raw.githubusercontent.com/ofrohn/d3-celestial/master/data/constellations.lines.json
"""
import csv
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "tools" / "data"
OUT  = ROOT / "src" / "skydata.c"

HYG_PATH   = DATA / "hyg_v36_1.csv"
LINES_PATH = DATA / "d3celestial_lines.json"

MAG_CUTOFF     = 6.5    # naked-eye limit; matches BSC5 footprint
MATCH_TOL_DEG  = 1.0    # endpoint → nearest star tolerance, degrees

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


def parse_d3celestial(path):
    """Yield (constellation_id, [(ra_deg, dec_deg), ...]) polylines.
    Each MultiLineString in the GeoJSON feature collection contains
    one or more line segments expressed as a list of [ra, dec]
    coordinate pairs (degrees, J2000).

    GeoJSON convention: longitudes wrap at ±180°, so d3-celestial
    encodes RA values in [180, 360) as negative numbers
    (e.g. RA 354.5° → -5.5° in the JSON). HYG stores RA in hours
    [0, 24) which we already convert to [0, 360°). Normalise the
    d3-celestial values to [0, 360°) here so the nearest-star match
    operates in a single coordinate system."""
    if not path.exists():
        fail(f"missing {path} — see the docstring for the URL")
    data = json.loads(path.read_text(encoding="utf-8"))
    for feat in data.get("features", []):
        cid = feat.get("id") or feat.get("properties", {}).get("name") or "?"
        geom = feat.get("geometry") or {}
        if geom.get("type") != "MultiLineString":
            continue
        for polyline in geom.get("coordinates", []):
            if not isinstance(polyline, list) or len(polyline) < 2:
                continue
            normed = []
            for p in polyline:
                if len(p) < 2: continue
                ra_deg, dec_deg = p[0], p[1]
                if ra_deg < 0.0: ra_deg += 360.0
                normed.append((ra_deg, dec_deg))
            yield cid, normed


def angular_dist_sq_deg(ra1_deg, dec1_deg, ra2_deg, dec2_deg):
    """Approximate angular distance² in deg², using flat-sky cos(dec)
    correction on RA. Wraps RA across the 0/360° seam (a star at
    RA 359° is 2° from a star at RA 1°, not 358°). Good to ~0.01°
    for nearby points and avoids haversine cost across 700×8870
    endpoint comparisons."""
    dra = ra1_deg - ra2_deg
    if   dra >  180.0: dra -= 360.0
    elif dra < -180.0: dra += 360.0
    cos_dec = math.cos(math.radians(0.5 * (dec1_deg + dec2_deg)))
    dra *= cos_dec
    ddec = dec1_deg - dec2_deg
    return dra * dra + ddec * ddec


def nearest_star(ra_deg, dec_deg, stars, tol_deg):
    """Return the index of the nearest star within tol_deg, or None.
    Brute-force scan — 700 endpoints × 8870 stars is <10M comparisons,
    well under a second in plain Python."""
    tol_sq = tol_deg * tol_deg
    best_i, best_d2 = None, tol_sq
    for i, s in enumerate(stars):
        ra_s_deg = s["ra_h"] * 15.0
        d2 = angular_dist_sq_deg(ra_deg, dec_deg, ra_s_deg, s["dec_deg"])
        if d2 < best_d2:
            best_d2 = d2
            best_i = i
    return best_i


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

    print(f"reading {LINES_PATH.name}", file=sys.stderr)
    line_segs = []
    dropped_endpoints = 0
    n_features = 0
    for cid, polyline in parse_d3celestial(LINES_PATH):
        n_features += 1
        # Cache per-polyline endpoint resolution so we only do nearest-star
        # lookups once per distinct endpoint, not once per segment side.
        resolved = []
        for ra_deg, dec_deg in polyline:
            idx = nearest_star(ra_deg, dec_deg, stars, MATCH_TOL_DEG)
            resolved.append(idx)
        for i in range(len(resolved) - 1):
            ai, bi = resolved[i], resolved[i + 1]
            if ai is None or bi is None:
                dropped_endpoints += 1
                continue
            line_segs.append((ai, bi))
    print(f"  {n_features} constellation features, "
          f"{len(line_segs)} segments emitted, "
          f"{dropped_endpoints} dropped (endpoint > {MATCH_TOL_DEG}° "
          f"from any mag<={MAG_CUTOFF} star)",
          file=sys.stderr)

    print(f"writing {OUT}", file=sys.stderr)
    with OUT.open("w", encoding="utf-8") as f:
        f.write("#include <stddef.h>\n")
        f.write('\n')
        f.write('#include "skydata.h"\n')
        f.write("\n")
        f.write("/* AUTO-GENERATED by tools/gen_skydata.py — do not edit by hand.\n")
        f.write(" *\n")
        f.write(" * This file is a DUAL-LICENSED derivative work. See LICENSE and\n")
        f.write(" * THIRD_PARTY_LICENSES.md in the repo root for the full per-component\n")
        f.write(" * terms; the summary below is the per-array minimum notice.\n")
        f.write(" *\n")
        f.write(" * ============================================================\n")
        f.write(" * sky_stars[]  — Creative Commons Attribution-ShareAlike 2.5\n")
        f.write(" * ============================================================\n")
        f.write(" * Derived from the HYG Database v3.6.1, © David Nash.\n")
        f.write(" * Source: https://github.com/astronexus/HYG-Database\n")
        f.write(" * Licence: https://creativecommons.org/licenses/by-sa/2.5/\n")
        f.write(" *\n")
        f.write(" * Changes made (CC BY-SA 2.5 §3(b)):\n")
        f.write(f" *   - filtered to V magnitude <= {MAG_CUTOFF} (naked-eye limit; matches BSC5\n")
        f.write(" *     footprint),\n")
        f.write(" *   - reformatted into the C SkyStar struct layout used by voidwatch,\n")
        f.write(" *   - spectral class column reduced to a single character.\n")
        f.write(" *\n")
        f.write(" * Redistributors must keep this attribution + share-alike notice\n")
        f.write(" * intact. The sky_stars[] data itself remains under CC BY-SA 2.5.\n")
        f.write(" *\n")
        f.write(" * ============================================================\n")
        f.write(" * sky_lines[]  — BSD 3-Clause (d3-celestial)\n")
        f.write(" * ============================================================\n")
        f.write(" * Derived from d3-celestial's `data/constellations.lines.json`,\n")
        f.write(" * © 2015 Olaf Frohn.\n")
        f.write(" * Source: https://github.com/ofrohn/d3-celestial\n")
        f.write(" * Licence: https://opensource.org/license/bsd-3-clause/\n")
        f.write(" *\n")
        f.write(" * Changes made (BSD-3-Clause attribution preserved):\n")
        f.write(" *   - GeoJSON [RA, Dec] endpoint pairs were resolved to nearest\n")
        f.write(f" *     stars in the filtered HYG table within {MATCH_TOL_DEG}°,\n")
        f.write(" *   - emitted as pairs of indices into sky_stars[] for the C\n")
        f.write(" *     renderer.\n")
        f.write(" *\n")
        f.write(" * BSD-3-Clause is permissive (MIT-compatible); voidwatch's binary\n")
        f.write(" * is therefore free of GPL contamination from this data source.\n")
        f.write(" * Redistributors must preserve Olaf Frohn's copyright notice in\n")
        f.write(" * documentation accompanying any redistribution — voidwatch does\n")
        f.write(" * so via THIRD_PARTY_LICENSES.md and CITATIONS.md.\n")
        f.write(" *\n")
        f.write(" * See CITATIONS.md for full upstream credits and references.\n")
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
