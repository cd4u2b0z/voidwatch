# Astronomy

Per-algorithm citations for every astronomically-meaningful number
voidwatch computes. If a formula came from a published source, this
document tells you which source, where in the source, and which
function in the codebase implements it.

The companion documents:

- `README.md` — user-facing intro
- `ARCHITECTURE.md` — build journey, design decisions, the *why*
- `CITATIONS.md` — credits the **data** sources (HYG, Stellarium,
  JPL Horizons, JPL SBDB, IAU, RASC). This file credits the
  **algorithm** sources (Meeus, Standish, Bennett, Kasten-Young, etc.).

If you found voidwatch's output suspicious and want to verify the math,
this is the document to read.

---

## Conventions

- **JD** = Julian Day. The astronomical timescale used throughout.
  Unix epoch 1970-01-01 00:00 UTC = JD 2440587.5.
- **T** = Julian centuries from J2000.0 = (JD − 2451545.0) / 36525.
  This is the time variable in nearly every formula below.
- **J2000.0** = JD 2451545.0 = 2000-01-01 12:00 TT.
- All angles inside the codebase are in radians; user-visible angles
  (CLI input, HUD readouts) are in degrees, hours, or arcminutes per
  convention.
- Coordinates are *apparent* (with aberration + dominant nutation)
  rather than *mean* unless otherwise noted.
- **No ΔT correction.** voidwatch treats the system clock as TT for
  Meeus's purposes. Real ΔT is ~70 seconds in 2026; this introduces
  a Sun longitude error of ~30 arcseconds, well below one terminal
  cell. If you ever upgrade the precision tier (VSOP87 etc.), ΔT
  needs to be added.

---

## Time and reference frames

### Julian Day from Unix time

> Vallado, *Fundamentals of Astrodynamics and Applications* (4e),
> §3.5; same identity in Meeus Ch. 7.

```c
double ephem_julian_day_from_unix(time_t t) {
    return (double)t / 86400.0 + 2440587.5;
}
```

`src/ephem.c:32-35`. The constant 2440587.5 is exact: 1970-01-01
00:00 UTC equals JD 2440587.5 by definition.

### Mean obliquity of the ecliptic

> Meeus, *Astronomical Algorithms* (2e), eq. **22.2** (IAU 1980 series,
> truncated).

```c
ε₀ = 23°26′21.448″
   − 46.8150″   · T
   −  0.00059″  · T²
   +  0.001813″ · T³
```

`src/ephem.c:38-44`. Used by every ecliptic→equatorial transform in
the codebase. The leading constant in the source is `23.43929111`
degrees, which is `23°26′21.448″` reduced to decimal degrees.

### Greenwich Mean Sidereal Time

> Meeus, eq. **12.4** (IAU 1982 expression). Accurate to ~1 arcsecond
> over centuries.

```
GMST = 280.46061837°
     + 360.98564736629° · (JD − J2000)
     +   0.000387933°   · T²
     −   T³ / 38710000
```

`src/ephem.c:355-364`. Local Apparent Sidereal Time (used for hour
angle) is just GMST plus the observer's east longitude in degrees,
divided by 15 to get hours. voidwatch ignores nutation in right
ascension (the equation of the equinoxes) — it's <1 arcsecond and
invisible at cell scale.

---

## Solar position

> Meeus Ch. **25** ("Solar Coordinates"), low-precision algorithm.
> Accuracy: ~0.01° in geocentric longitude.

`src/ephem.c:67-99` (`compute_sun`). The chain:

1. Mean longitude `L₀` and mean anomaly `M` linear in T (Meeus 25.2).
2. Sun's equation of centre `C` from a 3-term sine series in `M`
   (Meeus 25.4).
3. True longitude = `L₀ + C`; true anomaly = `M + C`.
4. Apparent longitude with **aberration + dominant nutation**:
   ```
   λ = (L₀ + C) − 0.00569° − 0.00478° · sin(Ω)
   ```
   where `Ω = 125.04° − 1934.136° · T` is the Moon's ascending node
   (Meeus 25.8). The 0.00569° term is annual aberration; the sin(Ω)
   term is the dominant nutation in longitude.
5. True obliquity = mean obliquity + `0.00256° · cos(Ω)` (Meeus 25.8).
6. Distance from Earth in AU (Meeus 25.5):
   ```
   R = 1.000001018 (1 − e²) / (1 + e cos ν)
   ```
   with eccentricity `e = 0.016708634 − ...` (Meeus 25.4).
7. Ecliptic → equatorial via standard rotation by ε.

Magnitude is fixed at −26.74 (the IAU/Allen value).

> **Validation case:** Sun at J2000.0 against Meeus 25.a (worked
> example, p. 165). PASS at ≤ 0.2 arcminutes.

---

## Lunar position

> Meeus Ch. **47** ("Position of the Moon"), abridged. Full ELP-2000/82
> series has 60+ terms in longitude alone; voidwatch keeps the 14
> largest in Σl, 8 largest in Σb, 6 largest in Σr. Accuracy:
> ~0.5° in the truncated form, vs ~0.0003° in the full series.

