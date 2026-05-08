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
#define TWOPI   (2.0 * M_PI)

/* ---- WGS-72 SGP4 constants (Vallado AIAA-2006-6753, getgravconst) ----
 *
 * SGP4 is fitted against TLEs that themselves were generated against
 * WGS-72. WGS-84 is physically newer but does NOT match published
 * verification vectors unless many associated choices change. Stay on
 * WGS-72 — voidwatch is a propagator, not a geodesy tool.
 */
#define WGS72_MU         398600.8                         /* km^3 / s^2  */
#define WGS72_RAD_KM     6378.135                          /* Earth ER, km */
/* xke = 60 / sqrt(R^3 / mu)  (= 1 / minutes-per-time-unit). Computed
 * literally rather than at runtime so the constant is grep-able. */
#define WGS72_XKE        0.0743669161331734132             /* 1/tu        */
#define WGS72_J2         0.001082616
#define WGS72_J3        -2.53881e-6
#define WGS72_J4        -1.65597e-6
#define WGS72_J3OJ2     (WGS72_J3 / WGS72_J2)
#define WGS72_AE         1.0           /* unit Earth radius (canonical) */

/* WGS-72 ellipsoid for the geodetic-observer ECEF transform (Phase 5).
 * Use the same datum SGP4 was fit against — mixing WGS-84 (for the
 * observer) with WGS-72 (for SGP4) introduces systematic offset that
 * hides under "visual-grade" but compounds for sat-pass timing. */
#define WGS72_FLATTENING (1.0 / 298.26)
#define WGS72_E2 (2.0 * WGS72_FLATTENING - WGS72_FLATTENING * WGS72_FLATTENING)

/* Earth rotation rate (IERS), rad/s. */
#define EARTH_OMEGA_RAD_S 7.2921150e-5

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

/* ---- Greenwich sidereal time (SGP4 form) ----------------------------
 *
 * Vallado 2013 eq 3-45 / "gstime" in NASA SGP4.c. Returns GMST in
 * radians at the given Julian Day UT1. Slightly different polynomial
 * form than Meeus 12.4 — keep this one because TLE epochs and Vallado
 * verification vectors are computed against this exact expression.
 *
 * Public name `satellite_gstime`; internal callers also use this.
 */
