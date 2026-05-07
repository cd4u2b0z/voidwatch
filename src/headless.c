#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "asteroid.h"
#include "comet.h"
#include "ephem.h"
#include "headless.h"
#include "vwconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* ---- Time helpers --------------------------------------------------- */

static void format_local(time_t t, char *buf, size_t cap) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%d %H:%M %Z", &tm);
}

static void format_local_short(time_t t, char *buf, size_t cap) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, cap, "%H:%M", &tm);
}

static void format_iso(time_t t, char *buf, size_t cap) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%S%z", &tm);
}

/* ---- Rise/set search ------------------------------------------------ *
 *
 * Bisection on alt(t)=0. Cheap because Meeus low-precision is itself
 * cheap; one ephem_compute is well under a millisecond. We coarse-search
 * in 10-minute steps for a sign change, then bisect to ~30s precision.
 */

static double alt_for_planet(const Observer *obs, int idx, time_t t) {
    EphemPosition pos;
    double jd = ephem_julian_day_from_unix(t);
    ephem_compute((EphemBody)idx, jd, &pos);
    ephem_to_topocentric(&pos, obs, jd);
    return pos.alt_rad;
}

static double alt_for_comet(const Observer *obs, int idx, time_t t) {
    CometState states[COMET_COUNT];
    double jd = ephem_julian_day_from_unix(t);
    comet_compute_all(jd, states);
    EphemPosition tmp = {
        .ra_rad      = states[idx].ra_rad,
        .dec_rad     = states[idx].dec_rad,
        .distance_au = states[idx].dist_au,
    };
    ephem_to_topocentric(&tmp, obs, jd);
    return tmp.alt_rad;
}

static double alt_for_asteroid(const Observer *obs, int idx, time_t t) {
    AsteroidState states[ASTEROID_COUNT];
    double jd = ephem_julian_day_from_unix(t);
    asteroid_compute_all(jd, states);
    EphemPosition tmp = {
        .ra_rad      = states[idx].ra_rad,
        .dec_rad     = states[idx].dec_rad,
        .distance_au = states[idx].dist_au,
    };
    ephem_to_topocentric(&tmp, obs, jd);
    return tmp.alt_rad;
}

typedef double (*alt_fn)(const Observer *obs, int idx, time_t t);

/* Coarse scan + bisection. Returns 0 + writes `*found_t` if a sign
 * change of the given direction is located within `horizon_seconds`,
 * else returns -1. Direction: +1 means "alt crosses zero going up"
 * (rise), -1 means going down (set). */
static int search_zero_crossing(const Observer *obs, alt_fn fn, int idx,
                                time_t start, long horizon_seconds,
                                int direction, time_t *found_t) {
    const long step = 600;             /* 10 minutes coarse */
    double prev = fn(obs, idx, start);
    for (long s = step; s <= horizon_seconds; s += step) {
        time_t t = start + s;
        double cur = fn(obs, idx, t);
        int rising  = prev < 0.0 && cur >= 0.0;
        int setting = prev > 0.0 && cur <= 0.0;
        int hit = (direction > 0 ? rising : setting);
        if (hit) {
            /* Bisect between (start+s-step) and (start+s). */
            time_t lo = start + s - step;
            time_t hi = t;
            double lo_alt = prev;
            for (int it = 0; it < 24; it++) {
                time_t mid = lo + (hi - lo) / 2;
                if (mid == lo || mid == hi) break;
                double mid_alt = fn(obs, idx, mid);
                if ((mid_alt < 0.0) == (lo_alt < 0.0)) {
                    lo = mid; lo_alt = mid_alt;
                } else {
                    hi = mid;
                }
            }
            *found_t = lo + (hi - lo) / 2;
            return 0;
        }
        prev = cur;
    }
    return -1;
}

/* Nearest-future rise (alt crosses 0 going up). */
static int find_next_rise(const Observer *obs, alt_fn fn, int idx,
                          time_t now, long horizon_seconds, time_t *out_t) {
    return search_zero_crossing(obs, fn, idx, now, horizon_seconds, +1, out_t);
}

