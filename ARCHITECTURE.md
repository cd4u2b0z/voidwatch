# Architecture

The technical architecture of voidwatch, written as a walkthrough of how
the project came together — what was built, in what order, and *why*. If
you've never built a renderer or an ephemeris from scratch and want to
see how a coder's brain breaks "I want a quiet space porthole in my
terminal" into ~16,000 lines of working C, this document is for you.

This isn't a reference manual. The headers are the manual. This is the
inside of the build.

Companion docs:
- `README.md` — user-facing intro
- `ASTRONOMY.md` — per-algorithm citations (Meeus chapters, etc.)
- `CITATIONS.md` — data-source credits (HYG, Stellarium, JPL, IAU)

---

## Vision

voidwatch is a terminal space observatory. Two modes share one
rendering pipeline:

- **Sandbox** (`voidwatch`) — N-body gravitational simulation with
  parallax stars, Perlin nebulae, particle FX, and audio reactivity.
  A "passive sensor feed" you leave running in a side panel.
- **Astro** (`voidwatch --astro`) — real ephemeris of the Sun, Moon,
  8 planets, ~8870 stars, 88 constellations, the Milky Way band,
  6 comets, 5 asteroids, 30 deep-sky objects, eclipses, meteor showers,
  and aurora — all rendered through the same sub-cell Braille
  framebuffer.

Both are built to be ambient. Slow drift, soft bloom, no UI shouting
at you. The point is to look up from your editor and see something
quietly correct happening.

The whole thing runs in C with libc + libm + libpthread + ALSA + FFTW3.
No D-Bus, no network, no runtime data files, no daemons. A fresh
`git clone && make && ./voidwatch` on a stock Arch box runs offline.

---

## Why I Built This

I wanted something to look at while working that wasn't a chat
notification or a Discord ping. terminal-based ambient stuff is a tiny
genre — astroterm, [pipes.sh], asciiquarium — and the existing options
are either too random (aquarium fish) or too close to a real planetarium
(astroterm is wonderful but heavy on identification, light on mood).

The thing that didn't exist yet: an *ambient* observatory. Real-position
stars and planets, but rendered like the slow horizon-glow of an empty
space station — no constant identification labels, no jittery
animations, no menu chrome. Sub-cell Braille rendering plus a float
framebuffer with phosphorescent decay was the trick that made it
visually distinct.

Sandbox mode came first because gravitational simulation is *fun* and
because rendering trails through `fb_decay` is one of those tricks that
looks like a lot of work and is actually a single multiply per pixel.
Once the rendering pipeline existed, swapping the N-body sim for a real
ephemeris felt obvious — same framebuffer, same composite rules, just
new sources of (x,y) and (r,g,b).

That's it. It's a hobby project that ended up working well enough I
shipped a v1.

---

## The Mind of the Project

These are the principles that shaped every decision. Every concrete
choice further down feels inevitable instead of arbitrary if you read
this section first.

1. **Float framebuffer, integer terminal.** Physics, blending, gamma
   all happen in linear-space float RGB at sub-cell resolution. The
   conversion to 0–255 ANSI truecolor is the *last* step. Doing
   compositing in 8-bit space is how you get banded gradients and
   crunchy edges.
2. **Decay is the clear.** `fb_clear` is the special case. The
   default is `fb_decay(0.92)` per frame — every pixel fades to ~5%
   in a second. That decay tail is the trail, the bloom, and the
   ambient warmth of the picture. One multiply per sub-pixel.
3. **No malloc in the hot path.** Every per-frame buffer is allocated
   at init or window resize and reused forever. The render loop
   should never touch the allocator.
4. **No runtime data files, no network, no D-Bus.** Catalogues are
   baked into the binary at build time. Location is CLI / file / env.
   Audio is ALSA-direct. The portability tax of session buses is
   real and we don't pay it.
5. **Audio is best-effort.** ALSA/FFTW failures collapse every
   modulation factor to identity (multiply by 1.0). The program never
   exits because audio doesn't work.
6. **Astro and sandbox don't share state.** The N-body sim is wholly
   decoupled from the ephemeris. Switching modes doesn't require
   either to know about the other.
7. **Compile-time `#define` for tuning, runtime config for ergonomics.**
   Most numeric tunables stay `#define`s in `config.h`. Only the
   knobs a user *would reasonably want to retune without rebuilding*
   (palette, magnitude cutoffs, fb_decay, gravity_g) live in the
   runtime config layer.
8. **Cite your sources.** Every algorithm has a paper / book chapter
   it came from. Every catalogue has a credit. Astronomy is full of
   "magic constants" — make them traceable.

These weren't all there on day one. Some emerged from pain (see
"Lessons" at the bottom).

> **PRO MOVE** — Principles like these are what let a project stay
> coherent across a multi-month build. Write yours down before you
> need them. When a decision feels ambiguous, check the list. If no
> principle covers it, you've found a new one — add it.

---

## Architecture Overview

```
+---------------------------------------------------------------------+
|                            voidwatch                                 |
+---------------------------------------------------------------------+
|                                                                      |
|  +------------+    +-------------+    +------------------+           |
|  |   CLI      |--->|   main.c    |--->|   Render loop    |           |
|  | (argv)     |    | (dispatch)  |    | (60 Hz frame +   |           |
|  +------------+    +------+------+    |  120 Hz physics) |           |
|                           |           +-------+----------+           |
|        +------------------+                   |                      |
|        |                                      v                      |
|        |  +-------------------+   +-----------------------------+    |
|        |  |   Headless mode   |   |        Composite             |   |
|        |  | (--tonight, etc.) |   |  decay  -> bg layers ->      |   |
|        |  | compute, print,   |   |  bodies -> render_flush ->   |   |
|        |  | exit (no term,    |   |  cursor-positioned overlays  |   |
|        |  | no audio)         |   +-------------+----------------+   |
|        |  +-------------------+                 |                    |
|        |                                        v                    |
|        |               +---------------------------------+           |
|        |               |  Float RGB framebuffer          |           |
|        |               |  (cols*2 x rows*4 sub-pixels)   |           |
|        |               |  fb_add | fb_max | fb_decay     |           |
|        |               +-----------------+---------------+           |
|        |                                 |                           |
|        |                                 v                           |
|        |               +---------------------------------+           |
|        |               | render_flush -> Braille glyphs  |           |
|        |               | + ANSI truecolor + gamma 1.18   |           |
|        |               +---------------------------------+           |
|        |                                                             |
|  +-----+---------------+    +----------------------------+           |
|  |  Audio worker       |    |  Compile-time catalogues   |           |
|  |  (ALSA + FFTW3)     |    |  HYG (~8870 stars),        |           |
|  |  thread, snapshot   |    |  Stellarium constellations,|           |
|  |  via mutex          |    |  Messier/NGC DSOs,         |           |
|  +---------------------+    |  meteor showers, comets,   |           |
|                             |  asteroids                 |           |
|                              +---------------------------+           |
+---------------------------------------------------------------------+
```

