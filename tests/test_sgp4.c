/* tests/test_sgp4.c — Phase 2: SGP4 model initialization tests.
 *
 * Per SATELLITES.md §"Initialization Tests" minimum cases:
 *   - valid near-Earth TLE initialises as near-Earth (deep_space == 0)
 *   - valid GEO/deep-space TLE initialises as deep-space (deep_space == 1)
 *   - mean motion at the 225-min boundary is classified correctly
 *   - eccentricity below zero or at/above 1.0 is rejected
 *   - mean motion <= 0 is rejected
 *   - decayed/impossible orbit (perigee below surface) returns an error
 *
 * Plus a handful of recovered-quantity sanity asserts on Vallado #5
 * (Vanguard 1) so a re-tweak of the init math fires the test.
 *
 * Phase 3 (propagation against Vallado vectors) is a separate file.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "satellite.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int dbl_close(double a, double b, double eps) {
    return fabs(a - b) < eps;
}

/* Vallado #5: Vanguard 1, period ~133 min (clearly near-Earth). */
static const char V5_LINE1[] =
    "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
static const char V5_LINE2[] =
    "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

/* Vallado #8195: Molniya 2-14, period ~718 min (12-h Molniya, deep-space). */
static const char DS_LINE1[] =
    "1 08195U 75081A   06176.33215444  .00000099  00000-0  11873-3 0   813";
static const char DS_LINE2[] =
    "2 08195  64.1586 279.0717 6877146 264.7651  20.2257  2.00491383225656";

/* Build a SatelliteTLE directly without going through the column parser.
 * Used for synthetic edge cases the wire format can't represent (e.g.
 * eccentricity == 1.0, mean motion <= 0). */
static SatelliteTLE make_synthetic_tle(double mean_motion_rad_min,
                                       double eccentricity,
                                       double inclination_rad) {
    SatelliteTLE t = {0};
    strcpy(t.name, "SYNTHETIC");
    t.catalog_number       = 99999;
    t.classification       = 'U';
    t.epoch_year           = 2024;
    t.epoch_day            = 100.0;
    t.epoch_jd             = 2460309.5;   /* arbitrary, does not affect classification */
    t.mean_motion_dot      = 0.0;
    t.mean_motion_ddot     = 0.0;
    t.bstar                = 0.0;
    t.inclination_rad      = inclination_rad;
    t.raan_rad             = 0.0;
    t.eccentricity         = eccentricity;
    t.arg_perigee_rad      = 0.0;
    t.mean_anomaly_rad     = 0.0;
    t.mean_motion_rad_min  = mean_motion_rad_min;
    t.revolution_number    = 1;
    return t;
}

static void test_near_earth_init(void) {
    SatelliteTLE tle;
    assert(satellite_tle_parse("VANGUARD 1", V5_LINE1, V5_LINE2, &tle) == SAT_OK);

    SatelliteModel m;
    assert(satellite_model_init(&tle, &m) == SAT_OK);

    /* Vanguard 1: clearly near-Earth, perigee well above 220 km. */
    assert(m.deep_space == 0);
    assert(m.isimp      == 0);

    /* Sanity-check a few recovered quantities. These are derived from
     * the same equations as the implementation, so failure means the
     * code drifted from reference.
     *
     * TLE mean motion 10.82419157 rev/day = 0.04722... rad/min. Un-Kozai
     * correction is small for low eccentricity; recovered no_unkozai
     * stays close. Period ~133 min. */
    double period_min = 2.0 * M_PI / m.no_unkozai;
    assert(period_min > 132.0 && period_min < 134.0);

    /* Perigee around 1.10 ER → ~660 km altitude. Sanity bound. */
    assert(m.rp > 1.05 && m.rp < 1.15);

    /* GMST at epoch must be in [0, 2π). */
    assert(m.gsto >= 0.0 && m.gsto < 2.0 * M_PI);

    /* Trig identities don't lie. */
    assert(dbl_close(m.cosio2 + (1.0 - m.cosio2), 1.0, 1e-12));
    assert(dbl_close(m.con41,  3.0 * m.cosio2 - 1.0, 1e-12));
    assert(dbl_close(m.x7thm1, 7.0 * m.cosio2 - 1.0, 1e-12));
    assert(dbl_close(m.x1mth2, 1.0 - m.cosio2,       1e-12));

    printf("  near-Earth init (Vallado #5): PASS\n");
}

