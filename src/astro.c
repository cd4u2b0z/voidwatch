#include <math.h>
#include <stdio.h>
#include <time.h>

#include "astro.h"
#include "framebuffer.h"
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
     * scales with screen length. */
    Color tint = { 0.45f, 0.55f, 0.70f };  /* dim cool cyan */
    float intensity = 0.10f;

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
    float box = sigma * 3.0f;
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
        float box = sigma * 3.0f;
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
    float box = sigma * 3.0f;

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
} MeteorShower;

static const MeteorShower meteor_showers[] = {
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
static const int meteor_shower_count =
    (int)(sizeof meteor_showers / sizeof meteor_showers[0]);

#define METEOR_POOL 96

typedef struct {
    float sx, sy;
    float vx, vy;
    float life, max_life;
    float intensity;
    int   active;
} Meteor;

static Meteor   met_pool[METEOR_POOL];
static double   met_spawn_accum = 0.0;
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
        float speed = 60.0f + met_randf() * 80.0f;
        m->vx = cosf(ang) * speed;
        m->vy = sinf(ang) * speed;
        m->max_life  = 0.4f + met_randf() * 0.4f;
        m->life      = m->max_life;
        m->intensity = 0.7f + met_randf() * 0.6f;
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
            fb_add(fb, x, y, k, k * 0.95f, k * 0.85f);
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

void astro_draw(const AstroState *st, Framebuffer *fb,
                int cols, int rows, const AudioSnapshot *snap) {
    (void)cols; (void)rows; (void)snap;

    /* Twilight horizon glow — only when Sun is in the twilight bands.
     * Drawn first so MW + stars overdraw it cleanly with fb_max. */
    twilight_draw(st, fb);

    /* Diffuse Milky Way wash sits behind the bodies — fb_max doesn't
     * accumulate, so re-stamping each frame is fine. */
    milkyway_draw(st, fb);

    /* Optional alt-az grid sits behind the constellations + stars. */
    if (st->show_grid) grid_draw(st, fb);

    /* Constellation stick figures *before* stars so brighter star stamps
     * cleanly overdraw line dots near the line endpoints. */
    constellations_draw(st, fb);
    stars_draw(st, fb);

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
    return best;
}

void astro_labels(const AstroState *st, FILE *out, int cols, int rows) {
    /* Cardinal markers — placed just *inside* the horizon ring (alt slightly
     * above 0) so glyphs aren't clipped by the projection edge. */
    const struct { const char *glyph; double az_deg; } cardinals[] = {
        { "N", 0.0 }, { "E", 90.0 }, { "S", 180.0 }, { "W", 270.0 },
    };
    int chr = p_byte(g_palette.hud.r * 0.7f);
    int chg = p_byte(g_palette.hud.g * 0.7f);
    int chb = p_byte(g_palette.hud.b * 0.7f);
    fprintf(out, "\x1b[38;2;%d;%d;%dm", chr, chg, chb);
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

    /* Top-left: lat/lon + LST. */
    double lat_deg = st->observer.lat_rad * RAD2DEG;
    double lon_deg = st->observer.lon_rad * RAD2DEG;
    int lst_h = (int)st->lst_hours;
    int lst_m = (int)((st->lst_hours - lst_h) * 60.0);
    fprintf(out, "\x1b[1;1HVW  %+06.2f\xC2\xB0  %+07.2f\xC2\xB0"
                 "  LST %02d:%02d", lat_deg, lon_deg, lst_h, lst_m);

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
    fputs("\x1b[0m", out);
}