Two modes, one pipeline. The pipeline is the framebuffer; the modes
just disagree on what to stamp into it.

---

## Build Journey (Chronological)

This is the part that's hard to find in normal architecture docs: the
*order* things were built and why. Skipping ahead would have made each
later layer worse.

### Phase 1 — Starfield + parallax + camera drift

Before I'd written a single body or a single physics step, I built the
framebuffer + the Braille flush + a 3-layer parallax starfield. That
was the entire program for the first few days: stars drifting past at
three different rates, with twinkle, in a terminal.

**Why first?** Because the rendering trick is the part of the project
that could fail. If sub-cell Braille looks wrong, or `fb_decay`-as-trail
is unreadable, or ANSI truecolor has banding — none of the rest of the
project saves it. Get the trick to work in isolation, then build on
top.

The Braille block at U+2800–U+28FF gives 256 distinct 2×4-dot glyphs.
Each terminal cell becomes 8 sub-pixels. At 80×24 you suddenly have
160×96 effective resolution. That's enough headroom for smooth
sub-pixel motion and Gaussian-bbox bloom that reads as light, not
pixels.

```c
// framebuffer.h — the entire write API, in two inlines
static inline void fb_add(Framebuffer *fb, int x, int y,
                          float r, float g, float b) {
    if ((unsigned)x >= (unsigned)fb->sub_w) return;
    if ((unsigned)y >= (unsigned)fb->sub_h) return;
    float *p = &fb->data[(y * fb->sub_w + x) * 3];
    p[0] += r; p[1] += g; p[2] += b;
}

static inline void fb_max(Framebuffer *fb, int x, int y,
                          float r, float g, float b) {
    /* per-channel max, not accumulating */
}
```

`fb_add` accumulates (good for things that should leave trails when
combined with decay — bodies, particles). `fb_max` pegs to a target
brightness without accumulating (good for things that should hold
steady — stars, nebula wash, Milky Way). Picking the right write
primitive for each layer is half the visual identity.

> **TIP** — Branchless inlines with unsigned-cast bounds checks are
> the difference between "looks fine in a profiler" and "the inner
> loop is invisible." Casting `int x` to `unsigned` and comparing
> against `sub_w` catches both negative and out-of-range in one
> compare instead of two.

> **GOTCHA** — `LUM_THRESHOLD = 0.04`. Drop it below ~0.03 and the
> Braille pass starts lighting up cells from accumulated noise floor —
> the void stops being void. There's a one-line comment in
> `config.h` so this doesn't get re-tweaked.

### Phase 2 — Bodies + N-body gravity + trails

Once stars worked, I added bodies. A `BodySystem` with a fixed-cap
array, a `body_step(dt)` that does semi-implicit Euler with force
softening, and a `body_draw` that stamps a Gaussian glow at each body.

The decision that made trails work for free: stamp bodies with `fb_add`
into the framebuffer, then let `fb_decay(0.92)` per frame do the rest.
The body fades behind itself as it moves. That's the whole effect.

Why semi-implicit (symplectic) Euler instead of plain Euler or RK4?

- **Plain Euler** drifts: energy grows monotonically, near-circular
  orbits spiral outward over hours.
- **RK4** is great for transient stuff but it's not symplectic —
  energy still drifts, just more slowly.
- **Semi-implicit Euler** is symplectic. It conserves phase-space
  volume, so energy oscillates within a bounded range instead of
  drifting. Orbits stay stable for hours of wall-clock runtime.

It's also two lines:

```c
v += a * dt;     // velocity from current acceleration
p += v * dt;     // position from new velocity
```

For a sandbox where the user is watching for ambient mood, not a
research-grade simulation, that's the right cost/benefit.

> **PRO MOVE** — Force softening (`r² + ε²` in the denominator instead
> of just `r²`) is what makes 1/r² gravity safe at small distances.
> Without it, two bodies drifting close enough to share a sub-pixel
> generate a force vector the size of the universe and your sandbox
> teleports to infinity. `GRAVITY_SOFTEN_SQ = 1.0` (one sub-pixel
> squared) is the floor.

> **GOTCHA** — Don't add a body kind without picking a stable initial
> velocity. An unbalanced N-body system goes chaotic within seconds.
> Each body kind in voidwatch (`PLANET`, `STAR`, `BLACK_HOLE`,
> `NEUTRON_STAR`, `NEBULA_CORE`) spawns at a config-pinned orbital
> fraction with `v = sqrt(GM/r)` for circular orbit around the
> central mass.

### Phase 3 — Particles + Perlin nebula

Particles came next. A fixed-arena `ParticleArena` with five kinds
(dust, gas, debris, photon, solar wind). The "fixed arena" part is the
trick: pre-allocate the cap, prefer `PK_DEAD` slots, fall back to
round-robin overwrite when saturated. No `malloc` ever runs in the
update loop.

```c
typedef struct {
    Particle *arr;
    int       cap;
    int       cursor;   // round-robin overwrite point
} ParticleArena;
```

Then the Perlin nebula. Two octaves of `stb_perlin` (vendored,
public-domain, single header), sampled *once per cell* not once per
sub-pixel. Why? Because the eye reads a Braille cell as a unit anyway —
sub-cell Perlin variation is invisible — and per-cell sampling is 8×
cheaper.

> **TIP** — When you're tempted to sample a noise function per pixel,
> ask: "what's the smallest spatial frequency the user can actually
> resolve?" If the answer is "one cell," sample per cell. The 8×
> speedup compounds across every nebula layer you might add later.

> **EASTER EGG** — `NEBULA_DRIFT_BIG = 0.004`. The nebula moves, but
> imperceptibly. Crank it past `0.01` and the wash starts to read as
> *flicker* or *vibration* — a UI bug, not deep space. Kept very low
> on purpose.

