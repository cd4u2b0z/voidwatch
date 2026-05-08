#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "vwconfig.h"

VWConfig g_config;

void vwconfig_init_defaults(void) {
    g_config.fb_decay              = FB_DECAY;
    g_config.star_mag_cutoff       = 6.5f;
    g_config.comet_mag_cutoff      = 8.0f;
    g_config.asteroid_mag_cutoff   = 9.5f;
    g_config.kp_index              = 3.0f;     /* normal activity baseline */
    g_config.gravity_g             = GRAVITY_G;
}

/* Strip leading/trailing whitespace in place. Returns the new start. */
static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* Apply one fully-qualified key (e.g. "astro.comet_mag_cutoff") with a
 * trimmed value string. Unknown keys silently ignored. */
static void apply_kv(const char *key, const char *value) {
    /* Tolerate bools and numerics — we treat them all as "is this float?" */
    char *end = NULL;
    double v = strtod(value, &end);
    if (end == value) return;             /* not a number — skip       */

    if      (strcmp(key, "visual.fb_decay")             == 0) g_config.fb_decay            = (float)v;
    else if (strcmp(key, "astro.star_mag_cutoff")       == 0) g_config.star_mag_cutoff     = (float)v;
    else if (strcmp(key, "astro.comet_mag_cutoff")      == 0) g_config.comet_mag_cutoff    = (float)v;
    else if (strcmp(key, "astro.asteroid_mag_cutoff")   == 0) g_config.asteroid_mag_cutoff = (float)v;
    else if (strcmp(key, "astro.kp_index")              == 0) g_config.kp_index            = (float)v;
    else if (strcmp(key, "sandbox.gravity_g")           == 0) g_config.gravity_g           = (float)v;
}

int vwconfig_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    char section[64] = "";
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;

        if (*p == '[') {
            char *end = strchr(p, ']');
            if (!end) continue;
            *end = '\0';
            char *name = trim(p + 1);
            snprintf(section, sizeof section, "%s", name);
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key   = trim(p);
        char *value = trim(eq + 1);
        if (!*key || !*value) continue;

        /* Strip optional surrounding quotes from the value. */
        size_t vlen = strlen(value);
        if (vlen >= 2 && ((value[0] == '"' && value[vlen-1] == '"') ||
                          (value[0] == '\'' && value[vlen-1] == '\''))) {
            value[vlen-1] = '\0';
            value++;
        }

        char fq[128];
        if (*section) snprintf(fq, sizeof fq, "%s.%s", section, key);
        else          snprintf(fq, sizeof fq, "%s", key);
        apply_kv(fq, value);
    }

    fclose(f);
    return 0;
}

void vwconfig_autoload(const char *cli_path) {
    vwconfig_init_defaults();

    if (cli_path && *cli_path) {
        /* CLI path: not finding it is intentional — surface to stderr
         * but don't fail the program. The user explicitly asked for
         * this file; warn them so they notice the typo. */
        if (vwconfig_load_file(cli_path) != 0) {
            fprintf(stderr,
                    "voidwatch: --config %s: file not found, using defaults\n",
                    cli_path);
        }
        return;
    }

    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/voidwatch/config.toml", xdg);
        if (vwconfig_load_file(path) == 0) return;
    }
    if (home && *home) {
        snprintf(path, sizeof path, "%s/.config/voidwatch/config.toml", home);
        if (vwconfig_load_file(path) == 0) return;
    }
    /* Defaults already in place. */
}