double satellite_gstime(double jd_ut1) {
    double tut1 = (jd_ut1 - 2451545.0) / 36525.0;
    double sec  = -6.2e-6 * tut1 * tut1 * tut1
                +  0.093104 * tut1 * tut1
                + (876600.0 * 3600.0 + 8640184.812866) * tut1
                +  67310.54841;
    /* 240 = 86400 / 360, converts seconds-of-time to degrees. Then
     * deg → rad and reduce to [0, 2π). */
    double rad = fmod(sec * DEG2RAD / 240.0, TWOPI);
    if (rad < 0.0) rad += TWOPI;
    return rad;
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

/* ---- Phase 2: SGP4 model initialization ------------------------------
 *
 * Map Vallado/NASA SGP4.c initl + sgp4init (near-Earth path only) onto
 * SatelliteModel. Variable names follow the reference where possible —
 * see SATELLITES.md "ugly but traceable beats pretty abstraction."
 *
 * Step order (Vallado):
 *   1. validate inputs (ecc in [0,1), n > 0, perigee >= 1 ER)
 *   2. copy TLE elements into SGP4 units
 *   3. recover original (un-Kozai) mean motion
 *   4. derive auxiliary quantities (ao, rp, gsto, ...)
 *   5. select atmospheric s/qoms2t based on perigee
 *   6. compute near-Earth secular + periodic coefficients
 *   7. classify deep-space (period >= 225 min) — Phase 4 fills the body
 *   8. compute t-power drag coefficients (only when not simplified)
 *
 * Phase 3 will implement satellite_propagate_teme; this routine returns
 * a model that's ready for it (or marked deep_space=1 to refuse).
 */
SatelliteStatus satellite_model_init(const SatelliteTLE *tle,
                                     SatelliteModel *sat) {
    if (!tle || !sat) return SAT_BAD_FIELD;

    /* Brief-mandated input rejection: ecc < 0 or >= 1, mean motion <= 0. */
    if (tle->eccentricity < 0.0 || tle->eccentricity >= 1.0)
        return SAT_BAD_FIELD;
    if (tle->mean_motion_rad_min <= 0.0)
        return SAT_BAD_FIELD;

    memset(sat, 0, sizeof *sat);

    /* ---- Step 2: TLE elements into SGP4 units (already in radians /
     * rad-per-minute coming out of the parser). */
    sat->ecco       = tle->eccentricity;
    sat->argpo      = tle->arg_perigee_rad;
    sat->inclo      = tle->inclination_rad;
    sat->mo         = tle->mean_anomaly_rad;
    sat->no_kozai   = tle->mean_motion_rad_min;
    sat->nodeo      = tle->raan_rad;
    sat->bstar      = tle->bstar;
    sat->jdsatepoch = tle->epoch_jd;

    const double x2o3 = 2.0 / 3.0;

    /* ---- Step 3 (initl): un-Kozai recovery + aux epoch quantities ---- */
    sat->eccsq  = sat->ecco * sat->ecco;
    sat->omeosq = 1.0 - sat->eccsq;
    sat->rteosq = sqrt(sat->omeosq);
    sat->cosio  = cos(sat->inclo);
    sat->cosio2 = sat->cosio * sat->cosio;
    sat->sinio  = sin(sat->inclo);

    /* ak: Kozai semi-major axis. del corrects to Brouwer mean motion. */
    double ak  = pow(WGS72_XKE / sat->no_kozai, x2o3);
    double d1  = 0.75 * WGS72_J2 * (3.0 * sat->cosio2 - 1.0)
               / (sat->rteosq * sat->omeosq);
    double del = d1 / (ak * ak);
    double adel = ak * (1.0 - del * del
                            - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    sat->no_unkozai = sat->no_kozai / (1.0 + del);

    /* ---- Step 4: derived auxiliaries ---- */
    sat->ao    = pow(WGS72_XKE / sat->no_unkozai, x2o3);
    double po  = sat->ao * sat->omeosq;
    sat->con42 = 1.0 - 5.0 * sat->cosio2;
    sat->con41 = -sat->con42 - sat->cosio2 - sat->cosio2;
    sat->ainv  = 1.0 / sat->ao;
    sat->posq  = po * po;
    sat->rp    = sat->ao * (1.0 - sat->ecco);
    sat->gsto  = satellite_gstime(sat->jdsatepoch);

    /* Brief-mandated: perigee below Earth surface = decayed orbit. NASA
     * SGP4.c comments this out (relies on the per-step mrt < 1 check),
     * but voidwatch's policy per SATELLITES.md is to refuse at init. */
    if (sat->rp < 1.0) return SAT_PROP_ERROR;

    /* ---- Step 5: atmospheric drag constants tuned by perigee altitude */
    double sfour  = 78.0 / WGS72_RAD_KM + 1.0;
    double q0     = (120.0 - 78.0) / WGS72_RAD_KM;
    double qzms24 = q0 * q0 * q0 * q0;
    double perige = (sat->rp - 1.0) * WGS72_RAD_KM;        /* km altitude */

    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        double q = (120.0 - sfour) / WGS72_RAD_KM;
        qzms24 = q * q * q * q;
        sfour = sfour / WGS72_RAD_KM + 1.0;
    }

    sat->isimp = (sat->rp < (220.0 / WGS72_RAD_KM + 1.0)) ? 1 : 0;

    /* ---- Step 6: secular and periodic near-Earth coefficients ------ */
    double pinvsq = 1.0 / sat->posq;
    double tsi    = 1.0 / (sat->ao - sfour);
    sat->eta      = sat->ao * sat->ecco * tsi;
    double etasq  = sat->eta * sat->eta;
    double eeta   = sat->ecco * sat->eta;
    double psisq  = fabs(1.0 - etasq);
    double coef   = qzms24 * pow(tsi, 4.0);
    double coef1  = coef / pow(psisq, 3.5);

    double cc2 = coef1 * sat->no_unkozai *
                 (sat->ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq))
                  + 0.375 * WGS72_J2 * tsi / psisq * sat->con41 *
                    (8.0 + 3.0 * etasq * (8.0 + etasq)));
    sat->cc1 = sat->bstar * cc2;

    double cc3 = 0.0;
    if (sat->ecco > 1.0e-4) {
        cc3 = -2.0 * coef * tsi * WGS72_J3OJ2 * sat->no_unkozai
            * sat->sinio / sat->ecco;
    }

    sat->x1mth2 = 1.0 - sat->cosio2;
    sat->cc4 = 2.0 * sat->no_unkozai * coef1 * sat->ao * sat->omeosq
             * (sat->eta * (2.0 + 0.5 * etasq)
                + sat->ecco * (0.5 + 2.0 * etasq)
                - WGS72_J2 * tsi / (sat->ao * psisq)
                  * (-3.0 * sat->con41
                       * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta))
                     + 0.75 * sat->x1mth2
                       * (2.0 * etasq - eeta * (1.0 + etasq))
                       * cos(2.0 * sat->argpo)));
    sat->cc5 = 2.0 * coef1 * sat->ao * sat->omeosq
             * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);

    double cosio4 = sat->cosio2 * sat->cosio2;
    double temp1  = 1.5 * WGS72_J2 * pinvsq * sat->no_unkozai;
    double temp2  = 0.5 * temp1 * WGS72_J2 * pinvsq;
    double temp3  = -0.46875 * WGS72_J4 * pinvsq * pinvsq * sat->no_unkozai;

    sat->mdot = sat->no_unkozai
              + 0.5 * temp1 * sat->rteosq * sat->con41
              + 0.0625 * temp2 * sat->rteosq
                * (13.0 - 78.0 * sat->cosio2 + 137.0 * cosio4);
    sat->argpdot = -0.5 * temp1 * sat->con42
                 + 0.0625 * temp2 * (7.0 - 114.0 * sat->cosio2 + 395.0 * cosio4)
                 + temp3 * (3.0 - 36.0 * sat->cosio2 + 49.0 * cosio4);
    double xhdot1 = -temp1 * sat->cosio;
    sat->nodedot = xhdot1
                 + (0.5 * temp2 * (4.0 - 19.0 * sat->cosio2)
                    + 2.0 * temp3 * (3.0 - 7.0 * sat->cosio2)) * sat->cosio;

    sat->omgcof = sat->bstar * cc3 * cos(sat->argpo);
    sat->xmcof  = 0.0;
    if (sat->ecco > 1.0e-4) {
        sat->xmcof = -x2o3 * coef * sat->bstar / eeta;
    }

    sat->nodecf = 3.5 * sat->omeosq * xhdot1 * sat->cc1;
    sat->t2cof  = 1.5 * sat->cc1;

    /* xlcof / aycof — guard against the i = 180° pole singularity. */
    const double cosio_eps = 1.5e-12;
    if (fabs(sat->cosio + 1.0) > cosio_eps) {
        sat->xlcof = -0.25 * WGS72_J3OJ2 * sat->sinio
                   * (3.0 + 5.0 * sat->cosio) / (1.0 + sat->cosio);
    } else {
        sat->xlcof = -0.25 * WGS72_J3OJ2 * sat->sinio
                   * (3.0 + 5.0 * sat->cosio) / cosio_eps;
    }
    sat->aycof = -0.5 * WGS72_J3OJ2 * sat->sinio;

    double delmotemp = 1.0 + sat->eta * cos(sat->mo);
    sat->delmo  = delmotemp * delmotemp * delmotemp;
    sat->sinmao = sin(sat->mo);
    sat->x7thm1 = 7.0 * sat->cosio2 - 1.0;

    /* ---- Step 7: deep-space classification ---- */
    if ((TWOPI / sat->no_unkozai) >= 225.0) {
        sat->deep_space = 1;
        sat->isimp      = 1;     /* deep-space implies simplified drag */
        /* dscom/dpper/dsinit lands in Phase 4. The propagator (Phase 3)
         * checks deep_space and returns SAT_DEEP_SPACE. */
    }

    /* ---- Step 8: t-power coefficients for full near-Earth drag ----- */
    if (!sat->isimp) {
        double cc1sq = sat->cc1 * sat->cc1;
        sat->d2 = 4.0 * sat->ao * tsi * cc1sq;
        double tmp = sat->d2 * tsi * sat->cc1 / 3.0;
        sat->d3 = (17.0 * sat->ao + sfour) * tmp;
        sat->d4 = 0.5 * tmp * sat->ao * tsi
                * (221.0 * sat->ao + 31.0 * sfour) * sat->cc1;
        sat->t3cof = sat->d2 + 2.0 * cc1sq;
        sat->t4cof = 0.25 * (3.0 * sat->d3
                           + sat->cc1 * (12.0 * sat->d2 + 10.0 * cc1sq));
        sat->t5cof = 0.2 * (3.0 * sat->d4
                          + 12.0 * sat->cc1 * sat->d3
                          + 6.0 * sat->d2 * sat->d2
                          + 15.0 * cc1sq * (2.0 * sat->d2 + cc1sq));
    }

    sat->error = 0;
    return SAT_OK;
}

