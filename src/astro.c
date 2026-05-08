#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "astro.h"
#include "dso.h"
#include "framebuffer.h"
#include "hud.h"
#include "palette.h"
#include "skydata.h"
#include "vwconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* Per-body draw tuning. */
typedef struct {
    Color color;
    float radius_sub;   /* Gaussian disc radius in sub-pixels */
    float intensity;    /* additive peak                       */
    float core_blend;   /* white-shift toward centre           */
} AstroStyle;

/* Forward decls — used by astro_update before their definitions. */
static void trails_capture(AstroState *st);
static unsigned met_rand32(void);     /* used by aurora flare scheduler */

static AstroStyle style_for(EphemBody body) {
    AstroStyle s = { .color = {1,1,1}, .radius_sub = 1.5f,
                     .intensity = 0.7f, .core_blend = 0.6f };
    switch (body) {
    case EPHEM_SUN:     s = (AstroStyle){ {1.00f, 0.90f, 0.55f}, 5.5f, 2.6f, 0.85f }; break;
    case EPHEM_MOON:    s = (AstroStyle){ {0.85f, 0.85f, 0.92f}, 4.5f, 2.0f, 0.40f }; break;
    case EPHEM_MERCURY: s = (AstroStyle){ {0.85f, 0.78f, 0.65f}, 1.4f, 0.8f, 0.30f }; break;
    case EPHEM_VENUS:   s = (AstroStyle){ {1.00f, 0.95f, 0.80f}, 1.8f, 1.6f, 0.55f }; break;
    case EPHEM_MARS:    s = (AstroStyle){ {1.00f, 0.55f, 0.40f}, 1.6f, 1.0f, 0.35f }; break;
    case EPHEM_JUPITER: s = (AstroStyle){ {1.00f, 0.88f, 0.70f}, 2.4f, 1.4f, 0.45f }; break;
    case EPHEM_SATURN:  s = (AstroStyle){ {0.95f, 0.85f, 0.65f}, 2.2f, 1.2f, 0.40f }; break;
    case EPHEM_URANUS:  s = (AstroStyle){ {0.65f, 0.85f, 0.95f}, 1.5f, 0.7f, 0.30f }; break;
    case EPHEM_NEPTUNE: s = (AstroStyle){ {0.55f, 0.65f, 0.95f}, 1.5f, 0.6f, 0.30f }; break;
    default: break;
    }
    return s;
}

void astro_update(AstroState *st, time_t now) {
    st->jd        = ephem_julian_day_from_unix(now);
    st->lst_hours = ephem_local_sidereal_hours(st->jd, st->observer.lon_rad);
    for (int i = 0; i < EPHEM_COUNT; i++) {
        ephem_compute((EphemBody)i, st->jd, &st->pos[i]);
        ephem_to_topocentric(&st->pos[i], &st->observer, st->jd);
    }

    /* Moon phase from geocentric Sun/Moon RA/Dec. Angular separation gives
     * the phase angle (illumination); signed RA difference gives waxing
     * vs waning. RA stands in for ecliptic longitude here — the residual
     * <0.5° error is invisible at our cell scale. */
    const EphemPosition *S = &st->pos[EPHEM_SUN];
    const EphemPosition *M = &st->pos[EPHEM_MOON];
    double cos_sep = sin(S->dec_rad) * sin(M->dec_rad)
                   + cos(S->dec_rad) * cos(M->dec_rad)
                   * cos(S->ra_rad - M->ra_rad);
    if (cos_sep >  1.0) cos_sep =  1.0;
    if (cos_sep < -1.0) cos_sep = -1.0;
    double sep = acos(cos_sep);                  /* [0, π] elongation */
    /* Wrap signed RA delta to (-π, π]. Positive = Moon east of Sun. */
    double dra = M->ra_rad - S->ra_rad;
    while (dra >  M_PI) dra -= 2.0 * M_PI;
    while (dra < -M_PI) dra += 2.0 * M_PI;
    /* Build a [0, 2π) elongation that wraps through full at π. */
    st->moon_elongation = (dra >= 0.0) ? sep : (2.0 * M_PI - sep);
    st->moon_illum      = (1.0 - cos_sep) * 0.5;

    /* Comets: ephemeris in equatorial coords, then topocentric pass to
     * fill alt/az for each. */
    comet_compute_all(st->jd, st->comets);
    for (int i = 0; i < COMET_COUNT; i++) {
        EphemPosition tmp = {
            .ra_rad      = st->comets[i].ra_rad,
            .dec_rad     = st->comets[i].dec_rad,
            .distance_au = st->comets[i].dist_au,
        };
        ephem_to_topocentric(&tmp, &st->observer, st->jd);
        st->comets[i].alt_rad = tmp.alt_rad;
        st->comets[i].az_rad  = tmp.az_rad;
    }

    /* Asteroids: same shape. */
    asteroid_compute_all(st->jd, st->asteroids);
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        EphemPosition tmp = {
            .ra_rad      = st->asteroids[i].ra_rad,
            .dec_rad     = st->asteroids[i].dec_rad,
            .distance_au = st->asteroids[i].dist_au,
        };
        ephem_to_topocentric(&tmp, &st->observer, st->jd);
        st->asteroids[i].alt_rad = tmp.alt_rad;
        st->asteroids[i].az_rad  = tmp.az_rad;
    }

    /* Bundled satellites (Phase 6). The cache inside satellite.c
     * handles parse/init lazily and refuses entries older than 30
     * days. Observer altitude is 0 km (Observer doesn't carry it). */
    satellite_compute_all(st->jd,
                          st->observer.lat_rad, st->observer.lon_rad,
                          0.0, st->satellites);

    trails_capture(st);
}

/* Project alt/az onto screen via azimuthal equidistant centered on zenith.
 * Returns 0 on success and sets sub_x/sub_y; returns -1 if below horizon. */
static int project(int sub_w, int sub_h, double alt, double az,
                   float *sub_x, float *sub_y) {
    if (alt < 0.0) return -1;
    double r_norm = (M_PI * 0.5 - alt) / (M_PI * 0.5);   /* 0 at zenith, 1 at horizon */
    /* Cells are ~2× taller than wide (Braille 2×4); compress y to compensate. */
    float center_x = (float)sub_w * 0.5f;
    float center_y = (float)sub_h * 0.5f;
    float radius   = fminf(center_x, center_y * 0.5f) * 0.92f;
    /* East = +x (az 90°), North = -y (az 0°). */
    *sub_x = center_x + (float)( sin(az) * r_norm) * radius;
    *sub_y = center_y + (float)(-cos(az) * r_norm) * radius * 2.0f;
    return 0;
}

/* ---- Milky Way band ----------------------------------------------------
 *
 * Precompute J2000 (RA, Dec) for points along the galactic equator. Per
 * frame, project each point through the current LST/observer to alt/az,
 * then stamp a Gaussian glow. The diffuse band emerges from many soft
 * stamps overlapping. Rendered with `fb_max` so it doesn't trail under
 * decay or saturate from accumulation.
 *
 * Galactic→equatorial J2000 rotation matrix (column-major equivalent of
 * the standard Liu/Hambly/Hobbs orientation). Galactic basis vector
 * components are mapped to equatorial via eq_i = sum_j R[i][j] * gal_j.
 * Reference vector check: galactic (1,0,0) → equatorial (-0.0549, -0.8734,
 * -0.4838) → RA 266.40°, Dec -28.94° (galactic center, Sgr A*).
 */
#define MW_SAMPLES 360
typedef struct { double ra, dec, weight; } MilkyPoint;
static MilkyPoint mw_points[MW_SAMPLES];
static int mw_inited = 0;

static void milkyway_init(void) {
    static const double R[3][3] = {
        { -0.054875539726,  0.494109453312, -0.867666135858 },
        { -0.873437108010, -0.444829589425, -0.198076386122 },
        { -0.483834985808,  0.746982251810,  0.455983795705 },
    };
    for (int i = 0; i < MW_SAMPLES; i++) {
        double l = (double)i * (2.0 * M_PI / MW_SAMPLES);
        /* Galactic equator: b=0. Cartesian galactic vector. */
        double gx = cos(l), gy = sin(l), gz = 0.0;
        double ex = R[0][0]*gx + R[0][1]*gy + R[0][2]*gz;
        double ey = R[1][0]*gx + R[1][1]*gy + R[1][2]*gz;
        double ez = R[2][0]*gx + R[2][1]*gy + R[2][2]*gz;
        mw_points[i].dec = asin(ez);
        mw_points[i].ra  = atan2(ey, ex);
        if (mw_points[i].ra < 0) mw_points[i].ra += 2.0 * M_PI;

        /* Brightness profile along longitude: peak at galactic center
         * (l=0), secondary swell around Carina/Cygnus regions, slow
         * floor everywhere else. */
        double base = 0.05;
        double core = 0.18 * exp(-(l * l) / (2.0 * 0.45 * 0.45));
        double l2   = l - 2.0 * M_PI;        /* wrap-aware second peak */
        double core2 = 0.18 * exp(-(l2 * l2) / (2.0 * 0.45 * 0.45));
        double cyg  = 0.10 * exp(-((l - 1.40) * (l - 1.40)) / (2.0 * 0.55 * 0.55));
        double car  = 0.10 * exp(-((l - 4.40) * (l - 4.40)) / (2.0 * 0.50 * 0.50));
        mw_points[i].weight = base + core + core2 + cyg + car;
    }
    mw_inited = 1;
}

/* Bennett (1982) atmospheric refraction: true altitude → apparent.
 * R in arcminutes; valid for h0 ≥ -1°. Below that we leave the body alone
 * (it's well under the horizon — refraction can't lift it visibly). */
static double refraction_arcmin(double h_true_rad) {
    double h_deg = h_true_rad * RAD2DEG;
    if (h_deg < -1.0) return 0.0;
    double t = (h_deg + 7.31 / (h_deg + 4.4)) * DEG2RAD;
    return 1.0 / tan(t);
}

static void radec_to_altaz(double ra, double dec, double lat,
                           double lst_rad, double *alt, double *az) {
    double H = lst_rad - ra;
    double sin_alt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(H);
    if (sin_alt >  1.0) sin_alt =  1.0;
    if (sin_alt < -1.0) sin_alt = -1.0;
    *alt = asin(sin_alt);
    /* Apparent altitude: lift by atmospheric refraction (~0.5° at horizon,
     * ~1' at 45°, negligible near zenith). Net effect: bodies stay visible
     * a beat after geometric horizon-set. */
    *alt += refraction_arcmin(*alt) * (DEG2RAD / 60.0);
    double y = -cos(dec) * sin(H);
    double x =  sin(dec) * cos(lat) - cos(dec) * sin(lat) * cos(H);
    *az = atan2(y, x);
    if (*az < 0.0) *az += 2.0 * M_PI;
}

/* Kasten-Young (1989) airmass — accurate to the horizon. X=1 at zenith,
 * X≈38 at the horizon. */
static double airmass(double alt_rad) {
    double h_deg = alt_rad * RAD2DEG;
    if (h_deg < 0.0) h_deg = 0.0;
    double denom = sin(alt_rad)
                 + 0.50572 * pow(h_deg + 6.07995, -1.6364);
    if (denom < 1e-6) denom = 1e-6;
    return 1.0 / denom;
}

/* Apply atmospheric extinction + reddening to a tint at given altitude.
 * `intensity` is scaled by V-band extinction; the colour channels lose
 * blue faster than red. k chosen lower than real-world (0.28) to keep
 * horizon-line objects legible — terminal aesthetic over photometric
 * truth. */
static void apply_extinction(double alt_rad, Color *tint, float *intensity) {
    double X = airmass(alt_rad);
    /* V-band: 0.18 mag/airmass — gentler than reality for legibility. */
    double dim_mag = 0.18 * (X - 1.0);
    if (dim_mag < 0.0) dim_mag = 0.0;
    float dim = (float)pow(10.0, -0.4 * dim_mag);
    *intensity *= dim;
    /* Reddening: blue dims faster than green than red. */
    double extra_b = 0.10 * (X - 1.0);
    double extra_g = 0.04 * (X - 1.0);
    if (extra_b < 0.0) extra_b = 0.0;
    if (extra_g < 0.0) extra_g = 0.0;
    tint->b *= (float)pow(10.0, -0.4 * extra_b);
    tint->g *= (float)pow(10.0, -0.4 * extra_g);
}

/* ---- Twilight horizon glow --------------------------------------------
 *
 * When the Sun is below but near the horizon, civil/nautical/astronomical
 * twilight tints the sky. We render this as a band hugging the horizon
 * ring, brightest in the azimuth quadrant facing the Sun, fading both
 * upward in altitude and around the sky. fb_max so it doesn't smear with
 * decay/LST drift.
 */
static float twilight_factor(double sun_alt_rad) {
    double h = sun_alt_rad * RAD2DEG;
    if (h >=  0.0) return 0.0f;        /* sun up — no twilight render */
    if (h >= -2.0) return 1.0f;
    if (h >= -6.0) return 1.0f - 0.45f * (float)((-2.0 - h) / 4.0);
    if (h >= -12.0) return 0.55f - 0.40f * (float)((-6.0 - h) / 6.0);
    if (h >= -18.0) return 0.15f * (float)(1.0 - (-12.0 - h) / 6.0);
    return 0.0f;
}

static void twilight_draw(const AstroState *st, Framebuffer *fb) {
    double sun_alt = st->pos[EPHEM_SUN].alt_rad;
    float k = twilight_factor(sun_alt);
    if (k <= 0.0f) return;

    /* Colour shifts through twilight bands: warm pink-orange just after
     * sunset, cool deep blue at astronomical. Lerp between two anchors. */
    double h_deg = sun_alt * RAD2DEG;
    float warm_w = 0.0f;
    if (h_deg >= -6.0) warm_w = (float)((h_deg + 6.0) / 6.0);   /* 0..1 */
    if (warm_w < 0.0f) warm_w = 0.0f;
    if (warm_w > 1.0f) warm_w = 1.0f;
    Color warm = { 1.00f, 0.55f, 0.45f };   /* civil pink-orange */
    Color cool = { 0.30f, 0.45f, 0.85f };   /* nautical/astro blue */
    Color tint = {
        warm.r * warm_w + cool.r * (1.0f - warm_w),
        warm.g * warm_w + cool.g * (1.0f - warm_w),
        warm.b * warm_w + cool.b * (1.0f - warm_w),
    };

    /* Sun azimuth — twilight is brightest in the Sun's quadrant. */
    double sun_az = st->pos[EPHEM_SUN].az_rad;

    /* Walk azimuth × low altitudes, stamp small Gaussians. fb_max so
     * stars/MW can still poke through brighter. */
    for (int azd = 0; azd < 360; azd += 2) {
        double az = azd * DEG2RAD;
        /* Cosine of azimuth offset from Sun: 1 facing Sun, -1 opposite. */
        double daz = az - sun_az;
        double azimuth_w = 0.5 + 0.5 * cos(daz);    /* 0..1 */
        /* Wrap-around — even the anti-Sun side gets some glow during civil. */
        azimuth_w = 0.30 + 0.70 * azimuth_w;

        for (int altd = 0; altd <= 22; altd += 2) {
            double alt = altd * DEG2RAD;
            float sx, sy;
            if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;

            /* Gaussian falloff in altitude — 0 brightest, ~22° gone. */
            float alt_fall = expf(-(altd * altd) / (2.0f * 9.0f * 9.0f));
            float v = k * (float)azimuth_w * alt_fall * 0.55f;
            if (v < 0.01f) continue;

            float sigma = 2.0f;
            float two_sigma_sq = 2.0f * sigma * sigma;
            float box = sigma * 2.5f;
            int x0 = (int)floorf(sx - box);
            int x1 = (int)ceilf (sx + box);
            int y0 = (int)floorf(sy - box);
            int y1 = (int)ceilf (sy + box);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
            if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

            for (int y = y0; y <= y1; y++) {
                float dy = (float)y - sy;
                for (int x = x0; x <= x1; x++) {
                    float dx = (float)x - sx;
                    float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                    float w = t * v;
                    if (w < 0.005f) continue;
                    fb_max(fb, x, y, tint.r * w, tint.g * w, tint.b * w);
                }
            }
        }
    }
}

