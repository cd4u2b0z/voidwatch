/* tests/test_satellite_look.c — Phase 5: TEME → topocentric look angles.
 *
 * Per SATELLITES.md §"Look-Angle Tests" synthetic cases:
 *   - observer at (0, 0, 0) lat/lon/alt
 *   - satellite directly above → alt ≈ π/2
 *   - satellite east on horizon → alt ≈ 0, az ≈ π/2
 *   - satellite north on horizon → alt ≈ 0, az ≈ 0
 *   - satellite below the horizon → above_horizon = 0
 *
 * Plus a smoke test wiring `satellite_state_compute` on a real TLE
 * (Vallado #5) at t=0, verifying the range is the propagated-position
 * magnitude as seen by an arbitrary surface observer.
 *
 * Geometry strategy: pick a jd, compute gmst from satellite_gstime,
 * then build r_teme = R_z(gmst) · r_ecef so the ECEF target lands
 * exactly where we want regardless of when the test runs.
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

/* WGS-72 equatorial radius. Observer at (lat=0, lon=0, alt=0) sits at
 * ECEF (a, 0, 0). Same constant the implementation uses. */
#define A_KM 6378.135

/* Build a TEME position that, when rotated by -gmst about z-axis (the
 * TEME→ECEF transform), lands at the requested ECEF point. */
static void teme_at_ecef(double r_ecef_x, double r_ecef_y, double r_ecef_z,
                         double gmst, double r_teme[3]) {
    double c = cos(gmst);
    double s = sin(gmst);
    /* R_z(+gmst) since the runtime applies R_z(-gmst). */
    r_teme[0] = c * r_ecef_x - s * r_ecef_y;
    r_teme[1] = s * r_ecef_x + c * r_ecef_y;
    r_teme[2] = r_ecef_z;
}

static void test_zenith(void) {
    /* Observer at (0,0,0), satellite 500 km straight up (ECEF (a+500, 0, 0)). */
    double jd = 2451545.0;
    double gmst = satellite_gstime(jd);
    double r_teme[3], v_teme[3] = {0};
    teme_at_ecef(A_KM + 500.0, 0.0, 0.0, gmst, r_teme);

    SatelliteState st;
    SatelliteStatus rc = satellite_eci_to_topocentric(
        r_teme, v_teme, jd, 0.0, 0.0, 0.0, &st);
    assert(rc == SAT_OK);
    assert(st.valid);
    assert(st.above_horizon == 1);
    assert(dbl_close(st.alt_rad, M_PI * 0.5, 1e-9));
    /* Az is degenerate at zenith (any value is correct); just verify
     * the range matches the satellite's altitude above the surface. */
    assert(dbl_close(st.range_km, 500.0, 1e-6));
    printf("  zenith: PASS (alt=%.6f, range=%.3f km)\n",
           st.alt_rad, st.range_km);
}

static void test_east_horizon(void) {
    /* Satellite at ECEF (a, 500, 0) — same x as observer, due east. */
    double jd = 2451545.0;
    double gmst = satellite_gstime(jd);
    double r_teme[3], v_teme[3] = {0};
    teme_at_ecef(A_KM, 500.0, 0.0, gmst, r_teme);

    SatelliteState st;
    SatelliteStatus rc = satellite_eci_to_topocentric(
        r_teme, v_teme, jd, 0.0, 0.0, 0.0, &st);
    assert(rc == SAT_OK);
    assert(dbl_close(st.alt_rad, 0.0,         1e-9));
    assert(dbl_close(st.az_rad,  M_PI * 0.5,  1e-9));
    assert(st.above_horizon == 0);   /* alt = 0 exactly is "not above" */
    printf("  east horizon: PASS (alt=%.3e, az=%.6f)\n",
           st.alt_rad, st.az_rad);
}

static void test_north_horizon(void) {
    /* ECEF (a, 0, 500) — equator-bound observer's north is +z ECEF. */
    double jd = 2451545.0;
    double gmst = satellite_gstime(jd);
    double r_teme[3], v_teme[3] = {0};
    teme_at_ecef(A_KM, 0.0, 500.0, gmst, r_teme);

    SatelliteState st;
    SatelliteStatus rc = satellite_eci_to_topocentric(
        r_teme, v_teme, jd, 0.0, 0.0, 0.0, &st);
    assert(rc == SAT_OK);
    assert(dbl_close(st.alt_rad, 0.0, 1e-9));
    /* Az = 0 means due north. */
    assert(dbl_close(st.az_rad, 0.0, 1e-9) ||
           dbl_close(st.az_rad, 2.0 * M_PI, 1e-9));
    printf("  north horizon: PASS (alt=%.3e, az=%.6f)\n",
           st.alt_rad, st.az_rad);
}

