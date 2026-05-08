/* tests/test_tle.c — Phase 1 TLE parser tests.
 *
 * Per SATELLITES.md §"TLE Tests" minimum cases:
 *   - known good TLE parses
 *   - line 1 checksum failure rejected
 *   - line 2 checksum failure rejected
 *   - catalog mismatch rejected
 *   - bad line number rejected
 *   - eccentricity 0006703 → 0.0006703
 *   - BSTAR  " 10270-3" → +1.0270e-4
 *   - BSTAR  "-11606-4" → -1.1606e-5
 *   - epoch year 99 → 1999, 00 → 2000, 56 → 2056, 57 → 1957
 *   - leap-year DOY conversion
 *
 * Plain C asserts. Exit 0 on pass.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satellite.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EPS_DBL 1e-9
#define EPS_DEG 1e-7      /* TLE deg fields have 4 decimals — 1e-7 rad ≈ 1e-5° */

static int dbl_close(double a, double b, double eps) {
    return fabs(a - b) < eps;
}

/* Recompute the col-69 checksum on a 69-char TLE line. Used by synthetic
 * tests that mutate fields and need to keep the parser past the checksum
 * gate. */
static void recompute_checksum(char *line) {
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9') sum += c - '0';
        else if (c == '-')        sum += 1;
    }
    line[68] = (char)('0' + (sum % 10));
}

/* Vallado AIAA-2006-6753 SGP4-VER.TLE catalog 5 (Vanguard 1, near-Earth).
 * Used as the base for many derived test cases. */
static const char VAL5_NAME[] = "VANGUARD 1";
static const char VAL5_LINE1[] =
    "1 00005U 58002B   00179.78495062  .00000023  00000-0  28098-4 0  4753";
static const char VAL5_LINE2[] =
    "2 00005  34.2682 348.7242 1859667 331.7664  19.3264 10.82419157413667";

static void test_known_good(void) {
    SatelliteTLE t = {0};
    SatelliteStatus rc = satellite_tle_parse(VAL5_NAME, VAL5_LINE1, VAL5_LINE2, &t);
    assert(rc == SAT_OK);

    assert(strcmp(t.name, "VANGUARD 1") == 0);
    assert(t.catalog_number  == 5);
    assert(t.classification  == 'U');
    assert(strcmp(t.international_designator, "58002B") == 0);

    assert(t.epoch_year == 2000);
    assert(dbl_close(t.epoch_day, 179.78495062, EPS_DBL));

    /* Epoch 2000 doy=179.78495062. doy=1 maps to JD(2000,1,1,0h)=2451544.5,
     * so epoch JD = 2451544.5 + 178.78495062 = 2451723.28495062 */
    assert(dbl_close(t.epoch_jd, 2451723.28495062, 1e-7));

    assert(dbl_close(t.mean_motion_dot,  0.00000023, 1e-12));
    assert(dbl_close(t.mean_motion_ddot, 0.0,        1e-12));
    /* BSTAR " 28098-4" → 0.28098 × 10^-4 = 2.8098e-5 */
    assert(dbl_close(t.bstar, 2.8098e-5, 1e-12));

    assert(t.ephemeris_type  == 0);
    assert(t.element_number  == 475);

    /* Line 2 expectations — convert deg → rad for inclination/RAAN/etc. */
    assert(dbl_close(t.inclination_rad,  34.2682  * M_PI / 180.0, EPS_DEG));
    assert(dbl_close(t.raan_rad,        348.7242  * M_PI / 180.0, EPS_DEG));
    assert(dbl_close(t.eccentricity,    0.1859667, 1e-9));
    assert(dbl_close(t.arg_perigee_rad, 331.7664  * M_PI / 180.0, EPS_DEG));
    assert(dbl_close(t.mean_anomaly_rad, 19.3264  * M_PI / 180.0, EPS_DEG));
    /* mean motion 10.82419157 rev/day → rad/min = × 2π / 1440 */
    assert(dbl_close(t.mean_motion_rad_min,
                     10.82419157 * 2.0 * M_PI / 1440.0, 1e-12));
    assert(t.revolution_number == 41366);

    printf("  known good: PASS\n");
}