static void milkyway_draw(const AstroState *st, Framebuffer *fb) {
    if (!mw_inited) milkyway_init();

    double lst_rad = st->lst_hours * (M_PI / 12.0);
    double lat     = st->observer.lat_rad;
    /* Soft band — sigma is much wider in y because Braille cells are 2× tall. */
    float sigma_x = 3.0f;
    float sigma_y = 5.0f;
    float two_sx2 = 2.0f * sigma_x * sigma_x;
    float two_sy2 = 2.0f * sigma_y * sigma_y;
    float box_x = sigma_x * 2.5f;
    float box_y = sigma_y * 2.5f;

    /* Cool blue-white wash. Tinted slightly toward the violet nebula tone
     * so it harmonises with the Perlin nebula layer. */
    Color tint = { 0.55f, 0.62f, 0.85f };

    for (int i = 0; i < MW_SAMPLES; i++) {
        double alt, az;
        radec_to_altaz(mw_points[i].ra, mw_points[i].dec, lat, lst_rad,
                       &alt, &az);
        if (alt < 0.0) continue;
        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;

        float w = (float)mw_points[i].weight;
        /* Real airmass extinction — Milky Way reddens + dims into the
         * horizon haze just like the stars do. */
        Color mw_tint = tint;
        float mw_intensity = w;
        apply_extinction(alt, &mw_tint, &mw_intensity);
        w = mw_intensity;

        int x0 = (int)floorf(sx - box_x);
        int x1 = (int)ceilf (sx + box_x);
        int y0 = (int)floorf(sy - box_y);
        int y1 = (int)ceilf (sy + box_y);
        if (x1 < 0 || y1 < 0)                  continue;
        if (x0 >= fb->sub_w || y0 >= fb->sub_h) continue;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t  = expf(-(dx*dx) / two_sx2 - (dy*dy) / two_sy2);
                float v  = t * w;
                if (v < 0.005f) continue;
                fb_max(fb, x, y, mw_tint.r * v, mw_tint.g * v, mw_tint.b * v);
            }
        }
    }
}

/* ---- Bright stars + constellation lines -------------------------------
 *
 * Per-frame: project each catalog star to alt/az/sub-pixel, stamp a small
 * Gaussian sized by magnitude, tinted by spectral class. Constellation
 * lines are drawn as sparse dotted segments between projected star
 * positions — only when both endpoints are above the horizon.
 *
 * Stars use fb_max so they hold a steady level across decay frames; lines
 * use fb_max for the same reason. (Trails would smear across the rotating
 * sky as the LST advances.)
 */
static Color spectral_tint(char cls) {
    switch (cls) {
    case 'O': return (Color){ 0.65f, 0.78f, 1.00f };
    case 'B': return (Color){ 0.78f, 0.88f, 1.00f };
    case 'A': return (Color){ 0.95f, 0.97f, 1.00f };
    case 'F': return (Color){ 1.00f, 0.97f, 0.88f };
    case 'G': return (Color){ 1.00f, 0.92f, 0.70f };
    case 'K': return (Color){ 1.00f, 0.78f, 0.55f };
    case 'M': return (Color){ 1.00f, 0.55f, 0.45f };
    default:  return (Color){ 1.00f, 1.00f, 1.00f };
    }
}

/* Magnitude → linear intensity. Brighter stars (lower mag) get a stronger
 * Gaussian peak; very dim stars fade out. Slope is gentler than the true
 * 2.512^Δmag because we want sub-mag-3 stars still legible. */
static float mag_to_intensity(float mag) {
    float k = powf(1.8f, 1.0f - mag);   /* mag 1.0 → 1.0, mag 0 → 1.8, mag 3 → 0.31 */
    if (k > 2.0f)  k = 2.0f;
    if (k < 0.10f) k = 0.10f;
    return k;
}

/* Gaussian σ from magnitude — brighter stars have a bigger soft halo, so
 * they read as "bigger" even though it's just bloom. */
static float mag_to_sigma(float mag) {
    if (mag < 0.5f) return 1.7f;
    if (mag < 1.5f) return 1.3f;
    if (mag < 2.5f) return 1.0f;
    return 0.8f;
}

/* Project a sky position (RA in radians, Dec in radians) for the current
 * observer. Returns 0 if above horizon and writes (sx, sy); -1 otherwise. */
static int project_sky(const AstroState *st, Framebuffer *fb,
                       double ra_rad, double dec_rad,
                       float *sx, float *sy, double *out_alt) {
    double lst_rad = st->lst_hours * (M_PI / 12.0);
    double alt, az;
    radec_to_altaz(ra_rad, dec_rad, st->observer.lat_rad, lst_rad, &alt, &az);
    if (out_alt) *out_alt = alt;
    if (alt < 0.0) return -1;
    return project(fb->sub_w, fb->sub_h, alt, az, sx, sy);
}

static void stars_draw(const AstroState *st, Framebuffer *fb) {
    float mag_cutoff = g_config.star_mag_cutoff;
    for (int i = 0; i < sky_stars_count; i++) {
        const SkyStar *s = &sky_stars[i];
        if (s->mag > mag_cutoff) continue;
        double ra  = s->ra_h * (M_PI / 12.0);
        double dec = s->dec_deg * DEG2RAD;
        float  sx, sy;
        double alt;
        if (project_sky(st, fb, ra, dec, &sx, &sy, &alt) != 0) continue;

        float intensity = mag_to_intensity(s->mag);
        Color tint = spectral_tint(s->spectral);
        apply_extinction(alt, &tint, &intensity);
        float sigma = mag_to_sigma(s->mag);
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 3.0f;

        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x1 < 0 || y1 < 0)                  continue;
        if (x0 >= fb->sub_w || y0 >= fb->sub_h) continue;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float v = t * intensity;
                if (v < 0.005f) continue;
                /* Hot core blends toward white — pure tint at the halo. */
                float w = t * t * 0.85f;
                float r  = (tint.r * (1.0f - w) + w) * v;
                float g  = (tint.g * (1.0f - w) + w) * v;
                float bl = (tint.b * (1.0f - w) + w) * v;
                fb_max(fb, x, y, r, g, bl);
            }
        }
    }
}

static void constellations_draw(const AstroState *st, Framebuffer *fb) {
    /* Dotted line — sample the segment at fixed parametric steps. Density
     * scales with screen length.
     *
     * Intensity must clear the background-star floor (mag_to_intensity
     * clamps to 0.10) plus the LUM_THRESHOLD + gamma combo (~0.06 in
     * linear after gamma 1.18), or every line dot near a star gets
     * masked by fb_max. 0.55 gives a clearly readable cyan dot that
     * still reads "dotted line" rather than "solid bar". */
    Color tint = { 0.55f, 0.75f, 1.00f };
    float intensity = 0.55f;

    for (int i = 0; i < sky_lines_count; i++) {
        const SkyStar *a = &sky_stars[sky_lines[i].a];
        const SkyStar *b = &sky_stars[sky_lines[i].b];
        float ax, ay, bx, by;
        double aa, bb;
        if (project_sky(st, fb, a->ra_h * (M_PI / 12.0),
                        a->dec_deg * DEG2RAD, &ax, &ay, &aa) != 0) continue;
        if (project_sky(st, fb, b->ra_h * (M_PI / 12.0),
                        b->dec_deg * DEG2RAD, &bx, &by, &bb) != 0) continue;

        /* Number of dots along the segment — enough to read as a line at
         * normal terminal sizes without saturating fb_max. */
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy);
        int   n   = (int)(len / 4.0f);
        if (n < 4)   n = 4;
        if (n > 64) n = 64;

        for (int k = 1; k < n; k++) {
            /* Skip every other sample for the "dotted" effect. */
            if ((k & 1) == 0) continue;
            float t  = (float)k / (float)n;
            float px = ax + dx * t;
            float py = ay + dy * t;
            int   ix = (int)px, iy = (int)py;
            if (ix < 0 || iy < 0 || ix >= fb->sub_w || iy >= fb->sub_h) continue;
            fb_max(fb, ix, iy,
                   tint.r * intensity, tint.g * intensity, tint.b * intensity);
        }
    }
}

/* Same as project() but doesn't reject below-horizon points. Used for
 * the Sun direction when computing Moon terminator orientation while the
 * Sun itself is below the horizon (i.e. night). */
static void project_unclipped(int sub_w, int sub_h, double alt, double az,
                              float *sub_x, float *sub_y) {
    double r_norm = (M_PI * 0.5 - alt) / (M_PI * 0.5);
    float center_x = (float)sub_w * 0.5f;
    float center_y = (float)sub_h * 0.5f;
    float radius   = fminf(center_x, center_y * 0.5f) * 0.92f;
    *sub_x = center_x + (float)( sin(az) * r_norm) * radius;
    *sub_y = center_y + (float)(-cos(az) * r_norm) * radius * 2.0f;
}

static void stamp_moon(Framebuffer *fb, float cx, float cy,
                       float sun_sx, float sun_sy,
                       const AstroStyle *s, double elongation,
                       double eclipse_f) {
    /* On-screen direction toward the Sun. Used to orient the terminator. */
    float ddx = sun_sx - cx;
    float ddy = sun_sy - cy;
    float dlen = sqrtf(ddx * ddx + ddy * ddy);
    if (dlen < 1e-3f) { ddx = 1.0f; ddy = 0.0f; dlen = 1.0f; }
    float dir_x = ddx / dlen;
    float dir_y = ddy / dlen;

    /* Phase angle from elongation: terminator threshold along dir is
     * cos(sep), where sep ∈ [0, π]. Lit side is the half facing the Sun. */
    double sep = elongation;
    if (sep > M_PI) sep = 2.0 * M_PI - sep;
    float threshold = (float)cos(sep);

    float sigma = s->radius_sub;
    float two_sigma_sq = 2.0f * sigma * sigma;
    /* 2σ clip — past this point the Gaussian tail crosses LUM_THRESHOLD
     * unevenly and produces speckled halos. Sharper edge, no fuzz. */
    float box = sigma * 2.0f;
    float R = sigma;     /* radius for offset normalisation */

    int x0 = (int)floorf(cx - box);
    int x1 = (int)ceilf (cx + box);
    int y0 = (int)floorf(cy - box);
    int y1 = (int)ceilf (cy + box);
    if (x1 < 0 || y1 < 0)                  return;
    if (x0 >= fb->sub_w || y0 >= fb->sub_h) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
    if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float t = expf(-(dx * dx + dy * dy) / two_sigma_sq);

            float xproj = (dx * dir_x + dy * dir_y) / R;
            /* Earthshine: the dark side glows faintly, ~7% — keeps the
             * full disc legible during heavy crescents. The terminator
             * mask only fully bites in the bright core; the Gaussian
             * halo stays radially symmetric. */
            float lit  = (xproj > threshold) ? 1.0f : 0.07f;
            float mask = (1.0f - t) + t * lit;

            float k = t * s->intensity * mask;
            if (k < 0.005f) continue;
            float w  = t * t * s->core_blend;
            float r  = (s->color.r * (1.0f - w) + w) * k;
            float g  = (s->color.g * (1.0f - w) + w) * k;
            float bl = (s->color.b * (1.0f - w) + w) * k;
            /* Lunar eclipse: dim + red-shift toward "blood moon" tone.
             * f=0 no effect, f=1 full umbra. */
            if (eclipse_f > 0.0) {
                float dim = 1.0f - 0.85f * (float)eclipse_f;
                r  *= dim + 0.20f * (float)eclipse_f;
                g  *= dim;
                bl *= dim * (1.0f - 0.50f * (float)eclipse_f);
            }
            fb_add(fb, x, y, r, g, bl);
        }
    }
}

/* ---- Sky grid -------------------------------------------------------- */

static void stamp_dot(Framebuffer *fb, float sx, float sy,
                      float r, float g, float b) {
    int x = (int)floorf(sx + 0.5f);
    int y = (int)floorf(sy + 0.5f);
    if (x < 0 || y < 0 || x >= fb->sub_w || y >= fb->sub_h) return;
    fb_max(fb, x, y, r, g, b);
}

static void grid_draw(const AstroState *st, Framebuffer *fb) {
    /* Faint alt-az grid: altitude rings every 30°, azimuth radials every
     * 30°. Stamped sparsely so stars/MW overdraw without the grid
     * fighting them. */
    Color tint = { 0.20f, 0.30f, 0.42f };
    double lst_rad = st->lst_hours * (M_PI / 12.0); (void)lst_rad;

    /* Altitude rings — walk az 0..360 step 1°, fixed alt. */
    for (int alt_deg = 0; alt_deg <= 60; alt_deg += 30) {
        if (alt_deg <= 0) continue;
        double alt = alt_deg * DEG2RAD;
        for (int az_deg = 0; az_deg < 360; az_deg += 1) {
            if ((az_deg / 4) % 2) continue;          /* dashed */
            double az = az_deg * DEG2RAD;
            float sx, sy;
            if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;
            stamp_dot(fb, sx, sy, tint.r, tint.g, tint.b);
        }
    }
    /* Az radials — walk alt 0..85° step 1°. Drawn at every 30° az.       */
    for (int az_deg = 0; az_deg < 360; az_deg += 30) {
        double az = az_deg * DEG2RAD;
        for (int alt_deg = 1; alt_deg <= 85; alt_deg += 1) {
            if ((alt_deg / 3) % 2) continue;          /* dashed */
            double alt = alt_deg * DEG2RAD;
            float sx, sy;
            if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;
            stamp_dot(fb, sx, sy, tint.r, tint.g, tint.b);
        }
    }
}

/* ---- Body trails ----------------------------------------------------- */

static void trails_capture(AstroState *st) {
    /* Push a new sample only after enough time has passed that the slowest
     * planet (Neptune) might've moved noticeably. ~1 minute of virt-time
     * is below 1 sub-pixel for any planet at 1× speed but visible at high
     * scrub — self-tunes. */
    const double min_dt_days = 1.0 / (24.0 * 60.0);
    if (st->trail_last_jd != 0.0 &&
        fabs(st->jd - st->trail_last_jd) < min_dt_days) return;
    st->trail_last_jd = st->jd;
    /* Skip Sun/Moon — Sun trail is uninteresting, Moon is too fast and
     * smears the screen. Trails for the 8 planets only. */
    for (int i = 0; i < EPHEM_COUNT; i++) {
        if (i == EPHEM_SUN || i == EPHEM_MOON) continue;
        int h = st->trail_head[i];
        st->trails[i][h].ra_rad  = st->pos[i].ra_rad;
        st->trails[i][h].dec_rad = st->pos[i].dec_rad;
        st->trails[i][h].valid   = 1;
        st->trail_head[i] = (h + 1) % ASTRO_TRAIL_LEN;
    }
}

static void trails_draw(const AstroState *st, Framebuffer *fb) {
    double lst_rad = st->lst_hours * (M_PI / 12.0);
    double lat     = st->observer.lat_rad;
    for (int i = 0; i < EPHEM_COUNT; i++) {
        if (i == EPHEM_SUN || i == EPHEM_MOON) continue;
        AstroStyle base = style_for((EphemBody)i);
        int head = st->trail_head[i];
        for (int n = 0; n < ASTRO_TRAIL_LEN; n++) {
            int idx = (head - 1 - n + ASTRO_TRAIL_LEN) % ASTRO_TRAIL_LEN;
            const TrailSample *s = &st->trails[i][idx];
            if (!s->valid) break;
            double alt, az;
            radec_to_altaz(s->ra_rad, s->dec_rad, lat, lst_rad, &alt, &az);
            if (alt < 0.0) continue;
            float sx, sy;
            if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;
            /* Newest = brightest, exponential fade back. */
            float age = (float)n / (float)ASTRO_TRAIL_LEN;
            float k = (1.0f - age) * (1.0f - age) * 0.55f;
            stamp_dot(fb, sx, sy,
                      base.color.r * k, base.color.g * k, base.color.b * k);
        }
    }
}