### Phase 4 — ALSA + FFTW3 audio reactivity

The audio worker was where I learned to take "best-effort" seriously.
ALSA capture in a worker thread, FFTW3 (single-precision) FFT, six
log-spaced bands per spec, per-band AGC + envelope follower. The
render thread reads a fresh value-copied snapshot once per frame and
threads it through every draw call.

```c
// audio.h
typedef struct {
    float bands[BAND_COUNT];  // AGC-normalised 0..1
    float transient;           // fast-decay onset detector
    int   active;              // 1 once worker has produced data
} AudioSnapshot;
```

The single most important design choice: **`audio_init()` failure is
non-fatal**. The capture thread never starts; `audio_snapshot()`
returns zeros forever; every modulation factor is `1.0 + 0.0 * MOD_X`
which is identity. The user just runs without reactivity.

This is *also* what makes `--no-audio` cheap: it's the same code path,
gated on a flag.

```c
// main.c
int audio_rc = no_audio ? 0 : audio_init(device);
// ... later, every frame:
AudioSnapshot snap = audio_snapshot();   // zeros if init failed
```

> **PRO MOVE** — Any subsystem you cannot guarantee will work on the
> user's machine should have a *coherent identity-element fallback*.
> "audio fails → bands are zero → factors are 1.0 → nothing visibly
> changes" is identity. The opposite, "audio fails → program exits"
> or "audio fails → some draws crash," is what you get when you don't
> design the fallback in from day one.

> **GOTCHA** — `BAND_SUB` (sub-bass) drives camera shake. But only
> the *render* camera, never the *physics* camera. If physics
> position depends on audio, the simulation becomes non-deterministic
> and any audio glitch corrupts orbital state. Audio is purely
> visual.

### Phase 5 — HUD + sectors + runtime config

The HUD is direct cursor-positioned ANSI emitted *after* the
framebuffer flush, not composited into the framebuffer. Why? Because
HUD widgets are sharp text — they shouldn't be subject to gamma
grading, decay, bloom, or any of the framebuffer's analog-feel
treatments. Bypassing the framebuffer keeps them crisp.

```c
// composite order (main.c)
fb_decay(...);            // 1. clear-via-fade
nebula_draw(...);         // 2. background
starfield_draw(...);      // 3. background
particle_draw(...);       // 4. additive accumulation
body_draw(...);           // 5. additive accumulation
render_flush(fb, stdout); // 6. fb -> Braille + truecolor
starfield_spikes(...);    // 7. cursor-positioned glyph overlays
hud_draw(...);            // 8. cursor-positioned HUD
hud_help_overlay(...);    // 9. cursor-positioned help (top of stack)
```

Everything from `fb_decay` through `render_flush` writes into the
framebuffer and gets gamma-graded together. Everything after
`render_flush` is direct ANSI that draws *on top* — outside the
analog-feel pipeline.

---

### Polish: chromatic body cores + colour grading

After the phases were done, a polish pass turned a "looks decent"
project into one I was happy to ship.

**Chromatic body cores** — each star/neutron-star stamp blends
quadratically from its tinted halo toward white at the bright centre.
Reads as a hot core with a coloured fringe instead of a flat blob.

**Gamma 1.18 + blue-shadow lift** — at flush time, the float pixel
gets `pow(c, 1/1.18)` (deepens blacks) and a quadratic lift on the
blue channel for low-intensity pixels (atmospheric depth in the
shadows). One multiply, one pow, one lerp per pixel. Massive
atmospheric improvement.

**Single-pixel dust** — early dust particles were a 5-cell `+` cross.
They read as deliberate sparkles, which is wrong for ambient
background dust. Dropped to a single sub-pixel. Photons and wind kept
the small cross because they actually *move*, and the halo reads as
bloom on the streak.

> **TIP** — Most "atmospheric depth" effects are one-line additions
> to your gamma function. Quadratic blue-shadow lift in voidwatch is
> literally `b += k * (1.0 - b) * (1.0 - b)`. Try it. It's free.

---

### Astro mode — Sun + Moon + 8 planets

Switching from "made-up sandbox" to "real ephemeris" is the architectural
hinge of the whole project. The good news is that the rendering
pipeline didn't change at all. Bodies became `EphemPosition`s; the
projection from `(alt, az)` to `(col, row)` is azimuthal-equidistant
centered on zenith; the framebuffer pipeline doesn't care where the
coordinates came from.

The ephemeris is a mix of two reference sources. Sun and Moon come
from Jean Meeus's *Astronomical Algorithms* (2nd ed.) — the gold
standard for "low-precision" astronomy. Solar position (Ch. 25) is
~0.01°; lunar (Ch. 47, abridged to the 14 + 8 + 6 largest periodic
terms) is ~0.5°. Planets use Standish's JPL "Approximate Positions"
elements (J2000 values + linear rates per century, valid 1800–2050)
solved via Kepler's equation — accurate to a few arcminutes inside
the validity window. A terminal cell at default zoom subtends ~1°,
so all errors are invisible. See `ASTRONOMY.md` for the full
algorithm-by-algorithm citations.

```c
// ephem.h
void ephem_compute(EphemBody body, double jd, EphemPosition *pos);
void ephem_to_topocentric(EphemPosition *pos,
                          const Observer *obs, double jd);
```

Pure functions. No rendering deps, no allocations, no thread-local
state. They take a Julian Day and an observer; they fill a struct.
That's all.

> **PRO MOVE** — When you're tempted to import a library for math
> that fits on a few pages of a published book, write it yourself.
> Meeus's book is 477 pages and *every formula has a worked
> example*. Implementing solar position from Ch. 25 is a long
> afternoon. Linking libnova would have been 30 seconds and a runtime
> dep. The afternoon paid off the first time someone asked "is this
> right?" — I could point them at chapter 25, equation 25.x, and the
> Sun J2000 worked example with a delta of 0.2 arcminutes.

> **GOTCHA** — Meeus's algorithms expect *Julian Day TT*. The
> difference between TT (Terrestrial Time) and UT1 (Universal Time)
> is currently ~70 seconds (ΔT). For a low-precision ephemeris on
> a terminal screen, ignoring ΔT is fine — you'd need to look at a
> clock to detect the error. If you ever upgrade to VSOP87, you'll
> need to handle ΔT properly.

### Tier 1 — bundled bright stars + constellations + Milky Way

