#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "astro.h"
#include "audio.h"
#include "body.h"
#include "config.h"
#include "framebuffer.h"
#include "hud.h"
#include "location.h"
#include "nebula.h"
#include "palette.h"
#include "particle.h"
#include "render.h"
#include "headless.h"
#include "starfield.h"
#include "term.h"
#include "vwconfig.h"

/* Fixed-timestep physics so orbits stay stable when frames stall.
 * Decoupled from the render rate; the loop accumulates dt and steps
 * the integrator in fixed slices. */
#define PHYSICS_HZ 120
#define PHYSICS_DT (1.0f / (float)PHYSICS_HZ)

static double ts_diff(struct timespec a, struct timespec b) {
    return (double)(a.tv_sec - b.tv_sec)
         + (double)(a.tv_nsec - b.tv_nsec) / 1e9;
}

static void trigger_supernova(BodySystem *bs, ParticleArena *pa, double t) {
    if (bs->count <= 0) return;
    int candidates[16];
    int n_cand = 0;
    for (int i = 0; i < bs->count && n_cand < 16; i++) {
        BodyKind k = bs->bodies[i].kind;
        if (k == BODY_STAR || k == BODY_NEUTRON_STAR) {
            candidates[n_cand++] = i;
        }
    }
    int origin_idx = (n_cand > 0)
        ? candidates[rand() % n_cand]
        : (rand() % bs->count);
    const Body *origin = &bs->bodies[origin_idx];
    particle_spawn_supernova(pa, origin);
    char msg[28];
    snprintf(msg, sizeof msg, "SUPERNOVA \xE2\x97\x86 %s",
             origin->kind == BODY_NEUTRON_STAR ? "NS-CORE" : "MAIN-SEQ");
    hud_log_event(t, msg);
}

static void print_help(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --astro                real-ephemeris mode (Sun + Moon + 8 planets)\n"
        "  --lat <deg>            observer latitude  (north positive)\n"
        "  --lon <deg>            observer longitude (east positive)\n"
        "  --list-devices         enumerate ALSA PCM capture sources and exit\n"
        "  --device <name>        ALSA PCM source (overrides $VOIDWATCH_AUDIO_DEVICE)\n"
        "  --no-audio             skip audio capture entirely (no ALSA, no FFT)\n"
        "  --theme <path>         load a key=value palette file\n"
        "                         (auto: $XDG_CONFIG_HOME/voidwatch/theme.conf)\n"
        "  --config <path>        load a TOML/INI runtime config file\n"
        "                         (auto: $XDG_CONFIG_HOME/voidwatch/config.toml)\n"
        "\n"
        "Headless / one-shot modes (compute, print, exit):\n"
        "  --tonight              text summary of what's worth watching tonight\n"
        "  --print-state          full ephemeris + comets + asteroids (text)\n"
        "  --print-state --json   same as JSON\n"
        "  --next <body>          next rise time for a planet/comet/asteroid\n"
        "  --year <year>          annual almanac for the given Gregorian year\n"
        "  --at <YYYY-MM-DD[Thh:mm:ss]>\n"
        "                         seed virtual clock at the given moment;\n"
        "                         applies to --astro and the headless modes\n"
        "  --validate             run internal sanity tests against JPL refs\n"
        "  --snapshot [cols rows] render one astro frame to stdout and exit\n"
        "\n"
        "  -h, --help             show this help and exit\n"
        "\n"
        "Astro mode location resolves in this order:\n"
        "  --lat/--lon  >  ~/.config/voidwatch/location.conf  >  "
        "$VOIDWATCH_LAT/$VOIDWATCH_LON  >  (0,0)\n"
        "\n"
        "Runtime keys: h toggle HUD, ? help overlay, q/Esc quit.\n"
        "Astro keys:   + / - speed, 0 reset, , / . scrub -1h / +1h\n"
        "              g grid, l constellation lines, d deep-sky,\n"
        "              a aurora, t trails, m geo/helio,\n"
        "              s star backdrop, c cursor (hjkl, Esc).\n",
        argv0);
}