/* ---- Galilean moons -------------------------------------------------- *
 *
 * Mean-motion only — accurate to a few Jupiter radii, plenty for terminal
 * scale where the disc is ~5px. Mean longitudes at J2000 epoch from
 * Meeus, simplified (we ignore mutual perturbations and the ~3° tilt of
 * Jupiter's equator to its orbit). The moons walk circular orbits in the
 * screen plane perpendicular to the line of sight; flat orientation is
 * Saturn-style horizontal projection — close enough for our cell scale.
 */
typedef struct {
    const char *name;
    double      period_d;
    double      a_rj;       /* semi-major in Jupiter radii  */
    double      m0_rad;     /* mean longitude at J2000 epoch */
    Color       tint;
} JovianMoon;

static const JovianMoon jovian[4] = {
    { "Io",        1.769138,  5.91, 1.86, { 1.00f, 0.92f, 0.55f } },
    { "Europa",    3.551181,  9.40, 3.50, { 0.90f, 0.90f, 0.85f } },
    { "Ganymede",  7.154553, 15.00, 5.31, { 0.85f, 0.78f, 0.65f } },
    { "Callisto", 16.689018, 26.36, 0.46, { 0.55f, 0.50f, 0.45f } },
};

static void galilean_draw(const AstroState *st, Framebuffer *fb) {
    /* Need Jupiter above horizon. */
    const EphemPosition *J = &st->pos[EPHEM_JUPITER];
    if (J->alt_rad < 0.0) return;
    float jx, jy;
    if (project(fb->sub_w, fb->sub_h, J->alt_rad, J->az_rad, &jx, &jy) != 0)
        return;

    /* Pixel scale: Jupiter's stamp radius is style.radius_sub. Real Io is
     * at 5.91 Jupiter radii — that maps to 5.91 * radius_sub pixels, which
     * is visually about right. */
    AstroStyle js = style_for(EPHEM_JUPITER);
    float scale = js.radius_sub;     /* 1 R_J → this many sub-pixels */

    double dt = st->jd - 2451545.0;  /* days from J2000 */
    for (int i = 0; i < 4; i++) {
        const JovianMoon *m = &jovian[i];
        double M = m->m0_rad + (2.0 * M_PI / m->period_d) * dt;
        /* Project onto the screen plane: x along sky-east, y suppressed
         * by ring inclination (~3° — we just flatten by 0.06 in y). */
        double dx = m->a_rj * cos(M);
        double dy = m->a_rj * sin(M) * 0.06;
        float sx = jx + (float)dx * scale;
        float sy = jy + (float)dy * scale;
        /* Tiny soft point. */
        float sigma = 0.9f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 2.5f;
        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float ddy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float ddx = (float)x - sx;
                float t = expf(-(ddx*ddx + ddy*ddy) / two_sigma_sq);
                float k = t * 1.1f;
                if (k < 0.01f) continue;
                fb_add(fb, x, y,
                       m->tint.r * k, m->tint.g * k, m->tint.b * k);
            }
        }
    }
}

/* ---- Saturn rings ---------------------------------------------------- */

/* Earth-Saturn ring opening angle (B). True B varies between -27° and
 * +27° over Saturn's 29.46-year orbit, function of Saturn's heliocentric
 * ecliptic longitude relative to its node line at ~169.5°. We don't track
 * heliocentric longitude here, but the synodic phase from J2000 gives a
 * sinusoid with the right period and amplitude — accurate to ~5° for our
 * purposes. */
static double saturn_ring_tilt_rad(double jd) {
    /* Saturn mean ecliptic longitude advances ~12.22°/yr; at J2000 ≈ 50°. */
    double dt_yr = (jd - 2451545.0) / 365.25;
    double lon_deg = 50.0 + 12.22 * dt_yr;
    double phase = (lon_deg - 169.5) * DEG2RAD;
    return 28.07 * DEG2RAD * sin(phase);
}

static void stamp_saturn(Framebuffer *fb, float cx, float cy,
                         const AstroStyle *s, double jd) {
    /* Disc first, then ring ellipse. Ring is wider than the disc and
     * tilted by B — we render it as 2 thin elliptical arcs (front + back)
     * and let the disc cut the visible front half by being drawn last. */
    double B = saturn_ring_tilt_rad(jd);
    float ring_a = s->radius_sub * 2.6f;          /* ring outer in x */
    float ring_b = ring_a * (float)fabs(sin(B));  /* projected y radius */
    if (ring_b < 0.4f) ring_b = 0.4f;             /* edge-on minimum */
    float ring_inner = ring_a * 0.65f;
    float thickness  = 0.35f;                     /* sub-pixels */

    Color rtint = s->color;
    /* Sample the ring as parametric ellipse. */
    int n_samples = (int)(ring_a * 14.0f);
    if (n_samples < 32) n_samples = 32;
    for (int i = 0; i < n_samples; i++) {
        double th = (double)i / n_samples * 2.0 * M_PI;
        for (float r = ring_inner; r <= ring_a; r += thickness) {
            float ex = (float)cos(th) * r;
            float ey = (float)sin(th) * r * (ring_b / ring_a);
            float sx = cx + ex;
            float sy = cy + ey;
            int x = (int)floorf(sx + 0.5f);
            int y = (int)floorf(sy + 0.5f);
            if (x < 0 || y < 0 || x >= fb->sub_w || y >= fb->sub_h) continue;
            /* Inner ring is brighter (Cassini gap roughly at 0.85 of outer). */
            float k = 0.45f;
            if (r > ring_a * 0.83f && r < ring_a * 0.88f) k = 0.15f; /* gap */
            fb_add(fb, x, y, rtint.r * k, rtint.g * k, rtint.b * k);
        }
    }

    /* Disc on top so the ring's near-side passes behind the planet. */
    {
        float sigma = s->radius_sub;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 2.0f;        /* 2σ clip — see stamp_body */
        int x0 = (int)floorf(cx - box);
        int x1 = (int)ceilf (cx + box);
        int y0 = (int)floorf(cy - box);
        int y1 = (int)ceilf (cy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - cy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - cx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float k = t * s->intensity;
                if (k < 0.005f) continue;
                float w = t * t * s->core_blend;
                float r  = (s->color.r * (1.0f - w) + w) * k;
                float g  = (s->color.g * (1.0f - w) + w) * k;
                float bl = (s->color.b * (1.0f - w) + w) * k;
                fb_add(fb, x, y, r, g, bl);
            }
        }
    }
}

static void stamp_body(Framebuffer *fb, float cx, float cy,
                       const AstroStyle *s) {
    float sigma = s->radius_sub;
    float two_sigma_sq = 2.0f * sigma * sigma;
    float box = sigma * 2.0f;        /* 2σ clip — see stamp_moon */

    int x0 = (int)floorf(cx - box);
    int x1 = (int)ceilf (cx + box);
    int y0 = (int)floorf(cy - box);
    int y1 = (int)ceilf (cy + box);
    if (x1 < 0 || y1 < 0)                  return;
    if (x0 >= fb->sub_w || y0 >= fb->sub_h) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
    if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - cx;
            float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
            float k = t * s->intensity;
            if (k < 0.005f) continue;
            float w = t * t * s->core_blend;
            float r  = (s->color.r * (1.0f - w) + w) * k;
            float g  = (s->color.g * (1.0f - w) + w) * k;
            float bl = (s->color.b * (1.0f - w) + w) * k;
            fb_add(fb, x, y, r, g, bl);
        }
    }
}

/* === Tier 4c: asteroids ============================================= *
 *
 * Stamped as small fuzzy points — neutral grey tint, no tail, no halo.
 * Mag-gated like comets so the table doesn't litter the sky with
 * invisible dots. Picks closer than the comet cutoff because asteroids
 * are typically dimmer (Vesta peaks ~5.1, Ceres ~6.6).
 */
static void asteroids_draw(const AstroState *st, Framebuffer *fb) {
    Color tint_base = { 0.85f, 0.82f, 0.75f };   /* neutral rocky grey */
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        const AsteroidState *a = &st->asteroids[i];
        if (!a->valid || a->alt_rad < 0.0) continue;
        if (a->mag > g_config.asteroid_mag_cutoff) continue;

        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, a->alt_rad, a->az_rad, &sx, &sy) != 0)
            continue;

        float intensity = powf(1.7f, 1.0f - (float)a->mag);
        if (intensity > 1.2f) intensity = 1.2f;
        if (intensity < 0.05f) continue;

        Color tint = tint_base;
        apply_extinction(a->alt_rad, &tint, &intensity);
        intensity *= st->bright_boost;

        float sigma = 0.85f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 2.5f;
        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float k = t * intensity;
                if (k < 0.005f) continue;
                fb_add(fb, x, y,
                       tint.r * k, tint.g * k, tint.b * k);
            }
        }
    }
}

/* === Phase 6: satellites ============================================ *
 *
 * Bundled near-Earth satellites stamped as sharp single-pixel points with
 * a tight bloom. fb_max instead of fb_add — ISS at orbital speed crosses
 * the horizon-to-zenith arc in ~3 minutes, so any decay-trail effect
 * smears into an unreadable streak. Better to draw fresh each frame and
 * let the eye track the motion.
 *
 * Staleness gating (PHASE6_SATELLITE_INTEGRATION.md):
 *   age <= 7 d   normal brightness
 *   7 < age <= 14 d  half-dim
 *   age > 14 d   hidden (compute path already gates >30 d entirely)
 */
static void satellites_draw(const AstroState *st, Framebuffer *fb) {
    if (!st->show_satellites) return;
    Color tint_base = { 0.78f, 0.92f, 1.00f };    /* cool white-cyan */
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        const SatelliteState *s = &st->satellites[i];
        if (!s->valid || !s->above_horizon) continue;

        double age = fabs(st->jd - satellite_epoch_jd(i));
        if (age > 14.0) continue;                  /* hide stale       */
        float age_dim = (age > 7.0) ? 0.5f : 1.0f;

        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, s->alt_rad, s->az_rad, &sx, &sy) != 0)
            continue;

        float intensity = 1.05f * age_dim;
        Color tint = tint_base;
        apply_extinction(s->alt_rad, &tint, &intensity);

        /* Tight Gaussian — sigma 0.7 produces a sub-pixel core with a
         * 1-cell halo, distinct from the wider planet/asteroid stamps. */
        float sigma = 0.7f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 2.5f;
        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float k = t * intensity;
                if (k < 0.02f) continue;
                fb_max(fb, x, y, tint.r * k, tint.g * k, tint.b * k);
            }
        }
    }
}

/* === Tier 5b: deep-sky objects ====================================== *
 *
 * Bundled Messier + bright NGC catalogue from `dso.{h,c}`. Each entry is
 * stamped as a soft Gaussian patch tinted by kind. Apparent angular size
 * maps to Gaussian sigma via sqrt-compression — M31 (178 arcmin) reads
 * as a wide patch, M57 (1.4 arcmin) reads as a near-point.
 */
static void dsos_draw(const AstroState *st, Framebuffer *fb) {
    static const Color tint_for_kind[5] = {
        { 0.70f, 0.50f, 0.55f },   /* GALAXY: warm pink dust            */
        { 0.95f, 0.40f, 0.45f },   /* NEBULA_BRIGHT: H-alpha red-pink   */
        { 0.40f, 0.85f, 0.75f },   /* NEBULA_PLANETARY: OIII cyan-green */
        { 0.75f, 0.85f, 1.00f },   /* CLUSTER_OPEN: hot blue-white      */
        { 1.00f, 0.85f, 0.60f },   /* CLUSTER_GLOBULAR: yellow-cream    */
    };
    for (int i = 0; i < dso_count; i++) {
        const DSObject *d = &dso_catalog[i];
        double ra  = d->ra_h * (M_PI / 12.0);
        double dec = d->dec_deg * DEG2RAD;
        float sx, sy;
        double alt;
        if (project_sky(st, fb, ra, dec, &sx, &sy, &alt) != 0) continue;

        float intensity = mag_to_intensity(d->mag) * 0.85f;
        if (intensity < 0.05f) continue;
        Color tint = tint_for_kind[d->kind];
        apply_extinction(alt, &tint, &intensity);

        /* Sigma scales with sqrt of apparent arcmin, with floor + ceiling
         * so M31's 178' patch doesn't swallow Andromeda whole and M57's
         * 1.4' bullseye doesn't shrink below a single sub-pixel. */
        float sigma = sqrtf(d->size_arcmin / 5.0f);
        if (sigma < 1.0f) sigma = 1.0f;
        if (sigma > 6.0f) sigma = 6.0f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 2.0f;

        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float k = t * intensity;
                if (k < 0.005f) continue;
                fb_add(fb, x, y, tint.r * k, tint.g * k, tint.b * k);
            }
        }
    }
}

/* === Tier 5c: aurora ================================================ *
 *
 * Shimmering vertical bands near the poleward horizon. Two knobs gate
 * visibility:
 *  - `show_aurora` (key `a`) — on/off toggle, default off so mid-lat
 *    observers don't get surprised.
 *  - `g_config.kp_index` (config) — geomagnetic activity 0..9. Aurora
 *    intensity scales with Kp; observer latitude must clear the auroral
 *    oval (~67° geomagnetic at quiet times, dropping ~2° per Kp step).
 *    Set Kp 7+ in config.toml to see aurora at mid-latitudes; Kp 9 is
 *    "Carrington event" territory and reaches ~30° lat.
 *
 * Colour: oxygen green at low altitudes (~5–10°), violet/red at higher
 * altitudes (~15–25°), with shimmer phase walking azimuth.
 */
static void aurora_draw(const AstroState *st, Framebuffer *fb) {
    if (!st->show_aurora) return;

    /* Kp gating. The aurora oval sits at roughly (67° - 2.5° × Kp)
     * geomagnetic latitude; we use geographic lat as a stand-in (off
     * by ~10° but cell-scale-irrelevant). Below that latitude the
     * aurora simply isn't visible. */
    float kp = g_config.kp_index;
    if (kp < 0.5f) return;
    double abs_lat_deg = fabs(st->observer.lat_rad) * RAD2DEG;
    double oval_lat = 67.0 - 2.5 * kp;
    if (abs_lat_deg < oval_lat - 5.0) return;     /* well below the oval */

    /* Intensity scales with Kp (0.3 at Kp 1, 1.0 at Kp 5, 1.6 at Kp 9)
     * and falls off as we move equatorward of the oval. */
    float kp_scale = 0.20f + 0.18f * kp;
    if (abs_lat_deg < oval_lat) {
        kp_scale *= (float)(1.0 - (oval_lat - abs_lat_deg) / 10.0);
        if (kp_scale <= 0.0f) return;
    }

    /* Self-paced from CLOCK_MONOTONIC — render-rate-independent shimmer. */
    static double t = 0.0;
    static struct timespec last;
    static int have_last = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (have_last) {
        double dt = (now.tv_sec  - last.tv_sec)
                  + (now.tv_nsec - last.tv_nsec) * 1e-9;
        if (dt > 0.5) dt = 0.5;
        t += dt;
    }
    last = now;
    have_last = 1;

    /* Sub-storm flares: a real auroral arc breathes on minute-scale
     * timescales — substorms last 15-60 minutes, peak in 5-10. We emulate
     * that with a Markov-style flare schedule: low chance per frame to
     * start a new flare, fixed-duration flare at 1.5-2.2× intensity. */
    static double flare_until = 0.0;
    static float  flare_mult  = 1.0f;
    if (t > flare_until) {
        /* ~1 in 3000 frames at 60fps → ~1 substorm per minute on average. */
        if ((met_rand32() & 0x7FF) == 0) {
            float strength = 1.5f + ((float)met_rand32() / 65535.0f) * 0.7f;
            flare_mult  = strength;
            flare_until = t + 12.0 + ((float)met_rand32() / 65535.0f) * 28.0;
        } else {
            flare_mult = 1.0f;
        }
    }

    /* Variety scaling with Kp: storm-level activity (Kp≥7) extends the
     * bands higher into the sky, reaching toward zenith at Kp 9. At
     * quiet Kp the band stays near the horizon. */
    float top_alt_kp_bonus = 0.0f;
    if (kp > 6.0f) top_alt_kp_bonus = (kp - 6.0f) * 8.0f;

    int north = (st->observer.lat_rad >= 0.0);
    double pole_az_rad = north ? 0.0 : M_PI;

    /* Walk az ±60° around the poleward direction, alt 1°..~25° + bonus. */
    for (int az_off = -60; az_off <= 60; az_off += 1) {
        double az = pole_az_rad + (double)az_off * DEG2RAD;
        if (az < 0.0)            az += 2.0 * M_PI;
        if (az >= 2.0 * M_PI)    az -= 2.0 * M_PI;

        float phase = (float)(t * 0.4) + (float)az_off * 0.13f;
        float intensity_base = 0.55f + 0.40f * sinf(phase);
        if (intensity_base < 0.18f) continue;

        float top_alt_deg = 18.0f + 8.0f * sinf(phase * 1.7f + 1.1f)
                          + top_alt_kp_bonus;
        if (top_alt_deg < 6.0f) continue;

        for (int alt_deg = 1; alt_deg < (int)top_alt_deg; alt_deg++) {
            double alt = (double)alt_deg * DEG2RAD;
            float sx, sy;
            if (project(fb->sub_w, fb->sub_h, alt, az, &sx, &sy) != 0) continue;

            float fall = 1.0f - (float)alt_deg / top_alt_deg;
            float k = intensity_base * fall * 0.32f * kp_scale * flare_mult;

            float tup = (float)alt_deg / top_alt_deg;
            float r = (0.05f + 0.55f * tup) * k;
            float g = (0.85f - 0.40f * tup) * k;
            float b = (0.20f + 0.55f * tup) * k;

            int ix = (int)(sx + 0.5f), iy = (int)(sy + 0.5f);
            fb_max(fb, ix, iy, r, g, b);
        }
    }
}

