# Third-Party Licenses

voidwatch bundles data from upstream sources whose licenses are
reproduced here (or linked to canonical full-text sources). This file
satisfies CC BY-SA 2.5's notice requirement and GPL-2.0's "include a
copy of this License" requirement.

For the per-component summary of which voidwatch artifact comes from
which source, see [LICENSE](LICENSE). For the academic / data-source
attribution table, see [CITATIONS.md](CITATIONS.md).

---

## 1. Creative Commons Attribution-ShareAlike 2.5  —  HYG Database v3.6.1

**Applies to:** `sky_stars[]` array in `src/skydata.c`.
**Source:** © David Nash — https://github.com/astronexus/HYG-Database
**License URL:** https://creativecommons.org/licenses/by-sa/2.5/legalcode
**Summary:** https://creativecommons.org/licenses/by-sa/2.5/

### Key obligations voidwatch meets

- **Attribution** (§4(c)) — HYG and David Nash credited in `LICENSE`,
  `CITATIONS.md`, and the file header of `src/skydata.c`.
- **Indicate changes made** (§4(b)) — the changes (V≤6.5 filter,
  reformatting into the C struct, single-character spectral class)
  are documented in `LICENSE` and the `src/skydata.c` header.
- **License notice** (§4(c)) — link to CC BY-SA 2.5 included.
- **ShareAlike** (§4(c)) — derivatives of `sky_stars[]` must remain
  under CC BY-SA 2.5 or a Creative Commons "Compatible License" (the
  CC BY-SA 4.0 outbound list, etc.). MIT alone does **not** satisfy
  this for the data table itself; that is why voidwatch's `LICENSE`
  is split per-component rather than blanket-MIT.

### Full text

The full legal text of CC BY-SA 2.5 is available at the URL above. By
reference, voidwatch incorporates it here. Redistributors who want a
self-contained copy of the full license text bundled with the source
should download the official version from creativecommons.org and
include it in their distribution.

---

## 2. GNU General Public License, version 2  —  Stellarium "modern_iau" skyculture

**Applies to:** `sky_lines[]` array in `src/skydata.c`. Because this is
linked into the compiled binary, **the voidwatch binary as distributed
is a combined work governed by GPL-2.0**.
**Source:** Stellarium project — https://github.com/Stellarium/stellarium
(skycultures/modern_iau/index.json)
**License URL:** https://www.gnu.org/licenses/old-licenses/gpl-2.0.html
**Stellarium's COPYING:** https://github.com/Stellarium/stellarium/blob/master/COPYING

### Key obligations voidwatch meets

- **Source availability** (§3(a)) — voidwatch source is publicly
  hosted at https://codeberg.org/cdubz/voidwatch.git . Recipients of
  any voidwatch binary have access to the corresponding source.
- **License notice** (§1) — the GPL-2.0 dependency is documented in
  `LICENSE`, `CITATIONS.md`, and the header of `src/skydata.c`.
- **Modified-files notice** (§2(a)) — `src/skydata.c` is auto-generated
  and clearly marked as a derivative; `tools/gen_skydata.py` shows the
  exact transformation.

### Full text

The full legal text of the GNU General Public License version 2 is
available at the URL above. By reference, voidwatch incorporates it
here for the sections of the codebase derived from Stellarium.
Redistributors of the binary must satisfy GPL-2.0 in their own
distribution channel — either by shipping source alongside, or by
offering a written guarantee of source access per GPL-2.0 §3(b).

---

## 3. Public Domain — JPL Horizons / SBDB

The bundled orbital elements for comets (`comet_elements[]`),
asteroids (`asteroid_elements[]`), and planet J2000+rate elements
(Standish via Meeus in `src/ephem.c`) are derived from NASA JPL
Horizons and Small-Body Database. As US Government work, these are
in the public domain.

JPL asks for attribution but imposes no licensing obligation.

---

## 4. Factual data with attribution — CelesTrak GP / TLE

The bundled near-Earth TLEs in `satellite_elements[]` (ISS, HST,
NOAA 19, CSS Tianhe) come from CelesTrak's GP query API. CelesTrak
publishes TLEs derived from US Space Force tracking. The numbers
themselves are factual/PD; CelesTrak asks for attribution for
redistributors.

CelesTrak guidance: https://celestrak.org/

---

## 5. Public domain / MIT-0 — stb_perlin.h

Sean Barrett's `stb_perlin.h` is dual-licensed at the user's option:

- Public domain (Unlicense)
- MIT-0 (https://opensource.org/license/mit-0)

The file's own header notice carries the canonical text.

Source: https://github.com/nothings/stb

---

## 6. Public domain — SGP4 algorithm

The Hoots & Roehrich (1980) Spacetrack Report No. 3 was published as
US Government work, public domain. Vallado, Crawford, Hujsak & Kelso
(2006), "Revisiting Spacetrack Report #3" (AIAA 2006-6753) and the
companion source code is published with terms that explicitly permit
reuse for any purpose, including commercial.

voidwatch's `src/satellite.c` is original code; the algorithm and
test vectors are PD-derived.

References:
- Hoots, F. R. & Roehrich, R. L. (1980). Spacetrack Report No. 3.
- Vallado, D. A. et al. (2006). https://celestrak.org/publications/AIAA/2006-6753/

---

## Building from source without GPL contamination

If your downstream use case is incompatible with GPL-2.0 (e.g., you
need pure-MIT for proprietary integration), you can build a clean
voidwatch by:

1. Replacing `tools/data/stellarium_modern.json` with a
   public-domain or permissively-licensed constellation-line source.
   The IAU constellation *boundaries* are factual; the *line figures*
   are conventional but were popularised by various PD star atlases
   (e.g., the Yale Bright Star Atlas, the U.S. Naval Observatory's
   atlases). Hand-curating the ~88 figures' ~700 line segments is a
   long afternoon's work.
2. Re-running `python3 tools/gen_skydata.py`.
3. Updating `LICENSE` and this file to reflect the new lineage.

The `sky_stars[]` HYG table can stay; CC BY-SA 2.5 is compatible with
permissive-licensed code projects as long as the data attribution
and share-alike survive.

---

## Asking the upstream

If anything here is materially wrong (especially the GPL inheritance
analysis), please open an issue at
https://codeberg.org/cdubz/voidwatch — voidwatch errs toward
over-disclosure and would rather know.