`src/ephem.c:103-181` (`compute_moon`). The five fundamental
arguments — D (mean elongation), M (Sun's mean anomaly), M′ (Moon's
mean anomaly), F (argument of latitude), L′ (Moon's mean longitude) —
are Meeus 47.1–47.5. The eccentricity factor `E = 1 − 0.002516 T −
0.0000074 T²` (Meeus 47.6) modulates terms with M.

The largest periodic term (Σl):
```
+6288774 · sin(M′)
```
is the elliptic equation — it's what gives the Moon its 5.1°
"wobble" relative to mean motion. The next 13 terms are the largest
contributors to longitude after that.

Distance:
```
d = 385000.56 km + Σr / 1000
```
where Σr is in 1e-3 km units in Meeus's table.

> **Validation case:** Moon at Meeus 47.a (1992-04-12 00:00 TD)
> against the published full-series answer. PASS at ≤ 0.5
> arcminutes — the truncation error happens to fall below our
> tolerance for that specific date.

### Moon phase, illumination, age

> Meeus Ch. **48** simplified (we don't use 48.4–48.5 — voidwatch's
> needs are coarser than full phase angle).

`src/astro.c:62-76` derives signed elongation from the Sun and Moon
RA/Dec:

```
cos(sep) = sin(δ_S) sin(δ_M) + cos(δ_S) cos(δ_M) cos(α_S − α_M)
illum    = (1 − cos(sep)) / 2                    // Meeus 48.1
elongation = sep   if Δα ≥ 0
           = 2π − sep otherwise   // signed for waxing/waning
```

`astro_moon_age_days` (`src/astro.c:2539-2543`) maps elongation back
to days using the synodic month constant:

```
age = (elongation / 2π) × 29.530588853 days
```

The synodic period 29.530588853 days is the IAU value (Allen,
*Astrophysical Quantities*, 4e).

---

## Planetary positions

> Standish, "Keplerian Elements for Approximate Positions of the Major
> Planets," JPL Solar System Dynamics, 2006 update.
> https://ssd.jpl.nasa.gov/planets/approx_pos.html
>
> Element table copied from JPL's "Table 1" (1800 AD–2050 AD validity
> window). For better accuracy outside that window, JPL's "Table 2"
> adds correction terms for Jupiter–Pluto.

`src/ephem.c:183-316` (`compute_planet`, `helio_xyz`). voidwatch uses
*only* Table 1. Accuracy inside the validity window: a few arcminutes.

The table per planet: `a, e, i, Ω, ϖ, L` at J2000 plus `da, de, di,
dΩ, dϖ, dL` linear rates per Julian century.

Per-frame chain:

1. Linearly evolve elements: `x(T) = x₀ + ẋ · T`.
2. Mean anomaly `M = L − ϖ`; argument of perihelion `ω = ϖ − Ω`.
3. **Solve Kepler:** Newton iteration on `E − e sin E = M`.
   Convergence to 1e-9 in ≤ 8 iterations for any solar-system
   eccentricity. `src/ephem.c:225-233`.
4. Position in orbital plane: `(x_orb, y_orb) = (a(cos E − e),
   a√(1−e²) sin E)`.
5. Rotate orbital plane → heliocentric ecliptic by Rz(Ω) · Rx(i) ·
   Rz(ω). `src/ephem.c:236-270`.