/* === Tier 4b: comets ================================================ *
 *
 * Bundled set in `comet.c`. Per frame we project each comet that's above
 * the horizon and bright enough (mag < cutoff). The head is a small
 * fuzzy stamp (Gaussian σ scaled by magnitude); the tail is a streak of
 * fading dots in screen-space, *anti-solar* — i.e. drawn from the comet
 * head along the direction from Sun to comet on the screen. Tail length
 * scales with brightness and inverse heliocentric distance, so close-to-
 * Sun apparitions look properly impressive.
 */
static void comets_draw(const AstroState *st, Framebuffer *fb,
                        float sun_sx, float sun_sy) {
    Color head_tint = { 0.85f, 0.95f, 1.00f };   /* cool blue-white */
    for (int i = 0; i < COMET_COUNT; i++) {
        const CometState *c = &st->comets[i];
        if (!c->valid || c->alt_rad < 0.0) continue;
        if (c->mag > g_config.comet_mag_cutoff) continue;

        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, c->alt_rad, c->az_rad, &sx, &sy) != 0)
            continue;

        /* Magnitude → intensity. Same slope as stars but offset down a
         * notch so a 4-mag comet reads about as bright as a 3-mag star
         * (comet light is diffuse). */
        float intensity = powf(1.6f, 1.0f - (float)c->mag);
        if (intensity > 1.6f) intensity = 1.6f;
        if (intensity < 0.05f) continue;

        Color tint = head_tint;
        apply_extinction(c->alt_rad, &tint, &intensity);
        intensity *= st->bright_boost;

        /* Head: small Gaussian — slightly bigger σ than a star at the
         * same magnitude, so it reads as fuzzy not point-like. */
        float sigma = 1.4f;
        if (c->mag < 3.0f) sigma = 2.0f;
        if (c->mag < 0.0f) sigma = 2.6f;
        float two_sigma_sq = 2.0f * sigma * sigma;
        float box = sigma * 3.0f;
        int x0 = (int)floorf(sx - box);
        int x1 = (int)ceilf (sx + box);
        int y0 = (int)floorf(sy - box);
        int y1 = (int)ceilf (sy + box);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
        if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y - sy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x - sx;
                float t = expf(-(dx*dx + dy*dy) / two_sigma_sq);
                float k = t * intensity;
                if (k < 0.005f) continue;
                fb_add(fb, x, y,
                       tint.r * k, tint.g * k, tint.b * k);
            }
        }

        /* Tail: anti-solar direction in screen space. Length ∝
         * intensity / r_helio — close approaches grow, distant comets
         * stay subtle. */
        float ddx = sx - sun_sx;
        float ddy = sy - sun_sy;
        float dlen = sqrtf(ddx * ddx + ddy * ddy);
        if (dlen < 1e-3f) continue;
        float ux = ddx / dlen;
        float uy = ddy / dlen;
        float r = (float)c->r_helio_au;
        if (r < 0.3f) r = 0.3f;
        float tail_px = intensity * 25.0f / r;
        if (tail_px > 70.0f) tail_px = 70.0f;
        int n_dots = (int)tail_px;
        for (int s = 1; s < n_dots; s++) {
            float k = intensity * (1.0f - (float)s / n_dots) * 0.6f;
            if (k < 0.01f) break;
            int dx = (int)(sx + ux * s + 0.5f);
            int dy = (int)(sy + uy * s + 0.5f);
            if (dx < 0 || dy < 0 || dx >= fb->sub_w || dy >= fb->sub_h) continue;
            fb_add(fb, dx, dy, tint.r * k, tint.g * k, tint.b * k);
        }
    }
}

/* === Tier 4: meteor showers ========================================= *
 *
 * Bundled table of major annual showers. Each entry is one shower; the
 * activity profile is a Gaussian centred on `peak_doy` with FWHM
 * `fwhm_days`. The active shower (max activity above floor) gates meteor
 * spawning — at most one shower contributes at a time.
 *
 * Spawn model: shower's per-minute rate = ZHR * gauss(doy_offset) *
 * sin(radiant_alt). Meteors emit from the projected radiant with
 * outward velocity, fade over their `max_life`. Streak rendered along
 * the velocity vector with a 14-px fading tail.
 *
 * No ephemeris dependency — radiant position is fixed J2000 RA/Dec
 * (precession ~0.014°/yr, cell-irrelevant).
 */
typedef struct {
    const char *name;
    int    peak_doy;        /* 1..366 */
    float  fwhm_days;
    double ra_h;
    double dec_deg;
    int    zhr;
    float  vel_factor;      /* km/s / 50; speed multiplier vs sporadic */
    Color  tint;            /* dominant emission colour                 */
} MeteorShower;

/* Velocity factors below are atmospheric entry speed / 50 km/s, so the
 * baseline 60-140 sub-px/s shower spawn becomes (factor × baseline).
 * Tints chosen from observational reports — Leonids are famously blue
 * from high-velocity ionised N₂; Geminids burn yellow from sodium-rich
 * chondritic dust; Draconids slow and pale from comet 21P's loose
 * weak particles. */
static const MeteorShower meteor_showers[] = {
    { "Quadrantids",    3, 1.0,  15.33,  49.7, 110, 0.85f, { 0.80f, 0.90f, 1.00f } }, /* white-blue, 41 km/s */
    { "Lyrids",       112, 2.0,  18.07,  34.0,  18, 1.00f, { 1.00f, 0.95f, 0.90f } }, /* white,      49 km/s */
    { "Eta Aquariids",126, 5.0,  22.50,  -1.0,  60, 1.30f, { 1.00f, 0.90f, 0.55f } }, /* yellow,     66 km/s */
    { "Perseids",     224, 4.0,   3.20,  58.0, 110, 1.20f, { 0.90f, 1.00f, 0.70f } }, /* yel-green,  59 km/s */
    { "Draconids",    281, 0.5,  17.43,  54.0,  10, 0.55f, { 1.00f, 0.95f, 0.85f } }, /* warm-white, 20 km/s */
    { "Orionids",     294, 3.0,   6.33,  16.0,  25, 1.30f, { 0.90f, 1.00f, 0.70f } }, /* yel-green,  66 km/s */
    { "Leonids",      321, 1.0,  10.13,  22.0,  15, 1.40f, { 0.65f, 0.80f, 1.00f } }, /* blue,       71 km/s */
    { "Geminids",     348, 2.0,   7.47,  33.0, 150, 0.75f, { 1.00f, 0.85f, 0.45f } }, /* yellow,     35 km/s */
    { "Ursids",       356, 0.5,  14.47,  76.0,  10, 0.75f, { 1.00f, 0.95f, 0.90f } }, /* white,      33 km/s */
};
static const int meteor_shower_count =
    (int)(sizeof meteor_showers / sizeof meteor_showers[0]);

#define METEOR_POOL 96

typedef struct {
    float sx, sy;
    float vx, vy;
    float life, max_life;
    float intensity;
    Color color;
    int   active;
} Meteor;

static Meteor   met_pool[METEOR_POOL];
static double   met_spawn_accum = 0.0;
static double   sporadic_spawn_accum = 0.0;
static unsigned met_rng = 0xC0FFEEu;

static unsigned met_rand32(void) {
    met_rng = met_rng * 1103515245u + 12345u;
    return (met_rng >> 16) & 0xFFFFu;
}
static float met_randf(void) {
    return (float)met_rand32() / 65535.0f;
}

static double doy_from_jd(double jd) {
    /* 1..366 day-of-year, modulo Julian year. Sufficient for shower
     * windows measured in days; calendar-exact DOY isn't worth the cost. */
    double frac = jd - 2451544.5;
    double yfrac = fmod(frac / 365.25, 1.0);
    if (yfrac < 0.0) yfrac += 1.0;
    return yfrac * 365.25 + 1.0;
}

static const MeteorShower *meteor_active_shower(double jd, float *out_rate_min) {
    double doy = doy_from_jd(jd);
    const MeteorShower *best = NULL;
    float best_rate = 0.0f;
    for (int i = 0; i < meteor_shower_count; i++) {
        const MeteorShower *s = &meteor_showers[i];
        double d = doy - s->peak_doy;
        if (d >  200.0) d -= 365.25;
        if (d < -200.0) d += 365.25;
        float sigma = s->fwhm_days / 2.355f;
        if (sigma < 0.1f) sigma = 0.1f;
        float gauss = expf(-(float)(d * d) / (2.0f * sigma * sigma));
        float rate  = (float)s->zhr * gauss;       /* per hour, ZHR units */
        if (rate > best_rate) { best_rate = rate; best = s; }
    }
    if (out_rate_min) *out_rate_min = best_rate / 60.0f;
    return best;
}

static void meteors_step_internal(const AstroState *st, Framebuffer *fb,
                                  double dt);
static void meteors_step(const AstroState *st, Framebuffer *fb) {
    /* Self-paced from CLOCK_MONOTONIC so this works regardless of how
     * often astro_draw is called. Independent of the astro virtual
     * clock — meteors are a render-rate effect, not an ephemeris one. */
    static struct timespec last;
    static int have_last = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = 0.0;
    if (have_last) {
        dt = (now.tv_sec  - last.tv_sec)
           + (now.tv_nsec - last.tv_nsec) * 1e-9;
        if (dt > 0.25) dt = 0.25;          /* clamp on stalls */
    }
    last = now;
    have_last = 1;
    meteors_step_internal(st, fb, dt);
}

/* Pick a sporadic-meteor colour by weighted random.
 * 70% warm-white (typical), 15% blue (high-velocity ionised N2),
 * 10% green (sodium/copper compounds), 5% red (slow). */
static Color sporadic_color(void) {
    float r = met_randf();
    if (r < 0.70f) return (Color){ 1.00f, 0.95f, 0.85f }; /* warm-white */
    if (r < 0.85f) return (Color){ 0.70f, 0.85f, 1.00f }; /* blue */
    if (r < 0.95f) return (Color){ 0.60f, 1.00f, 0.70f }; /* green */
    return (Color){ 1.00f, 0.50f, 0.40f };                /* red */
}

static void meteors_step_internal(const AstroState *st, Framebuffer *fb,
                                  double dt) {
    /* Tick existing meteors. */
    for (int i = 0; i < METEOR_POOL; i++) {
        if (!met_pool[i].active) continue;
        met_pool[i].sx += met_pool[i].vx * (float)dt;
        met_pool[i].sy += met_pool[i].vy * (float)dt;
        met_pool[i].life -= (float)dt;
        if (met_pool[i].life <= 0.0f) met_pool[i].active = 0;
    }

    /* === Sporadic background ===========================================
     *
     * Real sky has ~7-10 meteors/hr from random radiants — Earth sweeping
     * up the dust complex. The rate roughly doubles between 18:00 and
     * 06:00 local solar time as the apex of Earth's motion rotates into
     * view. Below the horizon we don't render any.
     *
     * Apex factor: 0.5 at 6pm (looking trailing), 2.0 at 6am (looking
     * leading). cosine-shaped over 24h centred on hour-angle = -6h.
     */
    if (st->pos[EPHEM_SUN].alt_rad < 0.0) {
        double sun_ra_h = st->pos[EPHEM_SUN].ra_rad * 12.0 / M_PI;
        double ha_h = st->lst_hours - sun_ra_h;
        while (ha_h >  12.0) ha_h -= 24.0;
        while (ha_h < -12.0) ha_h += 24.0;
        double apex = 1.25 + 0.75 * cos((ha_h + 6.0) * M_PI / 12.0);

        const double SPORADIC_PER_HR = 8.0;
        double rate_per_sec = (SPORADIC_PER_HR * apex) / 3600.0;
        sporadic_spawn_accum += rate_per_sec * dt;

        float cx = (float)fb->sub_w * 0.5f;
        float cy = (float)fb->sub_h * 0.5f;
        float r_max = fminf(cx, cy * 0.5f) * 0.85f;

        while (sporadic_spawn_accum >= 1.0) {
            sporadic_spawn_accum -= 1.0;
            int slot = -1;
            for (int i = 0; i < METEOR_POOL; i++) {
                if (!met_pool[i].active) { slot = i; break; }
            }
            if (slot < 0) break;
            Meteor *m = &met_pool[slot];
            /* Uniform random position within the projection disc. sqrt
             * for area-uniform sampling; y stretched 2× for terminal
             * cell aspect (matches project()). */
            float rr = sqrtf(met_randf()) * r_max;
            float th = met_randf() * 2.0f * (float)M_PI;
            m->sx = cx + rr * cosf(th);
            m->sy = cy + rr * sinf(th) * 2.0f;
            float ang = met_randf() * 2.0f * (float)M_PI;
            float speed = 40.0f + met_randf() * 60.0f;     /* slower than showers */
            m->vx = cosf(ang) * speed;
            m->vy = sinf(ang) * speed;
            m->max_life  = 0.35f + met_randf() * 0.5f;
            m->life      = m->max_life;
            m->intensity = 0.5f + met_randf() * 0.7f;
            m->color     = sporadic_color();
            m->active    = 1;
        }
    }

    /* === Active shower ================================================ */
    float rate_per_min = 0.0f;
    const MeteorShower *sh = meteor_active_shower(st->jd, &rate_per_min);
    if (!sh || rate_per_min < 0.05f) return;

    double ra  = sh->ra_h * (M_PI / 12.0);
    double dec = sh->dec_deg * DEG2RAD;
    double lst_rad = st->lst_hours * (M_PI / 12.0);
    double alt, az;
    radec_to_altaz(ra, dec, st->observer.lat_rad, lst_rad, &alt, &az);
    if (alt < 0.05) return;             /* radiant below horizon */
    float rx, ry;
    if (project(fb->sub_w, fb->sub_h, alt, az, &rx, &ry) != 0) return;

    /* ZHR is "if radiant at zenith" — multiply by sin(alt). */
    float rate_per_sec = (rate_per_min / 60.0f) * (float)sin(alt);
    met_spawn_accum += rate_per_sec * dt;

    while (met_spawn_accum >= 1.0) {
        met_spawn_accum -= 1.0;
        int slot = -1;
        for (int i = 0; i < METEOR_POOL; i++) {
            if (!met_pool[i].active) { slot = i; break; }
        }
        if (slot < 0) break;
        Meteor *m = &met_pool[slot];
        float ang    = met_randf() * 2.0f * (float)M_PI;
        float spread = met_randf() * 12.0f;
        m->sx = rx + cosf(ang) * spread;
        m->sy = ry + sinf(ang) * spread;
        /* Per-shower velocity factor: Leonids whip past ~1.4× the
         * sporadic baseline; Draconids drift at ~0.55×. */
        float speed = (60.0f + met_randf() * 80.0f) * sh->vel_factor;
        m->vx = cosf(ang) * speed;
        m->vy = sinf(ang) * speed;
        /* Faster meteors are also brighter and shorter-lived. */
        m->max_life  = (0.4f + met_randf() * 0.4f) / sh->vel_factor;
        m->life      = m->max_life;
        m->intensity = (0.7f + met_randf() * 0.6f)
                     * (0.85f + 0.30f * sh->vel_factor);
        m->color     = sh->tint;
        m->active    = 1;
    }
}

