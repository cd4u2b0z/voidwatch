#!/usr/bin/env python3
"""
Generate src/satdata.c from CelesTrak's GP query API.

Maintainer-only refresh tool. End users build voidwatch with no Python
and no network — the committed src/satdata.c carries the bundled
near-Earth satellite catalog.

Run from repo root:
    python3 tools/gen_satellites.py

Posture: voidwatch's bundled catalog is demo seed data, not long-term
truth. TLEs decay in days; rerun this script every couple of weeks
to refresh the bundled snapshot. The stale-TLE policy in voidwatch
itself dims/hides/refuses entries past 7/14/30 days, so a stale
bundle degrades gracefully even without a refresh.

Output license header notes:
    - voidwatch source wrapping the catalog: MIT.
    - The TLE numbers themselves are factual; CelesTrak (the upstream)
      is attributed in CITATIONS.md and asks attribution for
      redistributors. CelesTrak republishes US Space Force tracking
      so the underlying numbers are factual / public.

CelesTrak guidance (https://celestrak.org/NORAD/general/notice.php):
    - GP data is checked every 2 hours; do not poll aggressively.
    - The query shape is gp.php?CATNR=<n>&FORMAT=TLE.
"""

from __future__ import annotations

import sys
import textwrap
import time
import urllib.error
import urllib.request
from pathlib import Path

# ----------------------------------------------------------------------
# Curated catalog. Add / remove entries here, rerun the script to
# refresh src/satdata.c. Each entry must be near-Earth (period < 225
# min) — deep-space SDP4 is deferred. CSV catnr lookup at
# https://celestrak.org/satcat/search.php
# ----------------------------------------------------------------------

SATELLITES = [
    {
        "catnr": 25544,
        "display": "ISS (ZARYA)",
        # comma-separated, lowercased — the parser case-insensitives
        "aliases": "iss,zarya,25544",
        "comment": "International Space Station — the canonical visible-pass target",
    },
    {
        "catnr": 20580,
        "display": "HST",
        "aliases": "hst,hubble,20580",
        "comment": "Hubble Space Telescope",
    },
    {
        "catnr": 33591,
        "display": "NOAA 19",
        "aliases": "noaa,noaa19,noaa-19,33591",
        "comment": "NOAA 19 — polar weather satellite, reliable LEO sun-synchronous",
    },
    {
        "catnr": 48274,
        "display": "CSS (TIANHE)",
        "aliases": "css,tianhe,tiangong,48274",
        "comment": "CSS Tianhe — Chinese space station core module",
    },
]

GP_URL = "https://celestrak.org/NORAD/elements/gp.php?CATNR={catnr}&FORMAT=TLE"
RATE_LIMIT_SECONDS = 1.5   # polite pause between requests
USER_AGENT = "voidwatch-gen-satellites/1.0 (https://codeberg.org/cdubz/voidwatch)"

OUT = Path("src/satdata.c")


def fail(msg: str) -> None:
    print(f"gen_satellites: {msg}", file=sys.stderr)
    sys.exit(1)


def fetch_tle(catnr: int) -> tuple[str, str, str]:
    """Return (line0, line1, line2) verbatim from CelesTrak."""
    url = GP_URL.format(catnr=catnr)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            body = r.read().decode("ascii", errors="replace")
    except urllib.error.URLError as e:
        fail(f"fetch failed for catnr={catnr}: {e}")
    lines = [ln.rstrip("\r\n") for ln in body.splitlines() if ln.strip()]
    if len(lines) < 3:
        fail(f"catnr={catnr}: unexpected response shape (need 3 lines, got {len(lines)}):\n{body!r}")
    name, l1, l2 = lines[0].rstrip(), lines[1], lines[2]
    if not l1.startswith("1 ") or not l2.startswith("2 "):
        fail(f"catnr={catnr}: lines don't look like a TLE: {l1!r} / {l2!r}")
    return name, l1, l2


def tle_checksum_ok(line: str) -> bool:
    """Mod-10 checksum: digits add their value, '-' adds 1, all else 0.
    Match against the digit at column 69 (offset 68)."""
    if len(line) != 69:
        return False
    s = 0
    for c in line[:68]:
        if "0" <= c <= "9":
            s += ord(c) - ord("0")
        elif c == "-":
            s += 1
    expected = ord(line[68]) - ord("0")
    return 0 <= expected <= 9 and (s % 10) == expected


def near_earth(line2: str) -> bool:
    """Mean motion in rev/day at columns 53-63 (offsets 52-62). Period
    must be < 225 minutes for SGP4 near-Earth path. mean_motion >
    1440/225 = 6.4 rev/day."""
    try:
        n = float(line2[52:63])
    except (ValueError, IndexError):
        return False
    return n > 6.4


def emit_c_string(s: str) -> str:
    """Render a Python string as a C string literal — escape backslash
    and double-quote, leave printable ASCII alone (TLEs are pure ASCII)."""
    out = []
    for c in s:
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        else:
            out.append(c)
    return '"' + "".join(out) + '"'


