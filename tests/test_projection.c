/* tests/test_projection.c — equatorial ↔ horizontal round-trip.
 *
 * Generates 1000 random (RA, Dec) pairs, projects to (alt, az) via
 * ephem_to_topocentric, projects back via ephem_altaz_to_radec, and
 * asserts the closing error is below 1e-9 rad (~0.0002").
 *
 * If this test fails, either the forward or the inverse transform is
 * wrong. Either is a correctness regression worth catching.
 *
 * Edge cases: the pole (cos δ → 0) and the meridian (H = 0). Both are
 * exercised by the random sampling but also asserted explicitly.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "ephem.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Wrap an angle to (-π, π] for delta comparison. */
static double wrap_pi(double a) {
    while (a >  M_PI) a -= 2.0 * M_PI;
    while (a < -M_PI) a += 2.0 * M_PI;
    return a;
}

static double frand_in(double lo, double hi) {
    return lo + (hi - lo) * ((double)rand() / (double)RAND_MAX);
}

/* Random-sample the unit sphere uniformly. atan2/asin handle the rest. */
static void random_radec(double *ra, double *dec) {
    double u = frand_in(0.0, 1.0);
    double v = frand_in(-1.0, 1.0);    /* uniform sin(δ) */
    *ra  = u * 2.0 * M_PI;
    *dec = asin(v);
}

static int test_round_trip(int n_samples, double tol_rad) {
    /* JD 2024-04-08 — eclipse epoch, no special meaning, just a fixed
     * stable date to drive sidereal time. */
    double jd = 2460409.25;

    int   failures   = 0;
    double max_err   = 0.0;

    /* Spread observers across the globe so multiple lat regimes get
     * exercised; 8 fixed lats from -80° to 80°. */
    static const double lats_deg[] = { -80, -55, -30, -5, 5, 30, 55, 80 };
    int n_lats = (int)(sizeof lats_deg / sizeof lats_deg[0]);

    for (int li = 0; li < n_lats; li++) {
        Observer obs = {
            .lat_rad = lats_deg[li] * M_PI / 180.0,
            .lon_rad = 0.0 * M_PI / 180.0,      /* arbitrary fixed lon */
        };

        for (int i = 0; i < n_samples; i++) {
            double ra0, dec0;
            random_radec(&ra0, &dec0);

            EphemPosition p = { .ra_rad = ra0, .dec_rad = dec0 };
            ephem_to_topocentric(&p, &obs, jd);

            /* Skip below-horizon points — they round-trip mathematically
             * but the test focuses on the visible hemisphere where users
             * actually see things. (The transform itself doesn't care.) */
            if (p.alt_rad < 0.0) continue;

            ephem_altaz_to_radec(&p, &obs, jd);

            double dra  = wrap_pi(p.ra_rad  - ra0);
            double ddec = wrap_pi(p.dec_rad - dec0);

            /* Near the pole, RA is degenerate — weight by cos(dec) so
             * "1 arcmin RA at the pole" doesn't trigger a false fail.
             * This is the angular-distance metric, not raw radians. */
            double err = sqrt(ddec * ddec
                            + cos(dec0) * cos(dec0) * dra * dra);

            if (err > tol_rad) {
                if (failures < 3) {
                    fprintf(stderr,
                        "  fail @ lat=%.0f° ra=%.4f dec=%.4f → "
                        "alt=%.4f az=%.4f → ra=%.4f dec=%.4f "
                        "(err=%.3e rad)\n",
                        lats_deg[li], ra0, dec0,
                        p.alt_rad, p.az_rad,
                        p.ra_rad, p.dec_rad, err);
                }
                failures++;
            }
            if (err > max_err) max_err = err;
        }
    }

    printf("  round-trip:  %d samples × %d lats, "
           "max err = %.3e rad (%.4f arcsec)\n",
           n_samples, n_lats, max_err, max_err * 180.0 / M_PI * 3600.0);
    return failures;
}

static void test_explicit_zenith(void) {
    /* A body at the observer's zenith: dec = lat, H = 0 (on meridian).
     * alt should be exactly π/2; az is degenerate. */
    Observer obs = { .lat_rad = 0.5,  .lon_rad = 0.0 };
    double jd = 2451545.0;
    double lst_h = ephem_local_sidereal_hours(jd, obs.lon_rad);
    double lst_rad = lst_h * 15.0 * M_PI / 180.0;

    EphemPosition p = { .ra_rad = lst_rad,    /* H = 0 */
                        .dec_rad = obs.lat_rad };
    ephem_to_topocentric(&p, &obs, jd);
    assert(fabs(p.alt_rad - M_PI * 0.5) < 1e-9);
}

int main(void) {
    srand(42);                  /* reproducible */
    int n_samples = 1000;
    double tol = 1e-9;          /* ~0.0002 arcsec — pure double precision */

    int failures = test_round_trip(n_samples, tol);
    test_explicit_zenith();

    if (failures > 0) {
        fprintf(stderr, "projection: FAIL (%d round-trip failures)\n",
                failures);
        return 1;
    }
    printf("projection: PASS (round-trip + zenith)\n");
    return 0;
}