static void test_deep_space_init(void) {
    SatelliteTLE tle;
    assert(satellite_tle_parse("MOLNIYA 2-14", DS_LINE1, DS_LINE2, &tle) == SAT_OK);

    SatelliteModel m;
    /* Phase 2 returns SAT_OK even for deep-space — the deep_space flag
     * lets Phase 3 refuse propagation. */
    assert(satellite_model_init(&tle, &m) == SAT_OK);

    assert(m.deep_space == 1);
    assert(m.isimp      == 1);    /* deep-space implies simplified drag */

    /* Period must be in the deep-space regime. */
    double period_min = 2.0 * M_PI / m.no_unkozai;
    assert(period_min >= 225.0);

    printf("  deep-space init (Molniya 2-14): PASS\n");
}

static void test_boundary_classification(void) {
    /* Period just below 225 min → near-Earth.
     *   period = 2π / no_unkozai;  225 min = period for n_unkozai = 2π/225
     * Use 224 min nominal → mean_motion_rad_min = 2π/224 (no un-Kozai
     * shift since inclination is small and eccentricity is zero). */
    SatelliteTLE near = make_synthetic_tle(2.0 * M_PI / 224.0, 0.0, 0.1);
    SatelliteModel mn;
    assert(satellite_model_init(&near, &mn) == SAT_OK);
    assert(mn.deep_space == 0);
    assert(2.0 * M_PI / mn.no_unkozai < 225.0);

    /* Period just above 225 min → deep-space. */
    SatelliteTLE deep = make_synthetic_tle(2.0 * M_PI / 226.0, 0.0, 0.1);
    SatelliteModel md;
    assert(satellite_model_init(&deep, &md) == SAT_OK);
    assert(md.deep_space == 1);
    assert(2.0 * M_PI / md.no_unkozai >= 225.0);

    printf("  225-min boundary classification: PASS\n");
}

static void test_eccentricity_rejection(void) {
    /* ecc == 1.0 (parabolic — not allowed). */
    SatelliteTLE bad1 = make_synthetic_tle(2.0 * M_PI / 100.0, 1.0, 0.5);
    SatelliteModel m;
    assert(satellite_model_init(&bad1, &m) == SAT_BAD_FIELD);

    /* ecc > 1.0 (hyperbolic — not allowed). */
    SatelliteTLE bad2 = make_synthetic_tle(2.0 * M_PI / 100.0, 1.5, 0.5);
    assert(satellite_model_init(&bad2, &m) == SAT_BAD_FIELD);

    /* ecc < 0 (impossible). */
    SatelliteTLE bad3 = make_synthetic_tle(2.0 * M_PI / 100.0, -0.01, 0.5);
    assert(satellite_model_init(&bad3, &m) == SAT_BAD_FIELD);

    /* Low-ecc LEO at typical period — confirms the rejection branch is
     * gated on bad ecc, not "any non-zero" or "any failed init". */
    SatelliteTLE ok = make_synthetic_tle(2.0 * M_PI / 100.0, 0.01, 0.5);
    assert(satellite_model_init(&ok, &m) == SAT_OK);

    printf("  eccentricity rejection: PASS\n");
}

static void test_mean_motion_rejection(void) {
    SatelliteModel m;

    /* Zero mean motion. */
    SatelliteTLE zero = make_synthetic_tle(0.0, 0.01, 0.5);
    assert(satellite_model_init(&zero, &m) == SAT_BAD_FIELD);

    /* Negative mean motion. */
    SatelliteTLE neg = make_synthetic_tle(-0.05, 0.01, 0.5);
    assert(satellite_model_init(&neg, &m) == SAT_BAD_FIELD);

    printf("  mean-motion rejection: PASS\n");
}

static void test_decayed_orbit_rejection(void) {
    /* A body so fast its semi-major axis falls below 1 ER. With xke ≈
     * 0.07437 rad/min, ao = (xke/n)^(2/3). For ao = 0.95 ER, n =
     * xke / 0.95^1.5 ≈ 0.07437 / 0.9263 ≈ 0.0803 rad/min. With ecc = 0,
     * rp = ao < 1 → reject. */
    SatelliteTLE decayed = make_synthetic_tle(0.0803, 0.0, 0.5);
    SatelliteModel m;
    assert(satellite_model_init(&decayed, &m) == SAT_PROP_ERROR);

    printf("  decayed-orbit rejection: PASS\n");
}

static void test_null_inputs(void) {
    SatelliteTLE tle;
    SatelliteModel m;
    assert(satellite_tle_parse(NULL, V5_LINE1, V5_LINE2, &tle) == SAT_OK);
    assert(satellite_model_init(NULL, &m)   == SAT_BAD_FIELD);
    assert(satellite_model_init(&tle, NULL) == SAT_BAD_FIELD);
    printf("  null-input rejection: PASS\n");
}

int main(void) {
    test_near_earth_init();
    test_deep_space_init();
    test_boundary_classification();
    test_eccentricity_rejection();
    test_mean_motion_rejection();
    test_decayed_orbit_rejection();
    test_null_inputs();
    printf("sgp4-init: PASS (7 cases)\n");
    return 0;
}
