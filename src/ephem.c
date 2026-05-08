#include <math.h>
#include <stddef.h>
#include <string.h>

#include "ephem.h"

/* M_PI is POSIX/glibc, not C11. _POSIX_C_SOURCE doesn't expose it on all
 * libcs, so define it locally. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* All Meeus formulas come from "Astronomical Algorithms" 2e (J. Meeus, 1998).
 * Chapter references in comments below. */

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)
#define J2000   2451545.0

static double wrap_deg(double d) {
    d = fmod(d, 360.0);
    if (d < 0.0) d += 360.0;
    return d;
}

static double wrap_2pi(double r) {
    r = fmod(r, 2.0 * M_PI);
    if (r < 0.0) r += 2.0 * M_PI;
    return r;
}

double ephem_julian_day_from_unix(time_t t) {
    /* Unix epoch 1970-01-01 00:00 UTC = JD 2440587.5 */
    return (double)t / 86400.0 + 2440587.5;
}

/* Mean obliquity of the ecliptic (Meeus 22.2). */
static double mean_obliquity_rad(double T) {
    double eps0 =  23.43929111
                 - (46.8150     / 3600.0) * T
                 - ( 0.00059    / 3600.0) * T * T
                 + ( 0.001813   / 3600.0) * T * T * T;
    return eps0 * DEG2RAD;
}

/* Convert ecliptic (λ, β) to equatorial (α, δ). All radians. */
static void ecl_to_eq(double lam, double beta, double eps,
                      double *ra, double *dec) {
    double sin_b  = sin(beta);
    double cos_b  = cos(beta);
    double sin_l  = sin(lam);
    double cos_l  = cos(lam);
    double sin_e  = sin(eps);
    double cos_e  = cos(eps);

    double y = sin_l * cos_e - tan(beta) * sin_e;
    double x = cos_l;
    *ra  = wrap_2pi(atan2(y, x));
    /* Use direct formula via β if it's nonzero; for Sun β=0 we still get
     * the right thing because tan(0)=0. */
    (void)sin_b; (void)cos_b;
    *dec = asin(sin(beta) * cos_e + cos(beta) * sin_e * sin_l);
}

/* ===== Sun (Meeus Ch. 25, low-precision ~0.01°) ===================== */

static void compute_sun(double jd, EphemPosition *out) {
    double T = (jd - J2000) / 36525.0;

    double L0 = wrap_deg(280.46646 + 36000.76983 * T + 0.0003032 * T * T);
    double M  = wrap_deg(357.52911 + 35999.05029 * T - 0.0001537 * T * T);
    double e  = 0.016708634 - 0.000042037 * T - 0.0000001267 * T * T;

    double Mr = M * DEG2RAD;
    double C  = (1.914602 - 0.004817 * T - 0.000014 * T * T) * sin(Mr)
              + (0.019993 - 0.000101 * T)                    * sin(2.0 * Mr)
              +  0.000289                                    * sin(3.0 * Mr);

    double true_long = L0 + C;
    double true_anom = M  + C;
    double R = 1.000001018 * (1.0 - e * e)
             / (1.0 + e * cos(true_anom * DEG2RAD));

    /* Apparent (with aberration + nutation in longitude). */
    double Omega = 125.04 - 1934.136 * T;          /* deg */
    double lambda_deg = true_long - 0.00569 - 0.00478 * sin(Omega * DEG2RAD);
    double beta = 0.0;

    /* True obliquity (mean + dominant nutation term). */
    double eps = mean_obliquity_rad(T) + (0.00256 * DEG2RAD) * cos(Omega * DEG2RAD);

    double ra, dec;
    ecl_to_eq(lambda_deg * DEG2RAD, beta, eps, &ra, &dec);

    out->ra_rad      = ra;
    out->dec_rad     = dec;
    out->distance_au = R;
    out->magnitude   = -26.74;
}

/* ===== Moon (Meeus Ch. 47, truncated to dominant terms ~0.5°) ======= */

