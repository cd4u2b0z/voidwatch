# voidwatch

![C11](https://img.shields.io/badge/C-11-00599C?style=flat&logo=c&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-FCC624?style=flat&logo=linux&logoColor=black)
![License](https://img.shields.io/badge/license-MIT-brightgreen)
![Lines](https://img.shields.io/badge/lines-6.4k%20+%208870%20stars-lightgrey)
![Modes](https://img.shields.io/badge/modes-sandbox%20%2B%20astro-blue)
![Catalog](https://img.shields.io/badge/sky-HYG%20v3.6.1%20%2B%20Stellarium-cyan)
![No deps](https://img.shields.io/badge/runtime%20deps-libc%20libm%20alsa%20fftw-success)

Terminal space observatory. Two flavours: a phosphorescent **N-body
sandbox** (orbital sim with chromatic body cores, Perlin nebulae, audio-
reactive supernovae) and a real-ephemeris **planetarium** (8 planets,
8870 stars, 88 IAU constellations, meteor showers, eclipses, comets,
asteroids — all from real catalogues, all baked into the binary).

> *"Passive sensor feed" aesthetic — ambient, slow, never demands attention.*

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
./voidwatch --astro --no-audio
```

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
Iris). Saturn rings tilt with the 29.46-yr ring-plane cycle. Galilean
moons orbit Jupiter visibly. The HUD event log narrates shower activity
and eclipses as they begin/end.

**Heliocentric** (`m` = 1): top-down "looking at the solar system from
above the ecliptic." Sun fixed at centre, Earth + 7 planets at their
heliocentric ecliptic positions on a `sqrt(r_au)` compressed scale —
Mercury a few sub-pixels from the Sun, Neptune at the frame edge.
Decorative parallax star backdrop on by default (toggle with `s`).
Crank the time-scrub and watch the inner planets whip around while
Neptune barely moves.

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
voidwatch --next mars            # "Mars rises 2026-05-08 05:16 EDT  (in 9h26m)"
voidwatch --next vesta           # works for any bundled body name
```

These bypass terminal init, audio init, and the render loop — pure
compute + print + exit. Pipe into `jq`:

```bash
voidwatch --print-state --json | jq '.planets[] | select(.alt_deg > 0)'
```

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
  m         toggle geo / helio view
  s         toggle helio star backdrop
  g         toggle alt-az grid
  l         toggle constellation lines
  t         toggle planet trails
  c         toggle object cursor (then hjkl, Esc to exit)
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

- **Sandbox mode** — phases 1-5 LANDED (starfield, parallax, drift, N-body
  gravity, phosphorescent trails, Perlin nebula, particle FX, audio
  reactivity, HUD). Three exotic body kinds (neutron star, black hole,
  nebula core) ride on stable orbits.
- **Astro mode** — Tiers 1-4c LANDED (bundled bright stars, constellations,
  cardinal markers, Milky Way, moon phases, refraction, extinction,
  twilight, body trails, Galilean moons, Saturn rings, sky grid, object
  cursor, meteor showers, eclipses, comets, asteroids).
- **Tier 5** — HYG v3.6.1 + Stellarium modern skyculture LANDED with
  `tools/gen_skydata.py`. Named DSO catalogue, aurora effects, GeoNames
  city lookup all open.
- **Heliocentric view** — LANDED (`m` toggle). Top-down solar system
  with sqrt-scaled distances. Orbital path traces, comets/asteroids in
  helio, and a helio-specific scan readout still open.
- **HUD event log** — LANDED. Meteor shower / eclipse activity now
  announces on transition.
- **Headless** — `--tonight`, `--print-state`, `--next` LANDED.
- **Runtime config (TOML subset)** — LANDED. Inotify hot-reload still
  open.

Open polish: track mode (`T`), `--validate` self-test, in-program
search, inotify hot-reload, audio reactivity in astro mode.

Sources are fully cited in [CITATIONS.md](CITATIONS.md).

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

[MIT](LICENSE) for the voidwatch source. Bundled astronomical catalogues
(HYG, Stellarium, JPL Horizons / SBDB) keep their upstream terms — see
[CITATIONS.md](CITATIONS.md) for full attribution.
