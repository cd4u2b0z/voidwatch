# voidwatch

> Terminal space observatory — N-body sandbox + real-ephemeris planetarium with hand-built SGP4 satellite tracking, atmospheric refraction, and JSON output for shell pipelines.
> More information: <https://codeberg.org/cdubz/voidwatch>.

- Launch the N-body sandbox (default — phosphorescent star + planets + neutron star + black hole, audio-reactive supernovae):

`voidwatch`

- Launch the real-ephemeris planetarium (8870 stars, 8 planets, Sun/Moon, comets, asteroids, DSOs, near-Earth satellites):

`voidwatch --astro`

- Open the planetarium without ALSA capture (no audio reactivity, no microphone access):

`voidwatch --astro --no-audio`

- Set observer location explicitly (north-positive lat, east-positive lon):

`voidwatch --astro --lat {{32.78}} --lon {{-79.93}}`

- "What's up tonight?" — text summary of Sun/Moon/planets/asteroids + the active meteor shower:

`voidwatch --tonight`

- Full ephemeris + comets + asteroids + bundled satellites as JSON, piped into jq:

`voidwatch --print-state --json | jq '.planets[] | select(.alt_deg > 0)'`

- Next rise time for a planet, comet, asteroid, named DSO, or bundled satellite (by name or NORAD catalog number):

`voidwatch --next {{ISS}}`

- Annual almanac for a year — eclipses, conjunctions, Moon close passes, shower peaks, equinoxes/solstices:

`voidwatch --year {{2026}} | grep -i eclipse`

- Render one astro frame as ANSI to stdout (truecolor + Braille), defaults to 80×40:

`voidwatch --snapshot {{120}} {{40}} --astro`

- Snapshot a specific moment (e.g., the 2024 Great American Eclipse from Charleston):

`voidwatch --snapshot 200 50 --astro --at {{2024-04-08T18:18:00}} --lat {{32.78}} --lon {{-79.93}}`

- Run the internal sanity tests against Meeus's published worked examples + JPL Horizons:

`voidwatch --validate`

- Refresh the bundled satellite TLEs from CelesTrak (writes to user cache; rate-limited to 2h):

`voidwatch --update-tle`

- Print version and exit:

`voidwatch --version`

- List every ALSA capture source (with monitor tag for desktop-audio routing):

`voidwatch --list-devices`

- Use a specific ALSA device (or "pulse" for PipeWire/PulseAudio compat):

`voidwatch --device {{pulse}}`

- React to desktop audio on PipeWire (route the sink monitor through ALSA's pulse plugin):

`PULSE_SOURCE=$(pactl list short sources | awk '/monitor/{print $2; exit}') VOIDWATCH_AUDIO_DEVICE=pulse voidwatch`

- Visit the past or future — Halley's 2061 perihelion, Hale-Bopp 1997, etc.:

`voidwatch --astro --at {{2061-07-28T00:00:00}}`

- Toggle constellation lines in astro mode (key `l`), deep-sky objects (`d`), aurora (`a`), satellites (`i`), trails (`t`):

`voidwatch --astro    # then press l, d, a, i, t while running`

- Switch to the heliocentric "above the ecliptic" view, then back (key `m`):

`voidwatch --astro    # press m for top-down solar system, m again for all-sky`

- Search by name in astro mode (matches planets, comets, asteroids, DSOs, satellites incl. catalog numbers):

`voidwatch --astro    # press / then type "iss", "m31", "halley", "ceres", "25544"`

- Override the bundled palette with a custom theme:

`voidwatch --theme {{path/to/theme.conf}}`

- Override the runtime config (fb_decay, magnitude cutoffs, gravity_g):

`voidwatch --config {{path/to/config.toml}}`

- Wallust integration — drop a template and `wset` regenerates the theme:

`echo 'target = "~/.config/voidwatch/theme.conf"' >> ~/.config/wallust/templates/voidwatch.conf`