static void test_line1_checksum_fail(void) {
    char l1[80]; strcpy(l1, VAL5_LINE1);
    /* Flip one digit but leave the checksum unchanged → mismatch. */
    l1[42] = (l1[42] == '3') ? '4' : '3';

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, l1, VAL5_LINE2, &t);
    assert(rc == SAT_BAD_CHECKSUM);
    printf("  line 1 checksum failure: PASS\n");
}

static void test_line2_checksum_fail(void) {
    char l2[80]; strcpy(l2, VAL5_LINE2);
    l2[10] = (l2[10] == '4') ? '5' : '4';

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, VAL5_LINE1, l2, &t);
    assert(rc == SAT_BAD_CHECKSUM);
    printf("  line 2 checksum failure: PASS\n");
}

static void test_catalog_mismatch(void) {
    char l2[80]; strcpy(l2, VAL5_LINE2);
    /* Change the 5-digit catalog on line 2: 00005 → 00006. */
    l2[6] = '6';
    recompute_checksum(l2);

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, VAL5_LINE1, l2, &t);
    assert(rc == SAT_MISMATCH);
    printf("  catalog mismatch: PASS\n");
}

static void test_bad_line_number(void) {
    char l1[80]; strcpy(l1, VAL5_LINE1);
    l1[0] = '3';
    recompute_checksum(l1);

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, l1, VAL5_LINE2, &t);
    assert(rc == SAT_BAD_LINE);
    printf("  bad line number: PASS\n");
}

static void test_eccentricity_implied_decimal(void) {
    /* Replace the 7-char eccentricity field (offsets 26..32) with "0006703"
     * → expected eccentricity 0.0006703. */
    char l2[80]; strcpy(l2, VAL5_LINE2);
    memcpy(&l2[26], "0006703", 7);
    recompute_checksum(l2);

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, VAL5_LINE1, l2, &t);
    assert(rc == SAT_OK);
    assert(dbl_close(t.eccentricity, 0.0006703, 1e-12));
    printf("  eccentricity implied decimal: PASS\n");
}

static void test_bstar_positive(void) {
    /* Patch BSTAR (offsets 53..60, 8 chars) to " 10270-3"
     * → +0.10270 × 10^-3 = 1.0270e-4. */
    char l1[80]; strcpy(l1, VAL5_LINE1);
    memcpy(&l1[53], " 10270-3", 8);
    recompute_checksum(l1);

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, l1, VAL5_LINE2, &t);
    assert(rc == SAT_OK);
    assert(dbl_close(t.bstar, 1.0270e-4, 1e-12));
    printf("  BSTAR positive ` 10270-3`: PASS\n");
}

static void test_bstar_negative(void) {
    /* "-11606-4" → -0.11606 × 10^-4 = -1.1606e-5. */
    char l1[80]; strcpy(l1, VAL5_LINE1);
    memcpy(&l1[53], "-11606-4", 8);
    recompute_checksum(l1);

    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, l1, VAL5_LINE2, &t);
    assert(rc == SAT_OK);
    assert(dbl_close(t.bstar, -1.1606e-5, 1e-12));
    printf("  BSTAR negative `-11606-4`: PASS\n");
}

static void test_epoch_year_pivot(void) {
    char l1[80]; strcpy(l1, VAL5_LINE1);

    /* yy=99 → 1999 */
    l1[18] = '9'; l1[19] = '9';
    recompute_checksum(l1);
    SatelliteTLE t;
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 1999);

    /* yy=00 → 2000 */
    l1[18] = '0'; l1[19] = '0';
    recompute_checksum(l1);
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 2000);

    /* yy=56 → 2056 (last year that maps "into the future") */
    l1[18] = '5'; l1[19] = '6';
    recompute_checksum(l1);
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 2056);

    /* yy=57 → 1957 (Sputnik year, the pivot point) */
    l1[18] = '5'; l1[19] = '7';
    recompute_checksum(l1);
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 1957);

    printf("  epoch year pivot 99/00/56/57: PASS\n");
}

