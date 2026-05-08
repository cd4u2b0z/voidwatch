/* satellite.c — Phase 1: TLE parser only.
 *
 * SGP4 model init, propagation, and look angles are intentionally NOT
 * implemented in this file yet. See SATELLITES.md for the phase plan.
 *
 * TLE column layout sources:
 *   - CelesTrak NORAD Two-Line Element Set Format
 *     https://celestrak.org/NORAD/documentation/tle-fmt.php
 *   - Vallado, Crawford, Hujsak, Kelso, "Revisiting Spacetrack Report #3"
 *     AIAA 2006-6753
 *
 * Format reminders (1-indexed columns from the spec; we use 0-indexed
 * C offsets internally):
 *
 *   Line 1 (69 chars):
 *     col  1     line number ('1')
 *     col  3-7   catalog number
 *     col  8     classification
 *     col 10-17  international designator (yy nnn aaa)
 *     col 19-20  epoch year (2-digit)
 *     col 21-32  epoch day (NNN.NNNNNNNN)
 *     col 34-43  first derivative mean motion (rev/day^2, decimal)
 *     col 45-52  second derivative mean motion (implied 0., signed exponent)
 *     col 54-61  BSTAR drag (implied 0., signed exponent)
 *     col 63     ephemeris type
 *     col 65-68  element number
 *     col 69     checksum (mod 10 of digits + 1 per minus)
 *
 *   Line 2 (69 chars):
 *     col  1     line number ('2')
 *     col  3-7   catalog number (must match line 1)
 *     col  9-16  inclination (degrees, NNN.NNNN)
 *     col 18-25  RAAN (degrees)
 *     col 27-33  eccentricity (implied 0.NNNNNNN)
 *     col 35-42  argument of perigee (degrees)
 *     col 44-51  mean anomaly (degrees)
 *     col 53-63  mean motion (rev/day, NN.NNNNNNNN)
 *     col 64-68  revolution number at epoch
 *     col 69     checksum
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satellite.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DEG2RAD (M_PI / 180.0)

/* TLE_LINE_LEN excludes the trailing newline if present. */
#define TLE_LINE_LEN 69

/* ---- Small helpers --------------------------------------------------- */

/* Length of `line` ignoring a single trailing '\n'. Caller has already
 * checked that line is non-NULL. */
static int trimmed_len(const char *line) {
    int n = (int)strlen(line);
    if (n > 0 && line[n - 1] == '\n') n--;
    if (n > 0 && line[n - 1] == '\r') n--;
    return n;
}

/* Modulo-10 checksum of the first 68 characters of a TLE line.
 * Digits contribute their numeric value, '-' contributes 1, everything
 * else contributes 0. */
static int tle_checksum(const char *line) {
    int sum = 0;
    for (int i = 0; i < 68; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9') sum += c - '0';
        else if (c == '-')        sum += 1;
    }
    return sum % 10;
}

/* Copy [start, start+len) from line into a NUL-terminated buf (cap >= len+1).
 * Used to bridge fixed-column input to strtol/strtod which want C strings. */
static void copy_field(const char *line, int start, int len,
                       char *buf, int cap) {
    if (len + 1 > cap) len = cap - 1;
    memcpy(buf, line + start, (size_t)len);
    buf[len] = '\0';
}

static SatelliteStatus parse_int_field(const char *line, int start, int len,
                                       long *out) {
    char buf[32];
    if (len <= 0 || len >= (int)sizeof buf) return SAT_BAD_FIELD;
    copy_field(line, start, len, buf, sizeof buf);
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    /* Accept trailing spaces (TLE pads numeric fields). Reject if nothing
     * was consumed at all. */
    if (end == buf) return SAT_BAD_FIELD;
    while (*end == ' ') end++;
    if (*end != '\0') return SAT_BAD_FIELD;
    *out = v;
    return SAT_OK;
}

static SatelliteStatus parse_double_field(const char *line, int start, int len,
                                          double *out) {
    char buf[32];
    if (len <= 0 || len >= (int)sizeof buf) return SAT_BAD_FIELD;
    copy_field(line, start, len, buf, sizeof buf);
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return SAT_BAD_FIELD;
    while (*end == ' ') end++;
    if (*end != '\0') return SAT_BAD_FIELD;
    *out = v;
    return SAT_OK;
}

/* Parse an implied-leading-decimal field (eccentricity is the canonical
 * case): "0006703" → 0.0006703. No sign, no exponent. */
