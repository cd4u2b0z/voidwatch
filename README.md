# voidwatch

![Version](https://img.shields.io/badge/version-1.1.0-orange)
![C11](https://img.shields.io/badge/C-11-00599C?style=flat&logo=c&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat&logo=linux&logoColor=black)
![License](https://img.shields.io/badge/license-MIT%20source%20%E2%80%A2%20GPL--2.0%20binary-brightgreen)
![Lines](https://img.shields.io/badge/lines-10k%20src%20%2B%201.7k%20tests%20%2B%208870%20stars-lightgrey)
![Modes](https://img.shields.io/badge/modes-sandbox%20%2B%20astro-blue)
![Perspectives](https://img.shields.io/badge/perspectives-geo%20%2B%20helio-blueviolet)
![Catalog](https://img.shields.io/badge/sky-HYG%20v3.6.1%20%2B%20Stellarium%20%2B%2030%20DSOs-cyan)
![Satellites](https://img.shields.io/badge/satellites-SGP4%20%C2%B17nm%20vs%20Vallado-success)
![Tests](https://img.shields.io/badge/tests-9%2F9%20%E2%9C%93%20%E2%80%A2%20--validate%209%2F9-success)
![No deps](https://img.shields.io/badge/runtime%20deps-libc%20libm%20alsa%20fftw-success)

Terminal space observatory. Two flavours: a phosphorescent **N-body
sandbox** (orbital sim with chromatic body cores, Perlin nebulae, audio-
reactive supernovae) and a real-ephemeris **planetarium** (8 planets,
8870 stars, 88 IAU constellations, meteor showers, eclipses, comets,
asteroids, **near-Earth satellites via hand-built SGP4** , all from
real catalogues, all baked into the binary).

> *"Passive sensor feed" aesthetic — ambient, slow, never demands attention.*

### Sandbox

![sandbox demo](docs/sandbox.gif)

### Astro

![astro demo](docs/astro.gif)

*(Astro demo: real ephemeris → speed up 10× → toggle constellation lines
→ switch to heliocentric perspective → back to geocentric.)*

[Quick start](#quick-start) · [Modes](#modes) · [Headless / one-shot](#headless--one-shot) · [Keys](#keys) · [Configuration](#configuration) · [Data](CITATIONS.md)

---

## What it does

```
$ voidwatch --tonight
voidwatch — +32.78° -079.93°  2026-05-07 19:49 EDT

Body       Mag      Alt     Rise    Set
----       ---      ---     ----    ---
Sun        -26.74    +2.4°  06:30   20:01
Moon       -10.00   -66.9°  01:27   --:--
Mercury     -2.19    -5.6°  06:10   --:--
Venus       -4.39   +30.3°  08:10   22:23
Mars        +0.95   -22.7°  05:16   --:--
Jupiter     -2.04   +59.9°  10:37   00:42
Saturn      +1.06   -34.2°  04:49   --:--
Uranus      +5.81   +14.6°  07:12   21:03
Neptune     +7.94   -40.0°  04:28   --:--

Active meteor shower: Eta Aquariids — current rate ≈ 30.1/hr (peak ZHR 60 on DOY 126)

Asteroids (mag ≤ 9.5):
  1 Ceres     mag +6.59  alt +19.4°
  2 Pallas    mag +8.26  alt +14.4°
  4 Vesta     mag +7.34  alt +62.3°
  7 Iris      mag +9.30  alt +64.4°
```

That's voidwatch in headless mode. The interactive mode is a sub-pixel
Braille framebuffer rendering the same data as a live, zenith-projected
all-sky planetarium — astroterm for people who'd rather not spend their
CPU on Wine.

---

## Quick start

```bash
git clone https://codeberg.org/cdubz/voidwatch.git
cd voidwatch
make
make install            # → ~/.local/bin/voidwatch (PREFIX=$HOME/.local default)
voidwatch --astro --no-audio
```

`make install` puts the binary on your PATH (`~/.local/bin` is in
PATH on most modern setups; if not, override with
`make install PREFIX=/usr/local` and `doas`). To remove:
`make uninstall`.

You can also skip install and run from the build directory with
`./voidwatch …` — every flag works the same way.

Press `?` in-app for the key reference. `q` or `Esc` to quit.

If you want the ALSA-driven audio reactivity (sandbox supernova triggers
on bass spikes, render-camera shake on sub-band hits), drop `--no-audio`
and either talk into your mic or route desktop audio:

```bash
PULSE_SOURCE=$(pactl list short sources | awk '/monitor/{print $2; exit}') \
VOIDWATCH_AUDIO_DEVICE=pulse \
./voidwatch
```

---

## Modes

### Sandbox  (`./voidwatch`)

The default. A symplectic-Euler N-body simulation: a star + 5 planets +
a neutron star + a black hole + a violet nebula core, drifting on stable
orbits for hours. Phosphorescent trails from a fading float framebuffer,
Perlin-noise nebula wash with parallax, particle photons / solar wind /
debris, audio-reactive band shake, occasional supernova that converts
a star into a particle shell.

Fixed-timestep physics at 120 Hz, render-rate independent. No `malloc`
in the hot path.

### Astro  (`./voidwatch --astro`)

Real ephemeris with two perspectives — geocentric (default) and
heliocentric — toggle with **`m`**.

**Geocentric** (`m` = 0): observer's all-sky view, azimuthal-equidistant
projection from zenith. 8870 stars (HYG v3.6.1, V ≤ 6.5), 88 IAU
constellations (Stellarium "modern" skyculture), Sun + Moon + 8 planets
via Meeus, Milky Way band as a longitude-weighted Perlin overlay along
the J2000 galactic equator. Bennett refraction lift, Kasten-Young
airmass dimming, twilight horizon glow keyed off Sun altitude.

Plus: lunar phase + earthshine, **solar/lunar eclipses** (real angular
separation gating), 9 annual **meteor showers** (DOY-driven activity
profile), 6 bundled **comets** (1P/Halley, 2P/Encke, 109P/Swift-Tuttle,
21P/Giacobini-Zinner, 67P/Churyumov-Gerasimenko, Hale-Bopp) propagated
two-body Keplerian, 5 bundled **asteroids** (Ceres, Pallas, Vesta, Juno,
Iris), 30 named **deep-sky objects** (M31 Andromeda, M42 Orion Nebula,
M45 Pleiades, M57 Ring, Omega Centauri, etc. — toggle `d`), 4 bundled
**near-Earth satellites** (ISS, Hubble, NOAA 19, Tiangong/CSS — toggle
`i`) propagated through a hand-built SGP4 validated to ~7 nm against
Vallado's published vectors, and optional **aurora** shimmer near the
poleward horizon (toggle `a`).
Saturn rings tilt with the 29.46-yr ring-plane cycle. Galilean moons
orbit Jupiter visibly. The HUD event log narrates shower activity and
eclipses as they begin/end.

**Heliocentric** (`m` = 1): top-down "looking at the solar system from
above the ecliptic." Sun fixed at centre, Earth + 7 planets at their
heliocentric ecliptic positions on a `sqrt(r_au)` compressed scale —
Mercury a few sub-pixels from the Sun, Neptune at the frame edge.
**Orbital ellipses** drawn as faint dotted traces (one per planet,
sampled across one full period). Decorative parallax star backdrop on
by default (toggle `s`). Crank the time-scrub and watch the inner
planets whip around their orbital traces while Neptune barely moves.

Time controls: `+`/`-` speed (×10 steps), `0` reset, `,`/`.` scrub ±1h.
Cursor pick (`c` then `hjkl`) lands on planets, comets, or asteroids
and pins the scan readout to whatever's nearest.

Location resolves: `--lat`/`--lon` → `~/.config/voidwatch/location.conf`
→ `$VOIDWATCH_LAT` / `$VOIDWATCH_LON` → fallback.

---

## Headless / one-shot

For shell scripting, status bars, and "what's up tonight?":

```bash
voidwatch --tonight              # text summary (sample above)
voidwatch --print-state          # text columns
voidwatch --print-state --json   # JSON for piping
voidwatch --next mars            # "Mars rises 2026-05-08 05:16 EDT (in 9h26m)"
voidwatch --year 2026            # full annual almanac (sorted)
voidwatch --validate             # internal sanity tests vs JPL refs
voidwatch --snapshot 200 60      # render one frame as ANSI to stdout
voidwatch --astro --at 2061-07-28  # virtual clock starts at Halley's perihelion
```

These bypass terminal init, audio init, and the render loop — pure
compute + print + exit. Pipe into `jq`:

```bash
voidwatch --print-state --json | jq '.planets[] | select(.alt_deg > 0)'
```

`--at <YYYY-MM-DD[Thh:mm:ss]>` works with any of these *and* with
`--astro` itself — handy for visiting historical or future skies
(2024 Great American Eclipse, 1997 Hale-Bopp, 2061 Halley return).

---

## Keys

```
  q / Esc   quit
  h         toggle HUD
  ?         help overlay

  astro mode (--astro)
  + / -     speed up / slow down (10x)
  0         reset speed + scrub
  , / .     scrub time -1h / +1h
  j         jump to next event (eclipse / conjunction / shower)
  /         search: type a body name, Enter jumps + auto-tracks
  m         toggle geo / helio view
  s         toggle decorative star backdrop (geo + helio)
  g         toggle alt-az grid
  l         toggle constellation lines
  d         toggle deep-sky objects (M31, M42, …)
  a         toggle aurora
  i         toggle satellites (ISS, HST, NOAA 19, CSS)
  t         toggle planet trails (lowercase)
  T         toggle track mode (uppercase — cursor follows nearest body)
  c         toggle object cursor (then hjkl, Esc to exit)
  click     move cursor to clicked cell + arm track on nearest body
```

---

## Configuration

### Themes

`~/.config/voidwatch/theme.conf` (auto-loaded; `--theme <path>` to
override). Colour-only config:

```ini
void          = #0a0a14
star_m        = #ff8060
star_g        = #ffeec0
star_b        = #c8d8ff
nebula_violet = #6048a0
nebula_crimson = #c04060
hud           = #00ff88
hud_alert     = #ff6600
```

Wallust integration: drop a template at
`~/.config/wallust/templates/voidwatch.conf` with
`target = "~/.config/voidwatch/theme.conf"` and `wset` regenerates the
theme automatically.

### Runtime knobs

`~/.config/voidwatch/config.toml` (auto-loaded; `--config <path>` to
override). Currently exposed:

```toml
[visual]
fb_decay = 0.92                  # sandbox trail decay (astro always clears)

[astro]
star_mag_cutoff     = 6.5        # naked-eye limit; drop to 4.5 for "city sky"
comet_mag_cutoff    = 8.0
asteroid_mag_cutoff = 9.5

[sandbox]
gravity_g = 0.25
```

### Location

`~/.config/voidwatch/location.conf`:

```ini
lat = 32.78
lon = -79.93
```

No network calls. No geoclue. No DBus. By design.

---

## CLI

```bash
voidwatch                          # sandbox mode
voidwatch --astro                  # planetarium
voidwatch --no-audio               # skip ALSA capture
voidwatch --device <name>          # pick a PCM source
voidwatch --list-devices           # enumerate ALSA sources

voidwatch --lat 32.78 --lon -79.93 # observer location
voidwatch --theme path/to/theme.conf
voidwatch --config path/to/config.toml

voidwatch --tonight                # one-shot summary
voidwatch --print-state            # full ephemeris (text)
voidwatch --print-state --json     # full ephemeris (JSON)
voidwatch --next <body>            # next rise time

voidwatch --help
```

---

## Building from source

Requirements:

- C11 compiler (`gcc` or `clang`)
- `make`
- `libasound2-dev` / `alsa-lib` (audio capture)
- `libfftw3-dev` / `fftw` (FFT for audio bands)
- `libm`, `libpthread` (libc)

That's it. No CMake, no autotools, no package manager beyond your
distro's. The Makefile auto-globs `src/*.c`.

```bash
make            # build
make run        # build + run sandbox
make astro      # build + run --astro
make clean
```

Refreshing the bundled star + constellation tables (developer task,
not needed for end users):

```bash
# tools/data/ is gitignored; fetch upstream catalogues once
mkdir -p tools/data
curl -L -o tools/data/hyg_v36_1.csv.gz \
    https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/v3/hyg_v36_1.csv.gz
gunzip -k tools/data/hyg_v36_1.csv.gz
curl -L -o tools/data/stellarium_modern.json \
    https://raw.githubusercontent.com/Stellarium/stellarium/master/skycultures/modern/index.json

python3 tools/gen_skydata.py     # regenerates src/skydata.c
make
```

---

## Status

**Done.** Everything below is shipped, tested, and pushed to both
[Codeberg](https://codeberg.org/cdubz/voidwatch) and
[GitHub](https://github.com/cd4u2b0z/voidwatch).

- **Sandbox mode** — N-body simulation with phosphorescent trails,
  Perlin nebulae, particle FX (5 kinds), audio-reactive supernovae,
  three exotic body kinds (neutron star, black hole, nebula core).
- **Astro mode** — real ephemeris with two perspectives (`m`):
  - **Geocentric**: 8870 stars (HYG v3.6.1), 88 IAU constellations
    (Stellarium), Sun + Moon + 8 planets (Meeus + Standish), Milky
    Way band, Bennett refraction + Kasten-Young airmass extinction
    + twilight tint, sporadic meteors (~8/hr) + 9 named showers with
    per-shower colour & velocity, solar/lunar eclipses with live
    magnitude, planet-planet conjunctions, Moon-planet close passes,
    30 named DSOs (M31, M42, M45, M57, Pleiades, Omega Centauri, …),
    aurora with Kp gating and substorm flares.
  - **Heliocentric**: top-down solar system. Sun-centred, all 8 planets
    + Earth + 6 comets + 5 asteroids, full orbital traces, sqrt-scaled
    distances, decorative parallax backdrop, helio-specific scan
    readout (heliocentric distance + orbital period).
  - **Brightness compensation** across time-scrub speed: `bright_boost`
    scales per-frame intensity inversely with `fb_decay` so steady-
    state bloom stays consistent from 1× through ≥1000× scrub.
- **Near-Earth satellites** — hand-built SGP4 (Hoots-Roehrich 1980 /
  Vallado 2006), validated to ~7 nm position agreement against the
  published `tcppver.out` test vectors. Bundled catalog: ISS, HST,
  NOAA 19, Tiangong/CSS. TLE-age policy: dim past 7 d, hidden past
  14 d, refused past 30 d. JSON exposes age + stale flag. Cursor
  pick + track + name/alias/catalog search; `/iss` resolves and
  auto-jumps to the next AOS. `--update-tle` opt-in refresh from
  CelesTrak (rate-limited 2 h, writes user cache, atomic rename,
  bundled stays as offline-first fallback). Deep-space SDP4 refused
  cleanly with `SAT_DEEP_SPACE` — no silent wrong answers for
  GEO/Molniya.
- **HUD event log** — narrates shower activity, eclipse begin/peak/
  end, conjunctions, Moon close passes on transition.
- **Headless modes** — `--tonight`, `--print-state [--json]`,
  `--next <body>` (planets/comets/asteroids/satellites), `--year`
  (full annual almanac), `--snapshot`, `--validate` (9/9 against
  Meeus's published worked examples + JPL Horizons cross-checks for
  Mercury/Mars/Jupiter/Saturn at J2000.0), `--update-tle`.
- **Interactive controls** — time scrub (`+`/`-`/`,`/`.`/`0`), event
  jump (`j`), in-program search with auto-track (`/`), track mode
  (`T`), cursor pick (`c` + hjkl, or mouse-click), 9 toggle keys
  (grid/lines/DSOs/aurora/satellites/trails/star-backdrop/
  geo-helio/cursor).
- **Configuration** — runtime TOML knobs (`~/.config/voidwatch/config.toml`),
  wallust-driven theming (`~/.config/voidwatch/theme.conf`), location
  resolution (`--lat/--lon` → file → env → fallback), `--at <iso-date>`
  for arbitrary virtual time.
- **Tests** — 9 binaries via `make test`: framebuffer math, projection
  round-trip (machine-epsilon), JSON regression vs. golden, n-body
  symplectic-Euler conservation, TLE parser (12 cases), SGP4 init
  (7 cases), SGP4 propagation against Vallado vectors (7-nm
  agreement), satellite look-angle synthetic geometry, satellite
  bundle smoke. All green.
- **Maintainer tooling** — `tools/gen_skydata.py` (HYG + Stellarium
  → `src/skydata.c`), `tools/gen_satellites.py` (CelesTrak →
  `src/satdata.c`). End users build with no Python and no network.
- **Documentation** — [`ARCHITECTURE.md`](ARCHITECTURE.md) (build
  journey + design rationale), [`ASTRONOMY.md`](ASTRONOMY.md) (per-
  algorithm citations + `--validate` test table),
  [`CITATIONS.md`](CITATIONS.md) (data-source credits),
  [`THIRD_PARTY_LICENSES.md`](THIRD_PARTY_LICENSES.md) (per-component
  obligations), [`docs/voidwatch.1`](docs/voidwatch.1) (man page,
  `man -l` renderable), [`docs/voidwatch.page.md`](docs/voidwatch.page.md)
  (tldr-pages style).

Open low-priority polish (none of these is missed in normal use):
lunar standstills, eclipse path-of-totality, inotify hot-reload of
runtime config, audio reactivity in astro mode, GeoNames `--city`
shorthand, expand `--validate` to comets/asteroids.

Permanently deferred with documented reasons: VSOP87 (Meeus's
arcminute precision is already invisible at terminal cell scale),
deep-space SDP4 (every voidwatch satellite target is near-Earth;
refusing GEO/Molniya cleanly is the right call), runtime data engine
(would make voidwatch a worse Stellarium).

---

## Don't

- Don't lower `LUM_THRESHOLD` below ~0.03 — void cells start lighting up
  from accumulated noise floor.
- Don't `fb_clear` in sandbox-mode trail frames; `fb_decay` is the clear.
- Don't sample Perlin per sub-pixel. Once per cell is what the eye reads
  through Braille anyway, and it's 8× cheaper.
- Don't add a body kind without picking a stable initial velocity — an
  unbalanced N-body system will chaos within seconds.
- Don't expect ephemeris precision better than a few arcminutes — that's
  what Meeus low-precision gives, and a terminal cell is bigger than
  that anyway. For VSOP87-level accuracy, use a real planetarium.

---

## License

voidwatch is a combined work with per-component licensing — read
[LICENSE](LICENSE) and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md)
in full before redistributing. Short summary:

- **Original voidwatch source** — MIT.
- **Bundled `sky_stars[]` (HYG Database)** — CC BY-SA 2.5
  (share-alike; redistribute the table portion under the same terms).
- **Bundled `sky_lines[]` (Stellarium constellation figures)** —
  GPL-2.0. Because this data compiles into the binary, **the
  voidwatch binary as distributed is a combined work governed by
  GPL-2.0**. Source is publicly hosted, which satisfies GPL §3(a).
- **JPL Horizons/SBDB orbital elements + SGP4 reference algorithm** —
  US Government public domain.
- **CelesTrak TLE data** — factual; attribution requested.
- **Vendored `stb_perlin.h`** — public domain / MIT-0 at the user's
  option.

Forks that need pure-MIT downstream can replace the Stellarium-derived
constellation lines with a public-domain or permissively-licensed
source (one afternoon's work — see THIRD_PARTY_LICENSES.md). The HYG
star table can stay under CC BY-SA 2.5 as long as attribution +
share-alike notices survive.

Full data-source attribution lives in [CITATIONS.md](CITATIONS.md).
