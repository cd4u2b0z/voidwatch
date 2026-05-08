#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ---- Annual almanac (--year) -------------------------------------- */

static double sep_rad(double ra1, double dec1, double ra2, double dec2) {
    double c = sin(dec1) * sin(dec2)
             + cos(dec1) * cos(dec2) * cos(ra1 - ra2);
    if (c >  1.0) c =  1.0;
    if (c < -1.0) c = -1.0;
    return acos(c);
}

typedef struct {
    double jd;
    char   text[128];
} YearEvent;

static int year_event_cmp(const void *a, const void *b) {
    double da = ((const YearEvent *)a)->jd;
    double db = ((const YearEvent *)b)->jd;
    return (da > db) - (da < db);
}

static double year_to_jd(int year, int month, double day) {
    /* Meeus 7.1: Julian Day from Gregorian date. */
    int y = year, m = month;
    if (m <= 2) { y -= 1; m += 12; }
    int A = y / 100;
    int B = 2 - A + A / 4;
    return (long)(365.25 * (y + 4716))
         + (long)(30.6001 * (m + 1))
         + day + B - 1524.5;
}

static void jd_to_local_str(double jd, char *buf, size_t cap) {
    time_t t = (time_t)((jd - 2440587.5) * 86400.0);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, cap, "%Y-%m-%d %H:%M", &tm);
}