static SatelliteStatus parse_implied_decimal(const char *line, int start,
                                             int len, double *out) {
    char buf[32];
    if (len <= 0 || len + 2 >= (int)sizeof buf) return SAT_BAD_FIELD;
    buf[0] = '0';
    buf[1] = '.';
    for (int i = 0; i < len; i++) {
        char c = line[start + i];
        if (c == ' ') c = '0';
        if (c < '0' || c > '9') return SAT_BAD_FIELD;
        buf[2 + i] = c;
    }
    buf[2 + len] = '\0';
    char *end = NULL;
    double v = strtod(buf, &end);
    if (end == buf) return SAT_BAD_FIELD;
    *out = v;
    return SAT_OK;
}

/* Parse the BSTAR-style 8-char field with implied leading decimal and
 * trailing signed exponent.
 *
 * Layout:
 *   [sign(1)] [mantissa(5)] [exp_sign(1)] [exp_digit(1)]
 * Examples:
 *   " 10270-3"  →  +0.10270 × 10^-3
 *   "-11606-4"  →  -0.11606 × 10^-4
 *   "00000-0"   →  +0.00000 × 10^0  = 0.0
 *
 * Spaces in the mantissa or exponent digit are tolerated as zeros (some
 * Vallado-era TLEs leave the field blank for "no perturbation"). */
static SatelliteStatus parse_tle_exponent(const char *line, int start,
                                          int len, double *out) {
    if (len != 8) return SAT_BAD_FIELD;
    char sign = line[start];
    double mantissa_sign = (sign == '-') ? -1.0 : 1.0;

    char mant_buf[6] = {0};
    for (int i = 0; i < 5; i++) {
        char c = line[start + 1 + i];
        if (c == ' ') c = '0';
        if (c < '0' || c > '9') return SAT_BAD_FIELD;
        mant_buf[i] = c;
    }
    long mantissa_int = strtol(mant_buf, NULL, 10);
    double mantissa = (double)mantissa_int * 1e-5;

    char exp_sign = line[start + 6];
    char exp_dig  = line[start + 7];
    if (exp_dig == ' ') exp_dig = '0';
    if (exp_dig < '0' || exp_dig > '9') return SAT_BAD_FIELD;
    int exponent = exp_dig - '0';
    if (exp_sign == '-') exponent = -exponent;

    *out = mantissa_sign * mantissa * pow(10.0, exponent);
    return SAT_OK;
}

/* Gregorian (year, month, day, fractional hour) → Julian Day, UT.
 * Meeus eq. 7.1. Gregorian-only branch — voidwatch never sees pre-1582
 * TLEs (the format didn't exist until 1957). */
static double gregorian_to_jd(int Y, int M, int D, double hour_of_day) {
    if (M <= 2) { Y -= 1; M += 12; }
    int A = Y / 100;
    int B = 2 - A + A / 4;
    return floor(365.25 * (Y + 4716))
         + floor(30.6001 * (M + 1))
         + D + B - 1524.5
         + hour_of_day / 24.0;
}

/* Convert (year, day-of-year-with-fraction) to Julian Day UT.
 * day_of_year = 1.0 means Jan 1 00:00:00 UT. */
static double year_doy_to_jd(int year, double day_of_year) {
    double jd_jan1 = gregorian_to_jd(year, 1, 1, 0.0);
    return jd_jan1 + (day_of_year - 1.0);
}

/* Map TLE 2-digit year per the standard pivot:
 *   00..56 → 2000..2056
 *   57..99 → 1957..1999
 * Sputnik launched in 1957; the convention dates from there. */
static int tle_year_pivot(int yy) {
    return (yy < 57) ? (2000 + yy) : (1900 + yy);
}

/* ---- Public API ------------------------------------------------------ */

const char *satellite_status_string(SatelliteStatus s) {
    switch (s) {
        case SAT_OK:           return "ok";
        case SAT_BAD_LINE:     return "malformed TLE line";
        case SAT_BAD_CHECKSUM: return "TLE line checksum mismatch";
        case SAT_BAD_FIELD:    return "TLE numeric field would not parse";
        case SAT_MISMATCH:     return "TLE line 1 / line 2 catalog mismatch";
        case SAT_PROP_ERROR:   return "SGP4 propagator error";
        case SAT_DEEP_SPACE:   return "deep-space TLE not yet supported";
    }
    return "unknown";
}