/* Nearest-future set (alt crosses 0 going down). */
static int find_next_set(const Observer *obs, alt_fn fn, int idx,
                         time_t now, long horizon_seconds, time_t *out_t) {
    return search_zero_crossing(obs, fn, idx, now, horizon_seconds, -1, out_t);
}

/* ---- Active meteor shower ------------------------------------------- *
 *
 * Reused from astro.c, replicated here because that's compiled into the
 * render module. Small enough to duplicate. */
typedef struct {
    const char *name;
    int    peak_doy;
    float  fwhm_days;
    double ra_h, dec_deg;
    int    zhr;
} HShower;

static const HShower h_showers[] = {
    { "Quadrantids",    3, 1.0,  15.33,  49.7, 110 },
    { "Lyrids",       112, 2.0,  18.07,  34.0,  18 },
    { "Eta Aquariids",126, 5.0,  22.50,  -1.0,  60 },
    { "Perseids",     224, 4.0,   3.20,  58.0, 110 },
    { "Draconids",    281, 0.5,  17.43,  54.0,  10 },
    { "Orionids",     294, 3.0,   6.33,  16.0,  25 },
    { "Leonids",      321, 1.0,  10.13,  22.0,  15 },
    { "Geminids",     348, 2.0,   7.47,  33.0, 150 },
    { "Ursids",       356, 0.5,  14.47,  76.0,  10 },
};
static const int h_shower_count = sizeof h_showers / sizeof h_showers[0];

static const HShower *find_active_shower(double jd, float *out_zhr_now) {
    double frac = jd - 2451544.5;
    double yfrac = fmod(frac / 365.25, 1.0);
    if (yfrac < 0.0) yfrac += 1.0;
    double doy = yfrac * 365.25 + 1.0;
    const HShower *best = NULL;
    float best_rate = 0.0f;
    for (int i = 0; i < h_shower_count; i++) {
        const HShower *s = &h_showers[i];
        double d = doy - s->peak_doy;
        if (d >  200.0) d -= 365.25;
        if (d < -200.0) d += 365.25;
        float sigma = s->fwhm_days / 2.355f;
        if (sigma < 0.1f) sigma = 0.1f;
        float gauss = expf(-(float)(d * d) / (2.0f * sigma * sigma));
        float rate  = (float)s->zhr * gauss;
        if (rate > best_rate) { best_rate = rate; best = s; }
    }
    if (out_zhr_now) *out_zhr_now = best_rate;
    return best;
}

/* ---- Body name lookup ----------------------------------------------- */

static int strieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

typedef enum {
    LOOKUP_NONE = 0,
    LOOKUP_PLANET,
    LOOKUP_COMET,
    LOOKUP_ASTEROID,
} LookupKind;

typedef struct {
    LookupKind  kind;
    int         idx;
    const char *display;
} BodyLookup;

static BodyLookup lookup_body(const char *name) {
    BodyLookup r = { LOOKUP_NONE, -1, NULL };
    /* Planets: full name + 3-char short. */
    for (int i = 0; i < EPHEM_COUNT; i++) {
        const char *full  = ephem_name((EphemBody)i);
        const char *short_ = ephem_short((EphemBody)i);
        if (strieq(full, name) || strieq(short_, name)) {
            r.kind = LOOKUP_PLANET; r.idx = i; r.display = full;
            return r;
        }
    }
    for (int i = 0; i < COMET_COUNT; i++) {
        if (strieq(comet_elements[i].name, name)) {
            r.kind = LOOKUP_COMET; r.idx = i;
            r.display = comet_elements[i].name;
            return r;
        }
    }
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        if (strieq(asteroid_elements[i].name, name)) {
            r.kind = LOOKUP_ASTEROID; r.idx = i;
            r.display = asteroid_elements[i].name;
            return r;
        }
    }
    return r;
}

/* ---- Public API ----------------------------------------------------- */

