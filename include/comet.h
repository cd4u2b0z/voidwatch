#ifndef VOIDWATCH_COMET_H
#define VOIDWATCH_COMET_H

/* Bump this together with the table in comet.c — there's a static_assert
 * down there that yells if they fall out of sync. */
#define COMET_COUNT 6

/*
 * Hand-bundled set of perennial visual comets. Two-body Keplerian
 * propagation from osculating elements at perihelion epoch — accurate
 * to ~arcminutes near perihelion, drifts ~0.1°/yr at aphelion. That's
 * cell-scale tolerable for an observatory; precise comet ephemerides
 * need IAU MPC updates which conflict with the no-data-files posture.
 *
 * Apparent magnitude follows the standard cometary formula
 *     m = H + 5·log10(Δ) + 2.5·n·log10(r)
 * with H, n per comet (n≈4 nominal; n≈2 for "lazy" comets).
 *
 * Elements sourced from JPL Horizons / IAU MPC at the perihelion epoch
 * stored in `T_jd`. Long-period comets (P>200y) use period_yr=0; their
 * mean motion is computed from semi-major a = q/(1-e).
 */

typedef struct {
    const char *name;          /* short display name                     */
    double      T_jd;          /* perihelion epoch, Julian Day TT        */
    double      q_au;          /* perihelion distance, AU                */
    double      e;             /* eccentricity                           */
    double      i_deg;         /* inclination to ecliptic, degrees       */
    double      Omega_deg;     /* longitude of ascending node, degrees   */
    double      omega_deg;     /* argument of perihelion, degrees        */
    double      period_yr;     /* orbital period; 0 for long-period      */
    double      H_mag;         /* absolute (total) visual magnitude      */
    double      n_slope;       /* activity slope                         */
} CometElements;

typedef struct {
    double ra_rad;
    double dec_rad;
    double dist_au;            /* Earth → comet                           */
    double r_helio_au;         /* Sun → comet                             */
    double mag;                /* apparent total visual magnitude         */
    double alt_rad;
    double az_rad;
    int    valid;              /* 1 if this comet has a usable position   */
} CometState;

extern const CometElements comet_elements[];
extern const int            comet_count;

/* Compute geocentric apparent (RA, Dec) + magnitude for every bundled
 * comet at `jd`. Output array must be at least `comet_count` long. */
void comet_compute_all(double jd, CometState *out);

#endif