static void meteors_draw(Framebuffer *fb) {
    for (int i = 0; i < METEOR_POOL; i++) {
        if (!met_pool[i].active) continue;
        const Meteor *m = &met_pool[i];
        float age_k = m->life / m->max_life;
        float head  = m->intensity * age_k;
        if (head < 0.01f) continue;
        float vlen = sqrtf(m->vx * m->vx + m->vy * m->vy);
        if (vlen < 1e-3f) continue;
        float ux = -m->vx / vlen;
        float uy = -m->vy / vlen;
        for (int s = 0; s < 14; s++) {
            float k = head * (1.0f - (float)s / 14.0f);
            if (k < 0.01f) break;
            int x = (int)(m->sx + ux * s + 0.5f);
            int y = (int)(m->sy + uy * s + 0.5f);
            if (x < 0 || y < 0 || x >= fb->sub_w || y >= fb->sub_h) continue;
            fb_add(fb, x, y,
                   m->color.r * k, m->color.g * k, m->color.b * k);
        }
    }
}

/* === Tier 4: eclipses =============================================== *
 *
 * Detection is on real angular separation (RA/Dec), so the events fire at
 * the same cadence as reality (≈4 a year between solar+lunar). The visual
 * scaling is generous so the user actually sees something on a terminal.
 *
 *   - Solar: Moon transits Sun. After the Sun's stamp lands, dim a
 *     Moon-sized disc at the Moon's screen position centred over the
 *     Sun. Result: a "bite" out of the Sun, totality = full block.
 *   - Lunar: Moon enters Earth's umbra. `stamp_moon` is fed a shadow
 *     factor that dims the whole disc and red-shifts it (atmospheric
 *     refraction colour through the umbra).
 */
static double angular_sep_rad(double ra1, double dec1, double ra2, double dec2) {
    double c = sin(dec1) * sin(dec2)
             + cos(dec1) * cos(dec2) * cos(ra1 - ra2);
    if (c >  1.0) c =  1.0;
    if (c < -1.0) c = -1.0;
    return acos(c);
}

/* Sun + Moon angular radii sum ≈ 0.527° ≈ 0.0092 rad. We use 0.012 rad
 * (~0.69°) so partial eclipses register a tick before perfect alignment. */
static double solar_eclipse_factor(const AstroState *st) {
    const EphemPosition *S = &st->pos[EPHEM_SUN];
    const EphemPosition *M = &st->pos[EPHEM_MOON];
    double sep = angular_sep_rad(S->ra_rad, S->dec_rad,
                                 M->ra_rad, M->dec_rad);
    double f = (0.012 - sep) / 0.012;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

/* Earth umbra angular radius at Moon ≈ 0.7°, penumbra ≈ 1.3°. We trigger
 * within 0.022 rad (~1.26°) for totality-into-penumbra. */
static double lunar_eclipse_factor(const AstroState *st) {
    const EphemPosition *S = &st->pos[EPHEM_SUN];
    const EphemPosition *M = &st->pos[EPHEM_MOON];
    double anti_ra  = S->ra_rad + M_PI;
    double anti_dec = -S->dec_rad;
    double sep = angular_sep_rad(anti_ra, anti_dec,
                                 M->ra_rad, M->dec_rad);
    double f = (0.022 - sep) / 0.022;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return f;
}

/* Bite a Moon-shaped dim disc out of the Sun, centred at Moon's screen
 * position. f=1.0: full block (totality). f<1: partial — Moon disc still
 * stamped same size but at lower opacity. */
static void apply_solar_eclipse(Framebuffer *fb,
                                float moon_sx, float moon_sy,
                                float moon_R, float f) {
    float box = moon_R * 3.0f;
    int x0 = (int)floorf(moon_sx - box);
    int x1 = (int)ceilf (moon_sx + box);
    int y0 = (int)floorf(moon_sy - box);
    int y1 = (int)ceilf (moon_sy + box);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fb->sub_w - 1) x1 = fb->sub_w - 1;
    if (y1 > fb->sub_h - 1) y1 = fb->sub_h - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y - moon_sy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x - moon_sx;
            float t = expf(-(dx*dx + dy*dy) / (2.0f * moon_R * moon_R));
            float dim = 1.0f - t * f;
            float *p = &fb->data[(y * fb->sub_w + x) * 3];
            p[0] *= dim;
            p[1] *= dim;
            p[2] *= dim;
        }
    }
}

/* Edge-triggered HUD event surfacing. Module-static prev_* track the
 * last frame's "is this thing active?" state so we only push log lines
 * on transitions, not every frame. The values reset to "inactive" at
 * program start, so a voidwatch launched mid-eclipse will still log
 * the "begins" line on its first frame — better than silence. */
void astro_surface_events(const AstroState *st, double t_mono) {
    static const MeteorShower *prev_shower = NULL;
    static int    prev_solar = 0;
    static int    prev_lunar = 0;
    static double peak_solar_mag = 0;
    static double peak_lunar_mag = 0;

    /* Meteor shower — fires when the active shower *changes*, including
     * the transition from "no shower" to one (and vice versa). The
     * MIN_RATE floor is in showers/hr; below it we treat the shower as
     * inactive even if its profile is technically nonzero. */
    const float MIN_RATE_PER_HR = 1.0f;
    float rate_per_min = 0.0f;
    const MeteorShower *sh = meteor_active_shower(st->jd, &rate_per_min);
    int now_active = (sh != NULL && rate_per_min * 60.0f >= MIN_RATE_PER_HR);
    if (now_active && sh != prev_shower) {
        char buf[28];
        snprintf(buf, sizeof buf, "%s ~%.0f/hr", sh->name, rate_per_min * 60.0f);
        hud_log_event(t_mono, buf);
    } else if (!now_active && prev_shower != NULL) {
        char buf[28];
        snprintf(buf, sizeof buf, "%s ended", prev_shower->name);
        hud_log_event(t_mono, buf);
    }
    prev_shower = now_active ? sh : NULL;

    /* Solar eclipse — factor > 0 means the Moon is overlapping the Sun
     * by enough to dim the disc. Threshold of 0.05 to ignore numerical
     * noise on the boundary. Track peak magnitude across the window so
     * the end message can report it. */
    double sf = solar_eclipse_factor(st);
    int now_solar = (sf > 0.05);
    if (now_solar && sf > peak_solar_mag) peak_solar_mag = sf;
    if (now_solar && !prev_solar)
        hud_log_event(t_mono, "solar eclipse begins");
    else if (!now_solar && prev_solar) {
        char buf[28];
        snprintf(buf, sizeof buf, "solar eclipse — peak %.2f", peak_solar_mag);
        hud_log_event(t_mono, buf);
        peak_solar_mag = 0;
    }
    prev_solar = now_solar;

    /* Lunar eclipse — same shape. */
    double lf = lunar_eclipse_factor(st);
    int now_lunar = (lf > 0.05);
    if (now_lunar && lf > peak_lunar_mag) peak_lunar_mag = lf;
    if (now_lunar && !prev_lunar)
        hud_log_event(t_mono, "lunar eclipse begins");
    else if (!now_lunar && prev_lunar) {
        char buf[28];
        snprintf(buf, sizeof buf, "lunar eclipse — peak %.2f", peak_lunar_mag);
        hud_log_event(t_mono, buf);
        peak_lunar_mag = 0;
    }
    prev_lunar = now_lunar;

    /* Lunar close passes — Moon within ~0.5° of a planet. Strict
     * occultations (Moon's 0.26° disc actually covering the planet)
     * are rare from Earth's centre because they need the Moon to be
     * exactly on the planet's line of sight; most "occultation"
     * reports are observer-local parallax events. The 0.5° threshold
     * fires on the visually striking close-pair moments any observer
     * can see — same threshold as planet-planet conjunctions but
     * applied to the Moon-planet pair. Edge-triggered. */
    static int prev_occ[EPHEM_COUNT];
    const double OCC_THRESHOLD = 0.0087;       /* ~0.5° in radians */
    const EphemPosition *moon = &st->pos[EPHEM_MOON];
    for (int i = EPHEM_MERCURY; i <= EPHEM_NEPTUNE; i++) {
        const EphemPosition *p = &st->pos[i];
        if (moon->alt_rad < 0.0 || p->alt_rad < 0.0) {
            prev_occ[i] = 0;
            continue;
        }
        double sep = angular_sep_rad(moon->ra_rad, moon->dec_rad,
                                     p->ra_rad, p->dec_rad);
        int now_occ = (sep < OCC_THRESHOLD);
        if (now_occ && !prev_occ[i]) {
            char buf[28];
            snprintf(buf, sizeof buf, "Moon near %s (%.1f\xC2\xB0)",
                     ephem_short((EphemBody)i),
                     sep * RAD2DEG);
            hud_log_event(t_mono, buf);
        } else if (!now_occ && prev_occ[i]) {
            char buf[28];
            snprintf(buf, sizeof buf, "Moon clears %s",
                     ephem_short((EphemBody)i));
            hud_log_event(t_mono, buf);
        }
        prev_occ[i] = now_occ;
    }

    /* Planet-planet conjunctions — angular separation under ~1° (anything
     * tighter is too rare; anything looser is uninteresting). Mercury
     * through Neptune only — Sun is daytime, Moon's monthly sweep would
     * spam the log. 21 pair checks per frame, all dot-products. */
    static int prev_conj[EPHEM_COUNT][EPHEM_COUNT];
    const double CONJ_THRESHOLD = 0.0175;     /* 1.0° in radians */
    for (int i = EPHEM_MERCURY; i <= EPHEM_NEPTUNE; i++) {
        for (int j = i + 1; j <= EPHEM_NEPTUNE; j++) {
            const EphemPosition *a = &st->pos[i];
            const EphemPosition *b = &st->pos[j];
            /* Both must be above horizon to read as a conjunction. */
            if (a->alt_rad < 0.0 || b->alt_rad < 0.0) {
                prev_conj[i][j] = 0;
                continue;
            }
            double sep = angular_sep_rad(a->ra_rad, a->dec_rad,
                                         b->ra_rad, b->dec_rad);
            int now_conj = (sep < CONJ_THRESHOLD);
            if (now_conj && !prev_conj[i][j]) {
                char buf[28];
                snprintf(buf, sizeof buf, "%s-%s conj %.1f\xC2\xB0",
                         ephem_short((EphemBody)i),
                         ephem_short((EphemBody)j),
                         sep * RAD2DEG);
                hud_log_event(t_mono, buf);
            } else if (!now_conj && prev_conj[i][j]) {
                /* End: just a clean separation announcement. */
                char buf[28];
                snprintf(buf, sizeof buf, "%s-%s separating",
                         ephem_short((EphemBody)i),
                         ephem_short((EphemBody)j));
                hud_log_event(t_mono, buf);
            }
            prev_conj[i][j] = now_conj;
        }
    }
}

/* === In-program search jump (`/`) =================================== */

