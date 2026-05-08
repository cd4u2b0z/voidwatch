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

## 2. BSD 3-Clause License  —  d3-celestial constellation lines

**Applies to:** `sky_lines[]` array in `src/skydata.c`. Permissive
license — the voidwatch binary is **not** copylefted by this data.
**Source:** © 2015 Olaf Frohn — https://github.com/ofrohn/d3-celestial
**License URL:** https://opensource.org/license/bsd-3-clause/
**d3-celestial LICENSE:** https://github.com/ofrohn/d3-celestial/blob/master/LICENSE

### Verbatim copyright + disclaimer (BSD-3-Clause §1 / §2)

> Copyright (c) 2015, Olaf Frohn
> All rights reserved.
>
> Redistribution and use in source and binary forms, with or without
> modification, are permitted provided that the following conditions
> are met:
>
> 1. Redistributions of source code must retain the above copyright
>    notice, this list of conditions and the following disclaimer.
>
> 2. Redistributions in binary form must reproduce the above copyright
>    notice, this list of conditions and the following disclaimer in
>    the documentation and/or other materials provided with the
>    distribution.
>
> 3. Neither the name of the copyright holder nor the names of its
>    contributors may be used to endorse or promote products derived
>    from this software without specific prior written permission.
>
> THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
> "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
> LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
> FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
> COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
> INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
> BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
> LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
> CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
> LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
> ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
> POSSIBILITY OF SUCH DAMAGE.

### Key obligations voidwatch meets

- §1 (source-form notice) — Olaf Frohn's copyright + disclaimer are
  reproduced above and in the `src/skydata.c` per-array header block.
- §2 (binary-form notice) — this file plus `LICENSE` accompany binary
  distributions of voidwatch and reproduce the copyright + disclaimer.
- §3 (no endorsement) — voidwatch does not use Olaf Frohn's name or
  contributors' names to endorse or promote the project.

### Why d3-celestial replaced the previous Stellarium import

Earlier voidwatch versions used Stellarium's `modern_iau` skyculture
(GPL-2.0) for the constellation-line data. GPL-2.0 would have made the
voidwatch binary a GPL-2.0 combined work — legally fine but inconsistent
with the rest of voidwatch's posture. d3-celestial encodes the same
Western IAU 88-figure conventions under BSD-3-Clause, and the swap
restored a clean per-component license model where no copyleft term
applies to the binary.

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

## Building from source under stricter licensing constraints

The voidwatch binary as distributed is **free of GPL contamination**
(d3-celestial replaced Stellarium for `sky_lines[]` on 2026-05-08).
The remaining copyleft obligation is CC BY-SA 2.5 on the HYG-derived
`sky_stars[]` table.

If your downstream use case is incompatible with CC BY-SA 2.5 (e.g.,
you need a strict permissive-only data layer for proprietary
integration without share-alike), you can rebuild voidwatch by:

1. Swapping `tools/data/hyg_v36_1.csv` for the raw Yale Bright Star
   Catalog (BSC5, broadly considered factual / public domain). HYG
   itself aggregates BSC5 + Hipparcos + Gliese, all of which originate
   in non-copyrighted scientific data. You'd lose HYG's curated
   editorial choices but the underlying numbers are PD.
2. Adapting `tools/gen_skydata.py`'s `parse_hyg()` to read BSC5's
   field layout instead of HYG's CSV.
3. Updating `LICENSE`, this file, and `src/skydata.c`'s header to
   reflect the new lineage.

This is more invasive than the d3-celestial swap was (BSC5 has no
"proper name" column, so you'd need to attach IAU names from a
separate PD list). For most downstream users CC BY-SA 2.5 is
acceptable as long as the attribution and share-alike notice on
the data table survive.

---

## Asking the upstream

If anything here is materially wrong (especially the GPL inheritance
analysis), please open an issue at
https://codeberg.org/cdubz/voidwatch — voidwatch errs toward
over-disclosure and would rather know.