The first time astro mode showed *just* Sun/Moon/planets against a
plain black sky, it felt empty. Adding 75 hand-curated bright stars
fixed that. Then constellation lines, then cardinal markers, then a
simulated Milky Way band.

The Milky Way is a fun trick: precompute 360 points along the galactic
equator (J2000 RA/Dec via an embedded gal→eq matrix), project each
frame, stamp an anisotropic Gaussian (σx=3, σy=5) modulated by a
longitude profile (peaks at galactic centre, Cygnus, Carina). Uses
`fb_max`. About 80 lines of code; reads as an actual band of light.

> **TIP** — When something "looks empty" the answer is rarely "more
> features." It's "another visual layer at a different spatial
> frequency." Bright stars are sparse points. Constellation lines are
> sparse curves. The Milky Way is a smooth wash. Three different
> spatial frequencies, all real, all from public catalogues.

### Tier 2 — refraction + extinction + twilight

Atmospheric corrections turned the astro view from "computational" to
"believable."

- **Bennett's refraction formula** — 1982 paper, single equation,
  bumps true altitude to apparent altitude. Stars stay visible a beat
  past their geometric setting time, exactly as they do in real life.
- **Kasten-Young airmass** — hits ~38× at the horizon. Drives V-band
  dimming + differential blue/green loss (red survives airmass
  better, which is why sunsets are red). Stars near the horizon
  visibly redden.
- **Twilight tint** — `twilight_factor(sun_altitude)` lerps from warm
  pink-orange (civil twilight) to cool blue (astronomical twilight),
  stamped in a Gaussian glow brightest in the sun's azimuth quadrant.

> **EASTER EGG** — Real V-band atmospheric extinction is
> `k=0.28 mag/airmass`. voidwatch uses `k=0.18` — gentler than
> reality, because the goal is *legibility*, not photometric accuracy.
> Astronomers running astro mode at 4 AM on a clear night don't want
> Sirius to be invisible because it's only 5° above the horizon.

### Tier 3 — trails + Galilean moons + Saturn rings + cursor

Body trails are a 256-sample ring buffer of (RA, Dec) per planet,
captured only when JD has advanced ≥1 virtual minute since the last
sample, re-projected every frame. The "re-project every frame" part is
the key insight — you want the trail to show *motion against the
stars* (including retrograde), not the diurnal arc the planet drew
across the sky.

Saturn's rings are a sinusoidal model of ring tilt over the
29.46-year cycle. Two ellipses (outer ring + Cassini gap), each walked
at azimuth step 1°, with proper z-ordering: near-side ring sits
*behind* the disc by drawing the ring first, then stamping the disc
on top.

```c
// stamp_saturn — simplified
draw_ring_back();      // far-side ring
stamp_disc();          // Saturn's body
draw_ring_front();     // near-side ring (last, so it's on top)
```

> **PRO MOVE** — When you have a depth-ordered effect like Saturn's
> rings, don't try to do z-buffering in a 2D framebuffer. Just draw
> the layers in the right order. Rendering "the part behind, then
> the disc, then the part in front" is three draw calls and zero
> bookkeeping.

### Tier 4 — meteors + eclipses + comets + asteroids

Meteor showers became their own self-paced subsystem because they're
the only thing in voidwatch that runs at *render rate* (decoupled
from the astro virtual clock). At 1000× time scrub you don't want
meteors flying every frame — you want them streaking at real-time
visual cadence. Solution: meteors clock from `CLOCK_MONOTONIC`, not
from the virtual JD.

Comets are two-body Keplerian propagation from osculating elements
at perihelion epoch. Six bundled (Halley, Encke, Swift-Tuttle,
Giacobini-Zinner, Churyumov-Gerasimenko, Hale-Bopp) with magnitudes
following the standard cometary formula `m = H + 5·log₁₀(Δ) +
2.5·n·log₁₀(r)`. Anti-solar tails computed on the fly; tail length
∝ intensity / r_helio so close approaches grow visible plumes.

Eclipses fire on real angular separation thresholds:
- **Solar:** Sun-Moon angular sep < 0.012 rad (≈0.7°)
- **Lunar:** Moon vs anti-Sun sep < 0.022 rad (≈1.3°)

When triggered, `apply_solar_eclipse` dims a Moon-sized Gaussian
patch on the Sun's stamp at the Moon's screen position. Lunar
eclipses get a red-shifted dim factor on the Moon disc — the
"blood moon" effect.

> **GOTCHA** — When testing eclipses, *always* use `--at` to seed the
> virtual clock to a known eclipse time. JPL's eclipse catalogue is
> the source of truth (e.g. 2024-04-08 total solar). I learned this
> the hard way after spending an hour wondering why eclipse code
> "wasn't firing" — there just wasn't an eclipse happening that
> evening.

### Tier 5 — HYG + Stellarium + DSOs + aurora + IAU names

The bundled-bright-stars approach (75 hand-typed entries) hit a ceiling
the moment people started looking for stars by name. Promotion to a
proper catalogue meant choosing a source.

- **BSC5** (Yale Bright Star Catalogue) — the classic, ~9100 stars.
- **HYG v3.6.1** — aggregates Hipparcos + BSC5 + Gliese into one CSV
  with HIP/HD/HR/proper-name/Bayer cross-references.

I picked HYG over BSC5 because HYG already aggregates the cross-refs.
With BSC5 you'd need a parallel name database and a join step.

The pipeline is a two-stage thing: maintainer-only Python generator
reads `tools/data/hyg_v36_1.csv` + `tools/data/stellarium_modern.json`
and emits `src/skydata.c` (8870 stars at V≤6.5, ~692 constellation
line segments). The `tools/data/` directory is gitignored. The
generated `src/skydata.c` is committed. End users build with no
Python and no network.

> **PRO MOVE** — When you need a big catalogue, two-stage build is
> almost always the right answer. Stage 1 is "fetch / parse / curate"
> and runs only when the catalogue needs refreshing. Stage 2 is "C
> file with a giant initialised array." End users only see stage 2.
> No runtime parsing, no missing-file errors, no version skew between
> the binary and the data.

The render side gets a `g_config.star_mag_cutoff` (default 6.5) so
users can dial visibility down to simulate light pollution.

DSOs (deep-sky objects — Messier + bright NGC) are 30 hand-curated
entries with five rendering kinds: galaxy, bright nebula, planetary
nebula, open cluster, globular. Each kind has a distinct tint;
apparent angular size maps to Gaussian sigma via sqrt-compression
with a [1.0, 6.0] clamp. Pleiades shows up as an actual smudge with
a recognisable blue-white tint, not a single dot.