int headless_tonight(const Observer *obs, time_t now, FILE *out) {
    char now_buf[64];
    format_local(now, now_buf, sizeof now_buf);
    fprintf(out, "voidwatch — %+06.2f° %+07.2f°  %s\n\n",
            obs->lat_rad * RAD2DEG, obs->lon_rad * RAD2DEG, now_buf);

    /* Per-planet table: alt now, next rise/set within 24h. */
    fprintf(out, "%-9s  %-7s  %-6s  %-6s  %-7s\n",
            "Body", "Mag", "Alt", "Rise", "Set");
    fprintf(out, "%-9s  %-7s  %-6s  %-6s  %-7s\n",
            "----", "---", "---", "----", "---");
    for (int i = 0; i < EPHEM_COUNT; i++) {
        EphemPosition pos;
        double jd = ephem_julian_day_from_unix(now);
        ephem_compute((EphemBody)i, jd, &pos);
        ephem_to_topocentric(&pos, obs, jd);

        char rise_buf[16] = "--:--";
        char set_buf [16] = "--:--";
        time_t found;
        if (pos.alt_rad < 0.0) {
            if (find_next_rise(obs, alt_for_planet, i, now, 86400, &found) == 0)
                format_local_short(found, rise_buf, sizeof rise_buf);
        } else {
            if (find_next_set(obs, alt_for_planet, i, now, 86400, &found) == 0)
                format_local_short(found, set_buf, sizeof set_buf);
            /* Also fetch the next rise *after* it sets, if within 24h. */
            time_t next_rise_t;
            if (find_next_rise(obs, alt_for_planet, i,
                               (find_next_set(obs, alt_for_planet, i, now,
                                              86400, &found) == 0) ? found : now,
                               86400, &next_rise_t) == 0)
                format_local_short(next_rise_t, rise_buf, sizeof rise_buf);
        }

        fprintf(out, "%-9s  %+6.2f   %+5.1f°  %5s   %5s\n",
                ephem_name((EphemBody)i),
                pos.magnitude,
                pos.alt_rad * RAD2DEG,
                rise_buf, set_buf);
    }

    /* Active shower. */
    double jd = ephem_julian_day_from_unix(now);
    float  zhr_now = 0.0f;
    const HShower *sh = find_active_shower(jd, &zhr_now);
    fprintf(out, "\n");
    if (sh && zhr_now > 1.0f) {
        fprintf(out, "Active meteor shower: %s — current rate ≈ %.1f/hr "
                     "(peak ZHR %d on DOY %d)\n",
                sh->name, zhr_now, sh->zhr, sh->peak_doy);
    } else {
        fprintf(out, "No major meteor shower active.\n");
    }

    /* Comets above the visibility cutoff. */
    CometState comets[COMET_COUNT];
    comet_compute_all(jd, comets);
    fprintf(out, "\nComets (mag ≤ %.1f):\n", g_config.comet_mag_cutoff);
    int any_comet = 0;
    for (int i = 0; i < COMET_COUNT; i++) {
        if (comets[i].mag > g_config.comet_mag_cutoff) continue;
        EphemPosition tmp = {
            .ra_rad = comets[i].ra_rad, .dec_rad = comets[i].dec_rad,
            .distance_au = comets[i].dist_au,
        };
        ephem_to_topocentric(&tmp, obs, jd);
        fprintf(out, "  %-26s mag %+5.2f  alt %+5.1f°  r %4.2f AU\n",
                comet_elements[i].name, comets[i].mag,
                tmp.alt_rad * RAD2DEG, comets[i].r_helio_au);
        any_comet = 1;
    }
    if (!any_comet) fprintf(out, "  (none)\n");

    /* Asteroids above cutoff. */
    AsteroidState asts[ASTEROID_COUNT];
    asteroid_compute_all(jd, asts);
    fprintf(out, "\nAsteroids (mag ≤ %.1f):\n", g_config.asteroid_mag_cutoff);
    int any_ast = 0;
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        if (asts[i].mag > g_config.asteroid_mag_cutoff) continue;
        EphemPosition tmp = {
            .ra_rad = asts[i].ra_rad, .dec_rad = asts[i].dec_rad,
            .distance_au = asts[i].dist_au,
        };
        ephem_to_topocentric(&tmp, obs, jd);
        fprintf(out, "  %-26s mag %+5.2f  alt %+5.1f°\n",
                asteroid_elements[i].name, asts[i].mag,
                tmp.alt_rad * RAD2DEG);
        any_ast = 1;
    }
    if (!any_ast) fprintf(out, "  (none)\n");

    return 0;
}

