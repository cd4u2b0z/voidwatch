/* tests/test_satellite_bundle.c — Phase 6 bundled-catalog sanity.
 *
 * Verifies the static SatelliteElements table + the cached
 * satellite_compute_all path:
 *   - every bundled TLE parses
 *   - every bundled model initialises
 *   - every bundled satellite is near-Earth (deep-space deferred)
 *   - satellite_compute_all returns deterministic output for fixed JD
 *   - short names + epoch lookup handle out-of-range gracefully
 *   - TLE age sanity (refuses anything > 30 days at compute time)
 */

#include <assert.h>
#include <stdio.h>

#include "satellite.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void test_count_constant(void) {
    assert(satellite_count == SATELLITE_COUNT);
    assert(SATELLITE_COUNT > 0);
    printf("  count constant: PASS (SATELLITE_COUNT=%d)\n", SATELLITE_COUNT);
}

static void test_each_bundled_parses_and_inits(void) {
    /* Iterate every bundled entry and confirm the parser + init both
     * succeed AND the result is near-Earth. The static cache_init_once()
     * inside satellite.c does this lazily; here we run an independent
     * pass to surface specific failures with a useful index. */
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        const SatelliteElements *e = &satellite_elements[i];
        assert(e->name && e->line1 && e->line2);

        SatelliteTLE tle;
        SatelliteStatus rc = satellite_tle_parse(e->name, e->line1, e->line2, &tle);
        if (rc != SAT_OK) {
            fprintf(stderr, "  bundle[%d] %s parse failed: %s\n",
                    i, e->name, satellite_status_string(rc));
        }
        assert(rc == SAT_OK);
        assert(tle.catalog_number == e->catalog);

        SatelliteModel m;
        rc = satellite_model_init(&tle, &m);
        if (rc != SAT_OK) {
            fprintf(stderr, "  bundle[%d] %s init failed: %s\n",
                    i, e->name, satellite_status_string(rc));
        }
        assert(rc == SAT_OK);
        if (m.deep_space) {
            fprintf(stderr,
                "  bundle[%d] %s is deep-space — bundled catalog must "
                "be near-Earth only (refresh from CelesTrak)\n",
                i, e->name);
        }
        assert(!m.deep_space);
    }
    printf("  every bundled TLE parses + near-Earth: PASS\n");
}

static void test_compute_all_deterministic(void) {
    /* Two runs at the same JD must produce identical states. The
     * cache is module-static, so this also implicitly tests that the
     * cached models don't accumulate per-call drift. */
    double jd = 2461168.5;   /* 2026-05-08 00:00 UT, contemporary with TLEs */
    double lat = 51.48 * M_PI / 180.0;
    double lon = 0.0   * M_PI / 180.0;

    SatelliteState a[SATELLITE_COUNT], b[SATELLITE_COUNT];
    assert(satellite_compute_all(jd, lat, lon, 0.0, a) == SAT_OK);
    assert(satellite_compute_all(jd, lat, lon, 0.0, b) == SAT_OK);

    for (int i = 0; i < SATELLITE_COUNT; i++) {
        assert(a[i].valid == b[i].valid);
        if (!a[i].valid) continue;
        assert(a[i].alt_rad        == b[i].alt_rad);
        assert(a[i].az_rad         == b[i].az_rad);
        assert(a[i].range_km       == b[i].range_km);
        assert(a[i].above_horizon  == b[i].above_horizon);
    }
    printf("  compute_all deterministic: PASS\n");
}

static void test_compute_all_age_gate(void) {
    /* Push the JD 100 days past the bundled epochs — every entry
     * should refuse to propagate (valid=0). */
    double base = 0.0;
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        double e = satellite_epoch_jd(i);
        if (e > base) base = e;
    }
    double future = base + 100.0;
    SatelliteState st[SATELLITE_COUNT];
    assert(satellite_compute_all(future, 0.0, 0.0, 0.0, st) == SAT_OK);
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        if (satellite_epoch_jd(i) <= 0.0) continue;  /* failed init */
        assert(st[i].valid == 0);
    }
    printf("  age gate (>30d refuses): PASS\n");
}

static void test_short_names_present(void) {
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        const char *s = satellite_short_name(i);
        assert(s && s[0] != '\0');
    }
    /* Out-of-range returns a sentinel, not a crash. */
    const char *bad_a = satellite_short_name(-1);
    const char *bad_b = satellite_short_name(SATELLITE_COUNT);
    const char *bad_c = satellite_short_name(99999);
    assert(bad_a && bad_b && bad_c);
    printf("  short names + bounds: PASS\n");
}

static void test_epoch_jd_lookup(void) {
    /* In-range epoch JDs should be plausible Julian Days (post-2020). */
    for (int i = 0; i < SATELLITE_COUNT; i++) {
        double jd = satellite_epoch_jd(i);
        assert(jd > 2458849.5);    /* > 2020-01-01 */
        assert(jd < 2470000.0);    /* < ~2050      */
    }
    /* Out-of-range returns 0 (sentinel). */
    assert(satellite_epoch_jd(-1) == 0.0);
    assert(satellite_epoch_jd(SATELLITE_COUNT) == 0.0);
    printf("  epoch_jd lookup + bounds: PASS\n");
}

int main(void) {
    test_count_constant();
    test_each_bundled_parses_and_inits();
    test_compute_all_deterministic();
    test_compute_all_age_gate();
    test_short_names_present();
    test_epoch_jd_lookup();
    printf("satellite-bundle: PASS (6 cases)\n");
    return 0;
}