static void compute_moon(double jd, EphemPosition *out) {
    double T = (jd - J2000) / 36525.0;

    /* Mean longitude, mean elongation, sun anomaly, moon anomaly,
     * argument of latitude — all degrees. */
    double Lp = wrap_deg(218.3164477 + 481267.88123421 * T
                         - 0.0015786 * T*T + T*T*T/538841.0);
    double D  = wrap_deg(297.8501921 + 445267.1114034  * T
                         - 0.0018819 * T*T + T*T*T/545868.0);
    double M  = wrap_deg(357.5291092 +  35999.0502909  * T
                         - 0.0001536 * T*T);
    double Mp = wrap_deg(134.9633964 + 477198.8675055  * T
                         + 0.0087414 * T*T + T*T*T/69699.0);
    double F  = wrap_deg( 93.2720950 + 483202.0175233  * T
                         - 0.0036539 * T*T - T*T*T/3526000.0);

    double E  = 1.0 - 0.002516 * T - 0.0000074 * T*T;
    double E2 = E * E;

    double Dr = D * DEG2RAD;
    double Mr = M * DEG2RAD;
    double Mpr = Mp * DEG2RAD;
    double Fr = F * DEG2RAD;

    /* Σl in 1e-6 degrees — the largest 14 terms. */
    double Sl = 0.0;
    Sl += 6288774.0 * sin(Mpr);
    Sl += 1274027.0 * sin(2.0*Dr - Mpr);
    Sl +=  658314.0 * sin(2.0*Dr);
    Sl +=  213618.0 * sin(2.0*Mpr);
    Sl += -185116.0 * sin(Mr)               * E;
    Sl += -114332.0 * sin(2.0*Fr);
    Sl +=   58793.0 * sin(2.0*Dr - 2.0*Mpr);
    Sl +=   57066.0 * sin(2.0*Dr - Mr - Mpr) * E;
    Sl +=   53322.0 * sin(2.0*Dr + Mpr);
    Sl +=   45758.0 * sin(2.0*Dr - Mr)       * E;
    Sl +=  -40923.0 * sin(Mr - Mpr)          * E;
    Sl +=  -34720.0 * sin(Dr);
    Sl +=  -30383.0 * sin(Mr + Mpr)          * E;
    Sl +=   15327.0 * sin(2.0*Dr - 2.0*Fr);

    /* Σb in 1e-6 degrees — largest 8 terms. */
    double Sb = 0.0;
    Sb += 5128122.0 * sin(Fr);
    Sb +=  280602.0 * sin(Mpr + Fr);
    Sb +=  277693.0 * sin(Mpr - Fr);
    Sb +=  173237.0 * sin(2.0*Dr - Fr);
    Sb +=   55413.0 * sin(2.0*Dr - Mpr + Fr);
    Sb +=   46271.0 * sin(2.0*Dr - Mpr - Fr);
    Sb +=   32573.0 * sin(2.0*Dr + Fr);
    Sb +=   17198.0 * sin(2.0*Mpr + Fr);

    /* Σr in 1e-3 km — largest 6 terms. */
    double Sr = 0.0;
    Sr += -20905355.0 * cos(Mpr);
    Sr +=  -3699111.0 * cos(2.0*Dr - Mpr);
    Sr +=  -2955968.0 * cos(2.0*Dr);
    Sr +=   -569925.0 * cos(2.0*Mpr);
    Sr +=     48888.0 * cos(Mr)               * E;
    Sr +=     -3149.0 * cos(2.0*Fr);
    (void)E2;

    double lambda_deg = Lp + Sl / 1e6;
    double beta_deg   =      Sb / 1e6;
    double dist_km    = 385000.56 + Sr / 1e3;

    double Omega = 125.04 - 1934.136 * T;
    double eps = mean_obliquity_rad(T)
               + (0.00256 * DEG2RAD) * cos(Omega * DEG2RAD);

    double ra, dec;
    ecl_to_eq(lambda_deg * DEG2RAD, beta_deg * DEG2RAD, eps, &ra, &dec);

    out->ra_rad      = ra;
    out->dec_rad     = dec;
    out->distance_au = dist_km / 149597870.7;
    /* Phase-aware mag is involved (Meeus 48); use a plausible average. */
    out->magnitude   = -10.0;
}

