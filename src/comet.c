#include <math.h>

#include "comet.h"
#include "ephem.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* Sources for these elements: JPL Horizons / IAU Minor Planet Center,
 * osculating at the perihelion epoch listed. Two-body propagation drifts
 * over decades but stays cell-scale across the visibility window of each
 * apparition — fine for an observatory that re-runs the math every frame. */
const CometElements comet_elements[] = {
    {
        .name = "1P/Halley",
        .T_jd = 2446470.95,           /* 1986-Feb-09.4 perihelion */
        .q_au = 0.5859, .e = 0.96714,
        .i_deg = 162.262, .Omega_deg = 58.42, .omega_deg = 111.33,
        .period_yr = 75.32,
        .H_mag = 5.5, .n_slope = 8.0,
    },
    {
        .name = "2P/Encke",
        .T_jd = 2460240.16,           /* 2023-Oct-22.66 perihelion */
        .q_au = 0.3360, .e = 0.8483,
        .i_deg = 11.78, .Omega_deg = 334.57, .omega_deg = 186.55,
        .period_yr = 3.30,
        .H_mag = 11.5, .n_slope = 2.0,
    },
    {
        .name = "109P/Swift-Tuttle",
        .T_jd = 2448969.0,            /* 1992-Dec-12 perihelion */
        .q_au = 0.9596, .e = 0.9632,
        .i_deg = 113.45, .Omega_deg = 139.38, .omega_deg = 152.98,
        .period_yr = 133.28,
        .H_mag = 4.0, .n_slope = 4.0,
    },
    {
        .name = "21P/Giacobini-Zinner",
        .T_jd = 2458381.5,            /* 2018-Sep-10 perihelion */
        .q_au = 1.0136, .e = 0.7104,
        .i_deg = 31.99, .Omega_deg = 195.39, .omega_deg = 172.95,
        .period_yr = 6.62,
        .H_mag = 9.0, .n_slope = 2.0,
    },
    {
        .name = "67P/Churyumov-Gerasimenko",
        .T_jd = 2458982.6,            /* 2020-Nov-02 perihelion */
        .q_au = 1.2434, .e = 0.6406,
        .i_deg = 7.04, .Omega_deg = 50.15, .omega_deg = 12.78,
        .period_yr = 6.45,
        .H_mag = 11.0, .n_slope = 4.0,
    },
    {
        .name = "Hale-Bopp",
        .T_jd = 2450537.0,            /* 1997-Apr-01 perihelion */
        .q_au = 0.914, .e = 0.99511,
        .i_deg = 89.43, .Omega_deg = 282.47, .omega_deg = 130.59,
        .period_yr = 0.0,             /* effectively long-period (~2533y) */
        .H_mag = -0.5, .n_slope = 2.0,
    },
};
const int comet_count =
    (int)(sizeof comet_elements / sizeof comet_elements[0]);

/* Catch the COMET_COUNT macro drifting from the table size. */
_Static_assert(sizeof comet_elements / sizeof comet_elements[0] == COMET_COUNT,
               "COMET_COUNT in comet.h is out of sync with comet_elements[]");

/* Newton-Raphson on Kepler's equation. e<1 only — for the bundled comets
 * the maximum is Hale-Bopp at 0.995, which converges in ~6 iterations
 * with this seed. Long-period comets approaching e=1 would need a
 * universal-variables formulation; not in scope. */
static double solve_kepler_ecc(double M, double e) {
    /* Wrap M to (-π, π] for stable convergence near apoapsis. */
    while (M >  M_PI) M -= 2.0 * M_PI;
    while (M < -M_PI) M += 2.0 * M_PI;
    double E = (e < 0.8) ? M : M_PI;
    for (int it = 0; it < 30; it++) {
        double f  = E - e * sin(E) - M;
        double fp = 1.0 - e * cos(E);
        double dE = f / fp;
        E -= dE;
        if (fabs(dE) < 1e-10) break;
    }
    return E;
}