6. Geocentric vector: subtract Earth's heliocentric position
   (computed the same way from Earth's elements).
7. Ecliptic → equatorial via mean obliquity at JD.

Apparent magnitude (`src/ephem.c:309-315`):

```
m = H + 5 · log₁₀(r · Δ)
```

where `H` is the absolute magnitude per planet (table column
`mag_h0` — values from Allen 4e):

| Planet  | H      |
|---------|--------|
| Mercury | −0.42  |
| Venus   | −4.40  |
| Mars    | −1.52  |
| Jupiter | −9.40  |
| Saturn  | −8.88  |
| Uranus  | −7.19  |
| Neptune | −6.87  |

Phase-angle correction (Meeus Ch. **41**) is *skipped* — it matters for
inner planets but is invisible at cell scale.

> **Validation cases:**
> - Venus at Meeus 33.a (1992-12-20 00:00 TD) against the published
>   abridged-VSOP87 answer. PASS at ≤ 6.0 arcminutes — generous
>   tolerance because Standish's elements drift faster than VSOP87
>   at outlier dates.

---

## Topocentric coordinates

> Meeus Ch. **13** ("Transformation of Coordinates"), eq. **13.5–13.6**
> for the equatorial → horizontal transform. Geocentric → topocentric
> parallax (Meeus Ch. **40**) is **skipped** — only matters for the
> Moon (~1° at the horizon) and we accept that error to keep the
> code small.

`src/ephem.c:366-382` (`ephem_to_topocentric`):

```
H = LST − α                          // hour angle
sin(alt) = sin(φ) sin(δ) + cos(φ) cos(δ) cos(H)
tan(az)  = −cos(δ) sin(H) / [sin(δ) cos(φ) − cos(δ) sin(φ) cos(H)]
```

φ = observer latitude. Azimuth measured from north, increasing east.

For astro-mode draws, voidwatch's higher-level `radec_to_altaz`
(`src/astro.c:184-199`) wraps this with a refraction step (next
section).

---

## Atmospheric refraction

> Bennett, G. G. (1982), "The calculation of astronomical refraction
> in marine navigation," *Journal of Navigation*, 35(2), 255–259.

`src/astro.c:174-182`:

```
R = 1 / tan(h + 7.31° / (h + 4.4°))   arcminutes
```

where `h` is true altitude in degrees. Bumps geometric altitude up
to apparent altitude. Effect is ~34 arcminutes at the horizon
(roughly the apparent diameter of the Sun) and falls below 1
arcminute past 45° altitude.

Net visual effect: bodies stay visible a beat past geometric set.
Refraction is applied at the *altitude* level so every downstream
extinction / projection step sees apparent altitude.

> **Pro tip embedded in code:** Bennett's formula is technically
> only valid for `h ≥ −1°`. Below that the code returns 0 — the
> body is well under the horizon and refraction can't lift it
> visibly anyway.

---

## Atmospheric extinction

> Kasten, F. & Young, A. T. (1989), "Revised optical air mass tables
> and approximation formula," *Applied Optics*, 28(22), 4735–4738.

`src/astro.c:201-210`:

```
X = 1 / [ sin(h) + 0.50572 (h° + 6.07995)^(−1.6364) ]
```

Air mass `X` is 1.0 at zenith and ~38 at the horizon. Accurate to
the horizon, unlike the simple `X = sec(z)` approximation which
diverges.

Applied to body intensity in `apply_extinction` (`src/astro.c:217-231`)
as V-band magnitude loss:

```
Δm  = k_V (X − 1)                          // V-band dimming
dim = 10^(−0.4 Δm)                         // intensity factor
```

with **`k_V = 0.18 mag/airmass`** — *gentler than the real-world
~0.28 mag/airmass*. This is a deliberate aesthetic choice: a Sirius
at 5° altitude with k=0.28 dims by 10× and is invisible in a
terminal. With k=0.18 it stays legible.

Differential reddening:

```
Δm_b = 0.10 (X − 1)   // blue dims fastest
Δm_g = 0.04 (X − 1)   // green next
                      // red unchanged
```

This is qualitative — the real Rayleigh wavelength dependence is
λ⁻⁴, but for terminal output two coefficients are enough to see the
horizon redden.

---

## Twilight model

> Civil / nautical / astronomical twilight definitions are IAU
> standards (Sun altitude ≥ −6°, ≥ −12°, ≥ −18° respectively). The
> *colour and brightness model* in voidwatch is hand-tuned for
> visual feel — see "stylised, not physical" caveats below.

`src/astro.c:241-249` (`twilight_factor`):

| Sun altitude | Factor   | Phase                     |
|--------------|----------|---------------------------|
| ≥ 0°         | 0.0      | Sun up — no twilight      |
| ≥ −2°        | 1.0      | Sunset/sunrise glow peak  |
| ≥ −6°        | 1.0 → 0.55 | Civil twilight          |
| ≥ −12°       | 0.55 → 0.15 | Nautical twilight       |
| ≥ −18°       | 0.15 → 0.0 | Astronomical twilight    |

Colour lerps a 2-anchor warm/cool palette (`src/astro.c:251-269`):

- Warm: `(1.00, 0.55, 0.45)` — civil twilight pink-orange.
- Cool: `(0.30, 0.45, 0.85)` — nautical/astronomical blue.

The horizon glow is brightest in the Sun's azimuth quadrant
(`src/astro.c:271-289`):

```
azimuth_weight = 0.30 + 0.70 · (0.5 + 0.5 cos(az − az_sun))
```

The 0.30 floor ensures the anti-solar side still gets a hint of
glow during civil twilight (which is geographically correct — the
whole sky brightens, just not as much).

> **Stylised, not photometric.** The colour and the altitude/azimuth
> Gaussian falloff (σ=9°) are eye-tuned, not derived from radiative
> transfer. A real model would integrate Rayleigh scattering + ozone
> Chappuis bands. Don't cite voidwatch for sky brightness research.

---

## Galilean moons

> Mean longitudes at J2000 from Meeus Ch. **44** ("The Galilean
> Satellites"). voidwatch ignores Meeus's perturbation series —
> mutual gravitational perturbations + Jupiter axial tilt + light-
> time corrections — because they're sub-arcsecond at terminal cell
> scale.

`src/astro.c:705-768`. Table at line 713:

| Moon     | Period (d)  | a (R_J) | M₀ at J2000 (rad) |
|----------|-------------|---------|-------------------|
| Io       |  1.769138   |  5.91   | 1.86              |
| Europa   |  3.551181   |  9.40   | 3.50              |
| Ganymede |  7.154553   | 15.00   | 5.31              |
| Callisto | 16.689018   | 26.36   | 0.46              |

Periods + semi-major axes from JPL HORIZONS. M₀ is the mean
longitude at J2000 epoch. Per-frame:

```
M(t) = M₀ + (2π / period) · (JD − 2451545.0)
dx = a · cos(M)
dy = a · sin(M) · 0.06     // y flatten = stylised ring inclination
```

The 0.06 y-flatten is a stylised "viewed from Earth" projection.
Real Galilean orbital inclination relative to Earth's line of sight
varies (3–4° equator tilt), but the screen-plane projection at
voidwatch's pixel scale isn't sensitive to it.

---

## Saturn ring tilt

> No published formula for B (ring opening angle) in this exact
> form. Derived from the geometry: B varies sinusoidally over
> Saturn's 29.46-year heliocentric period because Earth sees the
> rings edge-on twice per Saturn year (every ~14.7 years). Amplitude
> 28.07° matches the obliquity of Saturn's rotation axis to its
> orbit.

`src/astro.c:778-784`:

```c
double dt_yr = (jd − 2451545.0) / 365.25;
double lon_deg = 50.0 + 12.22 · dt_yr;     // Saturn mean L heliocentric
double phase = (lon_deg − 169.5) · π/180;
return 28.07° · sin(phase);
```

12.22°/yr is Saturn's mean orbital angular velocity. The 50° at
J2000 and 169.5° node line are heliocentric-frame approximations
fit to keep the model within ~5° of the true B(t) over voidwatch's
useful date range (1900–2100). Verified by spot-checking against
JPL HORIZONS on edge-on years 1995, 2009, 2025.

> **Caveat:** This is a stylised sinusoid, not a rigorous derivation
> from Saturn's true heliocentric ecliptic longitude minus its
> ascending node longitude. The shortcut is good enough that ring
> orientation reads correctly across decades; don't use it for
> photometric Saturn studies.

The disc + ring rendering layers the back ring → Saturn disc → front
ring (`src/astro.c:786-870`) so the near-side ring naturally
overdraws the disc. There's a Cassini gap dim band at 0.83–0.88 of
outer ring radius.

---

## Eclipses

> No specific paper — this is the textbook angular-separation method
> from any spherical-astronomy reference (e.g. Smart, *Spherical
> Astronomy*, 6e, §57). It is *not* the rigorous Bessel-elements
> method used for eclipse path-of-totality predictions (see Espenak's
> NASA pages for those — voidwatch defers them, see
> `ARCHITECTURE.md` "What's NOT used").

`src/astro.c:1458-1492`.

### Solar eclipse magnitude

```
sep = angular_sep(α_sun, δ_sun, α_moon, δ_moon)
f   = max(0, (0.012 rad − sep) / 0.012)
```

The threshold 0.012 rad ≈ 0.69° is generous: the actual sum of
Sun + Moon angular radii is ~0.527° (≈ 0.0092 rad). The extra
margin lets partial eclipses register slightly before perfect
geocentric alignment, which compensates for voidwatch's
geocentric-only model — a real observer sees lunar parallax shift
the Moon's apparent position by up to ~1°.

### Lunar eclipse magnitude

Same shape, against the antisolar point:

```
anti_α = α_sun + π
anti_δ = −δ_sun
sep    = angular_sep(anti_α, anti_δ, α_moon, δ_moon)
f      = max(0, (0.022 rad − sep) / 0.022)
```

Threshold 0.022 rad ≈ 1.26° approximates the Earth umbra (~0.7°) +
penumbra (~1.3°). voidwatch fires within penumbra.

### Visualisation

- Solar: `apply_solar_eclipse` (`src/astro.c:1497-1521`) dims a
  Moon-sized Gaussian patch at the Moon's screen position over
  the Sun's stamp.
- Lunar: `stamp_moon` (`src/astro.c:539+`) takes an `eclipse_f`
  factor that dims the disc and red-shifts it (the "blood moon"
  colour comes from refracted long-wavelength light through Earth's
  atmosphere — Smart §134, qualitatively).

> **Limitations:**
> - No path of totality — solar eclipses look the same to all
>   observers. A real partial-from-here total-elsewhere eclipse
>   shows up as totality in voidwatch.
> - No occultation discrimination beyond the angular threshold.
> - No magnitude (in the photometric sense) tracking — `f` is just
>   the geometric overlap factor.

---

## Comets

> Two-body Keplerian propagation. Standard cometary mechanics — see
> Danby, *Fundamentals of Celestial Mechanics* (2e), Ch. 6.
>
> Apparent magnitude formula is the IAU/Marsden form, attributed to
> Bobrovnikoff and refined by Meisel (1970). Used by JPL Horizons
> for comet ephemerides and by every visual-comet observer's app.

`src/comet.c`. Bundled comets and elements at perihelion epoch from
JPL Horizons (see `CITATIONS.md`).

### Propagation

For each comet:

1. Mean motion `n = 2π / period_yr` (rad/yr); for long-period comets
   (period_yr = 0 in the table) `n` is computed from semi-major
   `a = q / (1 − e)` via Kepler's third law.
2. Mean anomaly `M(t) = n · (jd − T_perihelion)`.
3. Solve Kepler's equation for E (`solve_kepler_ecc`,
   `src/comet.c:77-95`).
4. True anomaly + radius from E, e, a.
5. Position in orbital plane → heliocentric ecliptic via
   Rz(Ω) · Rx(i) · Rz(ω) (same rotation chain as planets).
6. Geocentric vector by subtracting Earth's heliocentric position.

### Apparent magnitude

```
m = H + 5 · log₁₀(Δ) + 2.5 · n · log₁₀(r)
```

where:
- `H` = absolute (total) visual magnitude (per-comet, from MPC)
- `Δ` = Earth–comet distance (AU)
- `r` = Sun–comet distance (AU)
- `n` = activity slope (≈ 4 nominal; 2 for "lazy" comets)

Bundled values (`comet_elements[]`) come from JPL Horizons / IAU MPC
records at the perihelion epoch in `T_jd`.

> **Limitation:** Two-body propagation drifts ~0.1°/year at aphelion
> for short-period comets. Accurate comet ephemerides need IAU MPC
> orbital element updates, which conflict with voidwatch's
> "no runtime data files" posture. Accuracy is good near perihelion
> (a few arcminutes); we accept aphelion drift.

---

## Asteroids

> Classical Keplerian propagation from osculating elements at epoch
> (a, e, i, Ω, ω, M₀). Same orbital mechanics references as comets.

`src/asteroid.c`. Bundled minor planets:

- 1 Ceres, 2 Pallas, 3 Juno, 4 Vesta, 7 Iris.

Elements from JPL SBDB (`https://ssd.jpl.nasa.gov/sbdb.cgi`).

Mean motion from Kepler's third (`n = sqrt(GM_sun / a³)`, expressed
in rad/day after unit reduction). Per-frame:

