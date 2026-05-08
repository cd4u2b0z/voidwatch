# Citations

voidwatch leans on a handful of public astronomical catalogues and
algorithms. Everything below is bundled at compile time — no runtime
data files, no network calls.

## Star catalogue

- **HYG Database v3.6.1**
  https://github.com/astronexus/HYG-Database
  *© David Nash, CC BY-SA 2.5.* HYG aggregates the Hipparcos catalogue,
  the Yale Bright Star Catalog (BSC5), and the Gliese Catalog of Nearby
  Stars; voidwatch uses it for HIP / RA / Dec / V-magnitude / spectral
  class / IAU proper names. Filtered to V ≤ 6.5 (8870 stars) at
  generation time. See `tools/gen_skydata.py`.
- **IAU Working Group on Star Names**
  https://www.iau.org/public/themes/naming_stars/
  Star proper names ("Sirius", "Vega", …) ride along in the HYG
  `proper` column.

## Constellation figures

- **Stellarium "modern" skyculture (88 IAU constellations)**
  https://github.com/Stellarium/stellarium (GPL-2.0)
  Polylines from `skycultures/modern/index.json`. HIP indices are
  remapped to the filtered HYG star table during generation. 692 line
  segments survive the magnitude filter; ~3 endpoints are dropped per
  refresh because their stars sit dimmer than the cutoff.

## Solar system

- **Meeus, *Astronomical Algorithms*, 2nd ed.** (Willmann-Bell, 1998).
  Chapters 25 (Sun), 47 (Moon), 32 (planets, truncated VSOP87). Gives
  voidwatch ~few-arcminute geocentric positions — well under one terminal
  cell. Implemented in `src/ephem.c`.
- **NASA JPL HORIZONS / Small-Body Database**
  https://ssd.jpl.nasa.gov/horizons/, https://ssd.jpl.nasa.gov/sbdb.cgi
  Source for the bundled comet and asteroid orbital elements (perihelion
  epoch, eccentricity, inclination, node, argument of perihelion). See
  `src/comet.c` and `src/asteroid.c`.
- **Standish & Williams, *Keplerian Elements for Approximate Positions
  of the Major Planets*** (JPL).
  Element form used by `src/ephem.c`'s `helio_xyz`.

## Atmosphere & rendering

- **Bennett (1982)**, "The calculation of astronomical refraction in
  marine navigation." Used in `radec_to_altaz` for true→apparent altitude.
- **Kasten & Young (1989)**, "Revised optical air mass tables and
  approximation formula." Used in `apply_extinction` for the airmass +
  V-band dimming + reddening at low altitudes.
- **Liu / Hambly / Hobbs**, galactic→equatorial J2000 rotation matrix.
  Used in `milkyway_init`.

## Meteor showers

Peak DOY, FWHM, ZHR, and radiant J2000 from the IAU Meteor Data Center
and the *Observer's Handbook* (Royal Astronomical Society of Canada).

## Framebuffer + Braille

- **Unicode Consortium**, U+2580–U+259F (block elements) and
  U+2800–U+28FF (Braille patterns), used to address 2×4 sub-pixels per
  terminal cell.

## Vendored

- **stb_perlin.h** — Sean Barrett, public domain. `include/stb_perlin.h`.