/* Heliocentric ecliptic xyz for one comet at jd. Two-body Kepler. */
static void comet_helio_xyz(const CometElements *c, double jd,
                            double *x, double *y, double *z,
                            double *r_helio) {
    /* Semi-major axis from q + e. */
    double a = c->q_au / (1.0 - c->e);

    /* Mean motion (rad/day). */
    double n_rad_per_day;
    if (c->period_yr > 0.0) {
        n_rad_per_day = 2.0 * M_PI / (c->period_yr * 365.25);
    } else {
        /* Kepler's third law: n = sqrt(mu / a^3); mu_sun ≈ k^2 with k =
         * 0.01720209895 rad/day (Gaussian gravitational constant). */
        const double k = 0.01720209895;
        n_rad_per_day = k / sqrt(a * a * a);
    }

    double M = n_rad_per_day * (jd - c->T_jd);
    double E = solve_kepler_ecc(M, c->e);

    double cos_E = cos(E);
    double sin_E = sin(E);

    /* Position in orbital plane: x toward perihelion, y along motion. */
    double x_orb = a * (cos_E - c->e);
    double y_orb = a * sqrt(1.0 - c->e * c->e) * sin_E;

    /* Rotate orbital plane → ecliptic frame:
     *   1. rotate by ω about z (orbital plane)
     *   2. rotate by i about new-x (tilt out of ecliptic)
     *   3. rotate by Ω about z (twist node line to its longitude)
     */
    double inc   = c->i_deg     * DEG2RAD;
    double Omega = c->Omega_deg * DEG2RAD;
    double omega = c->omega_deg * DEG2RAD;
    double co = cos(omega), so = sin(omega);
    double cO = cos(Omega), sO = sin(Omega);
    double ci = cos(inc),   si = sin(inc);

    double xp = co * x_orb - so * y_orb;
    double yp = so * x_orb + co * y_orb;

    *x = cO * xp - sO * (yp * ci);
    *y = sO * xp + cO * (yp * ci);
    *z = yp * si;

    if (r_helio) *r_helio = sqrt(*x * *x + *y * *y + *z * *z);
}

void comet_compute_all(double jd, CometState *out) {
    double ex, ey, ez;
    ephem_earth_helio_xyz(jd, &ex, &ey, &ez);
    double eps = ephem_obliquity_rad(jd);
    double sin_e = sin(eps);
    double cos_e = cos(eps);

    for (int i = 0; i < comet_count; i++) {
        CometState *s = &out[i];
        const CometElements *c = &comet_elements[i];
        double cx, cy, cz, r;
        comet_helio_xyz(c, jd, &cx, &cy, &cz, &r);

        /* Geocentric ecliptic vector: comet − Earth. */
        double gx = cx - ex;
        double gy = cy - ey;
        double gz = cz - ez;
        double delta = sqrt(gx * gx + gy * gy + gz * gz);

        /* Ecliptic → equatorial: rotate about x by ε. */
        double xeq = gx;
        double yeq = gy * cos_e - gz * sin_e;
        double zeq = gy * sin_e + gz * cos_e;

        double ra  = atan2(yeq, xeq);
        if (ra < 0.0) ra += 2.0 * M_PI;
        double dec = asin(zeq / delta);

        s->ra_rad     = ra;
        s->dec_rad    = dec;
        s->dist_au    = delta;
        s->r_helio_au = r;

        /* Standard cometary apparent magnitude. */
        if (delta > 0.0 && r > 0.0) {
            s->mag = c->H_mag + 5.0 * log10(delta)
                              + 2.5 * c->n_slope * log10(r);
        } else {
            s->mag = c->H_mag;
        }

        /* alt/az filled in by caller via ephem_to_topocentric — we don't
         * have the Observer here. Init to 0. */
        s->alt_rad = 0.0;
        s->az_rad  = 0.0;
        s->valid   = 1;
    }
}