static int strieq_local(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (char)tolower((unsigned char)*a);
        char cb = (char)tolower((unsigned char)*b);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* Case-insensitive equal against any token in a comma-separated list.
 * Used for satellite aliases like "iss,zarya,25544". */
static int alias_match_local(const char *aliases, const char *name) {
    if (!aliases || !*name) return 0;
    size_t need = strlen(name);
    const char *p = aliases;
    while (*p) {
        const char *q = p;
        while (*q && *q != ',') q++;
        size_t len = (size_t)(q - p);
        if (len == need) {
            int eq = 1;
            for (size_t i = 0; i < len; i++) {
                if (tolower((unsigned char)p[i]) !=
                    tolower((unsigned char)name[i])) { eq = 0; break; }
            }
            if (eq) return 1;
        }
        p = (*q == ',') ? q + 1 : q;
    }
    return 0;
}

/* Case-insensitive substring match. Used for DSO names so the user
 * can type `m31` or `andromeda` and match "M31 Andromeda". */
static int stristr_local(const char *haystack, const char *needle) {
    if (!*needle) return 1;
    while (*haystack) {
        const char *h = haystack, *n = needle;
        while (*h && *n) {
            if (tolower((unsigned char)*h) != tolower((unsigned char)*n)) break;
            h++; n++;
        }
        if (!*n) return 1;
        haystack++;
    }
    return 0;
}

static double alt_search_planet(const Observer *obs, int idx, time_t t) {
    EphemPosition p;
    double jd = ephem_julian_day_from_unix(t);
    ephem_compute((EphemBody)idx, jd, &p);
    ephem_to_topocentric(&p, obs, jd);
    return p.alt_rad;
}

static double alt_search_comet(const Observer *obs, int idx, time_t t) {
    double jd = ephem_julian_day_from_unix(t);
    CometState states[COMET_COUNT];
    comet_compute_all(jd, states);
    EphemPosition tmp = {
        .ra_rad = states[idx].ra_rad,
        .dec_rad = states[idx].dec_rad,
        .distance_au = states[idx].dist_au,
    };
    ephem_to_topocentric(&tmp, obs, jd);
    return tmp.alt_rad;
}

static double alt_search_asteroid(const Observer *obs, int idx, time_t t) {
    double jd = ephem_julian_day_from_unix(t);
    AsteroidState states[ASTEROID_COUNT];
    asteroid_compute_all(jd, states);
    EphemPosition tmp = {
        .ra_rad = states[idx].ra_rad,
        .dec_rad = states[idx].dec_rad,
        .distance_au = states[idx].dist_au,
    };
    ephem_to_topocentric(&tmp, obs, jd);
    return tmp.alt_rad;
}

static double alt_search_dso(const Observer *obs, int idx, time_t t) {
    /* DSO position is fixed in J2000. Recompute alt/az from current LST. */
    double jd = ephem_julian_day_from_unix(t);
    double lst_h = ephem_local_sidereal_hours(jd, obs->lon_rad);
    double lst_rad = lst_h * 15.0 * DEG2RAD;
    double ra = dso_catalog[idx].ra_h * (M_PI / 12.0);
    double dec = dso_catalog[idx].dec_deg * DEG2RAD;
    double H = lst_rad - ra;
    double sin_alt = sin(dec) * sin(obs->lat_rad)
                   + cos(dec) * cos(obs->lat_rad) * cos(H);
    if (sin_alt >  1.0) sin_alt =  1.0;
    if (sin_alt < -1.0) sin_alt = -1.0;
    return asin(sin_alt);
}

typedef double (*search_alt_fn)(const Observer *obs, int idx, time_t t);

int astro_search_body(const AstroState *st, const char *name,
                      double *out_seconds, char *display_out, size_t cap,
                      int *out_kind, int *out_idx) {
    /* Resolve name → (kind, idx, display) */
    int   kind = 0, idx = -1;
    const char *display = NULL;
    for (int i = 0; i < EPHEM_COUNT; i++) {
        if (strieq_local(ephem_name((EphemBody)i),  name)
         || strieq_local(ephem_short((EphemBody)i), name)) {
            kind = 1; idx = i; display = ephem_name((EphemBody)i); break;
        }
    }
    if (kind == 0) {
        for (int i = 0; i < COMET_COUNT; i++) {
            if (strieq_local(comet_elements[i].name, name)) {
                kind = 2; idx = i; display = comet_elements[i].name; break;
            }
        }
    }
    if (kind == 0) {
        for (int i = 0; i < ASTEROID_COUNT; i++) {
            if (strieq_local(asteroid_elements[i].name, name)) {
                kind = 3; idx = i; display = asteroid_elements[i].name; break;
            }
        }
    }
    /* DSOs use substring match — names like "M31 Andromeda" should
     * accept either "m31" or "andromeda" alone. */
    if (kind == 0) {
        for (int i = 0; i < dso_count; i++) {
            if (stristr_local(dso_catalog[i].name, name)) {
                kind = 4; idx = i; display = dso_catalog[i].name; break;
            }
        }
    }
    /* Satellites — exact-match name OR comma-separated alias / catalog. */
    if (kind == 0) {
        for (int i = 0; i < SATELLITE_COUNT; i++) {
            const SatelliteElements *e = &satellite_elements[i];
            if (strieq_local(e->name, name)
             || strieq_local(satellite_short_name(i), name)
             || alias_match_local(e->aliases, name)) {
                kind = 5; idx = i; display = e->name; break;
            }
        }
    }
    if (kind == 0) return -1;

    /* Satellites short-circuit the rise-search: return immediately with
     * a zero scrub. The caller arms track, and the cursor will follow
     * the satellite while it's above the horizon. Implementing a proper
     * pass predictor (10-30s coarse step + AOS/LOS bisection) is on
     * the roadmap but isn't a Phase 6 acceptance gate. */
    if (kind == 5) {
        if (display && display_out) snprintf(display_out, cap, "%s", display);
        if (out_kind) *out_kind = kind;
        if (out_idx)  *out_idx  = idx;
        if (out_seconds) *out_seconds = 0.0;
        return 0;
    }

    if (display && display_out) {
        snprintf(display_out, cap, "%s", display);
    }
    if (out_kind) *out_kind = kind;
    if (out_idx)  *out_idx  = idx;

    /* Coarse 10-minute scan + bisection. Mirrors headless's
     * search_zero_crossing but specialised for "next rise" only. */
    search_alt_fn fn = (kind == 1) ? alt_search_planet
                     : (kind == 2) ? alt_search_comet
                     : (kind == 3) ? alt_search_asteroid
                     :               alt_search_dso;
    /* Convert st->jd back to unix time for the walker. */
    time_t now = (time_t)((st->jd - 2440587.5) * 86400.0);
    /* If body is currently above horizon, push past its next set first
     * so we land on a *rise* not the present moment. */
    double cur = fn(&st->observer, idx, now);
    if (cur > 0.0) {
        const long step = 600;
        const long horizon = 30L * 86400L;
        double prev = cur;
        for (long s = step; s <= horizon; s += step) {
            time_t t = now + s;
            double a = fn(&st->observer, idx, t);
            if (prev > 0.0 && a <= 0.0) {
                now = t + 60;     /* nudge past horizon */
                break;
            }
            prev = a;
        }
    }
    /* Now search forward for next rise. */
    const long step = 600;
    const long horizon = 30L * 86400L;
    double prev = fn(&st->observer, idx, now);
    for (long s = step; s <= horizon; s += step) {
        time_t t = now + s;
        double a = fn(&st->observer, idx, t);
        if (prev < 0.0 && a >= 0.0) {
            /* Bisect (now+s-step, now+s). */
            time_t lo = now + s - step, hi = t;
            double lo_alt = prev;
            for (int it = 0; it < 24; it++) {
                time_t mid = lo + (hi - lo) / 2;
                if (mid == lo || mid == hi) break;
                double mid_alt = fn(&st->observer, idx, mid);
                if ((mid_alt < 0.0) == (lo_alt < 0.0)) {
                    lo = mid; lo_alt = mid_alt;
                } else {
                    hi = mid;
                }
            }
            time_t target = lo + (hi - lo) / 2;
            *out_seconds = (double)(target - (time_t)((st->jd - 2440587.5) * 86400.0));
            return 0;
        }
        prev = a;
    }
    return -2;
}

/* Walk forward one day at a time looking for any event. Cheap enough
 * (Meeus is fast) that scanning a year takes a few hundred ms. Used by
 * the `J` key in main.c — jumps the virtual clock to the next event. */
int astro_find_next_event(const AstroState *st, double from_jd,
                          int max_days,
                          double *out_jd, char *label_out, size_t label_cap) {
    /* Skip the first half-day so we don't re-fire the event we're
     * currently sitting on. */
    double jd = from_jd + 0.5;
    double end = jd + (double)max_days;

    /* Need a writable AstroState-like for ephem_compute / sep checks.
     * Just reuse positions array values from `st->observer`. */
    Observer obs = st->observer;
    (void)obs;        /* lat used implicitly by some of the below */

    double prev_solar_sep = 1e9, prev_lunar_sep = 1e9;
    int    sm_set = 0;
    double prev_planet_sep[EPHEM_COUNT][EPHEM_COUNT];
    int    psep_set = 0;
    (void)prev_solar_sep; (void)prev_lunar_sep;
    for (int i = 0; i < EPHEM_COUNT; i++)
        for (int j = 0; j < EPHEM_COUNT; j++)
            prev_planet_sep[i][j] = 1e9;

    while (jd < end) {
        EphemPosition s, m;
        ephem_compute(EPHEM_SUN,  jd, &s);
        ephem_compute(EPHEM_MOON, jd, &m);

        /* Eclipse — sep crosses below threshold. */
        double sep_sm = angular_sep_rad(s.ra_rad, s.dec_rad,
                                         m.ra_rad, m.dec_rad);
        if (sep_sm < 0.012) {
            *out_jd = jd;
            snprintf(label_out, label_cap, "solar eclipse");
            return 0;
        }
        double anti_ra = s.ra_rad + M_PI, anti_dec = -s.dec_rad;
        double sep_lm = angular_sep_rad(anti_ra, anti_dec,
                                         m.ra_rad, m.dec_rad);
        if (sep_lm < 0.022) {
            *out_jd = jd;
            snprintf(label_out, label_cap, "lunar eclipse");
            return 0;
        }
        sm_set = 1;
        (void)sm_set;

        /* Planet-planet conjunction — pair sep dips below 1°. We need
         * the previous-step sep to detect the dip moment, not just any
         * sub-1° sample (which would always trigger if we stepped into
         * an active window). */
        EphemPosition p[EPHEM_COUNT];
        for (int i = EPHEM_MERCURY; i <= EPHEM_NEPTUNE; i++) {
            ephem_compute((EphemBody)i, jd, &p[i]);
        }
        for (int i = EPHEM_MERCURY; i <= EPHEM_NEPTUNE; i++) {
            for (int j = i + 1; j <= EPHEM_NEPTUNE; j++) {
                double sep = angular_sep_rad(p[i].ra_rad, p[i].dec_rad,
                                              p[j].ra_rad, p[j].dec_rad);
                if (psep_set && sep < 0.0175 && prev_planet_sep[i][j] >= 0.0175) {
                    *out_jd = jd;
                    snprintf(label_out, label_cap, "%s-%s conjunction",
                             ephem_short((EphemBody)i),
                             ephem_short((EphemBody)j));
                    return 0;
                }
                prev_planet_sep[i][j] = sep;
            }
        }
        psep_set = 1;

        /* Meteor shower peak — DOY match (within ±0.5 day). */
        for (int i = 0; i < meteor_shower_count; i++) {
            const MeteorShower *sh = &meteor_showers[i];
            double frac = jd - 2451544.5;
            double yfrac = fmod(frac / 365.25, 1.0);
            if (yfrac < 0.0) yfrac += 1.0;
            double doy = yfrac * 365.25 + 1.0;
            if (fabs(doy - sh->peak_doy) < 0.5) {
                *out_jd = jd;
                snprintf(label_out, label_cap, "%s peak", sh->name);
                return 0;
            }
        }

        jd += 1.0;
    }
    return -1;
}

/* === Heliocentric "above the ecliptic" view ========================= *
 *
 * Projection is top-down: heliocentric ecliptic (x, y) maps to screen,
 * z is dropped. Distance compression is sqrt(r_au) so Mercury (0.39 AU)
 * stays a few sub-pixels off the Sun while Neptune (30 AU) sits at the
 * frame edge, instead of Mercury collapsing into the Sun's halo.
 *
 * The y axis on screen is multiplied by 2 to compensate for terminal
 * cells being ~2× taller than wide; circular orbits then *look* circular
 * in display pixels rather than oval.
 *
 * No stars, no constellations, no Milky Way — heliocentric is its own
 * clean diagram. The horizon, alt/az, twilight, refraction, extinction
 * — all of those are observer-frame concepts that don't exist here.
 */
static void helio_map_au(double cx, double cy, float r_scale,
                         double x_au, double y_au,
                         float *sx, float *sy) {
    double r = sqrt(x_au * x_au + y_au * y_au);
    double r_screen = sqrt(r) * (double)r_scale;
    double theta = atan2(y_au, x_au);
    *sx = (float)(cx + r_screen * cos(theta));
    *sy = (float)(cy + r_screen * sin(theta) * 2.0);
}

/* Orbital periods in days for the heliocentric trace pass. From Kepler's
 * third law on the J2000 semi-major axes; the values match astronomical
 * unit tables to four sig figs. */
typedef struct {
    EphemBody body;
    double    period_days;
    Color     tint;
} HelioOrbit;

static const HelioOrbit helio_orbits[] = {
    { EPHEM_MERCURY,    87.97, { 0.45f, 0.40f, 0.32f } },
    { EPHEM_VENUS,     224.70, { 0.55f, 0.50f, 0.40f } },
    { EPHEM_MARS,      686.97, { 0.55f, 0.30f, 0.25f } },
    { EPHEM_JUPITER,  4332.59, { 0.50f, 0.45f, 0.35f } },
    { EPHEM_SATURN,  10759.22, { 0.50f, 0.45f, 0.30f } },
    { EPHEM_URANUS,  30688.50, { 0.30f, 0.45f, 0.50f } },
    { EPHEM_NEPTUNE, 60182.00, { 0.30f, 0.35f, 0.55f } },
};
static const int helio_orbit_count =
    (int)(sizeof helio_orbits / sizeof helio_orbits[0]);

static void helio_orbit_trace(const AstroState *st, Framebuffer *fb,
                              float cx, float cy, float r_scale,
                              EphemBody body, double period_days,
                              Color tint) {
    /* Walk one full period from now, sampling 240 evenly-spaced points.
     * Each point gets a single-pixel fb_max stamp — the result reads as
     * a continuous dim line at any reasonable terminal size. */
    const int N = 240;
    for (int i = 0; i < N; i++) {
        double jd_s = st->jd + ((double)i / N) * period_days;
        double x = 0, y = 0, z = 0;
        if (body == EPHEM_COUNT) {
            /* Sentinel for Earth, which isn't an EphemBody. */
            ephem_earth_helio_xyz(jd_s, &x, &y, &z);
        } else {
            ephem_helio_xyz_for(body, jd_s, &x, &y, &z);
        }
        float sx, sy;
        helio_map_au(cx, cy, r_scale, x, y, &sx, &sy);
        int ix = (int)floorf(sx + 0.5f);
        int iy = (int)floorf(sy + 0.5f);
        if (ix < 0 || iy < 0 || ix >= fb->sub_w || iy >= fb->sub_h) continue;
        fb_max(fb, ix, iy,
               tint.r * 0.45f, tint.g * 0.45f, tint.b * 0.45f);
    }
}

static void astro_draw_heliocentric(const AstroState *st, Framebuffer *fb) {
    float cx = (float)fb->sub_w * 0.5f;
    float cy = (float)fb->sub_h * 0.5f;
    float r_max = fminf(cx, cy * 0.5f) * 0.85f;
    /* Neptune at 30 AU sits at r_max; sqrt-scale picks the per-sqrt-AU
     * conversion factor accordingly. */
    float r_scale = r_max / (float)sqrt(30.5);

    /* Orbital ellipses first so the body discs and Sun stamp on top. */
    Color earth_orbit_tint = { 0.30f, 0.40f, 0.55f };
    helio_orbit_trace(st, fb, cx, cy, r_scale, EPHEM_COUNT, 365.25,
                      earth_orbit_tint);
    for (int k = 0; k < helio_orbit_count; k++) {
        helio_orbit_trace(st, fb, cx, cy, r_scale,
                          helio_orbits[k].body,
                          helio_orbits[k].period_days,
                          helio_orbits[k].tint);
    }

    /* Sun at centre — slightly smaller than the geocentric stamp because
     * the rest of the diagram is much smaller than the Sun's geocentric
     * disc would imply. */
    AstroStyle sun = style_for(EPHEM_SUN);
    sun.radius_sub = 4.0f;
    sun.intensity  = 2.2f * st->bright_boost;
    stamp_body(fb, cx, cy, &sun);

    /* Earth — explicit body in heliocentric. Cool blue dot. */
    double ex, ey, ez;
    ephem_earth_helio_xyz(st->jd, &ex, &ey, &ez);
    float esx, esy;
    helio_map_au(cx, cy, r_scale, ex, ey, &esx, &esy);
    AstroStyle earth = { {0.45f, 0.65f, 0.95f}, 1.4f,
                         1.3f * st->bright_boost, 0.40f };
    stamp_body(fb, esx, esy, &earth);

    /* Other planets. */
    for (int b = EPHEM_MERCURY; b <= EPHEM_NEPTUNE; b++) {
        double px, py, pz;
        ephem_helio_xyz_for((EphemBody)b, st->jd, &px, &py, &pz);
        float sx, sy;
        helio_map_au(cx, cy, r_scale, px, py, &sx, &sy);
        AstroStyle s = style_for((EphemBody)b);
        s.intensity *= st->bright_boost;
        stamp_body(fb, sx, sy, &s);
    }

    /* Comet orbital traces — sample 240 points across one full orbital
     * period (or ~75y for long-period comets). Off-screen samples just
     * fall outside the projection bounds and get clipped. Halley's
     * elongated orbit will show as a long ellipse reaching past Neptune;
     * Hale-Bopp's aphelion at ~370 AU clips entirely off screen, leaving
     * just the perihelion arc visible. */
    Color comet_orbit_tint = { 0.40f, 0.50f, 0.60f };
    for (int i = 0; i < COMET_COUNT; i++) {
        double period_d;
        if (comet_elements[i].period_yr > 0.0) {
            period_d = comet_elements[i].period_yr * 365.25;
        } else {
            /* Long-period: sample a 75-year arc around perihelion to
             * give a useful chunk without spending ages on aphelion. */
            period_d = 75.0 * 365.25;
        }
        double t0 = comet_elements[i].T_jd;
        if (period_d > 50000.0) period_d = 50000.0;     /* cap */
        const int N = 240;
        for (int k = 0; k < N; k++) {
            double jd_s = t0 - period_d * 0.5
                        + ((double)k / N) * period_d;
            double xc, yc, zc;
            comet_helio_xyz_for(i, jd_s, &xc, &yc, &zc);
            float sx, sy;
            helio_map_au(cx, cy, r_scale, xc, yc, &sx, &sy);
            int ix = (int)floorf(sx + 0.5f);
            int iy = (int)floorf(sy + 0.5f);
            if (ix < 0 || iy < 0 || ix >= fb->sub_w || iy >= fb->sub_h)
                continue;
            fb_max(fb, ix, iy,
                   comet_orbit_tint.r * 0.35f,
                   comet_orbit_tint.g * 0.35f,
                   comet_orbit_tint.b * 0.35f);
        }
    }

    /* Asteroid orbital traces — all in main belt, all easily fit. */
    Color ast_orbit_tint = { 0.45f, 0.40f, 0.35f };
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        double a = asteroid_elements[i].a_au;
        double period_d = pow(a, 1.5) * 365.25;
        const int N = 180;
        for (int k = 0; k < N; k++) {
            double jd_s = st->jd + ((double)k / N) * period_d;
            double xa, ya, za;
            asteroid_helio_xyz_for(i, jd_s, &xa, &ya, &za);
            float sx, sy;
            helio_map_au(cx, cy, r_scale, xa, ya, &sx, &sy);
            int ix = (int)floorf(sx + 0.5f);
            int iy = (int)floorf(sy + 0.5f);
            if (ix < 0 || iy < 0 || ix >= fb->sub_w || iy >= fb->sub_h)
                continue;
            fb_max(fb, ix, iy,
                   ast_orbit_tint.r * 0.35f,
                   ast_orbit_tint.g * 0.35f,
                   ast_orbit_tint.b * 0.35f);
        }
    }

    /* Comets — current heliocentric positions. Most have aphelions far
     * outside Neptune's orbit (Halley ~35 AU, Hale-Bopp ~370 AU); we
     * project them at their actual position and let the screen-radius
     * clip handle the rest. Tail points away from the Sun (which is at
     * screen centre), length scales with 1/r_helio so close-in comets
     * grow visible plumes. */
    Color comet_tint = { 0.85f, 0.95f, 1.00f };
    for (int i = 0; i < COMET_COUNT; i++) {
        double cx_au, cy_au, cz_au;
        comet_helio_xyz_for(i, st->jd, &cx_au, &cy_au, &cz_au);
        float sx, sy;
        helio_map_au(cx, cy, r_scale, cx_au, cy_au, &sx, &sy);
        if (sx < 0 || sy < 0 || sx >= fb->sub_w || sy >= fb->sub_h) continue;

        /* Head dot. */
        AstroStyle head = { comet_tint, 1.4f, 0.9f * st->bright_boost, 0.30f };
        stamp_body(fb, sx, sy, &head);

        /* Tail: outward from Sun. Length grows as r → q (perihelion). */
        float dx = sx - cx, dy = sy - cy;
        float dlen = sqrtf(dx*dx + dy*dy);
        if (dlen < 1e-3f) continue;
        float ux = dx / dlen, uy = dy / dlen;
        double r = sqrt(cx_au*cx_au + cy_au*cy_au + cz_au*cz_au);
        if (r < 0.3) r = 0.3;
        float tail_px = (float)(15.0 / r);
        if (tail_px > 50.0f) tail_px = 50.0f;
        int n = (int)tail_px;
        for (int s = 1; s < n; s++) {
            float k = (1.0f - (float)s / n) * 0.5f;
            if (k < 0.02f) break;
            int ix = (int)(sx + ux * s + 0.5f);
            int iy = (int)(sy + uy * s + 0.5f);
            if (ix < 0 || iy < 0 || ix >= fb->sub_w || iy >= fb->sub_h) continue;
            fb_add(fb, ix, iy,
                   comet_tint.r * k, comet_tint.g * k, comet_tint.b * k);
        }
    }

    /* Asteroids — neutral rocky dots, no tail. */
    Color ast_tint = { 0.85f, 0.82f, 0.75f };
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        double ax, ay, az;
        asteroid_helio_xyz_for(i, st->jd, &ax, &ay, &az);
        float sx, sy;
        helio_map_au(cx, cy, r_scale, ax, ay, &sx, &sy);
        if (sx < 0 || sy < 0 || sx >= fb->sub_w || sy >= fb->sub_h) continue;
        AstroStyle s = { ast_tint, 0.85f, 0.7f * st->bright_boost, 0.20f };
        stamp_body(fb, sx, sy, &s);
    }
}