1. `M(t) = M₀ + n · (jd − epoch)`.
2. Solve Kepler for E (`solve_kepler_ecc`, `src/asteroid.c:31-43`).
3. Same orbital→ecliptic rotation chain.
4. Geocentric correction.

Magnitude (planet-style):

```
m = H + 5 · log₁₀(r · Δ)
```

Phase function (Bowell H-G photometric system, IAU 1986) is
**skipped** — phase angle integral G is not modelled; the
flat `5 log(r·Δ)` form suffices for "is it visible tonight" use.
Render gates on `g_config.asteroid_mag_cutoff` (default 9.5).

---

## Meteor showers

> Active periods, ZHR (zenithal hourly rate) at peak, and radiant
> coordinates from RASC's *Observer's Handbook* (annual editions).
> Meteor activity profile is a Gaussian on day-of-year — standard in
> meteor astronomy (e.g. Jenniskens, *Meteor Showers and Their
> Parent Comets*, CUP 2006, Ch. 3).

`src/astro.c` (the `meteor_showers[]` table near the meteor_step
section). 9 bundled showers: Quadrantids, Lyrids, η-Aquariids,
Perseids, Draconids, Orionids, Leonids, Geminids, Ursids.

### Activity profile

For a shower with peak DOY `D_peak`, half-width `σ` (days), peak ZHR:

