#ifndef VOIDWATCH_SATELLITE_H
#define VOIDWATCH_SATELLITE_H

/*
 * Hand-built TLE / SGP4 satellite tracking for voidwatch.
 *
 * Phasing (see SATELLITES.md):
 *   1. TLE parser           — this header + parser implementation
 *   2. SGP4 model init      — Phase 2
 *   3. Near-Earth propagate — Phase 3 (validated against Vallado vectors)
 *   4. Deep-space SDP4      — Phase 4 (or refuse with SAT_DEEP_SPACE)
 *   5. TEME → look angles   — Phase 5
 *   6. astro integration    — Phase 6
 *
 * Each phase has a hard test gate. Do not skip ahead.
 */

typedef enum {
    SAT_OK                 =  0,
    SAT_BAD_LINE           = -1,   /* malformed line, wrong length, etc.    */
    SAT_BAD_CHECKSUM       = -2,   /* line checksum mismatch                */
    SAT_BAD_FIELD          = -3,   /* numeric field would not parse         */
    SAT_MISMATCH           = -4,   /* line1 / line2 catalog numbers differ  */
    SAT_PROP_ERROR         = -5,   /* SGP4 propagator returned an error     */
    SAT_DEEP_SPACE         = -6,   /* deep-space TLE, unsupported in P1-P3  */
} SatelliteStatus;

/*
 * Normalised TLE record. Output of satellite_tle_parse — purely numeric,
 * no SGP4 state yet. Angles are in radians, mean motion is in rad/min,
 * epoch is Julian Day (UT), distances are in their natural TLE units
 * (BSTAR is in inverse Earth radii per the SGP4 convention).
 */
typedef struct {
    char   name[32];                       /* line 0 (or "" if 2-line)    */
    int    catalog_number;                 /* NORAD catalog id            */
    char   classification;                 /* 'U' typical                  */
    char   international_designator[12];   /* "YYNNNAAA"                  */

    int    epoch_year;                     /* full 4-digit                */
    double epoch_day;                      /* day-of-year + fraction       */
    double epoch_jd;                       /* Julian Day, UT              */

    double mean_motion_dot;                /* rev/day^2 (line 1 field 9)   */
    double mean_motion_ddot;               /* rev/day^3 (implied exponent) */
    double bstar;                          /* 1/ER      (implied exponent) */
    int    ephemeris_type;
    int    element_number;

    double inclination_rad;
    double raan_rad;
    double eccentricity;                   /* 0..1                         */
    double arg_perigee_rad;
    double mean_anomaly_rad;
    double mean_motion_rad_min;            /* rev/day → rad/min            */
    int    revolution_number;
} SatelliteTLE;

/*
 * Forward declaration — SatelliteModel holds SGP4 coefficients and is
 * sized in Phase 2. For now callers only use SatelliteTLE.
 */
typedef struct SatelliteModel SatelliteModel;

/*
 * Per-frame propagation result. `r_teme_km` / `v_teme_km_s` are the
 * SGP4-native output; `alt_rad` / `az_rad` / `range_km` are the
 * observer-relative look angles produced in Phase 5. `valid` gates
 * everything — render code must check it before using any field.
 */
typedef struct {
    double r_teme_km[3];
    double v_teme_km_s[3];
    double alt_rad;
    double az_rad;
    double range_km;
    double range_rate_km_s;
    int    above_horizon;
    int    valid;
} SatelliteState;

/* ---- Phase 1: parser ------------------------------------------------- */

/*
 * Parse a 2- or 3-line TLE into normalised SatelliteTLE form.
 * `name` may be NULL (the catalog number is then used to fill name[]),
 * `line1` and `line2` are the 69-column TLE lines (trailing newline OK).
 * Returns SAT_OK on success, or one of SAT_BAD_LINE / SAT_BAD_CHECKSUM /
 * SAT_BAD_FIELD / SAT_MISMATCH on failure.
 */
SatelliteStatus satellite_tle_parse(const char *name,
                                    const char *line1,
                                    const char *line2,
                                    SatelliteTLE *out);

/* Human-readable description of a SatelliteStatus, for log/error output. */
const char *satellite_status_string(SatelliteStatus status);

/* ---- Phases 2-5 (declared, not yet implemented) ---------------------- */

SatelliteStatus satellite_model_init(const SatelliteTLE *tle,
                                     SatelliteModel *model);

SatelliteStatus satellite_propagate_teme(const SatelliteModel *model,
                                         double minutes_since_epoch,
                                         double r_km[3],
                                         double v_km_s[3]);

#endif