/* ---- Phase 3: near-Earth SGP4 propagation ---------------------------
 *
 * Mirrors Vallado/NASA SGP4.c `sgp4` for the near-Earth path. Variable
 * names follow the reference exactly so the line-by-line correspondence
 * stays grep-able when validating against tcppver.out vectors.
 *
 * Output is in TEME (True Equator Mean Equinox) ECI: r in km, v in km/s.
 * That frame is *not* J2000 / ICRF / what voidwatch's planet path uses —
 * Phase 5 converts TEME → ECEF → topocentric look angles for rendering.
 *
 * Deep-space (period ≥ 225 min) is refused with SAT_DEEP_SPACE until
 * Phase 4 lands dscom/dpper/dsinit/dspace.
 */
SatelliteStatus satellite_propagate_teme(const SatelliteModel *sat,
                                         double tsince,
                                         double r_km[3],
                                         double v_km_s[3]) {
    if (!sat || !r_km || !v_km_s) return SAT_BAD_FIELD;
    if (sat->deep_space)          return SAT_DEEP_SPACE;

    const double x2o3      = 2.0 / 3.0;
    const double vkmpersec = WGS72_RAD_KM * WGS72_XKE / 60.0;

    /* ---- Secular updates over time ---- */
    double xmdf   = sat->mo    + sat->mdot    * tsince;
    double argpdf = sat->argpo + sat->argpdot * tsince;
    double nodedf = sat->nodeo + sat->nodedot * tsince;
    double argpm  = argpdf;
    double mm     = xmdf;
    double t2     = tsince * tsince;
    double nodem  = nodedf + sat->nodecf * t2;
    double tempa  = 1.0 - sat->cc1 * tsince;
    double tempe  = sat->bstar * sat->cc4 * tsince;
    double templ  = sat->t2cof * t2;

    /* ---- Full near-Earth drag (only when perigee >= 220 km) ---- */
    if (!sat->isimp) {
        double delomg   = sat->omgcof * tsince;
        double delmtemp = 1.0 + sat->eta * cos(xmdf);
        double delm     = sat->xmcof
                        * (delmtemp * delmtemp * delmtemp - sat->delmo);
        double temp_mm  = delomg + delm;
        mm    = xmdf   + temp_mm;
        argpm = argpdf - temp_mm;
        double t3 = t2 * tsince;
        double t4 = t3 * tsince;
        tempa = tempa - sat->d2 * t2 - sat->d3 * t3 - sat->d4 * t4;
        tempe = tempe + sat->bstar * sat->cc5 * (sin(mm) - sat->sinmao);
        templ = templ + sat->t3cof * t3
                      + t4 * (sat->t4cof + tsince * sat->t5cof);
    }

    double nm    = sat->no_unkozai;
    double em    = sat->ecco;
    double inclm = sat->inclo;

    /* (skip dspace — deep-space refused at the top) */

    if (nm <= 0.0) return SAT_PROP_ERROR;

    double am = pow(WGS72_XKE / nm, x2o3) * tempa * tempa;
    nm        = WGS72_XKE / pow(am, 1.5);
    em        = em - tempe;

    /* Eccentricity sanity. Vallado's exact tolerance — em can drift
     * slightly negative under aggressive drag, which is physically
     * meaningless but numerically tolerable to -0.001. */
    if (em >= 1.0 || em < -0.001) return SAT_PROP_ERROR;
    if (em < 1.0e-6) em = 1.0e-6;       /* clamp away from singular */

    mm += sat->no_unkozai * templ;
    double xlm = mm + argpm + nodem;

    /* Wrap angles to [0, 2π). */
    nodem = fmod(nodem, TWOPI); if (nodem < 0.0) nodem += TWOPI;
    argpm = fmod(argpm, TWOPI); if (argpm < 0.0) argpm += TWOPI;
    xlm   = fmod(xlm,   TWOPI); if (xlm   < 0.0) xlm   += TWOPI;
    mm    = fmod(xlm - argpm - nodem, TWOPI);
    if (mm < 0.0) mm += TWOPI;

    double sinim = sin(inclm);
    double cosim = cos(inclm);

    /* Lunar-solar periodics (dpper) skipped — deep-space only. */
    double ep    = em;
    double xincp = inclm;
    double argpp = argpm;
    double nodep = nodem;
    double mp    = mm;
    double sinip = sinim;
    double cosip = cosim;

    /* ---- Long-period periodics setup ---- */
    double axnl = ep * cos(argpp);
    double temp = 1.0 / (am * (1.0 - ep * ep));
    double aynl = ep * sin(argpp) + temp * sat->aycof;
    double xl   = mp + argpp + nodep + temp * sat->xlcof * axnl;

    /* ---- Solve Kepler's equation (Newton, capped at 10 iters) ---- */
    double u   = fmod(xl - nodep, TWOPI); if (u < 0.0) u += TWOPI;
    double eo1 = u;
    double tem5 = 9999.9;
    double sineo1 = 0.0, coseo1 = 0.0;
    for (int ktr = 1; ktr <= 10 && fabs(tem5) >= 1.0e-12; ktr++) {
        sineo1 = sin(eo1);
        coseo1 = cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (fabs(tem5) >= 0.95) tem5 = (tem5 > 0.0) ? 0.95 : -0.95;
        eo1 += tem5;
    }

    /* ---- Short-period preliminaries ---- */
    double ecose = axnl * coseo1 + aynl * sineo1;
    double esine = axnl * sineo1 - aynl * coseo1;
    double el2   = axnl * axnl + aynl * aynl;
    double pl    = am * (1.0 - el2);
    if (pl < 0.0) return SAT_PROP_ERROR;

    double rl     = am * (1.0 - ecose);
    double rdotl  = sqrt(am) * esine / rl;
    double rvdotl = sqrt(pl) / rl;
    double betal  = sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su   = atan2(sinu, cosu);
    double sin2u = 2.0 * cosu * sinu;
    double cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    double temp1 = 0.5 * WGS72_J2 * temp;
    double temp2 = temp1 * temp;

    /* ---- Short-period periodic corrections ---- */
    double mrt   = rl * (1.0 - 1.5 * temp2 * betal * sat->con41)
                 + 0.5 * temp1 * sat->x1mth2 * cos2u;
    su          -= 0.25 * temp2 * sat->x7thm1 * sin2u;
    double xnode = nodep + 1.5 * temp2 * cosip * sin2u;
    double xinc  = xincp + 1.5 * temp2 * cosip * sinip * cos2u;
    double mvt   = rdotl - nm * temp1 * sat->x1mth2 * sin2u / WGS72_XKE;
    double rvdot = rvdotl + nm * temp1
                 * (sat->x1mth2 * cos2u + 1.5 * sat->con41) / WGS72_XKE;

    /* ---- Orientation vectors (perifocal → TEME) ---- */
    double sinsu = sin(su);
    double cossu = cos(su);
    double snod  = sin(xnode);
    double cnod  = cos(xnode);
    double sini  = sin(xinc);
    double cosi  = cos(xinc);
    double xmx = -snod * cosi;
    double xmy =  cnod * cosi;
    double ux = xmx * sinsu + cnod * cossu;
    double uy = xmy * sinsu + snod * cossu;
    double uz = sini * sinsu;
    double vx = xmx * cossu - cnod * sinsu;
    double vy = xmy * cossu - snod * sinsu;
    double vz = sini * cossu;

    /* ---- Position (km) and velocity (km/s) in TEME ---- */
    r_km[0]   = mrt * ux * WGS72_RAD_KM;
    r_km[1]   = mrt * uy * WGS72_RAD_KM;
    r_km[2]   = mrt * uz * WGS72_RAD_KM;
    v_km_s[0] = (mvt * ux + rvdot * vx) * vkmpersec;
    v_km_s[1] = (mvt * uy + rvdot * vy) * vkmpersec;
    v_km_s[2] = (mvt * uz + rvdot * vz) * vkmpersec;

    /* mrt < 1 ER means the satellite has decayed below the Earth's
     * surface during this propagation step. */
    if (mrt < 1.0) return SAT_PROP_ERROR;
    return SAT_OK;
}