/* ===== Planets (Standish elements, JPL "Approximate Positions") ===== */
/* J2000 elements + linear rates per Julian century; valid 1800-2050. */

typedef struct {
    double a, e, i, Omega, varpi, L;     /* values at J2000     */
    double da, de, di, dOmega, dvarpi, dL; /* rates per century */
    double mag_h0;                       /* base abs mag (H₀)   */
} OrbitalElements;

/* All angles in degrees, distances in AU. */
static const OrbitalElements ELEMS[] = {
    /* Mercury */ { 0.38709927,  0.20563593, 7.00497902, 48.33076593,  77.45779628, 252.25032350,
                    0.00000037,  0.00001906,-0.00594749, -0.12534081,   0.16047689, 149472.67411175,
                   -0.42 },
    /* Venus   */ { 0.72333566,  0.00677672, 3.39467605, 76.67984255, 131.60246718, 181.97909950,
                    0.00000390, -0.00004107,-0.00078890, -0.27769418,   0.00268329,  58517.81538729,
                   -4.40 },
    /* Mars    */ { 1.52371034,  0.09339410, 1.84969142, 49.55953891, -23.94362959,  -4.55343205,
                    0.00001847,  0.00007882,-0.00813131, -0.29257343,   0.44441088,  19140.30268499,
                   -1.52 },
    /* Jupiter */ { 5.20288700,  0.04838624, 1.30439695,100.47390909,  14.72847983,  34.39644051,
                   -0.00011607, -0.00013253,-0.00183714,  0.20469106,   0.21252668,   3034.74612775,
                   -9.40 },
    /* Saturn  */ { 9.53667594,  0.05386179, 2.48599187,113.66242448,  92.59887831,  49.95424423,
                   -0.00125060, -0.00050991, 0.00193609, -0.28867794,  -0.41897216,   1222.49362201,
                   -8.88 },
    /* Uranus  */ { 19.18916464, 0.04725744, 0.77263783, 74.01692503, 170.95427630, 313.23810451,
                   -0.00196176, -0.00004397,-0.00242939,  0.04240589,   0.40805281,    428.48202785,
                   -7.19 },
    /* Neptune */ { 30.06992276, 0.00859048, 1.77004347,131.78422574,  44.96476227, -55.12002969,
                    0.00026291,  0.00005105, 0.00035372, -0.00508664,  -0.32241464,    218.45945325,
                   -6.87 },
};

/* Earth elements (separate — used for geocentric correction). */
static const OrbitalElements EARTH = {
    1.00000261,  0.01671123, -0.00001531,   0.0,         102.93768193, 100.46457166,
    0.00000562, -0.00004392, -0.01294668,   0.0,           0.32327364,  35999.37244981,
    0.0
};

/* Solve Kepler: M = E - e*sin(E) for E. M, E in radians. */
static double solve_kepler(double M, double e) {
    double E = M;
    for (int i = 0; i < 8; i++) {
        double dE = (E - e * sin(E) - M) / (1.0 - e * cos(E));
        E -= dE;
        if (fabs(dE) < 1e-9) break;
    }
    return E;
}

/* Heliocentric ecliptic xyz of a planet at JD. */
static void helio_xyz(const OrbitalElements *o, double jd,
                      double *x, double *y, double *z) {
    double T = (jd - J2000) / 36525.0;

    double a     = o->a     + o->da     * T;
    double e     = o->e     + o->de     * T;
    double inc   = (o->i    + o->di     * T) * DEG2RAD;
    double Omega = (o->Omega+ o->dOmega * T) * DEG2RAD;
    double varpi = (o->varpi+ o->dvarpi * T) * DEG2RAD;
    double L     = (o->L    + o->dL     * T) * DEG2RAD;

    double M = wrap_2pi(L - varpi);
    double E = solve_kepler(M, e);
    double omega = varpi - Omega;     /* argument of perihelion */

    /* Position in orbital plane. */
    double x_orb = a * (cos(E) - e);
    double y_orb = a * sqrt(1.0 - e * e) * sin(E);

    /* Rotate orbital plane → ecliptic frame:
     *   1. by argument of perihelion (ω) about z
     *   2. by inclination (i)         about x
     *   3. by ascending node (Ω)      about z
     */
    double co = cos(omega), so = sin(omega);
    double cO = cos(Omega), sO = sin(Omega);
    double ci = cos(inc),   si = sin(inc);

    double xp = co * x_orb - so * y_orb;
    double yp = so * x_orb + co * y_orb;

    *x = cO * xp - sO * (yp * ci);
    *y = sO * xp + cO * (yp * ci);
    *z = yp * si;
}

