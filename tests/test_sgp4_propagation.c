/* tests/test_sgp4_propagation.c — Phase 3 oracle vector validation.
 *
 * Parses tests/data/sgp4_vectors.txt (Vallado SGP4-VER.TLE + tcppver.out)
 * and asserts our satellite_propagate_teme matches the published vectors
 * within the brief's strict tolerance:
 *
 *   |Δr| < 1e-3 km     (1 metre)
 *   |Δv| < 1e-6 km/s   (1 mm/s)
 *
 * If those don't hold, the brief is explicit: don't tune the constants —
 * find the branch or unit conversion error.
 *
 * Phase 3 covers near-Earth only; the fixture intentionally excludes
 * deep-space cases. Phase 4 will extend the fixture and the test.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "satellite.h"

#define POS_TOL_KM   1.0e-3
#define VEL_TOL_KMPS 1.0e-6

#define FIXTURE_PATH "tests/data/sgp4_vectors.txt"

static double v3_norm(const double a[3], const double b[3]) {
    double dx = a[0] - b[0];
    double dy = a[1] - b[1];
    double dz = a[2] - b[2];
    return sqrt(dx * dx + dy * dy + dz * dz);
}

/* Strip trailing newline and CR in place. Returns the new length. */
static int chomp(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
    return n;
}

/* Read one logical line, skipping blank lines and lines starting with '#'.
 * Returns 0 on EOF. */
static int read_meaningful_line(FILE *f, char *buf, size_t cap) {
    while (fgets(buf, (int)cap, f)) {
        chomp(buf);
        /* Skip leading spaces for the comment/blank check; preserve them
         * in the buffer (the TLE column parser depends on column 0). */
        const char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        return 1;
    }
    return 0;
}

typedef struct {
    int    n_steps;
    int    n_pass;
    double max_pos_err;
    double max_vel_err;
    char   name[64];
} BlockResult;

static int run_block(FILE *f, const char *name, BlockResult *res) {
    char l1[128], l2[128];
    if (!read_meaningful_line(f, l1, sizeof l1)) {
        fprintf(stderr, "  fixture: missing line 1 for %s\n", name);
        return -1;
    }
    if (!read_meaningful_line(f, l2, sizeof l2)) {
        fprintf(stderr, "  fixture: missing line 2 for %s\n", name);
        return -1;
    }

    SatelliteTLE tle;
    SatelliteStatus rc = satellite_tle_parse(name, l1, l2, &tle);
    if (rc != SAT_OK) {
        fprintf(stderr, "  %s: TLE parse failed (%s)\n",
                name, satellite_status_string(rc));
        return -1;
    }

    SatelliteModel sat;
    rc = satellite_model_init(&tle, &sat);
    if (rc != SAT_OK) {
        fprintf(stderr, "  %s: model init failed (%s)\n",
                name, satellite_status_string(rc));
        return -1;
    }
    if (sat.deep_space) {
        fprintf(stderr,
            "  %s: deep-space — fixture should not include this case in P3\n",
            name);
        return -1;
    }

    /* gcc -Wformat-truncation: cap explicitly even though the source
     * buffer can never overflow res->name in practice. */
    snprintf(res->name, sizeof res->name, "%.63s", name);
    res->n_steps = res->n_pass = 0;
    res->max_pos_err = res->max_vel_err = 0.0;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (strncmp(line, "ENDTLE", 6) == 0) return 0;
        if (line[0] == '#' || line[0] == '\0') continue;
        if (strncmp(line, "STEP ", 5) != 0) {
            fprintf(stderr, "  %s: unexpected line: %s\n", name, line);
            return -1;
        }

        double tsince, ex, ey, ez, evx, evy, evz;
        int n = sscanf(line + 5, "%lf %lf %lf %lf %lf %lf %lf",
                       &tsince, &ex, &ey, &ez, &evx, &evy, &evz);
        if (n != 7) {
            fprintf(stderr, "  %s: malformed STEP: %s\n", name, line);
            return -1;
        }

        double r[3], v[3];
        rc = satellite_propagate_teme(&sat, tsince, r, v);
        if (rc != SAT_OK) {
            fprintf(stderr, "  %s: propagate failed at t=%.1f (%s)\n",
                    name, tsince, satellite_status_string(rc));
            return -1;
        }

        double ref_r[3] = { ex, ey, ez };
        double ref_v[3] = { evx, evy, evz };
        double dr = v3_norm(r, ref_r);
        double dv = v3_norm(v, ref_v);

        if (dr > res->max_pos_err) res->max_pos_err = dr;
        if (dv > res->max_vel_err) res->max_vel_err = dv;
        res->n_steps++;

        if (dr < POS_TOL_KM && dv < VEL_TOL_KMPS) {
            res->n_pass++;
        } else {
            fprintf(stderr,
                "  %s @ t=%.1f: |Δr|=%.4e km |Δv|=%.4e km/s\n"
                "    actual r=(%.6f, %.6f, %.6f)\n"
                "    expect r=(%.6f, %.6f, %.6f)\n",
                name, tsince, dr, dv,
                r[0], r[1], r[2], ref_r[0], ref_r[1], ref_r[2]);
        }
    }

    fprintf(stderr, "  %s: ENDTLE marker missing\n", name);
    return -1;
}

int main(void) {
    FILE *f = fopen(FIXTURE_PATH, "r");
    if (!f) {
        fprintf(stderr, "fixture not found: %s\n", FIXTURE_PATH);
        fprintf(stderr, "  (run tests from the repo root: `make test`)\n");
        return 1;
    }

    int total_steps = 0, total_pass = 0;
    int n_blocks = 0, blocks_failed = 0;
    char line[256];

    while (read_meaningful_line(f, line, sizeof line)) {
        if (strncmp(line, "TLE ", 4) != 0) {
            fprintf(stderr, "fixture: expected `TLE <name>`, got: %s\n", line);
            fclose(f);
            return 1;
        }
        const char *name = line + 4;

        BlockResult res;
        if (run_block(f, name, &res) != 0) {
            blocks_failed++;
            n_blocks++;
            continue;
        }

        printf("  %-16s  %3d/%-3d steps   "
               "max |Δr|=%.3e km  max |Δv|=%.3e km/s\n",
               res.name, res.n_pass, res.n_steps,
               res.max_pos_err, res.max_vel_err);

        total_steps += res.n_steps;
        total_pass  += res.n_pass;
        n_blocks++;
        if (res.n_pass != res.n_steps) blocks_failed++;
    }
    fclose(f);

    if (blocks_failed > 0 || total_pass != total_steps) {
        printf("sgp4-prop: FAIL (%d/%d steps across %d blocks; %d blocks failed)\n",
               total_pass, total_steps, n_blocks, blocks_failed);
        return 1;
    }
    printf("sgp4-prop: PASS (%d steps across %d blocks)\n",
           total_steps, n_blocks);
    return 0;
}