def main() -> None:
    if not OUT.parent.exists():
        fail(f"cannot find {OUT.parent} — run from repo root")

    print(f"gen_satellites: refreshing {len(SATELLITES)} bundled near-Earth TLEs")
    print(f"               source: {GP_URL.format(catnr='<n>')}")
    print()

    fetched = []
    for i, sat in enumerate(SATELLITES):
        catnr = sat["catnr"]
        if i > 0:
            time.sleep(RATE_LIMIT_SECONDS)
        print(f"  [{i+1}/{len(SATELLITES)}] catnr={catnr} ({sat['display']}) ", end="", flush=True)
        name, l1, l2 = fetch_tle(catnr)
        if not tle_checksum_ok(l1):
            fail(f"\ncatnr={catnr}: line 1 checksum mismatch: {l1!r}")
        if not tle_checksum_ok(l2):
            fail(f"\ncatnr={catnr}: line 2 checksum mismatch: {l2!r}")
        if not near_earth(l2):
            fail(f"\ncatnr={catnr}: not near-Earth — period >= 225 min, would refuse SGP4 init")
        # epoch column at offsets 18-32 of line 1: yy + ddd.dddddddd
        yy = l1[18:20]
        epoch = l1[20:32]
        print(f" epoch={yy}{epoch[:6]}…")
        fetched.append({
            "catnr":   catnr,
            "display": sat["display"],
            "aliases": sat["aliases"],
            "comment": sat["comment"],
            "line1":   l1,
            "line2":   l2,
        })

    # ------------------------------------------------------------------
    # Emit src/satdata.c
    # ------------------------------------------------------------------
    today = time.strftime("%Y-%m-%d", time.gmtime())
    n = len(fetched)

    body = []
    body.append("/* AUTO-GENERATED by tools/gen_satellites.py — do not edit by hand.\n"
                " *\n"
                f" * Refreshed {today} from CelesTrak's GP query API:\n"
                " *   https://celestrak.org/NORAD/elements/gp.php?CATNR=<n>&FORMAT=TLE\n"
                " *\n"
                " * The voidwatch source code wrapping this catalog is MIT-licensed (see\n"
                " * LICENSE in the repo root). The TLE numbers themselves are factual\n"
                " * data derived from US Space Force tracking and republished by\n"
                " * CelesTrak. CelesTrak does not assert copyright on the numbers but\n"
                " * requests attribution for redistributors — voidwatch credits CelesTrak\n"
                " * in CITATIONS.md and THIRD_PARTY_LICENSES.md.\n"
                " *\n"
                " * TLEs decay in days. voidwatch's stale-policy in src/satellite.c\n"
                " * (and AstroState's render gating) dims at >7d, hides at >14d, and\n"
                " * refuses propagation at >30d. Refresh by rerunning the generator.\n"
                " *\n"
                " * Every entry must be near-Earth (mean motion > 6.4 rev/day, period\n"
                " * < 225 min) — deep-space SDP4 is deferred. The generator validates\n"
                " * this; if a future entry trips the check, it bails rather than\n"
                " * silently emitting a TLE that voidwatch would refuse at runtime.\n"
                " */\n\n"
                '#include "satellite.h"\n\n')
    body.append(f"const SatelliteElements satellite_elements[SATELLITE_COUNT] = {{\n")
    for s in fetched:
        body.append(f"    /* {s['comment']} */\n")
        body.append( "    {\n")
        body.append(f"        .name    = {emit_c_string(s['display'])},\n")
        body.append(f"        .line1   = {emit_c_string(s['line1'])},\n")
        body.append(f"        .line2   = {emit_c_string(s['line2'])},\n")
        body.append(f"        .aliases = {emit_c_string(s['aliases'])},\n")
        body.append(f"        .catalog = {s['catnr']},\n")
        body.append( "    },\n")
    body.append("};\n")
    body.append("const int satellite_count = SATELLITE_COUNT;\n")

    # utf-8: comment blocks include em-dashes; voidwatch's other source
    # files are utf-8 too and gcc handles them fine.
    OUT.write_text("".join(body), encoding="utf-8")

    # Sanity-check that SATELLITE_COUNT in the header matches what we
    # wrote. If not, the next compile will fail; warn explicitly.
    hdr = Path("include/satellite.h").read_text(encoding="utf-8")
    import re
    m = re.search(r"#define\s+SATELLITE_COUNT\s+(\d+)", hdr)
    if not m:
        fail("could not find #define SATELLITE_COUNT in include/satellite.h")
    declared = int(m.group(1))
    if declared != n:
        print()
        print(f"!! warning: include/satellite.h declares SATELLITE_COUNT = {declared}")
        print(f"   but {n} entries were emitted. Update the #define and rebuild.")
        sys.exit(2)

    print()
    print(f"gen_satellites: wrote {OUT} ({n} satellites)")
    print(f"               next: `make` and verify `make test` is green.")


if __name__ == "__main__":
    main()
