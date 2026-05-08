#include <math.h>

#include "asteroid.h"
#include "ephem.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG2RAD (M_PI / 180.0)

/* J2000.0 osculating elements (epoch JD 2451545.0). Values rounded to a
 * few arcminutes — well below cell-scale visibility. Sources: JPL Small-
 * Body Database. */
const AsteroidElements asteroid_elements[] = {
    { "1 Ceres",  2.7660, 0.0786, 10.587,  80.41, 73.62, 151.23, 2451545.0, 3.34 },
    { "2 Pallas", 2.7728, 0.2299, 34.834, 172.91, 310.41, 80.61, 2451545.0, 4.13 },
    { "4 Vesta",  2.3617, 0.0892,  7.135, 103.81, 151.20, 152.94,2451545.0, 3.20 },
    { "3 Juno",   2.6705, 0.2569, 12.991, 169.86, 247.97, 144.59,2451545.0, 5.33 },
    { "7 Iris",   2.3850, 0.2300,  5.520, 259.55, 145.32,  20.13,2451545.0, 5.51 },
};
const int asteroid_count =
    (int)(sizeof asteroid_elements / sizeof asteroid_elements[0]);

_Static_assert(sizeof asteroid_elements / sizeof asteroid_elements[0]
               == ASTEROID_COUNT,
               "ASTEROID_COUNT in asteroid.h is out of sync "
               "with asteroid_elements[]");

/* Same shape as comet.c's solver. Duplicated rather than abstracted —
 * the function is 12 lines and the abstraction would not pay back. */
static double solve_kepler_ecc(double M, double e) {
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

static void asteroid_helio_xyz(const AsteroidElements *a, double jd,
                               double *x, double *y, double *z,
                               double *r_helio) {
    /* Mean motion from Kepler's third law. */
    const double k = 0.01720209895;
    double n_rad_per_day = k / sqrt(a->a_au * a->a_au * a->a_au);

    double M = a->M0_deg * DEG2RAD + n_rad_per_day * (jd - a->epoch_jd);
    double E = solve_kepler_ecc(M, a->e);

    double cos_E = cos(E);
    double sin_E = sin(E);
    double x_orb = a->a_au * (cos_E - a->e);
    double y_orb = a->a_au * sqrt(1.0 - a->e * a->e) * sin_E;

    double inc   = a->i_deg     * DEG2RAD;
    double Omega = a->Omega_deg * DEG2RAD;
    double omega = a->omega_deg * DEG2RAD;
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

void asteroid_helio_xyz_for(int idx, double jd,
                            double *x, double *y, double *z) {
    if (idx < 0 || idx >= asteroid_count) {
        if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
        return;
    }
    double r;
    asteroid_helio_xyz(&asteroid_elements[idx], jd, x, y, z, &r);
}

void asteroid_compute_all(double jd, AsteroidState *out) {
    double ex, ey, ez;
    ephem_earth_helio_xyz(jd, &ex, &ey, &ez);
    double eps = ephem_obliquity_rad(jd);
    double sin_e = sin(eps);
    double cos_e = cos(eps);

    for (int i = 0; i < asteroid_count; i++) {
        AsteroidState *s = &out[i];
        const AsteroidElements *a = &asteroid_elements[i];
        double cx, cy, cz, r;
        asteroid_helio_xyz(a, jd, &cx, &cy, &cz, &r);

        double gx = cx - ex;
        double gy = cy - ey;
        double gz = cz - ez;
        double delta = sqrt(gx * gx + gy * gy + gz * gz);

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
        s->mag        = (delta > 0.0 && r > 0.0)
                      ? a->H_mag + 5.0 * log10(r * delta)
                      : a->H_mag;
        s->alt_rad    = 0.0;
        s->az_rad     = 0.0;
        s->valid      = 1;
    }
}