Aurora is a pure-effect layer (no real Kp data — that would need a
network call). `g_config.kp_index` is the user's "Kp dial" — 0 = no
aurora, 3 = baseline visible, 9 = severe storm with flares. Phase
walks azimuth + time (`CLOCK_MONOTONIC`-paced for shimmer that's
independent of astro speed).

### Polish — heliocentric mode + headless modes + --validate

The heliocentric view was a late addition that ended up being one of
the most-loved features. Same code, different perspective: instead of
"observer's all-sky from Earth," you get "top-down looking at the
solar system from above the ecliptic." Sun fixed at centre,
Earth + 7 planets at heliocentric ecliptic (x, y), z dropped.
`sqrt(r_au)` distance compression keeps Mercury a few sub-pixels off
the Sun while Neptune sits at the frame edge.

Comets and asteroids extended naturally — same heliocentric vector,
same projection. Comets in helio render with anti-solar tails in
screen-space; asteroids as neutral rocky points.

Headless modes (`--tonight`, `--print-state`, `--next`, `--year`,
`--snapshot`, `--validate`) were the final feature push before v1.
They bypass terminal setup, audio init, and the render loop entirely.
Compute, print, exit. Designed for shell-pipeline use:

```sh
voidwatch --next mars --lat 51.48 --lon 0.0
voidwatch --year 2026 | grep -i eclipse
voidwatch --print-state --json | jq '.planets[] | select(.alt_rad > 0)'
```

`--validate` was the most important pre-1.0 addition. Now 9 worked
examples from Meeus + JPL Horizons cross-checks:

| Test                | Reference                                      | Tolerance |
|---------------------|------------------------------------------------|-----------|
| Sun J2000.0         | Meeus Ch. 25 + JPL Horizons cross-check        | 30′       |
| Sun Meeus 25.a      | Meeus 25.a worked example, p. 165              |  6′       |
| Sun 2024-04-08      | JPL Horizons (Great American Eclipse epoch)    | 30′       |
| Moon Meeus 47.a     | Meeus 47.a full-series worked answer           | 30′       |
| Venus Meeus 33.a    | Meeus 33.a abridged-VSOP87 worked answer       | 15′       |
| Mercury J2000.0     | JPL Horizons (CENTER 500@399, Q 1)             |  6′       |
| Mars J2000.0        | JPL Horizons (CENTER 500@399, Q 1)             |  6′       |
| Jupiter J2000.0     | JPL Horizons (CENTER 500@399, Q 1)             | 15′       |
| Saturn J2000.0      | JPL Horizons (CENTER 500@399, Q 1)             | 15′       |

All nine PASS — typical observed deltas are well below the tolerance
band (Mercury 0.5′, Mars 0.3′, Jupiter 5.9′, Saturn 9.4′ at J2000.0).
The point isn't to be a research-grade reference; it's that *anyone
can verify the implementation is correct against published gold
standards*. Run `voidwatch --validate` and the receipts print
themselves.

> **PRO MOVE** — A `--validate` self-test for any computed quantity
> with a published reference is one of the highest-ROI features you
> can add. It takes one afternoon and pays off forever. Every time
> someone asks "are you sure that planet is in the right place?" you
> answer with `voidwatch --validate` and they go away satisfied.

---

### Post-v1: hand-built SGP4 satellite tracking

After v1 the obvious "what's missing" question pointed at a single
target: real satellites. The Meeus pipeline propagates Sun/Moon/planets
beautifully but is nothing close to the right tool for orbital objects
that change position degree-by-degree minute-to-minute. Different
algorithm family (SGP4), different reference frame (TEME), different
unit conventions (rev/day, Earth radii), different staleness reality
(TLEs decay in days). This was a separate project that bolts onto the
same renderer.

The discipline was strict: parse → init → propagate → look angles →
render integration, **each gated by tests before the next phase
starts**. Six phases, three commits' worth of math validated against
Vallado's published verification vectors before a single satellite
appeared on screen.

**Phase 1 — TLE parser.** Fixed-column 69-character format. Implied
leading decimals on eccentricity (`0006703` → `0.0006703`), implied
decimals + signed exponents on BSTAR (` 10270-3` → `1.0270e-4`),
mod-10 checksum verification, NORAD year pivot (00–56 → 2000–2056,
57–99 → 1957–1999). Meeus-form Gregorian-to-JD for the epoch — *not*
`mktime`, which is timezone-aware and would make the parser produce
different answers depending on `TZ`. Twelve test cases against
Vallado catalog #5 (Vanguard 1) used as the canonical TLE.

**Phase 2 — Model init.** WGS-72 constants (mu, R⊕, xke, J2/J3/J4)
in a single labeled block. Un-Kozai mean motion recovery, semi-major
axis, perigee, atmospheric drag coefficients (cc1/cc4/cc5), secular
rates (mdot, argpdot, nodedot), t-power coefficients (d2, d3, d4,
t2cof–t5cof). Variable names follow Vallado/NASA SGP4.c verbatim — the
brief was explicit: "ugly but traceable beats pretty abstraction."
Deep-space (period ≥ 225 min) is classified but not propagated;
voidwatch's targets are all near-Earth.

**Phase 3 — Near-Earth propagation.** The actual SGP4 step: secular
updates over time, drag, Kepler iteration, short-period periodics,
perifocal-to-TEME orientation rotation. Output in km / km/s. The
acceptance gate was Vallado's published `tcppver.out` vectors:

| Test                  | Brief tolerance | Observed         |
|-----------------------|-----------------|------------------|
| Vanguard 1 (13 steps) | < 1e-3 km       | < 7e-9 km        |
| DELTA 1 DEB (25 steps)| < 1e-6 km/s     | < 8e-10 km/s     |

Seven nanometres over 4320 minutes of propagation. The code is
bit-faithful to the reference; that's what test vectors are for.

> **PRO MOVE** — When you implement a published algorithm, find its
> reference test vectors *first* and write the test fixture before
> writing the algorithm. You'll know within minutes whether you're
> drifting; without the oracle you can spend days debugging "why does
> ISS pass over my house at the wrong time."