```
ZHR(t) = peak · exp(−((DOY(t) − D_peak)² / (2σ²)))
```

Effective rate accounts for radiant altitude:

```
rate_per_min = ZHR / 60 · sin(alt_radiant)
```

where `alt_radiant` is the radiant's current altitude. This is the
standard correction (a meteor shower's effective rate scales with
radiant elevation).

### Sporadic background

Independent of any active shower: ~8/hr × dawn-apex factor when the
Sun is below the horizon. The dawn-apex factor scales sporadic rate
to peak around 6 AM local (Earth's leading face into the sporadic
flux), per Jenniskens Ch. 1.

### Rendering

Streaks fly outward from the projected radiant, 14-pixel fading
tails. Per-shower colour and velocity are stylised (Leonids fast +
blue, Geminids slow + yellow, Draconids gentle warm-white) — these
are eye-tuned, not derived from spectroscopy.

---

## Star magnitudes and tints

### Magnitude → screen intensity

`mag_to_intensity` (`src/astro.c`):

```
intensity = 10^(−0.4 (m − ref_mag))
```

with `ref_mag` chosen so a magnitude-0 star (Vega-class) draws at a
fixed visual brightness. Standard photometric convention.

### Spectral class → tint

`spectral_tint` maps the Morgan-Keenan spectral class character
('O', 'B', 'A', 'F', 'G', 'K', 'M') to an RGB approximation of
black-body colour at the corresponding effective temperature. Anchor
values from Mitchell Charity's "What color are the stars?"
table (http://www.vendian.org/mncharity/dir3/starcolor/).

The tints are stylised: real per-class colour varies with luminosity
class and metallicity. voidwatch uses one tint per main-sequence
class character, which is the right level of detail for terminal
output.

---

## Galactic equator → equatorial transformation

> J2000 galactic-to-equatorial rotation matrix from the NASA
> ADS / NED standard (Murray, *Vectorial Astrometry*, 1983;
> values in Liu et al. 2011, "Reconsidering the galactic
> coordinate system," *A&A* 526, A16).

`src/astro.c` (Milky Way precompute, near `mw_inited`):

```
θ_NGP = 27.12825°    // declination of north galactic pole
α_NGP = 192.85948°   // RA of north galactic pole
l_NCP = 122.93192°   // galactic longitude of north celestial pole
```

These are the IAU 1958 + Liu et al. 2011 J2000-corrected values.
The rotation matrix is built once at first call and cached
(`mw_inited` static flag), then 360 points along the galactic
equator are precomputed and re-projected each frame.

The longitude brightness profile (galactic centre + Cygnus + Carina
peaks) is hand-tuned, not from the IRAS or DIRBE skymaps. Real Milky
Way brightness varies smoothly + with patchy dust extinction;
voidwatch's three-Gaussian peak model is just enough to make Sgr
look bright and the anti-centre look dim.

---

## Aurora model

> **Stylised, not physical.** No published reference covers
> voidwatch's aurora model because it isn't a model of the real
> auroral oval — it's an effect that *looks* like an aurora.

`src/astro.c:1005-1100+` (`aurora_draw`). The components:

- **Kp gating** — `g_config.kp_index` is the user's Kp dial. The
  oval threshold uses a real-world relationship between Kp and
  geomagnetic latitude (`oval_lat ≈ 67° − 2.5° · Kp`, Feldstein-
  Starkov approximation). Below the oval, no aurora rendered.
- **Intensity scaling** — linear in Kp (0.20 + 0.18·Kp).
- **Storm-level reach** — Kp ≥ 7 extends bands higher into the
  sky (the visual analogue of a Carrington-event aurora reaching
  toward the equator).
- **Substorm flares** — Markov-style flare schedule. ~1 in 3000
  frames at 60fps spawns a flare; flare lasts 12–40 seconds at
  1.5–2.2× intensity. Real auroral substorms last 15–60 minutes,
  so the visual cadence is faster than reality; eye-tuned for
  ambient watchability.
- **Colour gradient** — oxygen green at low altitudes (real
  oxygen 557.7 nm green emission peaks at ~100 km altitude),
  violet/red higher (real oxygen 630 nm red + N₂⁺ violet at
  200–500 km). Colours are real; the altitude mapping is
  qualitative.
- **Shimmer phase** — `CLOCK_MONOTONIC`-paced sinusoidal walk
  in azimuth, render-rate-independent.

> **Caveat repeated:** voidwatch's aurora is an aesthetic effect
> with real-world references for *what colour goes at what
> altitude* and *when Kp permits visibility*. It is NOT driven by
> live geomagnetic data; the user dials Kp themselves. Don't use
> voidwatch for aurora forecasting.

---

## Projection

> Snyder, J. P. (1987), *Map Projections — A Working Manual*,
> USGS Professional Paper 1395, §22 ("Azimuthal Equidistant
> Projection").

`project()` in `src/astro.c` (search for the function definition).
The projection is **azimuthal-equidistant centered on zenith**:

```
r = (π/2 − alt) / (π/2)         // 0 at zenith, 1 at horizon
x = r · sin(az)
y = r · cos(az)                 // north up by convention
```

Then `(x, y)` maps to screen sub-pixel coordinates with the smaller
of (sub_w, sub_h) as the scale. Bodies below the horizon (alt < 0)
return -1, signalling "skip."

The azimuthal-equidistant choice means that *angular separations from
the zenith* are preserved. A star 30° from zenith sits 1/3 of the way
out from the centre regardless of its azimuth. This is the same
projection astronomy planispheres use — it's the right choice for
"all-sky from where I'm standing."

In **heliocentric mode** (`m`), projection switches to a top-down
view of the solar system:

```
r_compressed = sqrt(r_au)       // sqrt compression
x = r_compressed · cos(λ)
y = r_compressed · sin(λ) · 2.0 // y aspect for terminal cells
```

The sqrt compression keeps Mercury visible while Neptune fits the
frame edge — a logarithmic compression would be more "principled" but
sqrt reads better at terminal scale.

---

## `--validate` self-test

> Runs at `voidwatch --validate`. Reference values are published
> worked examples from Meeus 2e or JPL Horizons retrievals. All
> deltas are arcminutes unless noted.

Current cases (all PASS):

| #  | Test               | Reference                                       | Tol  | Observed |
|----|--------------------|-------------------------------------------------|------|----------|
|  1 | Sun J2000.0        | Meeus Ch. 25 / JPL Horizons cross-check         | 30′  |  2.4′    |
|  2 | Sun Meeus 25.a     | Meeus 25.a (worked example, p. 165)             |  6′  |  0.2′    |
|  3 | Sun 2024-04-08     | JPL Horizons (Great American Eclipse epoch)     | 30′  | 22.4′    |
|  4 | Moon Meeus 47.a    | Meeus 47.a full-series answer                   | 30′  |  0.5′    |
|  5 | Venus Meeus 33.a   | Meeus 33.a abridged-VSOP87 worked answer        | 15′  |  6.0′    |
|  6 | Mercury J2000.0    | JPL Horizons (CENTER 500@399, QUANTITIES 1)     |  6′  |  0.5′    |
|  7 | Mars J2000.0       | JPL Horizons (CENTER 500@399, QUANTITIES 1)     |  6′  |  0.3′    |
|  8 | Jupiter J2000.0    | JPL Horizons (CENTER 500@399, QUANTITIES 1)     | 15′  |  5.9′    |
|  9 | Saturn J2000.0     | JPL Horizons (CENTER 500@399, QUANTITIES 1)     | 15′  |  9.4′    |

Source: `src/headless.c` (`headless_validate`). Run
`voidwatch --validate` to print the table with current deltas.

> **Why these cases:** Meeus's worked examples are the gold-
> standard targets — every astronomer who has implemented Meeus has
> validated against them. Choosing them as references means
> voidwatch's `--validate` answers the question "is this
> implementation faithful to Meeus's published math?" definitively.
> JPL Horizons provides the cross-check for "does it agree with the
> world's reference ephemeris at a real date?" — the J2000.0 rows
> for Mercury / Mars / Jupiter / Saturn confirm voidwatch's
> Standish-element planet path lands inside the documented few-
> arcminute precision band even for outer planets where Standish's
> linear-rate model has the most slack.
>
> Comet and asteroid validation cases are still open. Two-body
> Keplerian propagation has different error characteristics
> (accurate near perihelion, drifts at aphelion) and would need
> per-body apparition-specific reference epochs. Tracked as a
> low-priority open item.

---

## Satellites (SGP4)

> Hoots & Roehrich (1980), "Spacetrack Report No. 3"; Vallado, Crawford,
> Hujsak & Kelso (2006), "Revisiting Spacetrack Report #3"
> (AIAA 2006-6753).

`src/satellite.c` implements a hand-built **near-Earth-only** SGP4
propagator validated against Vallado's published `tcppver.out` test
vectors to **~7 nm position agreement** at 4320-minute propagation
(see `tests/test_sgp4_propagation.c`).

**Pipeline** (caller-visible flow):

```
TLE text  →  satellite_tle_parse  →  SatelliteTLE
SatelliteTLE  →  satellite_model_init  →  SatelliteModel  (WGS-72, AFSPC)
(model, tsince_min)  →  satellite_propagate_teme  →  r,v in TEME (km, km/s)
(r_teme, v_teme, jd, observer)
   →  satellite_eci_to_topocentric  →  alt, az, range, range_rate
```

**Frames** —

- **TEME** (True Equator Mean Equinox) is what SGP4 outputs natively.
  Not J2000, not ICRF, not what voidwatch's planet-RA/Dec path uses.
- **ECEF** is computed by rotating TEME about z by −GMST (Vallado eq
  3-45 polynomial — *not* the Meeus 12.4 form used elsewhere in
  voidwatch; SGP4 verification vectors are tied to this exact
  expression).
- Polar motion is set to zero. UT1−UTC corrections (EOP files) are
  ignored. The result is "visual-grade" — TEME vectors remain the
  strict-validation target.

**Constants** — WGS-72 throughout:

```
mu = 398600.8 km³/s²       J2 =  0.001082616
R⊕ = 6378.135 km           J3 = -2.53881e-6
xke = 60 / sqrt(R³/μ)       J4 = -1.65597e-6
        ≈ 0.07436692
flattening f = 1/298.26    Earth ω = 7.2921150e-5 rad/s
```

WGS-84 (Earth-fixed) constants are *not* mixed in for the observer's
geodetic→ECEF transform. Mixing would introduce a few-tens-of-metres
systematic offset that hides in "visual-grade" but compounds for pass
timing, so the observer ellipsoid stays on WGS-72 too.

**Deep-space (SDP4) deferred.** voidwatch's bundled targets (ISS,
Hubble, NOAA 19, Tiangong) are all near-Earth (period < 225 min).
Anything with period ≥ 225 min returns `SAT_DEEP_SPACE` from
`satellite_model_init` and does not propagate. The dscom / dpper /
dsinit / dspace body can land later without changing any public API.

**Look-angle convention** — north = 0 azimuth, east = π/2 (clockwise
from north, matching voidwatch's planet path). Altitude positive is
above the horizon.

**Stale-TLE policy** (enforced in `satellite_compute_all` and the
astro render path):

| TLE age            | Behavior                                       |
| ------------------ | ---------------------------------------------- |
| ≤ 7 days           | Normal (full brightness, full label)           |
| 7 < age ≤ 14 days  | Half-dim brightness, label fades               |
| 14 < age ≤ 30 days | Hidden from render; JSON shows `stale: true`   |
| > 30 days          | `compute_all` refuses; `valid: false`          |

This is policy, not SGP4 math — TLEs degrade ~1-3 km/day, so a fresh
fetch is preferable to inferring from old elements.

---

## Things voidwatch deliberately doesn't compute

For completeness, here are corrections / refinements voidwatch
does *not* apply, with notes on why:

- **ΔT (TT − UT1)** — ~70 seconds in 2026; introduces ~30″
  Sun-longitude error. Below cell scale. Adding ΔT becomes
  important if precision is upgraded.
- **Nutation full series** — voidwatch uses only the dominant
  Ω-term in nutation (Meeus 22.4 leading term). Full IAU 1980
  series has 106 terms; precision tier doesn't justify them.
- **Precession** — implicit in the J2000 element framework. We
  don't precess back to "of date" coordinates because we render
  apparent positions at JD directly via the Ω-term shortcut.
- **Aberration** (annual + diurnal) — only the dominant 0.00569°
  annual aberration term in Sun longitude. Not applied to other
  bodies because it's <20″ and invisible.
- **Light-time correction** — 8.3 minutes for the Sun, ~3 minutes
  for the inner planets, up to 4 hours for Neptune. Not applied;
  geocentric positions are computed at observer JD without
  retarded-time correction. Below cell scale for everything inside
  Saturn's orbit.
- **Geocentric → topocentric parallax** — only matters for the
  Moon (~1° at horizon). Skipped; we accept the error.
- **Phase angle in planet magnitudes** — only matters for inner
  planets (Mercury and Venus crescent phases). Skipped.
- **VSOP87** — the higher-precision planetary theory. Would replace
  Standish's elements with thousands of periodic terms per planet.
  Accuracy already below cell scale; VSOP87 satisfies a checklist,
  not a user.
- **Bessel elements** for solar eclipses — needed for path-of-
  totality predictions. Not applied; eclipse magnitude is geocentric
  only.
- **Saturn ring photometric corrections** — Müller's formula for
  ring-modulated apparent brightness. Skipped; ring tilt drives
  visual presentation only.
- **Bowell H-G phase function** for asteroids. Skipped; asteroid
  magnitudes are flat `H + 5 log(r·Δ)`.

---

## References

### Algorithm sources

- **Meeus, J.** (1998). *Astronomical Algorithms* (2nd ed.).
  Willmann-Bell. ISBN 978-0-943396-61-3.
  *The* reference for low-precision computational astronomy.
  voidwatch uses chapters 7, 12, 13, 22, 25, 47, 48 directly.
- **Standish, E. M.** (1992 / 2006 update).
  "Keplerian Elements for Approximate Positions of the Major
  Planets." JPL Solar System Dynamics, NASA.
  https://ssd.jpl.nasa.gov/planets/approx_pos.html
- **Bennett, G. G.** (1982). "The calculation of astronomical
  refraction in marine navigation." *Journal of Navigation* 35(2),
  255–259.
- **Kasten, F. & Young, A. T.** (1989). "Revised optical air mass
  tables and approximation formula." *Applied Optics* 28(22),
  4735–4738.
- **Snyder, J. P.** (1987). *Map Projections — A Working Manual*.
  USGS Professional Paper 1395.
- **Liu, J.-C., Zhu, Z., Hu, B.** (2011). "Reconsidering the
  galactic coordinate system." *Astronomy & Astrophysics* 526, A16.
- **Allen, C. W.** (1973 / 4e by Cox 2000). *Astrophysical
  Quantities*. AIP Press / Springer.
- **Smart, W. M.** (1977). *Textbook on Spherical Astronomy* (6e,
  rev. by Green). Cambridge University Press.
- **Danby, J. M. A.** (1992). *Fundamentals of Celestial Mechanics*
  (2nd ed.). Willmann-Bell.
- **Jenniskens, P.** (2006). *Meteor Showers and Their Parent
  Comets*. Cambridge University Press.
- **Hoots, F. R. & Roehrich, R. L.** (1980). "Spacetrack Report No. 3:
  Models for Propagation of NORAD Element Sets." NORAD/USAF.
- **Vallado, D. A., Crawford, P., Hujsak, R. & Kelso, T. S.** (2006).
  "Revisiting Spacetrack Report #3." AIAA 2006-6753 (AAS/AIAA
  Astrodynamics Specialist Conference).
  https://celestrak.org/publications/AIAA/2006-6753/

### Data sources

For catalogue / ephemeris data sources (HYG, Stellarium, JPL
Horizons, JPL SBDB, IAU Constellations, RASC), see `CITATIONS.md`.

---

## Where to dig deeper in the codebase

- `src/ephem.c` — the entire pure-math layer. ~410 lines, one
  function per body, no rendering.
- `src/astro.c:174-231` — refraction + extinction + airmass.
- `src/astro.c:1458-1521` — eclipse magnitude + visualisation.
- `src/comet.c` — Keplerian propagation + cometary magnitude.
- `src/asteroid.c` — same shape, planet-style magnitude.
- `src/headless.c` — `--validate` test cases.

If you're verifying an algorithm against this document, those are
the line ranges to read.
