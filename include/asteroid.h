#ifndef VOIDWATCH_ASTEROID_H
#define VOIDWATCH_ASTEROID_H

/*
 * Hand-bundled minor planets. Same Keplerian propagation as comet.c,
 * but with classic (a, e, i, Ω, ω, M0 at epoch) elements rather than
 * (T, q) — asteroids stay on stable circular-ish orbits and don't get
 * the perihelion-relative parametrisation comets do.
 *
 * Magnitude follows the planet-style formula `m = H + 5·log10(r·Δ)`;
 * phase-angle correction skipped (cell-scale invisible). H is absolute
 * (V) magnitude at 1 AU from both Sun and observer.
 */

#define ASTEROID_COUNT 5

typedef struct {
    const char *name;
    double      a_au;
    double      e;
    double      i_deg;
    double      Omega_deg;
    double      omega_deg;
    double      M0_deg;
    double      epoch_jd;
    double      H_mag;
} AsteroidElements;

typedef struct {
    double ra_rad;
    double dec_rad;
    double dist_au;
    double r_helio_au;
    double mag;
    double alt_rad;
    double az_rad;
    int    valid;
} AsteroidState;

extern const AsteroidElements asteroid_elements[];
extern const int               asteroid_count;

void asteroid_compute_all(double jd, AsteroidState *out);

/* Heliocentric ecliptic position for the indexed asteroid at `jd`. */
void asteroid_helio_xyz_for(int idx, double jd,
                            double *x, double *y, double *z);

#endif