**Phase 4 — Deep-space SDP4: deferred.** Dscom/dpper/dsinit/dspace
adds ~600 lines for lunar-solar perturbations, half-day and 1-day
resonance, Lyddane handling. Voidwatch's intended targets (ISS,
Hubble, NOAA, Starlink, Tiangong) are all near-Earth. Deep-space TLEs
return `SAT_DEEP_SPACE` from init and never silently produce wrong
answers. The body can land later without breaking the public API.

**Phase 5 — TEME → topocentric.** SGP4 outputs in True Equator Mean
Equinox ECI; voidwatch's renderer wants observer-relative alt/az.
Pipeline: TEME → ECEF (rotate by −GMST about z), observer geodetic
→ ECEF (WGS-72 ellipsoid), range vector, ECEF → SEZ at observer,
asin(Z/range) for altitude, atan2(E, −S) for azimuth. Range rate from
the projected ECEF velocity. Visual-grade — polar motion zero, no EOP
files, no UT1−UTC. The strict-grade gate is upstream (TEME vectors).

**Phase 6 — astro integration.** Bundled near-Earth catalog (ISS,
Hubble, NOAA 19, Tiangong/CSS), internal lazy parse+init cache (parse
each TLE once, reuse forever), single per-frame entry point
`satellite_compute_all`. `satellites_draw` stamps sharp single-pixel
cool-white-cyan points via `fb_max` — fb_add with decay would smear
ISS across the sky as it moves. Compact short names in `astro_labels`
(ISS, HST, NOAA, CSS). Cursor pick + track + search + JSON output
extended to handle satellites alongside planets/comets/asteroids/DSOs.

**Stale-TLE policy.** TLEs decay quickly. Bundled snapshots are
demo-grade by definition. Policy:

| TLE age            | Behavior                                       |
| ------------------ | ---------------------------------------------- |
| ≤ 7 days           | Normal brightness                              |
| 7 < age ≤ 14 days  | Dim 50%                                        |
| 14 < age ≤ 30 days | Hidden from render; JSON marks `stale: true`   |
| > 30 days          | `compute_all` refuses; `valid: false`          |

> **PRO MOVE** — When you bake demo data into a binary that *expires*,
> bake an expiration policy too. Don't pretend stale TLEs are fresh
> just because they parse. Tell the user, dim them, hide them,
> eventually refuse them. The user will ask "where's ISS?" once and
> then understand the policy forever.

The whole satellite stack is ~1100 lines across `include/satellite.h`
(public API), `src/satellite.c` (parser + SGP4 + topocentric +
catalog), with the validated math sitting under three test binaries
(`test_tle`, `test_sgp4`, `test_sgp4_propagation`) and a render-side
smoke test (`test_satellite_look`) plus a bundled-catalog test
(`test_satellite_bundle`). All commit-gated; all repeatable.

---

## Tools of the Trade

The standard library plus a handful of pillars. Each one earns its
place.

### C11 + libc + libm

The whole project. C11 because designated initialisers and
`static_assert` are nice. libm for `sin`, `cos`, `pow`, `sqrt`. No
fancy compiler-specific extensions; should compile on any reasonable
C11 toolchain.

### POSIX (termios, pthreads, signals, clock_gettime)

The portability layer. `termios` for raw mode + alt screen.
`pthreads` for the audio worker. `SIGWINCH` for resize, `SIGINT` /
`SIGTERM` for clean exit. `clock_gettime(CLOCK_MONOTONIC)` for the
frame clock (don't use `gettimeofday` — it's wall-time and it can go
backwards on NTP correction).

### ALSA (libasound)

Direct ALSA capture. PipeWire on modern systems provides ALSA
compat, so the same `snd_pcm_open("default")` call works on PipeWire,
PulseAudio with the ALSA plugin, and bare ALSA. This is why
voidwatch doesn't need a session bus.

### FFTW3 (single-precision)

`-lfftw3f`. Single precision is enough for visual reactivity — the
differences between f32 and f64 FFT output are below `LUM_THRESHOLD`
once envelope-followed and AGC'd. Single-precision is also faster,
smaller, and the plan is computed once at init.

### stb_perlin.h (vendored)

Sean Barrett's public-domain single-header Perlin noise. Vendored
into `include/`. No build-system entanglement, no version skew. Just
include it.

> **PRO MOVE** — Sean Barrett's `stb` family (image, vorbis, perlin,
> truetype) is the gold standard for vendor-friendly single-header
> libraries. Public domain, no build dance, drop-in. If you see
> someone reaching for a 30-MB library to get noise, hand them
> `stb_perlin.h` instead.

### VHS (charmbracelet)

Demo recordings. The `.tape` file in `docs/` is a small DSL that
drives a virtual terminal, records to GIF. Reproducible and
checked-in — anyone can regenerate the demos with `vhs docs/astro.tape`.

### Meeus's *Astronomical Algorithms*, 2e

Not software, but it's the most-used "library" in the project. Every
ephemeris formula traces to a chapter and equation. The
`--validate` test references its worked examples directly.

### HYG database v3.6.1 + Stellarium constellation modern.json

The catalogues. HYG at `astronexus.com/projects/hyg`; Stellarium's
constellation art is `share/stellarium/skycultures/modern_iau`.
Both bundled at compile time, both credited in CITATIONS.md.

### What's NOT used (and why)

- **CMake / meson / autotools** — 50-line Makefile builds in <5s and
  has `-MMD -MP` dep tracking. Reconsider when (a) the codebase
  doubles or (b) cross-platform CI matrix becomes a real ask.
- **D-Bus / geoclue / MPRIS** — pulls in a session bus runtime dep
  that conflicts with "runs on a fresh Arch install with libc + libm
  + ALSA + FFTW3 + nothing else."
- **libnova / libnova-ng / Stellarium's libstellarium** — Meeus is
  cheaper to link and easier to verify. See PRO MOVE under "Astro
  mode" above.
- **VSOP87** — Meeus low-precision is already <few arcmin; a terminal
  cell subtends ~1°. The error is invisible. VSOP87 satisfies a
  checklist, not a user.
- **ncurses** — too high-level. We need raw ANSI control sequences
  for cursor positioning, truecolor, alt-screen — `printf("\x1b[...")`
  is exactly what we want. Let the framebuffer do the heavy lifting.
- **CMake-based test framework (Catch2, cmocka)** — when tests come,
  they'll be plain C asserts in `tests/`, run from `make test`.

---

## Directory Structure