/* ---- Phase 5: TEME → observer look angles ---------------------------
 *
 * Pipeline (per SATELLITES.md "Phase 5"):
 *   1. r_teme, v_teme  → propagated by satellite_propagate_teme
 *   2. r_teme  → r_ecef     (rotate by -GMST about z; ignore polar motion)
 *   3. v_teme  → v_ecef     (rotate, then subtract Earth rotation × r)
 *   4. observer (lat, lon, h)  → obs_ecef (WGS-72 ellipsoid)
 *   5. range_ecef = r_ecef - obs_ecef
 *   6. range_ecef → SEZ (south, east, zenith) at observer
 *   7. (alt, az, range) from SEZ; range_rate from v_sat_ecef·range_hat
 *
 * Visual-grade — polar motion zero, no UT1-UTC, no nutation. Strict-
 * grade work belongs upstream of TEME (Vallado vector validation in
 * test_sgp4_propagation.c covers that gate).
 */

static void teme_to_ecef_pos(const double r_teme[3], double gmst,
                             double r_ecef[3]) {
    double c = cos(gmst);
    double s = sin(gmst);
    /* R_z(-gmst) — rotates from inertial-frame TEME into Earth-fixed. */
    r_ecef[0] =  c * r_teme[0] + s * r_teme[1];
    r_ecef[1] = -s * r_teme[0] + c * r_teme[1];
    r_ecef[2] =  r_teme[2];
}