int main(int argc, char **argv) {
    const char *device      = NULL;
    const char *theme_path  = NULL;
    const char *config_path = NULL;
    const char *next_body   = NULL;
    int         year_arg    = 0;
    int         no_audio    = 0;
    int         astro_mode  = 0;
    double      cli_lat     = NAN;
    double      cli_lon     = NAN;
    enum { HL_NONE = 0, HL_TONIGHT, HL_PRINT_STATE, HL_NEXT, HL_YEAR, HL_VALIDATE, HL_SNAPSHOT } headless = HL_NONE;
    int snap_cols = 80, snap_rows = 40;
    int         json_output = 0;
    time_t      at_time     = 0;
    int         at_set      = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--list-devices") == 0) {
            audio_list_devices(stdout);
            return 0;
        } else if (strcmp(a, "--no-audio") == 0) {
            no_audio = 1;
        } else if (strcmp(a, "--device") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --device requires an argument.\n");
                return 2;
            }
            device = argv[++i];
        } else if (strcmp(a, "--astro") == 0) {
            astro_mode = 1;
        } else if (strcmp(a, "--lat") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --lat requires a value.\n");
                return 2;
            }
            const char *s = argv[++i];
            char *end = NULL;
            cli_lat = strtod(s, &end);
            if (end == s || *end != '\0') {
                fprintf(stderr, "voidwatch: --lat: not a number: %s\n", s);
                return 2;
            }
        } else if (strcmp(a, "--lon") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --lon requires a value.\n");
                return 2;
            }
            const char *s = argv[++i];
            char *end = NULL;
            cli_lon = strtod(s, &end);
            if (end == s || *end != '\0') {
                fprintf(stderr, "voidwatch: --lon: not a number: %s\n", s);
                return 2;
            }
        } else if (strcmp(a, "--theme") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --theme requires a path.\n");
                return 2;
            }
            theme_path = argv[++i];
        } else if (strcmp(a, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --config requires a path.\n");
                return 2;
            }
            config_path = argv[++i];
        } else if (strcmp(a, "--tonight") == 0) {
            headless = HL_TONIGHT;
        } else if (strcmp(a, "--print-state") == 0) {
            headless = HL_PRINT_STATE;
        } else if (strcmp(a, "--json") == 0) {
            json_output = 1;
        } else if (strcmp(a, "--next") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --next requires a body name.\n");
                return 2;
            }
            headless = HL_NEXT;
            next_body = argv[++i];
        } else if (strcmp(a, "--validate") == 0) {
            headless = HL_VALIDATE;
        } else if (strcmp(a, "--snapshot") == 0) {
            headless = HL_SNAPSHOT;
            /* Optional next two args: cols rows. Don't consume them if
             * they're not numeric — could be other flags. */
            if (i + 2 < argc) {
                char *e1 = NULL, *e2 = NULL;
                long c = strtol(argv[i + 1], &e1, 10);
                long r = strtol(argv[i + 2], &e2, 10);
                if (e1 && *e1 == '\0' && e2 && *e2 == '\0' && c > 0 && r > 0) {
                    snap_cols = (int)c;
                    snap_rows = (int)r;
                    i += 2;
                }
            }
        } else if (strcmp(a, "--at") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --at requires an ISO date.\n");
                return 2;
            }
            const char *s = argv[++i];
            int y = 0, mo = 0, d = 0, h = 12, mi = 0, sec = 0;
            int n = sscanf(s, "%d-%d-%dT%d:%d:%d",
                           &y, &mo, &d, &h, &mi, &sec);
            if (n != 6) {
                /* Try date-only: defaults to noon local. */
                if (sscanf(s, "%d-%d-%d", &y, &mo, &d) != 3) {
                    fprintf(stderr,
                        "voidwatch: --at: not an ISO date "
                        "(YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS): %s\n", s);
                    return 2;
                }
                h = 12; mi = 0; sec = 0;
            }
            struct tm tm = {0};
            tm.tm_year = y - 1900;
            tm.tm_mon  = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min  = mi;
            tm.tm_sec  = sec;
            tm.tm_isdst = -1;
            at_time = mktime(&tm);
            if (at_time == (time_t)-1) {
                fprintf(stderr, "voidwatch: --at: invalid date: %s\n", s);
                return 2;
            }
            at_set = 1;
        } else if (strcmp(a, "--year") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "voidwatch: --year requires a year.\n");
                return 2;
            }
            const char *s = argv[++i];
            char *end = NULL;
            long y = strtol(s, &end, 10);
            if (end == s || *end != '\0' || y < 1900 || y > 2200) {
                fprintf(stderr, "voidwatch: --year: bad year: %s\n", s);
                return 2;
            }
            headless  = HL_YEAR;
            year_arg  = (int)y;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "voidwatch: unknown argument: %s\n", a);
            print_help(argv[0]);
            return 2;
        }
    }

    /* Bigger stdout buffer — one fwrite per frame is the goal. */
    static char outbuf[1 << 17];
    setvbuf(stdout, outbuf, _IOFBF, sizeof outbuf);

    srand((unsigned)time(NULL));
    palette_autoload(theme_path);
    vwconfig_autoload(config_path);

    /* Headless / one-shot modes: compute, print, exit. No terminal init,
     * no audio, no render loop. Location resolution is the only state we
     * need from the regular pipeline. */
    if (headless != HL_NONE) {
        /* --validate doesn't need an observer — it tests geocentric
         * positions against fixed references. Short-circuit here. */
        if (headless == HL_VALIDATE) return headless_validate(stdout);

        Observer obs = {0};
        int fb_loc = 0;
        if (location_resolve(cli_lat, cli_lon, &obs, &fb_loc) != 0) {
            fprintf(stderr, "voidwatch: invalid latitude/longitude.\n");
            return 2;
        }
        time_t now = at_set ? at_time : time(NULL);
        switch (headless) {
            case HL_TONIGHT:     return headless_tonight(&obs, now, stdout);
            case HL_PRINT_STATE: return headless_print_state(&obs, now, stdout, json_output);
            case HL_NEXT:        return headless_next_rise(&obs, now, next_body, stdout);
            case HL_YEAR:        return headless_year(&obs, year_arg, stdout);
            case HL_SNAPSHOT:    return headless_snapshot(&obs, now, snap_cols, snap_rows, stdout);
            case HL_VALIDATE:
            case HL_NONE: break;
        }
    }

    AstroState astro = {0};
    astro.show_star_backdrop = 1;      /* parallax backdrop on by default */
    astro.show_dso           = 1;      /* DSOs visible by default in geo */
    /* astro.show_aurora stays 0 — most observers are at mid-latitudes
     * where aurora isn't visible. Press `a` to force-render. */
    if (astro_mode) {
        int fb_loc = 0;
        if (location_resolve(cli_lat, cli_lon, &astro.observer, &fb_loc) != 0) {
            fprintf(stderr, "voidwatch: invalid latitude/longitude.\n");
            return 2;
        }
        if (fb_loc) {
            fprintf(stderr,
                "voidwatch: no location set — defaulting to 0,0 (Atlantic).\n"
                "  set --lat/--lon, $VOIDWATCH_LAT/$VOIDWATCH_LON, or\n"
                "  ~/.config/voidwatch/location.conf.\n");
        }
    }

    term_setup();

    /* Audio is best-effort. Failure leaves the snapshot zeroed and every
     * modulation factor collapses to 1.0 (identity). */
    int audio_rc = no_audio ? 0 : audio_init(device);

    int cols = 0, rows = 0;
    Framebuffer   fb = {0};
    Starfield     sf = {0};
    BodySystem    bs = {0};
    ParticleArena pa = {0};

    int initialized      = 0;
    int bodies_initialized = 0;
    int show_hud  = 1;
    int show_help = 0;
    int quitting  = 0;
    float cam_x = 0.0f, cam_y = 0.0f;
    double t_total = 0.0;
    double phys_accum = 0.0;
    /* 0 (not -1e9) so the no-audio fallback gates on t_total directly: the
     * first fallback fires at ~SUPERNOVA_FALLBACK_PERIOD, not on frame 1. */
    double last_supernova_t = 0.0;

    /* Astro-mode virtual clock. `astro_speed` scales time progression;
     * `astro_offset` is a free additive scrub in seconds. The virtual
     * `now` we feed astro_update is real time + offset + integrated speed.
     * If --at was passed, seed the offset so the virtual clock starts
     * at the requested moment. */
    double astro_speed  = 1.0;
    double astro_offset = at_set ? (double)(at_time - time(NULL)) : 0.0;

    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);

    while (!term_should_quit() && !quitting) {
        /* Drain queued keys this frame. Order is meaningful — Esc/q must
         * win even if a help/hud key sits behind it. */
        for (;;) {
            int k = term_poll_key();
            if (!k) break;
            /* In astro cursor mode hjkl move the reticle instead of toggling
             * HUD. `c` toggles cursor mode; Esc exits cursor mode if active,
             * else quits. */
            if (k == 'q' || k == 'Q') { quitting = 1; break; }
            else if (k == 27) {
                if (astro_mode && astro.cursor_active) astro.cursor_active = 0;
                else                                    { quitting = 1; break; }
            }
            else if (astro_mode && astro.cursor_active &&
                     (k == 'h' || k == 'j' || k == 'k' || k == 'l')) {
                int dx = (k == 'l') - (k == 'h');
                int dy = (k == 'j') - (k == 'k');
                astro.cursor_col += dx * 2;
                astro.cursor_row += dy;
            }
            else if (k == 'h' || k == 'H')       { show_hud  = !show_hud;  }
            else if (k == '?')                   { show_help = !show_help; }
            else if (astro_mode && (k == 'g' || k == 'G')) {
                astro.show_grid = !astro.show_grid;
            }
            else if (astro_mode && (k == 'l' || k == 'L')) {
                astro.show_constellations = !astro.show_constellations;
            }
            else if (astro_mode && (k == 't' || k == 'T')) {
                astro.show_trails = !astro.show_trails;
            }
            else if (astro_mode && (k == 'm' || k == 'M')) {
                astro.view_mode = !astro.view_mode;
            }
            else if (astro_mode && (k == 's' || k == 'S')) {
                astro.show_star_backdrop = !astro.show_star_backdrop;
            }
            else if (astro_mode && (k == 'd' || k == 'D')) {
                astro.show_dso = !astro.show_dso;
            }
            else if (astro_mode && (k == 'a' || k == 'A')) {
                astro.show_aurora = !astro.show_aurora;
            }
            /* Capital T toggles track mode (lowercase t is trails). */
            else if (astro_mode && k == 'T') {
                if (astro.track_active) {
                    astro.track_active = 0;
                    hud_log_event(t_total, "track off");
                } else {
                    astro_track_arm(&astro, cols, rows);
                    if (astro.track_active) {
                        char buf[32];
                        snprintf(buf, sizeof buf, "tracking %d:%d",
                                 astro.track_kind, astro.track_idx);
                        hud_log_event(t_total, buf);
                    }
                }
            }
            else if (astro_mode && (k == 'c' || k == 'C')) {
                astro.cursor_active = !astro.cursor_active;
                if (astro.cursor_active && astro.cursor_col == 0
                                        && astro.cursor_row == 0) {
                    astro.cursor_col = cols / 2;
                    astro.cursor_row = rows / 2;
                }
            }
            /* Astro-mode time controls. Silently ignored in sandbox mode
             * (the n-body sim has no notion of "rewind"). */
            else if (astro_mode && (k == '+' || k == '=')) {
                astro_speed *= 10.0;
                if (astro_speed > 1e6) astro_speed = 1e6;
            }
            else if (astro_mode && k == '-') {
                astro_speed /= 10.0;
                if (astro_speed < 1e-6 && astro_speed > -1e-6) astro_speed = 1e-6;
            }
            else if (astro_mode && k == '0') {
                astro_speed  = 1.0;
                astro_offset = 0.0;
            }
            else if (astro_mode && k == ',') { astro_offset -= 3600.0; }
            else if (astro_mode && k == '.') { astro_offset += 3600.0; }
            else if (astro_mode && (k == 'j' || k == 'J')
                     && !astro.cursor_active) {
                /* Jump the virtual clock to the next "interesting" event:
                 * eclipse, planet-planet conjunction, shower peak. Search
                 * up to 400 days forward from the current virt_jd. */
                double jd_now = ephem_julian_day_from_unix(
                                  time(NULL) + (time_t)astro_offset);
                double jd_event;
                char   label[28];
                if (astro_find_next_event(&astro, jd_now, 400,
                                          &jd_event, label, sizeof label) == 0) {
                    /* Convert event JD back to a unix-time offset. */
                    time_t target_unix =
                        (time_t)((jd_event - 2440587.5) * 86400.0);
                    astro_offset += (double)(target_unix - (time(NULL)
                                              + (time_t)astro_offset));
                    char buf[40];
                    snprintf(buf, sizeof buf, "jump → %s", label);
                    hud_log_event(t_total, buf);
                } else {
                    hud_log_event(t_total, "no events in next 400 days");
                }
            }
        }
        if (quitting) break;

        if (term_consume_resize() || !initialized) {
            int new_cols, new_rows;
            term_get_size(&new_cols, &new_rows);
            if (new_cols != cols || new_rows != rows || !initialized) {
                if (initialized) {
                    fb_free(&fb);
                    starfield_free(&sf);
                    particle_free(&pa);
                    /* Body system is intentionally preserved across resize —
                     * it lives in its own world coordinates, decoupled from
                     * viewport size. Reinitialising would wipe orbital state. */
                }
                cols = new_cols;
                rows = new_rows;
                if (fb_init(&fb, cols, rows) != 0)                  break;
                if (starfield_init(&sf, cols, rows) != 0)           break;
                if (!astro_mode && !bodies_initialized) {
                    if (body_system_init(&bs, cols * 2, rows * 4) != 0) break;
                    bodies_initialized = 1;
                }
                if (particle_init(&pa) != 0)                        break;
                /* Re-centre the camera on the new system. */
                cam_x = 0.0f;
                cam_y = 0.0f;
                fputs("\x1b[2J", stdout);
                initialized = 1;
            }
        }

        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        double dt = ts_diff(frame_start, last);
        if (dt > 0.25) dt = 0.25; /* clamp after stalls */
        last = frame_start;
        t_total += dt;

        cam_x += CAMERA_DRIFT_X * (float)dt;
        cam_y += CAMERA_DRIFT_Y * (float)dt;

        /* Step physics in fixed slices; cap iterations to avoid death-spiral
         * after a long stall. */
        AudioSnapshot snap = audio_snapshot();

        if (astro_mode) {
            /* Integrate speed adjustments into the offset so scrub keys
             * compose cleanly with speed-up/slow-down. */
            astro_offset += dt * (astro_speed - 1.0);
            time_t virt_now = time(NULL) + (time_t)astro_offset;
            astro_update(&astro, virt_now);
            astro_track_tick(&astro, cols, rows);
            astro_surface_events(&astro, t_total);
        }

        /* Supernova: hard transient + cooldown spawns a photon shell from
         * a luminous body. Sandbox-only — astro mode bodies are real and
         * don't explode on a kick drum. */
        if (!astro_mode
            && snap.transient > SUPERNOVA_THRESHOLD
            && t_total - last_supernova_t > SUPERNOVA_COOLDOWN) {
            trigger_supernova(&bs, &pa, t_total);
            last_supernova_t = t_total;
        }
        /* No-audio fallback: the transient path never fires without ALSA, so
         * keep the event alive on a slow timer. Sandbox-only. */
        if (!astro_mode && no_audio
            && t_total - last_supernova_t > SUPERNOVA_FALLBACK_PERIOD) {
            trigger_supernova(&bs, &pa, t_total);
            last_supernova_t = t_total;
        }

        phys_accum += dt;
        int steps = 0;
        while (phys_accum >= PHYSICS_DT && steps < 8) {
            if (!astro_mode) body_step(&bs, PHYSICS_DT);
            particle_update(&pa, &bs, fb.sub_w, fb.sub_h,
                            cam_x, cam_y, PHYSICS_DT, &snap);
            phys_accum -= PHYSICS_DT;
            steps++;
        }
        if (phys_accum > PHYSICS_DT * 4.0) phys_accum = 0.0;

        /* Sub-bass camera shake: cheap white-noise jitter, applied only
         * to the *render* camera so it doesn't perturb starfield wrap or
         * physics state. */
        float sub = snap.bands[BAND_SUB];
        float shake_x = 0.0f, shake_y = 0.0f;
        if (sub > 0.01f) {
            float r1 = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
            float r2 = ((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f;
            shake_x = r1 * sub * MOD_SUBBASS_SHAKE;
            shake_y = r2 * sub * MOD_SUBBASS_SHAKE;
        }
        float rcam_x = cam_x + shake_x;
        float rcam_y = cam_y + shake_y;

        /* Astro decay scales with time-scrub speed. At 1x (real time,
         * most use) we use the sandbox-grade 0.92 — full star flicker /
         * twinkle pulses, no body smear because per-frame body motion
         * is sub-pixel. At 1000x+ scrub we drop toward 0.5 — bodies
         * fade in ~4 frames so they don't leave grid-blob trails when
         * they're crossing many pixels per frame. Log interpolation
         * across the [1, 1000] range gives a smooth handoff. */
        if (astro_mode) {
            double s = fabs(astro_speed);
            float decay;
            if (s <= 1.0)            decay = 0.92f;
            else if (s >= 1000.0)    decay = 0.50f;
            else {
                float t = (float)(log10(s) / 3.0);
                decay = 0.92f - 0.42f * t;
            }
            fb_decay(&fb, decay);
        }
        else if (g_config.fb_decay < 0.999f) fb_decay(&fb, g_config.fb_decay);
        else                                 fb_clear(&fb);

        /* Sandbox-only background layers. Astro mode owns its full
         * composite (HYG stars, MW, twilight, etc. for geo; clean black
         * for helio) — letting the parallax starfield + Perlin nebula
         * leak through would just produce flickering noise behind the
         * real ephemeris. */
        if (!astro_mode) {
            nebula_draw(&fb, rcam_x, rcam_y, t_total, &snap);
            starfield_draw(&sf, &fb, rcam_x, rcam_y, t_total, &snap);
            particle_draw(&pa, &fb, rcam_x, rcam_y);
        }
        if (astro_mode) {
            /* Decorative parallax starfield as a backdrop. In helio
             * mode it's the only star layer; in geo mode it sits behind
             * the real HYG catalogue, giving a lively twinkle on top of
             * the real positions. Toggle off with `s` for a pure
             * astronomical view. */
            if (astro.show_star_backdrop) {
                starfield_draw(&sf, &fb, rcam_x, rcam_y, t_total, &snap);
            }
            astro_draw(&astro, &fb, cols, rows, &snap);
        } else {
            body_draw(&bs, &fb, rcam_x, rcam_y, &snap);
        }
        render_flush(&fb, stdout);
        if (!astro_mode) {
            starfield_spikes(&sf, stdout, cols, rows,
                             rcam_x, rcam_y, t_total, &snap);
        }
        if (astro_mode) {
            astro_labels(&astro, stdout, cols, rows);
        }
        if (show_hud) {
            hud_draw(stdout, cols, rows, &bs, &snap,
                     cam_x, cam_y, t_total, astro_mode);
            if (astro_mode) {
                astro_hud(&astro, stdout, cols, rows, t_total,
                          astro_speed, astro_offset);
            }
        }
        if (show_help) {
            hud_help_overlay(stdout, cols, rows);
        }
        fflush(stdout);

        struct timespec frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        long elapsed_ns = (long)((frame_end.tv_sec  - frame_start.tv_sec)  * 1000000000L)
                        + (long) (frame_end.tv_nsec - frame_start.tv_nsec);
        long sleep_ns = FRAME_NS - elapsed_ns;
        if (sleep_ns > 0) {
            struct timespec ts = { sleep_ns / 1000000000L,
                                   sleep_ns % 1000000000L };
            nanosleep(&ts, NULL);
        }
    }

    if (initialized) {
        fb_free(&fb);
        starfield_free(&sf);
        particle_free(&pa);
    }
    if (bodies_initialized) body_system_free(&bs);
    audio_shutdown();
    term_teardown();

    if (audio_rc != 0) {
        fprintf(stderr,
            "voidwatch: audio capture unavailable.\n"
            "  set $VOIDWATCH_AUDIO_DEVICE (e.g. \"pulse\", \"pipewire\",\n"
            "  or a specific PCM source) and route a monitor in pavucontrol.\n");
    }
    return 0;
}