```
voidwatch/
  README.md          User-facing intro + screenshots
  ARCHITECTURE.md    This file (build journey + design narrative)
  ASTRONOMY.md       Per-algorithm citations (Meeus chapters, etc.)
  CITATIONS.md       Data-source credits (HYG, Stellarium, JPL, IAU)
  LICENSE            MIT
  Makefile           single-file glob build, -MMD/-MP dep tracking
  compile_commands.json   clangd manifest

  include/           one header per module (declarations only)
    framebuffer.h    fb_add / fb_max / fb_decay (the core data structure)
    render.h         Braille flush + gamma + shadow lift
    term.h           raw mode, alt screen, signals, key polling
    palette.h        themable colour table
    config.h         compile-time scalars (#defines + static const)

    body.h           N-body system, symplectic Euler
    starfield.h      parallax stars + diffraction spikes
    nebula.h         two-octave Perlin wash
    particle.h       fixed-arena particles + supernova one-shot
    stb_perlin.h     vendored, public-domain
    audio.h          ALSA + FFTW3 + AGC + envelope follower

    ephem.h          pure-math Meeus ephemeris (no rendering deps)
    astro.h          astro composite glue (projection, MW, planets, etc.)
    skydata.h        bundled HYG + constellation tables (generated)
    dso.h            bundled Messier/NGC catalog
    comet.h          bundled comets + Keplerian propagation
    asteroid.h       bundled minor planets + Keplerian
    satellite.h      hand-built SGP4 + bundled near-Earth catalog
    location.h       observer lat/lon resolution chain
    headless.h       one-shot text/JSON modes

    hud.h            cursor-positioned overlays (sector, scan, log, meter)
    vwconfig.h       runtime config (TOML/INI subset)

  src/               implementations (one .c per .h)

  tools/             maintainer-only scripts
    gen_skydata.py   reads HYG + Stellarium, emits src/skydata.c
    data/            gitignored cache of upstream catalogues

  docs/              demo recordings (VHS .tape source + .gif output)
    sandbox.tape     22s ambient sandbox demo
    astro.tape       astro feature tour
    sandbox.gif      generated
    astro.gif        generated
```

---

## Data Flow

### Sandbox frame (per render tick)

```
main loop
   |
   v
 fb_decay(0.92)              clear-via-fade
   |
   v
 nebula_draw(snap)            two-octave Perlin via fb_max
   |
   v
 starfield_draw(snap)         parallax stars via fb_max + twinkle
   |
   v
 particle_update(snap)        ----+   fixed-step physics
 particle_draw                    |   sandbox-only
 body_step                         |
 body_draw(snap)               <--+
   |
   v
 render_flush(fb, stdout)     Braille glyphs + ANSI truecolor
   |
   v
 starfield_spikes             cursor-positioned overlay
 hud_draw                     cursor-positioned overlay
 hud_help_overlay             cursor-positioned overlay (top of stack)
   |
   v
 fflush(stdout)
```

### Astro frame (per render tick)

```
time(NULL) + offset + speed*dt
   |
   v
 ephem_julian_day_from_unix
   |
   v
 ephem_compute(body, jd)      Meeus -> geocentric (RA, Dec)
   |
   v
 ephem_to_topocentric         + Bennett refraction -> (alt, az)
   |
   v
 project(alt, az)             azimuthal-equidistant -> (col, row)
   |
   v
 apply_extinction(mag, alt)   Kasten-Young airmass -> dimmed RGB
   |
   v
 stamp_disc / stamp_moon /
 stamp_planet / draw_ring     Gaussian footprints into fb (fb_add / fb_max)
   |
   v
 (same render_flush + cursor overlays as sandbox)
```

The point: **the framebuffer doesn't care which mode you're in.** Both
modes are just sources of `(x, y, r, g, b)` triples. The pipeline is
universal.

---

## The Catalogues

### HYG v3.6.1 → src/skydata.c (~8870 stars)

