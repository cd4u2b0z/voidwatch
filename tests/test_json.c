/* tests/test_json.c — headless --print-state --json regression.
 *
 * Runs `voidwatch --print-state --json --at 2024-04-08T18:00:00
 * --lat 51.48 --lon 0.0` and diffs the output against the checked-in
 * golden file at tests/golden/print_state.json.
 *
 * If anything in the astro pipeline changes — ephem, comet propagation,
 * asteroid magnitudes, refraction, JSON formatting — this test fires.
 *
 * The fixed --at date avoids any wall-clock dependence; TZ=UTC is forced
 * via setenv so the printed ISO string is reproducible across machines
 * regardless of the user's local timezone.
 *
 * Update protocol: when a *deliberate* astronomy change lands, regenerate
 * the golden with:
 *   TZ=UTC ./voidwatch --print-state --json --at 2024-04-08T18:00:00 \
 *     --lat 51.48 --lon 0.0 > tests/golden/print_state.json
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define GOLDEN_PATH "tests/golden/print_state.json"
#define ACTUAL_TMP  "tests/golden/.actual.json"

static int run_voidwatch(void) {
    /* fork+exec so we can set the env without leaking it. */
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        /* child: redirect stdout to ACTUAL_TMP, set TZ=UTC, exec. */
        FILE *out = freopen(ACTUAL_TMP, "w", stdout);
        if (!out) { perror("freopen"); _exit(127); }
        setenv("TZ", "UTC", 1);
        char *const argv[] = {
            "./voidwatch",
            "--print-state", "--json",
            "--at",  "2024-04-08T18:00:00",
            "--lat", "51.48",
            "--lon", "0.0",
            NULL
        };
        execv(argv[0], argv);
        perror("execv ./voidwatch");
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "voidwatch exited with status %d\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

static char *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static int find_first_diff_line(const char *a, const char *b) {
    int line = 1;
    while (*a && *b) {
        const char *ea = strchr(a, '\n');
        const char *eb = strchr(b, '\n');
        size_t la = ea ? (size_t)(ea - a) : strlen(a);
        size_t lb = eb ? (size_t)(eb - b) : strlen(b);
        if (la != lb || memcmp(a, b, la) != 0) {
            fprintf(stderr, "  first diff at line %d:\n", line);
            fprintf(stderr, "    expected: %.*s\n", (int)la, a);
            fprintf(stderr, "    actual:   %.*s\n", (int)lb, b);
            return line;
        }
        line++;
        a = ea ? ea + 1 : a + la;
        b = eb ? eb + 1 : b + lb;
    }
    if (*a || *b) {
        fprintf(stderr, "  files differ in length past line %d\n", line);
        return line;
    }
    return 0;
}

int main(void) {
    if (run_voidwatch() != 0) {
        fprintf(stderr, "json: FAIL (could not run voidwatch — "
                        "is the binary built? `make` first.)\n");
        return 1;
    }

    size_t glen = 0, alen = 0;
    char *golden = slurp(GOLDEN_PATH, &glen);
    char *actual = slurp(ACTUAL_TMP,  &alen);
    if (!golden) {
        fprintf(stderr, "json: FAIL (golden file missing: %s)\n", GOLDEN_PATH);
        free(actual);
        return 1;
    }
    if (!actual) {
        fprintf(stderr, "json: FAIL (output file missing: %s)\n", ACTUAL_TMP);
        free(golden);
        return 1;
    }

    int rc = 0;
    if (glen != alen || memcmp(golden, actual, glen) != 0) {
        fprintf(stderr, "json: FAIL — output drift from golden\n");
        find_first_diff_line(golden, actual);
        fprintf(stderr,
            "  if this change is intentional, regenerate with:\n"
            "    TZ=UTC ./voidwatch --print-state --json "
            "--at 2024-04-08T18:00:00 --lat 51.48 --lon 0.0 \\\n"
            "      > %s\n", GOLDEN_PATH);
        rc = 1;
    } else {
        printf("json: PASS (%zu bytes match golden)\n", glen);
        unlink(ACTUAL_TMP);
    }

    free(golden);
    free(actual);
    return rc;
}