/* Cell-coord screen position for a body in heliocentric mode. Used by
 * astro_labels when view_mode=1. */
static void helio_cell_for(const AstroState *st, int cols, int rows,
                           EphemBody b, int *out_col, int *out_row) {
    int sub_w = cols * 2, sub_h = rows * 4;
    float cx = (float)sub_w * 0.5f;
    float cy = (float)sub_h * 0.5f;
    float r_max = fminf(cx, cy * 0.5f) * 0.85f;
    float r_scale = r_max / (float)sqrt(30.5);
    double x = 0, y = 0, z = 0;
    ephem_helio_xyz_for(b, st->jd, &x, &y, &z);
    float sx, sy;
    helio_map_au(cx, cy, r_scale, x, y, &sx, &sy);
    *out_col = (int)(sx / 2.0f);
    *out_row = (int)(sy / 4.0f);
}

void astro_draw(const AstroState *st, Framebuffer *fb,
                int cols, int rows, const AudioSnapshot *snap) {
    (void)cols; (void)rows; (void)snap;

    if (st->view_mode == 1) {
        astro_draw_heliocentric(st, fb);
        return;
    }

    /* Twilight horizon glow — only when Sun is in the twilight bands.
     * Drawn first so MW + stars overdraw it cleanly with fb_max. */
    twilight_draw(st, fb);

    /* Aurora — when toggled on (`a`), shimmering streaks near the
     * poleward horizon. Drawn after twilight (so it stays visible at
     * dusk) but before stars (so stars overdraw the upper bands). */
    aurora_draw(st, fb);

    /* Diffuse Milky Way wash sits behind the bodies — fb_max doesn't
     * accumulate, so re-stamping each frame is fine. */
    milkyway_draw(st, fb);

    /* Optional alt-az grid sits behind the constellations + stars. */
    if (st->show_grid) grid_draw(st, fb);

    /* Constellation stick figures are an opt-in overlay (`l`). When on,
     * draw before stars so brighter star stamps cleanly overdraw line
     * dots near the endpoints. Default off — most of the time the user
     * wants a clean naked sky. */
    if (st->show_constellations) constellations_draw(st, fb);
    stars_draw(st, fb);

    /* DSOs (M31, M42, etc.) — fuzzy patches between stars and planets.
     * Default on; toggle with `d`. */
    if (st->show_dso) dsos_draw(st, fb);

    /* Trails behind the planet discs — additive so newest sample blends
     * into the planet glow seamlessly. */
    if (st->show_trails) trails_draw(st, fb);

    /* Sun position projected unclipped — needed even when below the horizon
     * to orient the lunar terminator on the night side. */
    float sun_sx, sun_sy;
    project_unclipped(fb->sub_w, fb->sub_h,
                      st->pos[EPHEM_SUN].alt_rad,
                      st->pos[EPHEM_SUN].az_rad,
                      &sun_sx, &sun_sy);

    /* Eclipse factors computed once per frame. */
    double solar_f = solar_eclipse_factor(st);
    double lunar_f = lunar_eclipse_factor(st);

    /* Cache Moon's screen position for the solar-eclipse overlay. */
    int   moon_visible = 0;
    float moon_sx_cached = 0, moon_sy_cached = 0;
    AstroStyle moon_style = style_for(EPHEM_MOON);

    for (int i = 0; i < EPHEM_COUNT; i++) {
        const EphemPosition *p = &st->pos[i];
        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, p->alt_rad, p->az_rad, &sx, &sy) != 0)
            continue;
        AstroStyle s = style_for((EphemBody)i);
        /* Speed-aware intensity compensation — see AstroState.bright_boost.
         * Without this, planets dim ~6× when scrub speed pushes fb_decay
         * down to suppress trails. */
        s.intensity *= st->bright_boost;
        if ((EphemBody)i == EPHEM_MOON) {
            stamp_moon(fb, sx, sy, sun_sx, sun_sy, &s,
                       st->moon_elongation, lunar_f);
            moon_visible = 1;
            moon_sx_cached = sx; moon_sy_cached = sy;
        } else if ((EphemBody)i == EPHEM_SATURN) {
            stamp_saturn(fb, sx, sy, &s, st->jd);
        } else {
            stamp_body(fb, sx, sy, &s);
        }
    }

    /* Solar eclipse: dim a Moon-sized region centred on the Moon over the
     * Sun's stamp. Only meaningful when both are above horizon. */
    if (solar_f > 0.0 && moon_visible &&
        st->pos[EPHEM_SUN].alt_rad > 0.0) {
        apply_solar_eclipse(fb, moon_sx_cached, moon_sy_cached,
                            moon_style.radius_sub, (float)solar_f);
    }

    /* Galilean moons — drawn last so they stamp over Jupiter's halo. */
    galilean_draw(st, fb);

    /* Comets after planets so the head can sit on top of any
     * coincidental star/MW underlay; before meteors so a meteor streak
     * crossing a comet still reads brightest. */
    comets_draw(st, fb, sun_sx, sun_sy);

    /* Asteroids — dim, neutral, no tail. Same composite slot. */
    asteroids_draw(st, fb);

    /* Satellites — sharp cool-white points, fb_max so fast motion
     * doesn't smear. Drawn after slow targets so they sit on top. */
    satellites_draw(st, fb);

    /* Meteors above everything else — they're the brightest, fastest
     * thing in the sky when active. */
    meteors_step(st, fb);
    meteors_draw(fb);
}

static int p_byte(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0)   i = 0;
    if (i > 255) i = 255;
    return i;
}

/* Find the above-horizon body whose cell-projection is nearest to (col,
 * row). Walks planets, comets, and asteroids; meteors excluded because
 * they're transient particles. */
typedef enum {
    PICK_NONE = 0,
    PICK_PLANET,
    PICK_COMET,
    PICK_ASTEROID,
    PICK_DSO       = 4,    /* search-only, used by track_cell_for */
    PICK_SATELLITE = 5,    /* bundled near-Earth satellites        */
} PickKind;

typedef struct {
    PickKind kind;
    int      idx;
} CursorPick;

static double pick_d2(int sub_w, int sub_h, double alt, double az,
                      int col, int row) {
    float sx, sy;
    if (project(sub_w, sub_h, alt, az, &sx, &sy) != 0) return 1e18;
    double bx = sx / 2.0;
    double by = sy / 4.0;
    double dx = bx - col;
    double dy = (by - row) * 2.0;     /* cells are ~2:1 in screen */
    return dx * dx + dy * dy;
}

static CursorPick find_nearest_target(const AstroState *st, int cols, int rows,
                                      int col, int row) {
    CursorPick best = { PICK_NONE, -1 };
    double best_d2 = 1e18;
    int sub_w = cols * 2, sub_h = rows * 4;
    for (int i = 0; i < EPHEM_COUNT; i++) {
        if (st->pos[i].alt_rad < 0.0) continue;
        double d2 = pick_d2(sub_w, sub_h, st->pos[i].alt_rad,
                            st->pos[i].az_rad, col, row);
        if (d2 < best_d2) { best_d2 = d2; best.kind = PICK_PLANET; best.idx = i; }
    }
    for (int i = 0; i < COMET_COUNT; i++) {
        if (!st->comets[i].valid || st->comets[i].alt_rad < 0.0) continue;
        if (st->comets[i].mag > g_config.comet_mag_cutoff)      continue;
        double d2 = pick_d2(sub_w, sub_h, st->comets[i].alt_rad,
                            st->comets[i].az_rad, col, row);
        if (d2 < best_d2) { best_d2 = d2; best.kind = PICK_COMET; best.idx = i; }
    }
    for (int i = 0; i < ASTEROID_COUNT; i++) {
        if (!st->asteroids[i].valid || st->asteroids[i].alt_rad < 0.0) continue;
        if (st->asteroids[i].mag > g_config.asteroid_mag_cutoff)       continue;
        double d2 = pick_d2(sub_w, sub_h, st->asteroids[i].alt_rad,
                            st->asteroids[i].az_rad, col, row);
        if (d2 < best_d2) { best_d2 = d2; best.kind = PICK_ASTEROID; best.idx = i; }
    }
    /* Satellites — only when the overlay is on, only when above
     * horizon. Stale gating mirrors the render path. */
    if (st->show_satellites) {
        for (int i = 0; i < SATELLITE_COUNT; i++) {
            const SatelliteState *s = &st->satellites[i];
            if (!s->valid || !s->above_horizon) continue;
            double age = fabs(st->jd - satellite_epoch_jd(i));
            if (age > 14.0) continue;
            double d2 = pick_d2(sub_w, sub_h, s->alt_rad, s->az_rad,
                                col, row);
            if (d2 < best_d2) {
                best_d2 = d2; best.kind = PICK_SATELLITE; best.idx = i;
            }
        }
    }
    return best;
}

/* Track mode helpers — placed here so they can call find_nearest_target
 * + the PickKind enum without a forward declaration dance.
 *
 * `kind` is the same int we use for search (1=planet, 2=comet,
 * 3=asteroid, 4=DSO). PickKind happens to align for kinds 1-3; DSO
 * is search-only and uses its own RA/Dec lookup. */
static int track_cell_for(const AstroState *st, int cols, int rows,
                          int kind, int idx, int *out_col, int *out_row) {
    int sub_w = cols * 2, sub_h = rows * 4;
    double alt = -1.0, az = 0.0;
    if (kind == PICK_PLANET   && idx >= 0 && idx < EPHEM_COUNT) {
        alt = st->pos[idx].alt_rad;
        az  = st->pos[idx].az_rad;
    } else if (kind == PICK_COMET    && idx >= 0 && idx < COMET_COUNT) {
        alt = st->comets[idx].alt_rad;
        az  = st->comets[idx].az_rad;
    } else if (kind == PICK_ASTEROID && idx >= 0 && idx < ASTEROID_COUNT) {
        alt = st->asteroids[idx].alt_rad;
        az  = st->asteroids[idx].az_rad;
    } else if (kind == PICK_DSO && idx >= 0 && idx < dso_count) {
        /* Recompute from fixed J2000 RA/Dec + current LST. */
        double lst_rad = st->lst_hours * (M_PI / 12.0);
        double ra  = dso_catalog[idx].ra_h * (M_PI / 12.0);
        double dec = dso_catalog[idx].dec_deg * DEG2RAD;
        radec_to_altaz(ra, dec, st->observer.lat_rad, lst_rad, &alt, &az);
    } else if (kind == PICK_SATELLITE && idx >= 0 && idx < SATELLITE_COUNT) {
        const SatelliteState *s = &st->satellites[idx];
        if (!s->valid || !s->above_horizon) return -1;
        alt = s->alt_rad;
        az  = s->az_rad;
    } else {
        return -1;
    }
    if (alt < 0.0) return -1;
    float sx, sy;
    if (project(sub_w, sub_h, alt, az, &sx, &sy) != 0) return -1;
    int c = (int)(sx / 2.0f);
    int r = (int)(sy / 4.0f);
    if (c < 1) c = 1;
    if (r < 1) r = 1;
    if (c > cols) c = cols;
    if (r > rows) r = rows;
    *out_col = c;
    *out_row = r;
    return 0;
}

void astro_track_arm(AstroState *st, int cols, int rows) {
    int col = st->cursor_col;
    int row = st->cursor_row;
    if (col == 0 && row == 0) {
        col = cols / 2;
        row = rows / 2;
    }
    CursorPick p = find_nearest_target(st, cols, rows, col, row);
    if (p.kind == PICK_NONE) {
        st->track_active = 0;
        return;
    }
    st->track_active = 1;
    st->track_kind   = (int)p.kind;
    st->track_idx    = p.idx;
}

void astro_track_tick(AstroState *st, int cols, int rows) {
    if (!st->track_active) return;
    int c, r;
    if (track_cell_for(st, cols, rows,
                       st->track_kind, st->track_idx, &c, &r) != 0) {
        st->track_active = 0;       /* body dropped below horizon */
        return;
    }
    st->cursor_col = c;
    st->cursor_row = r;
}