`tools/gen_skydata.py` reads the upstream CSV, filters to `V ≤ 6.5`
(naked-eye plus a touch), and emits a static C array of
`{ ra, dec, mag, spectral_class, name }` records. Names prefer the
IAU proper name (HYG's `proper` column) and fall back to Bayer
designation. Generated `.c` is ~440KB.

### Stellarium modern.json → src/skydata.c (~692 segments)

Same generator parses the constellation lines. Each segment is
`{ star_idx_a, star_idx_b }` referencing the bundled star table.
~88 figures total once you mag-filter the underlying stars.

### Hand-curated DSOs (30) → src/dso.c

Messier favourites (M31 Andromeda, M42 Orion Nebula, M45 Pleiades,
M8 Lagoon, M57 Ring, M1 Crab, M13 Hercules, etc.) plus bright NGCs
(NGC 7000 North America, NGC 6960 Veil, etc.). Five `DSOKind`s with
distinct render tints. Apparent angular size → Gaussian sigma via
`sqrt(arcmin / 30) * 6, clamp [1.0, 6.0]`.

### JPL Horizons comets (6) → src/comet.c

Halley, Encke, Swift-Tuttle, Giacobini-Zinner, Churyumov-Gerasimenko,
Hale-Bopp. Osculating elements at perihelion epoch. Two-body
Keplerian propagation. Apparent magnitude via `m = H + 5log₁₀(Δ) +
2.5·n·log₁₀(r)`.

### JPL SBDB asteroids (5) → src/asteroid.c

1 Ceres, 2 Pallas, 4 Vesta, 3 Juno, 7 Iris. Classic
`(a, e, i, Ω, ω, M0)` elements at epoch. Mean motion from Kepler's
third. Magnitude via planet-style `H + 5log₁₀(r·Δ)`. Stamped as
small fuzzy points; `g_config.asteroid_mag_cutoff` (default 9.5)
gates render.

### Meteor showers (9) → src/astro.c

Quadrantids, Lyrids, Eta Aquariids, Perseids, Draconids, Orionids,
Leonids, Geminids, Ursids. DOY-based Gaussian activity profile.
Per-shower colour + velocity (Leonids fast & blue, Geminids slow &
yellow, Draconids gentle warm-white). Sporadic background fires at
~8/hr × dawn-apex factor when sun is below horizon.

### Galilean moons (4) → src/astro.c

Io, Europa, Ganymede, Callisto. Period + semi-major + initial mean
anomaly. No perturbations or Jupiter axial tilt — terminal scale
doesn't read them. Stamped at `a_rj * jupiter.radius_sub` from
Jupiter, flattened ×0.06 in y for ring-plane feel.

### CelesTrak TLEs (4) → src/satellite.c

ISS (ZARYA, NORAD 25544), HST (NORAD 20580), NOAA 19 (NORAD 33591),
CSS Tianhe (NORAD 48274). Snapshot fetched from CelesTrak's
`gp.php?CATNR=<n>&FORMAT=TLE` endpoint and pasted into
`satellite_elements[]` as triple-line literals. Refresh by re-running
the GP query when this file is older than ~14 days. Treat as demo
seed data; the staleness gate hides anything past 14 days and refuses
anything past 30. End users build offline; the network fetch is a
maintainer task only.

---

## Wallust Theming

Themes are file-driven because they're the most common thing a user
wants to change without rebuilding (and because wallust integration
needs a runtime path).

The format is dead-simple `key = #RRGGBB`:

```ini
# ~/.config/voidwatch/theme.conf
void          = #0a0e14
star_m        = #ff6b35
star_g        = #ffd166
star_b        = #6dcff6
nebula_violet = #7b3fbf
nebula_crimson= #c43d3d
hud           = #7fbbb3
hud_alert     = #d699b6
```

Wallust integration is a one-time setup at the *wallust template*
level — voidwatch itself just reads the file:

```
~/.config/wallust/templates/voidwatch.conf
  → expands to ~/.config/voidwatch/theme.conf via wallust placeholders
  → next `wset` regenerates; voidwatch picks it up on next run
```

> **PRO MOVE** — When you need runtime configurability for *one
> specific thing* and the rest of your config can stay compile-time,
> don't build a general TOML system. Build the smallest config layer
> that solves the actual problem. voidwatch's palette layer is ~100
> lines. The full TOML config layer (`vwconfig`) came later, when the
> need for it was specific.

---

## Lessons (the Coder's Mind)

The principles up at the top weren't there from day one. These are
where they came from.

### 1. Get the rendering trick to work first

If sub-cell Braille had looked bad, no amount of clever ephemeris
would have saved the project. Phase 1 was 100% rendering, 0%
features. That meant by Phase 2, every new feature was bolted onto a
proven visual foundation. The opposite — building features on
unproven rendering — leaves you with a mountain of features and a
broken-looking program.

### 2. Decay is the clear

`fb_clear` is one of those things you write because that's what you
*think* a frame loop does. Then you realise that *not clearing* and
multiplying by 0.92 instead gives you trails, bloom, and atmospheric
warmth for the cost of one multiply per pixel. That single design
choice defines voidwatch's visual identity.

### 3. Pre-allocate everything

The render loop never touches `malloc`. Every per-frame buffer is
allocated at init or window resize. The body system *intentionally
survives a terminal resize* — only the framebuffer / starfield /
particle arena get reinitialised, because the body system lives in
its own world coordinates that don't depend on viewport size.
Reinitialising on resize would wipe orbital state.

### 4. Identity-element fallbacks

If audio fails, bands are zero, and `1.0 + 0.0 * MOD_X = 1.0`. Every
modulation collapses to identity. The user just runs without
reactivity. The opposite design — "audio fails, program exits" or
"audio fails, half the draws are NaN" — is what you get when you
don't design the fallback in from day one.

### 5. Pure functions are easier to verify

`ephem_compute` is a pure function. Given a JD and a body, it returns
geocentric (RA, Dec). No allocations, no thread-local state, no
side effects. That's what made `--validate` cheap to write — the
test is *literally* the function call with a known input and a known
output. If `ephem_compute` had been a method on a class with internal
state, validation would have been an order of magnitude harder.

### 6. Two-stage builds for big catalogues

Maintainer-only Python generator → committed C file with a giant
initialised array. End users build offline with no Python, no
network, no version skew. The generator runs only when the catalogue
needs refreshing. This pattern works for *anything* large you'd
otherwise read at runtime.

### 7. Compile-time tuning until you have a reason

Most of voidwatch's numeric tunables are still `#define`s in
`config.h`. The bar to promote one to runtime config is "a user
might reasonably want to change this without rebuilding." That's a
small list. Don't pre-design a TOML system you don't need.

### 8. Cite your sources, especially the magic constants

`k=0.18 mag/airmass` is a magic constant. So is `LUM_THRESHOLD = 0.04`.
So is `NEBULA_DRIFT_BIG = 0.004`. Without comments explaining *why
that value*, future-you will mess with them and not understand why
the picture suddenly looks wrong. Inline `// don't tweak below X`
comments next to every magic constant pay for themselves the first
time someone goes hunting.

### 9. Sandbox vs astro: don't share state

The N-body sim and the ephemeris are two different programs that
share a renderer. They don't share data structures, they don't share
units, they don't share clocks. Every time I was tempted to
"unify" them, I'd have leaked sandbox quirks into astro or vice
versa. Two parallel worlds is the right design.

### 10. Headless modes are basically free

Once your astro pipeline is pure functions, `--tonight`, `--print-state`,
`--next`, `--year`, `--validate`, `--snapshot` are all under 100
lines each. They reuse the same primitives the renderer uses, just
print instead of stamp. **If your code is shaped right, half your
features come for free.** This is the test of whether your design is
shaped right.

### 11. Cite every formula, defend every decision

This document exists so the same calls don't get re-litigated every
six months. "Why didn't we use VSOP87?" "Why no D-Bus?" "Why
fb_decay = 0.92 specifically?" The answers should live somewhere
persistent and visible to anyone touching the code. That's what this
file is for.

---

## Where to Start Reading

If you want to understand voidwatch in 30 minutes:

1. **`include/framebuffer.h`** (40 lines) — the core data structure.
2. **`src/main.c:340-679`** — the render loop. Composite order, mode
   dispatch, key handling.
3. **`include/ephem.h`** + **`src/ephem.c`** — the astro pipeline's
   pure-math layer.
4. **`src/astro.c`** sections on `astro_draw` and projection — how
   ephem coordinates become pixels.
5. This document, if you want the *why* behind any of the above.

`ASTRONOMY.md` is the per-algorithm citations doc — every Meeus
chapter, every constant, every reference. `CITATIONS.md` credits the
data sources (HYG, Stellarium, JPL, IAU). Three docs, three
audiences:

- README → users
- ARCHITECTURE.md (this file) → contributors
- ASTRONOMY.md → astronomers verifying the math
- CITATIONS.md → upstream credit