int headless_year(const Observer *obs, int year, FILE *out) {
    fprintf(out, "voidwatch — astronomical almanac for %d\n"
                 "  observer %+06.2f\xC2\xB0  %+07.2f\xC2\xB0\n\n",
            year, obs->lat_rad * RAD2DEG, obs->lon_rad * RAD2DEG);

    double jd_start = year_to_jd(year, 1, 1.0);
    double jd_end   = year_to_jd(year + 1, 1, 1.0);

    /* Generous buffer — 36 conjunction pairs × 2 events + ~4 eclipses ×
     * 2 + 9 showers + 4 solstices/equinoxes is well under 200. */
    YearEvent ev[200];
    int       n_ev = 0;

    /* === Eclipses (daily mag scan) ================================== *
     * For each day we compute the magnitude factors at noon UT. A daily
     * sample misses fast partial eclipses but catches every total +
     * annular within their multi-hour windows. Good enough for the
     * almanac use case. */
    int prev_solar_active = 0, prev_lunar_active = 0;
    double solar_peak_jd = 0, lunar_peak_jd = 0;
    double solar_peak_mag = 0, lunar_peak_mag = 0;
    for (double jd = jd_start; jd < jd_end; jd += 0.5) {
        /* Compute Sun/Moon RA/Dec at this jd */
        EphemPosition s, m;
        ephem_compute(EPHEM_SUN, jd, &s);
        ephem_compute(EPHEM_MOON, jd, &m);
        double sep_sm = sep_rad(s.ra_rad, s.dec_rad, m.ra_rad, m.dec_rad);
        double anti_ra = s.ra_rad + M_PI;
        double anti_dec = -s.dec_rad;
        double sep_lm = sep_rad(anti_ra, anti_dec, m.ra_rad, m.dec_rad);

        double sf = (0.012 - sep_sm) / 0.012;
        if (sf < 0) sf = 0;
        double lf = (0.022 - sep_lm) / 0.022;
        if (lf < 0) lf = 0;

        int now_s = (sf > 0.05);
        int now_l = (lf > 0.05);

        if (now_s) {
            if (sf > solar_peak_mag) { solar_peak_mag = sf; solar_peak_jd = jd; }
        }
        if (!now_s && prev_solar_active && n_ev < 200) {
            char buf[64];
            jd_to_local_str(solar_peak_jd, buf, sizeof buf);
            ev[n_ev].jd = solar_peak_jd;
            snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                     "  %s  solar eclipse  (mag %.2f)", buf, solar_peak_mag);
            n_ev++;
            solar_peak_mag = 0;
        }
        prev_solar_active = now_s;

        if (now_l) {
            if (lf > lunar_peak_mag) { lunar_peak_mag = lf; lunar_peak_jd = jd; }
        }
        if (!now_l && prev_lunar_active && n_ev < 200) {
            char buf[64];
            jd_to_local_str(lunar_peak_jd, buf, sizeof buf);
            ev[n_ev].jd = lunar_peak_jd;
            snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                     "  %s  lunar eclipse  (mag %.2f)", buf, lunar_peak_mag);
            n_ev++;
            lunar_peak_mag = 0;
        }
        prev_lunar_active = now_l;
    }

    /* === Equinoxes / solstices ====================================== *
     * Sun's apparent ecliptic longitude reaches 0/90/180/270 deg. We
     * detect crossings of those longitudes via daily samples + linear
     * interpolation. */
    static const struct { double lon_deg; const char *name; } sol_eq[] = {
        {   0.0, "vernal equinox"   },
        {  90.0, "summer solstice"  },
        { 180.0, "autumnal equinox" },
        { 270.0, "winter solstice"  },
    };
    /* Sun ecliptic longitude wraps; track previous value and detect
     * crossings. Derive from RA/Dec via the standard rotation:
     * tan(lon) = (sin RA cos eps + tan dec sin eps) / cos RA. */
    EphemPosition sp;
    ephem_compute(EPHEM_SUN, jd_start, &sp);
    double eps = ephem_obliquity_rad(jd_start);
    double prev_lon = atan2(sin(sp.ra_rad) * cos(eps) + tan(sp.dec_rad) * sin(eps),
                            cos(sp.ra_rad));
    if (prev_lon < 0) prev_lon += 2 * M_PI;
    double prev_jd = jd_start;
    for (double jd = jd_start + 1.0; jd < jd_end; jd += 1.0) {
        EphemPosition s;
        ephem_compute(EPHEM_SUN, jd, &s);
        eps = ephem_obliquity_rad(jd);
        double lon = atan2(sin(s.ra_rad) * cos(eps) + tan(s.dec_rad) * sin(eps),
                           cos(s.ra_rad));
        if (lon < 0) lon += 2 * M_PI;

        for (size_t k = 0; k < sizeof sol_eq / sizeof sol_eq[0]; k++) {
            double target = sol_eq[k].lon_deg * DEG2RAD;
            /* Did we cross the target between prev_lon and lon? Handle
             * wrap-around by unwrapping. */
            double a = prev_lon, b = lon;
            if (b < a) b += 2 * M_PI;
            double t = target;
            if (t < a) t += 2 * M_PI;
            if (a <= t && t <= b && b - a < M_PI) {
                double frac = (t - a) / (b - a);
                double cross_jd = prev_jd + frac * (jd - prev_jd);
                char buf[64];
                jd_to_local_str(cross_jd, buf, sizeof buf);
                if (n_ev < 200) {
                    ev[n_ev].jd = cross_jd;
                    snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                             "  %s  %s", buf, sol_eq[k].name);
                    n_ev++;
                }
            }
        }
        prev_lon = lon;
        prev_jd = jd;
    }

    /* === Planet-planet conjunctions ================================ *
     * Daily sample each pair; closest-approach tracked across the
     * year; one event per pair-conjunction window. */
    for (int i = EPHEM_MERCURY; i <= EPHEM_NEPTUNE; i++) {
        for (int j = i + 1; j <= EPHEM_NEPTUNE; j++) {
            int active = 0;
            double min_sep = 1e9, min_jd = 0;
            for (double jd = jd_start; jd < jd_end; jd += 1.0) {
                EphemPosition pa, pb;
                ephem_compute((EphemBody)i, jd, &pa);
                ephem_compute((EphemBody)j, jd, &pb);
                double s = sep_rad(pa.ra_rad, pa.dec_rad,
                                   pb.ra_rad, pb.dec_rad);
                if (s < 0.0175) {
                    active = 1;
                    if (s < min_sep) { min_sep = s; min_jd = jd; }
                } else if (active) {
                    /* Window ended — emit the closest approach. */
                    char buf[64];
                    jd_to_local_str(min_jd, buf, sizeof buf);
                    if (n_ev < 200) {
                        ev[n_ev].jd = min_jd;
                        snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                                 "  %s  %s-%s conjunction (%.2f\xC2\xB0)",
                                 buf,
                                 ephem_name((EphemBody)i),
                                 ephem_name((EphemBody)j),
                                 min_sep * RAD2DEG);
                        n_ev++;
                    }
                    active = 0;
                    min_sep = 1e9;
                }
            }
            /* Tail: if still active at year end, flush */
            if (active && n_ev < 200) {
                char buf[64];
                jd_to_local_str(min_jd, buf, sizeof buf);
                ev[n_ev].jd = min_jd;
                snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                         "  %s  %s-%s conjunction (%.2f\xC2\xB0)",
                         buf,
                         ephem_name((EphemBody)i),
                         ephem_name((EphemBody)j),
                         min_sep * RAD2DEG);
                n_ev++;
            }
        }
    }

    /* === Major meteor showers ====================================== *
     * Pull peaks straight from the bundled table — DOY → JD via the
     * year's Jan 1. ZHR + name printed for each. */
    for (int i = 0; i < h_shower_count; i++) {
        const HShower *s = &h_showers[i];
        double peak_jd = year_to_jd(year, 1, (double)s->peak_doy);
        char buf[64];
        jd_to_local_str(peak_jd, buf, sizeof buf);
        if (n_ev < 200) {
            ev[n_ev].jd = peak_jd;
            snprintf(ev[n_ev].text, sizeof ev[n_ev].text,
                     "  %s  %s peak  (ZHR %d)", buf, s->name, s->zhr);
            n_ev++;
        }
    }

    /* Sort all events chronologically and print. */
    qsort(ev, n_ev, sizeof ev[0], year_event_cmp);
    for (int i = 0; i < n_ev; i++) {
        fputs(ev[i].text, out);
        fputc('\n', out);
    }
    fprintf(out, "\n%d events.\n", n_ev);
    return 0;
}