static void compute_planet(EphemBody body, double jd, EphemPosition *out) {
    int idx = -1;
    switch (body) {
        case EPHEM_MERCURY: idx = 0; break;
        case EPHEM_VENUS:   idx = 1; break;
        case EPHEM_MARS:    idx = 2; break;
        case EPHEM_JUPITER: idx = 3; break;
        case EPHEM_SATURN:  idx = 4; break;
        case EPHEM_URANUS:  idx = 5; break;
        case EPHEM_NEPTUNE: idx = 6; break;
        default:                    break;
    }
    if (idx < 0) {
        memset(out, 0, sizeof *out);
        return;
    }

    double px, py, pz, ex, ey, ez;
    helio_xyz(&ELEMS[idx], jd, &px, &py, &pz);
    helio_xyz(&EARTH,      jd, &ex, &ey, &ez);

    double dx = px - ex, dy = py - ey, dz = pz - ez;
    double delta = sqrt(dx*dx + dy*dy + dz*dz);     /* Earth-planet AU  */
    double r     = sqrt(px*px + py*py + pz*pz);     /* Sun-planet AU    */

    double lambda = atan2(dy, dx);
    double beta   = atan2(dz, sqrt(dx*dx + dy*dy));

    double T   = (jd - J2000) / 36525.0;
    double eps = mean_obliquity_rad(T);
    double ra, dec;
    ecl_to_eq(wrap_2pi(lambda), beta, eps, &ra, &dec);

    out->ra_rad      = ra;
    out->dec_rad     = dec;
    out->distance_au = delta;

    /* Apparent magnitude — H + 5 log10(r·Δ). Phase-angle correction skipped
     * (relevant only for inner planets; cell scale won't notice). */
    if (r > 0.0 && delta > 0.0) {
        out->magnitude = ELEMS[idx].mag_h0 + 5.0 * log10(r * delta);
    } else {
        out->magnitude = ELEMS[idx].mag_h0;
    }
}

/* ===== Public dispatch ============================================== */

void ephem_compute(EphemBody body, double jd, EphemPosition *out) {
    memset(out, 0, sizeof *out);
    switch (body) {
        case EPHEM_SUN:  compute_sun (jd, out); break;
        case EPHEM_MOON: compute_moon(jd, out); break;
        default:         compute_planet(body, jd, out); break;
    }
}

void ephem_earth_helio_xyz(double jd, double *x, double *y, double *z) {
    helio_xyz(&EARTH, jd, x, y, z);
}

void ephem_helio_xyz_for(EphemBody body, double jd,
                         double *x, double *y, double *z) {
    int idx = -1;
    switch (body) {
        case EPHEM_SUN:     *x = 0; *y = 0; *z = 0; return;
        case EPHEM_MERCURY: idx = 0; break;
        case EPHEM_VENUS:   idx = 1; break;
        case EPHEM_MARS:    idx = 2; break;
        case EPHEM_JUPITER: idx = 3; break;
        case EPHEM_SATURN:  idx = 4; break;
        case EPHEM_URANUS:  idx = 5; break;
        case EPHEM_NEPTUNE: idx = 6; break;
        default: *x = 0; *y = 0; *z = 0; return;
    }
    helio_xyz(&ELEMS[idx], jd, x, y, z);
}

double ephem_obliquity_rad(double jd) {
    double T = (jd - J2000) / 36525.0;
    return mean_obliquity_rad(T);
}