void astro_labels(const AstroState *st, FILE *out, int cols, int rows) {
    int chr = p_byte(g_palette.hud.r * 0.7f);
    int chg = p_byte(g_palette.hud.g * 0.7f);
    int chb = p_byte(g_palette.hud.b * 0.7f);
    fprintf(out, "\x1b[38;2;%d;%d;%dm", chr, chg, chb);

    /* Heliocentric: label every body next to its top-down position.
     * Skip the cardinal/horizon machinery — neither concept applies. */
    if (st->view_mode == 1) {
        const struct { EphemBody b; const char *name; } helio_bodies[] = {
            { EPHEM_SUN,     "SUN" },
            { EPHEM_MERCURY, "MER" },
            { EPHEM_VENUS,   "VEN" },
            { EPHEM_MARS,    "MAR" },
            { EPHEM_JUPITER, "JUP" },
            { EPHEM_SATURN,  "SAT" },
            { EPHEM_URANUS,  "URA" },
            { EPHEM_NEPTUNE, "NEP" },
        };
        /* Earth gets its own label slot. */
        int ecol = 0, erow = 0;
        helio_cell_for(st, cols, rows, EPHEM_SUN, &ecol, &erow);
        for (size_t i = 0; i < sizeof helio_bodies / sizeof helio_bodies[0]; i++) {
            int col = 0, row = 0;
            helio_cell_for(st, cols, rows, helio_bodies[i].b, &col, &row);
            int lx = col + 2;
            if (lx < 1 || lx >= cols - 4 || row < 1 || row > rows) continue;
            fprintf(out, "\x1b[%d;%dH%s", row, lx, helio_bodies[i].name);
        }
        /* Earth: same approach but explicit since it isn't an EphemBody. */
        {
            float cx = (float)cols, cy = (float)rows;        /* tmp */
            (void)cx; (void)cy;
            int sub_w = cols * 2, sub_h = rows * 4;
            float scx = (float)sub_w * 0.5f;
            float scy = (float)sub_h * 0.5f;
            float r_max = fminf(scx, scy * 0.5f) * 0.85f;
            float r_scale = r_max / (float)sqrt(30.5);
            double x, y, z;
            ephem_earth_helio_xyz(st->jd, &x, &y, &z);
            float sx, sy;
            helio_map_au(scx, scy, r_scale, x, y, &sx, &sy);
            int col = (int)(sx / 2.0f) + 2;
            int row = (int)(sy / 4.0f);
            if (col >= 1 && col < cols - 4 && row >= 1 && row <= rows) {
                fprintf(out, "\x1b[%d;%dHEAR", row, col);
            }
        }
        fputs("\x1b[0m", out);
        return;
    }

    /* Geocentric — cardinal markers + body labels. */
    const struct { const char *glyph; double az_deg; } cardinals[] = {
        { "N", 0.0 }, { "E", 90.0 }, { "S", 180.0 }, { "W", 270.0 },
    };
    for (size_t c = 0; c < sizeof cardinals / sizeof cardinals[0]; c++) {
        float sx, sy;
        if (project(cols * 2, rows * 4,
                    2.0 * DEG2RAD,                   /* nudge inside horizon */
                    cardinals[c].az_deg * DEG2RAD,
                    &sx, &sy) != 0) continue;
        int cx = (int)(sx / 2.0f);
        int cy = (int)(sy / 4.0f);
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;
        if (cx > cols) cx = cols;
        if (cy > rows) cy = rows;
        fprintf(out, "\x1b[%d;%dH%s", cy, cx, cardinals[c].glyph);
    }

    for (int i = 0; i < EPHEM_COUNT; i++) {
        const EphemPosition *p = &st->pos[i];
        if (p->alt_rad < 0.0) continue;

        float sx, sy;
        if (project(cols * 2, rows * 4, p->alt_rad, p->az_rad, &sx, &sy) != 0)
            continue;
        int cx = (int)(sx / 2.0f) + 2;
        int cy = (int)(sy / 4.0f);
        if (cx < 1 || cy < 1 || cx >= cols - 8 || cy >= rows - 1) continue;
        /* Reserve top + bottom-right corners for HUD. */
        if (cy <= 1 && cx >= cols - 28)             continue;
        if (cy >= rows - 5 && cx >= cols - 16)      continue;

        /* Fade by altitude — lower bodies dim out toward the horizon. */
        float k = (float)(p->alt_rad / (M_PI * 0.5));
        if (k < 0.4f) k = 0.4f;
        fprintf(out, "\x1b[38;2;%d;%d;%dm",
                p_byte(g_palette.hud.r * k),
                p_byte(g_palette.hud.g * k),
                p_byte(g_palette.hud.b * k));
        fprintf(out, "\x1b[%d;%dH%s", cy, cx, ephem_short((EphemBody)i));
    }

    /* Satellite labels — compact 3-4 char names next to each visible
     * non-stale satellite. Shares the planet-label corner-reservation
     * rules so HUD widgets aren't trampled. Hidden when the overlay
     * is toggled off. */
    if (st->show_satellites) {
        for (int i = 0; i < SATELLITE_COUNT; i++) {
            const SatelliteState *s = &st->satellites[i];
            if (!s->valid || !s->above_horizon) continue;
            double age = fabs(st->jd - satellite_epoch_jd(i));
            if (age > 14.0) continue;

            float sx, sy;
            if (project(cols * 2, rows * 4, s->alt_rad, s->az_rad, &sx, &sy) != 0)
                continue;
            int cx = (int)(sx / 2.0f) + 2;
            int cy = (int)(sy / 4.0f);
            if (cx < 1 || cy < 1 || cx >= cols - 6 || cy >= rows - 1) continue;
            if (cy <= 1 && cx >= cols - 28)             continue;
            if (cy >= rows - 5 && cx >= cols - 16)      continue;

            /* Fade by altitude like planets, plus a half-dim past 7d. */
            float k = (float)(s->alt_rad / (M_PI * 0.5));
            if (k < 0.5f) k = 0.5f;
            if (age > 7.0) k *= 0.5f;
            fprintf(out, "\x1b[38;2;%d;%d;%dm",
                    p_byte(g_palette.hud.r * k),
                    p_byte(g_palette.hud.g * k),
                    p_byte(g_palette.hud.b * k));
            fprintf(out, "\x1b[%d;%dH%s", cy, cx, satellite_short_name(i));
        }
    }

    /* Cursor reticle in cursor-mode. Drawn last so it sits over labels.
     * Uses HUD palette at full intensity. */
    if (st->cursor_active) {
        int cx = st->cursor_col;
        int cy = st->cursor_row;
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;
        if (cx > cols) cx = cols;
        if (cy > rows) cy = rows;
        fprintf(out, "\x1b[38;2;%d;%d;%dm",
                p_byte(g_palette.hud.r),
                p_byte(g_palette.hud.g),
                p_byte(g_palette.hud.b));
        /* Crosshair: '-' on left/right, '|' above/below, '+' centre. */
        if (cx > 1)        fprintf(out, "\x1b[%d;%dH-", cy, cx - 1);
        if (cx < cols)     fprintf(out, "\x1b[%d;%dH-", cy, cx + 1);
        if (cy > 1)        fprintf(out, "\x1b[%d;%dH|", cy - 1, cx);
        if (cy < rows)     fprintf(out, "\x1b[%d;%dH|", cy + 1, cx);
        fprintf(out, "\x1b[%d;%dH+", cy, cx);
    }
    fputs("\x1b[0m", out);
}

const char *astro_moon_phase_name(double elongation_rad) {
    /* Wrap to [0, 2π) and bin into the standard 8 phases with conventional
     * 45° windows centred on the cardinal phases. */
    double e = elongation_rad;
    while (e < 0.0)         e += 2.0 * M_PI;
    while (e >= 2.0 * M_PI) e -= 2.0 * M_PI;
    double octant = e * (4.0 / M_PI);   /* [0, 8) */
    int idx = ((int)(octant + 0.5)) & 7;
    static const char *names[8] = {
        "New", "Waxing Crescent", "First Quarter", "Waxing Gibbous",
        "Full", "Waning Gibbous", "Last Quarter", "Waning Crescent",
    };
    return names[idx];
}

double astro_moon_age_days(double elongation_rad) {
    double e = elongation_rad;
    while (e < 0.0)         e += 2.0 * M_PI;
    while (e >= 2.0 * M_PI) e -= 2.0 * M_PI;
    return (e / (2.0 * M_PI)) * 29.530588853;
}

void astro_hud(const AstroState *st, FILE *out, int cols, int rows, double t,
               double speed, double offset_s) {
    if (cols < 40 || rows < 8) return;

    int hr = p_byte(g_palette.hud.r);
    int hg = p_byte(g_palette.hud.g);
    int hb = p_byte(g_palette.hud.b);
    fprintf(out, "\x1b[38;2;%d;%d;%dm", hr, hg, hb);

    /* Top-left: location/time. Geocentric shows lat/lon + LST; heliocentric
     * shows the JD and a "HELIO" tag — observer-frame coordinates don't
     * apply when we're rendering the solar system from above. */
    if (st->view_mode == 1) {
        fprintf(out, "\x1b[1;1HVW  HELIO  jd=%.4f", st->jd);
    } else {
        double lat_deg = st->observer.lat_rad * RAD2DEG;
        double lon_deg = st->observer.lon_rad * RAD2DEG;
        int lst_h = (int)st->lst_hours;
        int lst_m = (int)((st->lst_hours - lst_h) * 60.0);
        fprintf(out, "\x1b[1;1HVW  %+06.2f\xC2\xB0  %+07.2f\xC2\xB0"
                     "  LST %02d:%02d", lat_deg, lon_deg, lst_h, lst_m);
    }

    /* Second line shows clock state when off real-time. Negative speed isn't
     * supported yet; sub-1 speeds read as "1/N×" for legibility. */
    if (speed != 1.0 || offset_s != 0.0) {
        char speed_buf[16];
        if (speed >= 1.0) snprintf(speed_buf, sizeof speed_buf, "%gx", speed);
        else              snprintf(speed_buf, sizeof speed_buf, "1/%gx", 1.0 / speed);
        long offs = (long)offset_s;
        long sign = offs < 0 ? -1 : 1;
        long aoff = offs < 0 ? -offs : offs;
        long oh = aoff / 3600;
        long om = (aoff / 60) % 60;
        fprintf(out, "\x1b[2;1H  speed %s  scrub %c%ldh%02ldm",
                speed_buf, sign < 0 ? '-' : '+', oh, om);
    }

    /* Live eclipse magnitude — only printed during the eclipse window.
     * Sits on row 3 so it doesn't clobber the speed/scrub line. */
    if (st->view_mode != 1) {
        double sf = solar_eclipse_factor(st);
        double lf = lunar_eclipse_factor(st);
        if (sf > 0.05) {
            fprintf(out, "\x1b[3;1H  ECLIPSE solar %.2f", sf);
        } else if (lf > 0.05) {
            fprintf(out, "\x1b[3;1H  ECLIPSE lunar %.2f", lf);
        }
    }

    /* Search prompt at the bottom row when active. Vim-style: '/' opens,
     * keystrokes append, Enter triggers, Esc cancels. */
    if (st->search_active) {
        fprintf(out, "\x1b[%d;1H/ %s_", rows, st->search_buf);
    }

    /* Top-right: rotating panel through above-horizon bodies, dwell 6s. */
    int visible[EPHEM_COUNT];
    int n_vis = 0;
    for (int i = 0; i < EPHEM_COUNT; i++) {
        if (st->pos[i].alt_rad >= 0.0) visible[n_vis++] = i;
    }
    /* Cursor mode: dispatch on whatever's nearest the reticle. */
    CursorPick pick = { PICK_NONE, -1 };
    if (st->cursor_active) {
        pick = find_nearest_target(st, cols, rows,
                                   st->cursor_col, st->cursor_row);
    }

    if (pick.kind == PICK_COMET) {
        const CometState     *c  = &st->comets[pick.idx];
        const CometElements  *ce = &comet_elements[pick.idx];
        int x0 = cols - 32 + 1;
        fprintf(out, "\x1b[1;%dH[ %-22s ]", x0, ce->name);
        fprintf(out, "\x1b[2;%dH  alt %3.0f\xC2\xB0  az %3.0f\xC2\xB0",
                x0, c->alt_rad * RAD2DEG, c->az_rad * RAD2DEG);
        fprintf(out, "\x1b[3;%dH  Mag  %+5.2f", x0, c->mag);
        fprintf(out, "\x1b[4;%dH  Dist %5.2f AU", x0, c->dist_au);
        fprintf(out, "\x1b[5;%dH  r    %5.2f AU", x0, c->r_helio_au);
    }
    else if (pick.kind == PICK_ASTEROID) {
        const AsteroidState    *a  = &st->asteroids[pick.idx];
        const AsteroidElements *ae = &asteroid_elements[pick.idx];
        int x0 = cols - 32 + 1;
        fprintf(out, "\x1b[1;%dH[ %-22s ]", x0, ae->name);
        fprintf(out, "\x1b[2;%dH  alt %3.0f\xC2\xB0  az %3.0f\xC2\xB0",
                x0, a->alt_rad * RAD2DEG, a->az_rad * RAD2DEG);
        fprintf(out, "\x1b[3;%dH  Mag  %+5.2f", x0, a->mag);
        fprintf(out, "\x1b[4;%dH  Dist %5.2f AU", x0, a->dist_au);
        fprintf(out, "\x1b[5;%dH  r    %5.2f AU", x0, a->r_helio_au);
    }
    else if (n_vis == 0) {
        fprintf(out, "\x1b[1;%dH[ NO BODIES ABOVE HORIZON ]", cols - 30);
    } else {
        int sel = visible[((int)(t / 6.0)) % n_vis];
        if (pick.kind == PICK_PLANET) sel = pick.idx;
        const EphemPosition *p = &st->pos[sel];
        int x0 = cols - 32 + 1;
        fprintf(out, "\x1b[1;%dH[ %-9s  %3.0f\xC2\xB0 alt %3.0f\xC2\xB0 az ]",
                x0, ephem_name((EphemBody)sel),
                p->alt_rad * RAD2DEG, p->az_rad * RAD2DEG);
        if (st->view_mode == 1) {
            /* Helio panel: RA/Dec are observer-frame, useless here.
             * Surface heliocentric distance + orbital period progress
             * (mean anomaly fraction) instead. */
            double xh = 0, yh = 0, zh = 0;
            ephem_helio_xyz_for((EphemBody)sel, st->jd, &xh, &yh, &zh);
            double r_helio = sqrt(xh*xh + yh*yh + zh*zh);
            /* Period in years from semi-major a (a^1.5); voidwatch
             * doesn't expose `a` directly so estimate from distance. */
            double period_yr = 0.0;
            for (int k = 0; k < helio_orbit_count; k++) {
                if (helio_orbits[k].body == (EphemBody)sel) {
                    period_yr = helio_orbits[k].period_days / 365.25;
                    break;
                }
            }
            fprintf(out, "\x1b[2;%dH  r    %6.3f AU", x0, r_helio);
            fprintf(out, "\x1b[3;%dH  Dist %6.3f AU", x0, p->distance_au);
            fprintf(out, "\x1b[4;%dH  Mag  %+5.2f", x0, p->magnitude);
            if (period_yr > 0)
                fprintf(out, "\x1b[5;%dH  P    %5.2f yr", x0, period_yr);
        } else {
            fprintf(out, "\x1b[2;%dH  RA   %02d^h%02d^m",
                    x0, (int)(p->ra_rad * RAD2DEG / 15.0),
                    (int)(fmod(p->ra_rad * RAD2DEG / 15.0, 1.0) * 60.0));
            fprintf(out, "\x1b[3;%dH  Dec  %+05.1f\xC2\xB0",
                    x0, p->dec_rad * RAD2DEG);
            fprintf(out, "\x1b[4;%dH  Mag  %+5.2f", x0, p->magnitude);
            fprintf(out, "\x1b[5;%dH  Dist %5.2f AU", x0, p->distance_au);
            if (sel == EPHEM_MOON) {
                fprintf(out, "\x1b[6;%dH  %-15s %3.0f%%",
                        x0,
                        astro_moon_phase_name(st->moon_elongation),
                        st->moon_illum * 100.0);
                fprintf(out, "\x1b[7;%dH  age %4.1fd",
                        x0, astro_moon_age_days(st->moon_elongation));
            }
        }
    }
    fputs("\x1b[0m", out);
}