int headless_print_state(const Observer *obs, time_t now, FILE *out, int json) {
    double jd = ephem_julian_day_from_unix(now);
    double lst_h = ephem_local_sidereal_hours(jd, obs->lon_rad);
    char iso_buf[64];
    format_iso(now, iso_buf, sizeof iso_buf);

    EphemPosition planets[EPHEM_COUNT];
    for (int i = 0; i < EPHEM_COUNT; i++) {
        ephem_compute((EphemBody)i, jd, &planets[i]);
        ephem_to_topocentric(&planets[i], obs, jd);
    }
    CometState comets[COMET_COUNT];
    comet_compute_all(jd, comets);
    AsteroidState asts[ASTEROID_COUNT];
    asteroid_compute_all(jd, asts);

    if (!json) {
        fprintf(out, "time          %s  jd=%.5f  lst_h=%.4f\n",
                iso_buf, jd, lst_h);
        fprintf(out, "observer      lat=%+.4f lon=%+.4f\n",
                obs->lat_rad * RAD2DEG, obs->lon_rad * RAD2DEG);
        fprintf(out, "\n%-9s  %7s  %7s  %6s  %6s  %7s  %8s\n",
                "body", "ra_h", "dec_deg", "alt", "az", "mag", "dist_au");
        for (int i = 0; i < EPHEM_COUNT; i++) {
            const EphemPosition *p = &planets[i];
            fprintf(out, "%-9s  %7.3f  %+7.2f  %+6.2f  %6.2f  %+7.2f  %8.4f\n",
                    ephem_name((EphemBody)i),
                    p->ra_rad * RAD2DEG / 15.0,
                    p->dec_rad * RAD2DEG,
                    p->alt_rad * RAD2DEG,
                    p->az_rad  * RAD2DEG,
                    p->magnitude,
                    p->distance_au);
        }
        return 0;
    }

    /* JSON output — careful with commas; build manually. */
    fprintf(out, "{\n");
    fprintf(out, "  \"time\": {\"iso\": \"%s\", \"jd\": %.6f, \"lst_hours\": %.6f},\n",
            iso_buf, jd, lst_h);
    fprintf(out, "  \"observer\": {\"lat_deg\": %.6f, \"lon_deg\": %.6f},\n",
            obs->lat_rad * RAD2DEG, obs->lon_rad * RAD2DEG);
    fprintf(out, "  \"planets\": [\n");
    for (int i = 0; i < EPHEM_COUNT; i++) {
        const EphemPosition *p = &planets[i];
        fprintf(out,
                "    {\"name\": \"%s\", \"ra_h\": %.4f, \"dec_deg\": %.4f, "
                "\"alt_deg\": %.4f, \"az_deg\": %.4f, "
                "\"mag\": %.3f, \"dist_au\": %.6f}%s\n",
                ephem_name((EphemBody)i),
                p->ra_rad * RAD2DEG / 15.0,
                p->dec_rad * RAD2DEG,
                p->alt_rad * RAD2DEG,
                p->az_rad  * RAD2DEG,
                p->magnitude, p->distance_au,
                (i == EPHEM_COUNT - 1) ? "" : ",");
    }
    fprintf(out, "  ],\n");

    fprintf(out, "  \"comets\": [\n");
    for (int i = 0; i < COMET_COUNT; i++) {
        EphemPosition tmp = {
            .ra_rad = comets[i].ra_rad, .dec_rad = comets[i].dec_rad,
            .distance_au = comets[i].dist_au,
        };
        ephem_to_topocentric(&tmp, obs, jd);
        fprintf(out,
                "    {\"name\": \"%s\", \"ra_h\": %.4f, \"dec_deg\": %.4f, "
                "\"alt_deg\": %.4f, \"az_deg\": %.4f, "
                "\"mag\": %.3f, \"dist_au\": %.4f, \"r_helio_au\": %.4f}%s\n",
                comet_elements[i].name,
                comets[i].ra_rad * RAD2DEG / 15.0,
                comets[i].dec_rad * RAD2DEG,
                tmp.alt_rad * RAD2DEG,
                tmp.az_rad  * RAD2DEG,
                comets[i].mag, comets[i].dist_au, comets[i].r_helio_au,
                (i == COMET_COUNT - 1) ? "" : ",");
    }
    fprintf(out, "  ],\n");

    fprintf(out, "  \"asteroids\": [\n");
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        EphemPosition tmp = {
            .ra_rad = asts[i].ra_rad, .dec_rad = asts[i].dec_rad,
            .distance_au = asts[i].dist_au,
        };
        ephem_to_topocentric(&tmp, obs, jd);
        fprintf(out,
                "    {\"name\": \"%s\", \"ra_h\": %.4f, \"dec_deg\": %.4f, "
                "\"alt_deg\": %.4f, \"az_deg\": %.4f, "
                "\"mag\": %.3f, \"dist_au\": %.4f, \"r_helio_au\": %.4f}%s\n",
                asteroid_elements[i].name,
                asts[i].ra_rad * RAD2DEG / 15.0,
                asts[i].dec_rad * RAD2DEG,
                tmp.alt_rad * RAD2DEG,
                tmp.az_rad  * RAD2DEG,
                asts[i].mag, asts[i].dist_au, asts[i].r_helio_au,
                (i == ASTEROID_COUNT - 1) ? "" : ",");
    }
    fprintf(out, "  ],\n");

    /* Active shower block. */
    float zhr_now = 0.0f;
    const HShower *sh = find_active_shower(jd, &zhr_now);
    if (sh && zhr_now > 1.0f) {
        fprintf(out,
                "  \"active_shower\": {\"name\": \"%s\", "
                "\"current_rate_per_hour\": %.2f, "
                "\"peak_zhr\": %d, \"peak_doy\": %d}\n",
                sh->name, zhr_now, sh->zhr, sh->peak_doy);
    } else {
        fprintf(out, "  \"active_shower\": null\n");
    }
    fprintf(out, "}\n");
    return 0;
}

