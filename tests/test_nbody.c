/* tests/test_nbody.c — symplectic-Euler conservation regression.
 *
 * Builds a deterministic 2-body system (binary on a circular orbit) and
 * runs body_step at 120 Hz for 10 simulated minutes (72,000 ticks). For a
 * symplectic integrator on a near-circular orbit, total energy and total
 * angular momentum should oscillate within a bounded range, not drift.
 *
 * Tolerances are tuned to catch a switch to non-symplectic integration
 * (plain Euler drifts ~1% per orbit at this timestep) while accepting
 * legitimate symplectic oscillation (~0.05% energy bound).
 *
 * If a contributor swaps the integrator without realising it must be
 * symplectic, this test fires.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "body.h"
#include "vwconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double total_energy(const BodySystem *bs, double G) {
    double KE = 0.0, PE = 0.0;
    for (int i = 0; i < bs->count; i++) {
        const Body *bi = &bs->bodies[i];
        KE += 0.5 * bi->mass * (bi->vx * bi->vx + bi->vy * bi->vy);
        for (int j = i + 1; j < bs->count; j++) {
            const Body *bj = &bs->bodies[j];
            double dx = bj->wx - bi->wx;
            double dy = bj->wy - bi->wy;
            /* Use the *softened* potential to match the integrator. With
             * softening ε, U = -G m1 m2 / sqrt(r² + ε²). */
            double r_eff = sqrt(dx*dx + dy*dy + GRAVITY_SOFTEN_SQ);
            PE -= G * bi->mass * bj->mass / r_eff;
        }
    }
    return KE + PE;
}

static double total_angular_momentum(const BodySystem *bs) {
    /* L_z = Σ m_i (x_i v_y_i − y_i v_x_i) about the origin. The choice of
     * origin shifts L by a constant only if total linear momentum is zero;
     * we initialise the system with zero net momentum so any drift in L is
     * a real integrator artefact. */
    double L = 0.0;
    for (int i = 0; i < bs->count; i++) {
        const Body *b = &bs->bodies[i];
        L += b->mass * (b->wx * b->vy - b->wy * b->vx);
    }
    return L;
}

int main(void) {
    /* gravity_g lives in g_config; init the runtime config defaults so
     * body_step has a sensible value. We're not loading any file. */
    vwconfig_init_defaults();

    BodySystem bs = {0};
    bs.capacity = 2;
    bs.count    = 2;
    bs.bodies   = calloc(2, sizeof(Body));
    assert(bs.bodies);

    /* Two-body binary about origin. m1 at +x, m2 at -x, equal-and-opposite
     * tangential velocities → total linear momentum = 0, total angular
     * momentum about origin nonzero (the test signal).
     *
     * Use big enough separation that softening is negligible (r=20 vs
     * GRAVITY_SOFTEN_SQ=1.0 → softening contributes 0.13% to r_eff). */
    double m1 = 2.0, m2 = 1.0;
    double r1 =  20.0 * m2 / (m1 + m2);   /* m1 closer to barycentre */
    double r2 =  20.0 * m1 / (m1 + m2);
    double mu = g_config.gravity_g * (m1 + m2);
    double v_rel = sqrt(mu / 20.0);        /* circular relative speed */
    double v1 = v_rel * m2 / (m1 + m2);
    double v2 = v_rel * m1 / (m1 + m2);

    bs.bodies[0] = (Body){
        .wx = (float)r1, .wy = 0.0f,
        .vx = 0.0f,      .vy = (float)v1,
        .mass = (float)m1, .radius = 1.0f, .intensity = 1.0f,
        .kind = BODY_PLANET,
    };
    bs.bodies[1] = (Body){
        .wx = (float)-r2, .wy = 0.0f,
        .vx = 0.0f,       .vy = (float)-v2,
        .mass = (float)m2, .radius = 1.0f, .intensity = 1.0f,
        .kind = BODY_PLANET,
    };

    double G = g_config.gravity_g;
    double E0 = total_energy(&bs, G);
    double L0 = total_angular_momentum(&bs);

    double dt = 1.0 / 120.0;          /* PHYSICS_HZ */
    int    steps = 72000;             /* 10 simulated minutes */

    double E_max = E0, E_min = E0;
    double L_max = L0, L_min = L0;

    for (int s = 0; s < steps; s++) {
        body_step(&bs, (float)dt);
        double E = total_energy(&bs, G);
        double L = total_angular_momentum(&bs);
        if (E > E_max) E_max = E;
        if (E < E_min) E_min = E;
        if (L > L_max) L_max = L;
        if (L < L_min) L_min = L;
    }

    double E_drift_rel = (E_max - E_min) / fabs(E0);
    double L_drift_rel = (L_max - L_min) / fabs(L0);

    /* Symplectic Euler bounds: energy oscillates within ~O(dt) of the
     * exact orbit — for our parameters that's <1%. Angular momentum is
     * exactly conserved by the leapfrog/symplectic family on a 1D
     * gradient field; we expect float-precision drift only. */
    const double E_tol = 0.05;
    const double L_tol = 1e-3;

    printf("  initial E=%.6e  L=%.6e\n", E0, L0);
    printf("  energy   drift = %.3e (tol %.1e)\n", E_drift_rel, E_tol);
    printf("  ang.mom. drift = %.3e (tol %.1e)\n", L_drift_rel, L_tol);

    int fail = 0;
    if (E_drift_rel > E_tol) {
        fprintf(stderr, "  energy drift exceeds tolerance — "
                        "integrator not symplectic?\n");
        fail = 1;
    }
    if (L_drift_rel > L_tol) {
        fprintf(stderr, "  angular momentum drift exceeds tolerance\n");
        fail = 1;
    }

    /* Bound check: bodies haven't escaped to infinity or collapsed onto
     * each other. (A non-symplectic integrator will show one of those
     * failure modes by 72k steps.) */
    double dx = bs.bodies[1].wx - bs.bodies[0].wx;
    double dy = bs.bodies[1].wy - bs.bodies[0].wy;
    double r  = sqrt(dx*dx + dy*dy);
    if (r < 5.0 || r > 60.0) {
        fprintf(stderr,
            "  bodies wandered to r=%.2f (started at 20.0)\n", r);
        fail = 1;
    }

    free(bs.bodies);
    if (fail) {
        printf("nbody: FAIL\n");
        return 1;
    }
    printf("nbody: PASS (72000 steps, energy + L conserved)\n");
    return 0;
}