static void test_leap_year_doy_conversion(void) {
    /* 2000 is a leap year. doy=60 means Feb 29 in 2000, but Mar 1 in 2001.
     *
     * 2000-02-29 00:00 UT → Gregorian-to-JD:
     *   M=2 ≤ 2 so Y=1999, M=14
     *   A=19, B=2-19+4 = -13
     *   floor(365.25 * 6715) = floor(2452653.75) = 2452653
     *   floor(30.6001 * 15) = floor(459.0015) = 459
     *   2452653 + 459 + 29 + (-13) - 1524.5 = 2451603.5
     *
     * 2001-03-01 00:00 UT:
     *   M=3 > 2 so Y=2001, M=3
     *   A=20, B=2-20+5 = -13
     *   floor(365.25 * 6717) = floor(2453384.25) = 2453384
     *   floor(30.6001 * 4) = 122
     *   2453384 + 122 + 1 + (-13) - 1524.5 = 2451969.5
     *
     * Sanity check: 2000-02-29 + 366 days (full leap year + 1) =
     *   2451603.5 + 366 = 2451969.5  ✓
     */
    char l1[80]; strcpy(l1, VAL5_LINE1);

    /* yy=00, doy=60 (Feb 29 2000 00:00 UT). */
    l1[18] = '0'; l1[19] = '0';
    /* doy field is 12 chars at offsets 20..31, format NNN.NNNNNNNN.
     * "060.00000000" — 3-digit DOY zero-padded. */
    memcpy(&l1[20], "060.00000000", 12);
    recompute_checksum(l1);
    SatelliteTLE t;
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 2000);
    assert(dbl_close(t.epoch_jd, 2451603.5, 1e-9));

    /* yy=01, doy=60 (Mar 1 2001 00:00 UT). */
    l1[18] = '0'; l1[19] = '1';
    recompute_checksum(l1);
    assert(satellite_tle_parse(NULL, l1, VAL5_LINE2, &t) == SAT_OK);
    assert(t.epoch_year == 2001);
    assert(dbl_close(t.epoch_jd, 2451969.5, 1e-9));

    printf("  leap-year DOY conversion: PASS\n");
}

static void test_status_strings(void) {
    /* Smoke-test: every status enum has a non-empty string. */
    SatelliteStatus codes[] = {
        SAT_OK, SAT_BAD_LINE, SAT_BAD_CHECKSUM, SAT_BAD_FIELD,
        SAT_MISMATCH, SAT_PROP_ERROR, SAT_DEEP_SPACE,
    };
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
        const char *s = satellite_status_string(codes[i]);
        assert(s != NULL);
        assert(s[0] != '\0');
    }
    printf("  status strings: PASS\n");
}

static void test_default_name_when_null(void) {
    /* When `name` arg is NULL, parser should synthesize a name from the
     * catalog number. */
    SatelliteTLE t;
    SatelliteStatus rc = satellite_tle_parse(NULL, VAL5_LINE1, VAL5_LINE2, &t);
    assert(rc == SAT_OK);
    assert(strcmp(t.name, "NORAD 5") == 0);
    printf("  default name from catalog: PASS\n");
}

int main(void) {
    test_known_good();
    test_line1_checksum_fail();
    test_line2_checksum_fail();
    test_catalog_mismatch();
    test_bad_line_number();
    test_eccentricity_implied_decimal();
    test_bstar_positive();
    test_bstar_negative();
    test_epoch_year_pivot();
    test_leap_year_doy_conversion();
    test_status_strings();
    test_default_name_when_null();
    printf("tle: PASS (12 cases)\n");
    return 0;
}