static void test_below_horizon(void) {
    /* Satellite on the antipode side — ECEF (-a-500, 0, 0). The line
     * of sight goes through Earth, alt = -π/2. */
    double jd = 2451545.0;
    double gmst = satellite_gstime(jd);
    double r_teme[3], v_teme[3] = {0};
    teme_at_ecef(-(A_KM + 500.0), 0.0, 0.0, gmst, r_teme);

    SatelliteState st;
    SatelliteStatus rc = satellite_eci_to_topocentric(
        r_teme, v_teme, jd, 0.0, 0.0, 0.0, &st);
    assert(rc == SAT_OK);
    assert(st.above_horizon == 0);
    assert(st.alt_rad < 0.0);
    /* Range should be ~2a + 500 (through Earth). */
    assert(dbl_close(st.range_km, 2.0 * A_KM + 500.0, 1e-6));
    printf("  below horizon: PASS (alt=%.6f, range=%.3f km)\n",
           st.alt_rad, st.range_km);
}

static void test_state_compute_smoke(void) {
    /* Vallado #5 (Vanguard 1) at t=0. Range from any surface observer
     * should be < 2 × ~13000 km (max possible distance at apogee from
     * antipodal observer) and > a few hundred km (worst-case zenith
     * passage). Just sanity-check that propagate + topocentric chain
     * runs and produces plausible numbers. */
    SatelliteTLE tle;
    const char *l1 =
        "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
    const char *l2 =
        "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";
    assert(satellite_tle_parse("VANGUARD 1", l1, l2, &tle) == SAT_OK);

    SatelliteModel sat;
    assert(satellite_model_init(&tle, &sat) == SAT_OK);

    SatelliteState st;
    SatelliteStatus rc = satellite_state_compute(&sat, 0.0,
        32.7 * M_PI / 180.0, -79.9 * M_PI / 180.0, 0.0, &st);
    assert(rc == SAT_OK);
    assert(st.valid);
    /* Range from any Earth observer to a 7022-km-radius orbit must be
     * in the few-hundred to ~14000 km range. */
    assert(st.range_km > 100.0);
    assert(st.range_km < 20000.0);
    /* alt and az always in their canonical ranges. */
    assert(st.alt_rad >= -M_PI * 0.5 && st.alt_rad <= M_PI * 0.5);
    assert(st.az_rad  >= 0.0          && st.az_rad  <  2.0 * M_PI);
    printf("  state_compute smoke: PASS (range=%.1f km, alt=%.3f°)\n",
           st.range_km, st.alt_rad * 180.0 / M_PI);
}

static void test_gstime_range(void) {
    /* GMST is in [0, 2π) for any input. Smoke-check at a few epochs. */
    double jds[] = { 2451545.0, 2440587.5, 2460000.0, 2470000.0, 2433281.5 };
    for (size_t i = 0; i < sizeof jds / sizeof jds[0]; i++) {
        double g = satellite_gstime(jds[i]);
        assert(g >= 0.0 && g < 2.0 * M_PI);
    }
    /* Continuity: gstime over a small step shouldn't jump by 2π. */
    double a = satellite_gstime(2451545.0);
    double b = satellite_gstime(2451545.0 + 1.0 / 1440.0);  /* +1 min */
    /* Earth rotates ~0.25°/min so the difference is small modulo 2π. */
    double diff = b - a;
    if (diff < -M_PI) diff += 2.0 * M_PI;
    if (diff >  M_PI) diff -= 2.0 * M_PI;
    assert(fabs(diff) < 0.01);    /* < ~0.6° */
    printf("  gstime range + continuity: PASS\n");
}

int main(void) {
    test_zenith();
    test_east_horizon();
    test_north_horizon();
    test_below_horizon();
    test_state_compute_smoke();
    test_gstime_range();
    printf("satellite-look: PASS (6 cases)\n");
    return 0;
}