double ephem_local_sidereal_hours(double jd, double lon_east_rad) {
    /* GMST formula from Meeus 12.4 (good ~1 arcsec). */
    double T = (jd - J2000) / 36525.0;
    double gmst_deg = 280.46061837
                    + 360.98564736629 * (jd - J2000)
                    +   0.000387933   * T * T
                    -                  T * T * T / 38710000.0;
    double lst_deg  = wrap_deg(gmst_deg + lon_east_rad * RAD2DEG);
    return lst_deg / 15.0;
}

void ephem_to_topocentric(EphemPosition *pos, const Observer *obs, double jd) {
    double lst_h   = ephem_local_sidereal_hours(jd, obs->lon_rad);
    double lst_rad = lst_h * 15.0 * DEG2RAD;
    double H = lst_rad - pos->ra_rad;          /* hour angle */

    double sin_lat = sin(obs->lat_rad);
    double cos_lat = cos(obs->lat_rad);
    double sin_dec = sin(pos->dec_rad);
    double cos_dec = cos(pos->dec_rad);

    double sin_alt = sin_lat * sin_dec + cos_lat * cos_dec * cos(H);
    double alt = asin(sin_alt);
    double az  = atan2(-cos_dec * sin(H),
                       sin_dec * cos_lat - cos_dec * sin_lat * cos(H));
    pos->alt_rad = alt;
    pos->az_rad  = wrap_2pi(az);
}

/* Inverse: (alt, az, lat) → (δ, H), then α = LST − H. Derived from the
 * same identities the forward path uses (Meeus 13.5–13.6 inverted). */
void ephem_altaz_to_radec(EphemPosition *pos, const Observer *obs, double jd) {
    double lst_h   = ephem_local_sidereal_hours(jd, obs->lon_rad);
    double lst_rad = lst_h * 15.0 * DEG2RAD;

    double sin_lat = sin(obs->lat_rad);
    double cos_lat = cos(obs->lat_rad);
    double sin_alt = sin(pos->alt_rad);
    double cos_alt = cos(pos->alt_rad);
    double sin_az  = sin(pos->az_rad);
    double cos_az  = cos(pos->az_rad);

    double sin_dec = sin_lat * sin_alt + cos_lat * cos_alt * cos_az;
    if (sin_dec >  1.0) sin_dec =  1.0;
    if (sin_dec < -1.0) sin_dec = -1.0;
    double dec = asin(sin_dec);
    double cos_dec = cos(dec);

    /* H from atan2(sin(H), cos(H)). At exact poles cos_dec → 0; the atan2
     * still gives a defined (degenerate) answer. */
    double sH, cH;
    if (cos_dec < 1e-12) {
        sH = 0.0;
        cH = 1.0;
    } else {
        sH = -sin_az * cos_alt / cos_dec;
        cH = (sin_alt - sin_dec * sin_lat) / (cos_dec * cos_lat);
    }
    double H  = atan2(sH, cH);
    double ra = wrap_2pi(lst_rad - H);

    pos->ra_rad  = ra;
    pos->dec_rad = dec;
}

const char *ephem_name(EphemBody b) {
    switch (b) {
        case EPHEM_SUN:     return "Sun";
        case EPHEM_MOON:    return "Moon";
        case EPHEM_MERCURY: return "Mercury";
        case EPHEM_VENUS:   return "Venus";
        case EPHEM_MARS:    return "Mars";
        case EPHEM_JUPITER: return "Jupiter";
        case EPHEM_SATURN:  return "Saturn";
        case EPHEM_URANUS:  return "Uranus";
        case EPHEM_NEPTUNE: return "Neptune";
        default:            return "?";
    }
}

const char *ephem_short(EphemBody b) {
    switch (b) {
        case EPHEM_SUN:     return "SUN";
        case EPHEM_MOON:    return "MON";
        case EPHEM_MERCURY: return "MER";
        case EPHEM_VENUS:   return "VEN";
        case EPHEM_MARS:    return "MAR";
        case EPHEM_JUPITER: return "JUP";
        case EPHEM_SATURN:  return "SAT";
        case EPHEM_URANUS:  return "URA";
        case EPHEM_NEPTUNE: return "NEP";
        default:            return "???";
    }
}