int headless_next_rise(const Observer *obs, time_t now,
                       const char *name, FILE *out) {
    BodyLookup b = lookup_body(name);
    if (b.kind == LOOKUP_NONE) {
        fprintf(stderr, "voidwatch: --next: unknown body: %s\n", name);
        return 2;
    }

    /* If currently above the horizon, look ahead for the next set then
     * rise after that. Otherwise look for the next rise from now. */
    alt_fn fn = NULL;
    switch (b.kind) {
        case LOOKUP_PLANET:   fn = alt_for_planet;   break;
        case LOOKUP_COMET:    fn = alt_for_comet;    break;
        case LOOKUP_ASTEROID: fn = alt_for_asteroid; break;
        case LOOKUP_NONE:     return 2;
    }
    double now_alt = fn(obs, b.idx, now);

    const long thirty_days = 30L * 86400L;
    time_t search_from = now;
    if (now_alt > 0.0) {
        time_t set_t;
        if (find_next_set(obs, fn, b.idx, now, thirty_days, &set_t) == 0) {
            search_from = set_t + 60;        /* nudge past horizon */
        }
    }

    time_t rise_t;
    if (find_next_rise(obs, fn, b.idx, search_from, thirty_days, &rise_t) != 0) {
        fprintf(out, "%s: no rise within 30 days "
                     "(maybe circumpolar above or below for your latitude).\n",
                b.display);
        return 0;
    }

    char rise_buf[64];
    format_local(rise_t, rise_buf, sizeof rise_buf);
    long delta = (long)(rise_t - now);
    long h = delta / 3600;
    long m = (delta / 60) % 60;
    fprintf(out, "%s rises %s  (in %ldh%02ldm)\n",
            b.display, rise_buf, h, m);
    return 0;
}