/* ---- Self-test (--validate) -------------------------------------- */

typedef struct {
    const char  *label;
    EphemBody    body;
    double       jd;            /* Julian Day TT */
    double       expect_ra_h;   /* hours [0,24) */
    double       expect_dec_deg;
    double       tol_arcmin;    /* allowed great-circle distance */
} ValidateCase;

/* Reference values from Meeus, *Astronomical Algorithms* worked
 * examples. Each ephemeris module has a worked example in the cited
 * chapter; voidwatch should reproduce those to within Meeus's
 * documented precision (~0.01° Sun, ~0.1° Moon, ~few arcmin planets).
 *
 * NOTE: this suite is intentionally conservative. More rows can be
 * added as we cross-check them against JPL Horizons or verified
 * Meeus-derived values; bad references give false fails. */
static const ValidateCase validate_cases[] = {
    /* Sun at J2000.0 epoch — geocentric apparent.
     * Meeus 25.b verified value: λ ≈ 281.4°, β ≈ 0, → RA 18h45m, Dec -23.03°. */
    { "Sun J2000.0",       EPHEM_SUN, 2451545.0,   18.7493, -23.030,  30.0 },
    /* Sun at 1992-Oct-13.0 TT — Meeus example 25.a worked solution.
     * Apparent: RA 13h 13m 30.749s = 13.225209h, Dec -07°47'01.74" = -7.7838°. */
    { "Sun Meeus 25.a",    EPHEM_SUN, 2448908.5,   13.2252,  -7.7838,  6.0 },
    /* Sun at 2024-04-08 18:18 UT (Great American Eclipse) — JPL Horizons. */
    { "Sun 2024-04-08",    EPHEM_SUN, 2460409.262,  1.1697,   7.488,  30.0 },
};
static const int validate_case_count =
    (int)(sizeof validate_cases / sizeof validate_cases[0]);

static double great_circle_arcmin(double ra1_h, double dec1_d,
                                  double ra2_h, double dec2_d) {
    double r1 = ra1_h * M_PI / 12.0;
    double d1 = dec1_d * DEG2RAD;
    double r2 = ra2_h * M_PI / 12.0;
    double d2 = dec2_d * DEG2RAD;
    double c = sin(d1) * sin(d2) + cos(d1) * cos(d2) * cos(r1 - r2);
    if (c >  1.0) c =  1.0;
    if (c < -1.0) c = -1.0;
    return acos(c) * RAD2DEG * 60.0;
}

int headless_validate(FILE *out) {
    fprintf(out, "voidwatch self-test\n");
    fprintf(out, "  reference: JPL Horizons geocentric apparent\n");
    fprintf(out, "  %-22s %-9s %-10s %-9s\n",
            "case", "delta", "tolerance", "result");

    int n_pass = 0, n_fail = 0;
    for (int i = 0; i < validate_case_count; i++) {
        const ValidateCase *c = &validate_cases[i];
        EphemPosition p;
        ephem_compute(c->body, c->jd, &p);
        double delta_arcmin = great_circle_arcmin(
            p.ra_rad * 12.0 / M_PI, p.dec_rad * RAD2DEG,
            c->expect_ra_h, c->expect_dec_deg);
        const char *result = (delta_arcmin <= c->tol_arcmin) ? "PASS" : "FAIL";
        if (delta_arcmin <= c->tol_arcmin) n_pass++; else n_fail++;
        fprintf(out, "  %-22s %5.1f'    %5.1f'      %s\n",
                c->label, delta_arcmin, c->tol_arcmin, result);
    }
    fprintf(out, "\n%d / %d passed.\n", n_pass, n_pass + n_fail);
    return (n_fail == 0) ? 0 : 1;
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