static void teme_v_to_ecef(const double r_teme[3], const double v_teme[3],
                           double gmst, double v_ecef[3]) {
    /* v_ecef = R_z(-gmst) · v_teme  −  ω × r_ecef. The ω vector points
     * along Earth's spin axis (+z), so the cross product is just an
     * in-plane swap. Earth rotation rate is rad/s, r is km → km/s. */
    double r_ecef[3];
    teme_to_ecef_pos(r_teme, gmst, r_ecef);
    double c = cos(gmst);
    double s = sin(gmst);
    double vx_rot =  c * v_teme[0] + s * v_teme[1];
    double vy_rot = -s * v_teme[0] + c * v_teme[1];
    double vz_rot =  v_teme[2];
    v_ecef[0] = vx_rot - (-EARTH_OMEGA_RAD_S * r_ecef[1]);
    v_ecef[1] = vy_rot - ( EARTH_OMEGA_RAD_S * r_ecef[0]);
    v_ecef[2] = vz_rot;
}

/* Geodetic (lat, lon, alt above ellipsoid) → ECEF. Latitude north
 * positive, longitude east positive, altitude in km. WGS-72 datum so
 * we stay consistent with the SGP4 frame. */
static void geodetic_to_ecef(double lat_rad, double lon_rad,
                             double alt_km, double r_ecef[3]) {
    double sinlat = sin(lat_rad);
    double coslat = cos(lat_rad);
    double sinlon = sin(lon_rad);
    double coslon = cos(lon_rad);
    /* Prime vertical radius of curvature. */
    double N = WGS72_RAD_KM / sqrt(1.0 - WGS72_E2 * sinlat * sinlat);
    r_ecef[0] = (N + alt_km) * coslat * coslon;
    r_ecef[1] = (N + alt_km) * coslat * sinlon;
    r_ecef[2] = (N * (1.0 - WGS72_E2) + alt_km) * sinlat;
}