SatelliteStatus satellite_tle_parse(const char *name,
                                    const char *line1,
                                    const char *line2,
                                    SatelliteTLE *out) {
    if (!line1 || !line2 || !out) return SAT_BAD_LINE;

    int n1 = trimmed_len(line1);
    int n2 = trimmed_len(line2);
    if (n1 != TLE_LINE_LEN || n2 != TLE_LINE_LEN) return SAT_BAD_LINE;

    if (line1[0] != '1') return SAT_BAD_LINE;
    if (line2[0] != '2') return SAT_BAD_LINE;

    /* Checksums first — if they fail every numeric below is suspect. */
    int cs1_expected = line1[68] - '0';
    int cs2_expected = line2[68] - '0';
    if (cs1_expected < 0 || cs1_expected > 9) return SAT_BAD_LINE;
    if (cs2_expected < 0 || cs2_expected > 9) return SAT_BAD_LINE;
    if (tle_checksum(line1) != cs1_expected) return SAT_BAD_CHECKSUM;
    if (tle_checksum(line2) != cs2_expected) return SAT_BAD_CHECKSUM;

    memset(out, 0, sizeof *out);

    SatelliteStatus rc;
    long lv;
    double dv;

    /* ---- Line 1 ------------------------------------------------------ */

    if ((rc = parse_int_field(line1, 2, 5, &lv)) != SAT_OK) return rc;
    int catalog_1 = (int)lv;
    out->catalog_number = catalog_1;

    out->classification = line1[7];

    /* International designator: yy nnn aaa, columns 10-17 (offsets 9-16).
     * Empty / whitespace is acceptable for older or simulated TLEs. */
    char intl_buf[12] = {0};
    int intl_len = 0;
    for (int i = 9; i <= 16 && intl_len < 11; i++) {
        char c = line1[i];
        if (c != ' ') intl_buf[intl_len++] = c;
    }
    intl_buf[intl_len] = '\0';
    memcpy(out->international_designator, intl_buf, sizeof out->international_designator);

    if ((rc = parse_int_field(line1, 18, 2, &lv)) != SAT_OK) return rc;
    out->epoch_year = tle_year_pivot((int)lv);

    if ((rc = parse_double_field(line1, 20, 12, &dv)) != SAT_OK) return rc;
    out->epoch_day = dv;
    out->epoch_jd  = year_doy_to_jd(out->epoch_year, out->epoch_day);

    if ((rc = parse_double_field(line1, 33, 10, &dv)) != SAT_OK) return rc;
    out->mean_motion_dot = dv;

    if ((rc = parse_tle_exponent(line1, 44, 8, &dv)) != SAT_OK) return rc;
    out->mean_motion_ddot = dv;

    if ((rc = parse_tle_exponent(line1, 53, 8, &dv)) != SAT_OK) return rc;
    out->bstar = dv;

    /* Ephemeris type (col 63 / offset 62) is a single character. */
    if (line1[62] >= '0' && line1[62] <= '9') {
        out->ephemeris_type = line1[62] - '0';
    } else {
        out->ephemeris_type = 0;
    }

    if ((rc = parse_int_field(line1, 64, 4, &lv)) != SAT_OK) return rc;
    out->element_number = (int)lv;

    /* ---- Line 2 ------------------------------------------------------ */

    if ((rc = parse_int_field(line2, 2, 5, &lv)) != SAT_OK) return rc;
    int catalog_2 = (int)lv;
    if (catalog_2 != catalog_1) return SAT_MISMATCH;

    if ((rc = parse_double_field(line2, 8, 8, &dv)) != SAT_OK) return rc;
    out->inclination_rad = dv * DEG2RAD;

    if ((rc = parse_double_field(line2, 17, 8, &dv)) != SAT_OK) return rc;
    out->raan_rad = dv * DEG2RAD;

    if ((rc = parse_implied_decimal(line2, 26, 7, &dv)) != SAT_OK) return rc;
    out->eccentricity = dv;

    if ((rc = parse_double_field(line2, 34, 8, &dv)) != SAT_OK) return rc;
    out->arg_perigee_rad = dv * DEG2RAD;

    if ((rc = parse_double_field(line2, 43, 8, &dv)) != SAT_OK) return rc;
    out->mean_anomaly_rad = dv * DEG2RAD;

    if ((rc = parse_double_field(line2, 52, 11, &dv)) != SAT_OK) return rc;
    /* TLE mean motion is rev/day; SGP4 wants rad/min.
     *   1 rev/day = 2π rad/day = 2π/1440 rad/min */
    out->mean_motion_rad_min = dv * 2.0 * M_PI / 1440.0;

    if ((rc = parse_int_field(line2, 63, 5, &lv)) != SAT_OK) return rc;
    out->revolution_number = (int)lv;

    /* ---- Name -------------------------------------------------------- */

    if (name && name[0] != '\0') {
        /* Strip newline + trailing spaces, cap at 31. */
        int len = (int)strlen(name);
        while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r'
                                                || name[len - 1] == ' ')) {
            len--;
        }
        if (len > (int)sizeof out->name - 1) len = (int)sizeof out->name - 1;
        memcpy(out->name, name, (size_t)len);
        out->name[len] = '\0';
    } else {
        snprintf(out->name, sizeof out->name, "NORAD %d", catalog_1);
    }

    return SAT_OK;
}
