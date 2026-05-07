#include <math.h>
#include <stdio.h>

#include "astro.h"
#include "framebuffer.h"
#include "palette.h"
#include "skydata.h"

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

static void radec_to_altaz(double ra, double dec, double lat,
                           double lst_rad, double *alt, double *az) {
    double H = lst_rad - ra;
    double sin_alt = sin(dec) * sin(lat) + cos(dec) * cos(lat) * cos(H);
    if (sin_alt >  1.0) sin_alt =  1.0;
    if (sin_alt < -1.0) sin_alt = -1.0;
    *alt = asin(sin_alt);
    double y = -cos(dec) * sin(H);
    double x =  sin(dec) * cos(lat) - cos(dec) * sin(lat) * cos(H);
    *az = atan2(y, x);
    if (*az < 0.0) *az += 2.0 * M_PI;
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
        /* Slight altitude fade: lower sky reads dimmer (atmospheric stub). */
        float k = (float)(alt / (M_PI * 0.5));
        if (k < 0.3f) k = 0.3f;
        w *= k;

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
                fb_max(fb, x, y, tint.r * v, tint.g * v, tint.b * v);
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
    for (int i = 0; i < sky_stars_count; i++) {
        const SkyStar *s = &sky_stars[i];
        double ra  = s->ra_h * (M_PI / 12.0);
        double dec = s->dec_deg * DEG2RAD;
        float  sx, sy;
        double alt;
        if (project_sky(st, fb, ra, dec, &sx, &sy, &alt) != 0) continue;

        float intensity = mag_to_intensity(s->mag);
        /* Atmospheric extinction stub: dim stars near the horizon. */
        float k = (float)(alt / (M_PI * 0.5));
        if (k < 0.25f) k = 0.25f;
        intensity *= 0.6f + 0.4f * k;

        Color tint = spectral_tint(s->spectral);
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
                       const AstroStyle *s, double elongation) {
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
            fb_add(fb, x, y, r, g, bl);
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

void astro_draw(const AstroState *st, Framebuffer *fb,
                int cols, int rows, const AudioSnapshot *snap) {
    (void)cols; (void)rows; (void)snap;

    /* Diffuse Milky Way wash sits behind the bodies — fb_max doesn't
     * accumulate, so re-stamping each frame is fine. */
    milkyway_draw(st, fb);

    /* Constellation stick figures *before* stars so brighter star stamps
     * cleanly overdraw line dots near the line endpoints. */
    constellations_draw(st, fb);
    stars_draw(st, fb);

    /* Sun position projected unclipped — needed even when below the horizon
     * to orient the lunar terminator on the night side. */
    float sun_sx, sun_sy;
    project_unclipped(fb->sub_w, fb->sub_h,
                      st->pos[EPHEM_SUN].alt_rad,
                      st->pos[EPHEM_SUN].az_rad,
                      &sun_sx, &sun_sy);

    for (int i = 0; i < EPHEM_COUNT; i++) {
        const EphemPosition *p = &st->pos[i];
        float sx, sy;
        if (project(fb->sub_w, fb->sub_h, p->alt_rad, p->az_rad, &sx, &sy) != 0)
            continue;
        AstroStyle s = style_for((EphemBody)i);
        if ((EphemBody)i == EPHEM_MOON) {
            stamp_moon(fb, sx, sy, sun_sx, sun_sy, &s, st->moon_elongation);
        } else {
            stamp_body(fb, sx, sy, &s);
        }
    }
}

static int p_byte(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0)   i = 0;
    if (i > 255) i = 255;
    return i;
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
    if (n_vis == 0) {
        fprintf(out, "\x1b[1;%dH[ NO BODIES ABOVE HORIZON ]", cols - 30);
    } else {
        int sel = visible[((int)(t / 6.0)) % n_vis];
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