/* Rotate an ECEF vector into the local SEZ frame at observer (lat, lon).
 * SEZ = (South, East, Zenith). */
static void ecef_to_sez(const double r_ecef[3],
                        double lat_rad, double lon_rad,
                        double sez[3]) {
    double sinlat = sin(lat_rad);
    double coslat = cos(lat_rad);
    double sinlon = sin(lon_rad);
    double coslon = cos(lon_rad);
    /* Standard rotation: R = R_y(π/2 − φ) · R_z(λ) reduced. */
    sez[0] =  sinlat * coslon * r_ecef[0]
            + sinlat * sinlon * r_ecef[1]
            - coslat          * r_ecef[2];
    sez[1] = -sinlon          * r_ecef[0]
            + coslon          * r_ecef[1];
    sez[2] =  coslat * coslon * r_ecef[0]
            + coslat * sinlon * r_ecef[1]
            + sinlat          * r_ecef[2];
}

SatelliteStatus satellite_eci_to_topocentric(const double r_teme_km[3],
                                             const double v_teme_km_s[3],
                                             double jd_ut1,
                                             double obs_lat_rad,
                                             double obs_lon_east_rad,
                                             double obs_alt_km,
                                             SatelliteState *out) {
    if (!r_teme_km || !v_teme_km_s || !out) return SAT_BAD_FIELD;

    /* Carry the TEME vectors through unchanged for callers that want
     * both. */
    out->r_teme_km[0] = r_teme_km[0];
    out->r_teme_km[1] = r_teme_km[1];
    out->r_teme_km[2] = r_teme_km[2];
    out->v_teme_km_s[0] = v_teme_km_s[0];
    out->v_teme_km_s[1] = v_teme_km_s[1];
    out->v_teme_km_s[2] = v_teme_km_s[2];

    /* TEME → ECEF rotation. */
    double gmst = satellite_gstime(jd_ut1);
    double sat_ecef[3], sat_v_ecef[3];
    teme_to_ecef_pos(r_teme_km, gmst, sat_ecef);
    teme_v_to_ecef(r_teme_km, v_teme_km_s, gmst, sat_v_ecef);

    /* Observer geodetic → ECEF. */
    double obs_ecef[3];
    geodetic_to_ecef(obs_lat_rad, obs_lon_east_rad, obs_alt_km, obs_ecef);

    /* Range vector in ECEF. */
    double range_ecef[3] = {
        sat_ecef[0] - obs_ecef[0],
        sat_ecef[1] - obs_ecef[1],
        sat_ecef[2] - obs_ecef[2],
    };

    /* SEZ at observer. */
    double sez[3];
    ecef_to_sez(range_ecef, obs_lat_rad, obs_lon_east_rad, sez);

    double range_km = sqrt(sez[0] * sez[0] + sez[1] * sez[1]
                         + sez[2] * sez[2]);
    if (range_km < 1.0e-9) {
        /* Observer and satellite coincident — degenerate, but fill
         * something sane and bail. */
        out->alt_rad = M_PI * 0.5;
        out->az_rad  = 0.0;
        out->range_km = 0.0;
        out->range_rate_km_s = 0.0;
        out->above_horizon = 1;
        out->valid = 1;
        return SAT_OK;
    }

    out->alt_rad  = asin(sez[2] / range_km);
    /* Azimuth from north, increasing east. SEZ has S = -north, so
     * north-component = -sez[0], east-component = sez[1]. */
    double az = atan2(sez[1], -sez[0]);
    if (az < 0.0) az += TWOPI;
    out->az_rad   = az;
    out->range_km = range_km;
    out->above_horizon = (out->alt_rad > 0.0) ? 1 : 0;

    /* Range rate: project the satellite's ECEF velocity onto the line
     * of sight (observer is fixed in ECEF). */
    double inv = 1.0 / range_km;
    double rhat[3] = {
        range_ecef[0] * inv,
        range_ecef[1] * inv,
        range_ecef[2] * inv,
    };
    out->range_rate_km_s = sat_v_ecef[0] * rhat[0]
                         + sat_v_ecef[1] * rhat[1]
                         + sat_v_ecef[2] * rhat[2];

    out->valid = 1;
    return SAT_OK;
}

SatelliteStatus satellite_state_compute(const SatelliteModel *sat,
                                        double tsince_min,
                                        double obs_lat_rad,
                                        double obs_lon_east_rad,
                                        double obs_alt_km,
                                        SatelliteState *out) {
    if (!sat || !out) return SAT_BAD_FIELD;
    out->valid = 0;

    double r[3], v[3];
    SatelliteStatus rc = satellite_propagate_teme(sat, tsince_min, r, v);
    if (rc != SAT_OK) return rc;

    /* tsince is minutes since TLE epoch (JD UT). */
    double jd = sat->jdsatepoch + tsince_min / 1440.0;
    return satellite_eci_to_topocentric(r, v, jd,
                                        obs_lat_rad, obs_lon_east_rad,
                                        obs_alt_km, out);
}
