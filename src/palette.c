#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "palette.h"

Palette g_palette;

void palette_load_default(void) {
    g_palette = (Palette){
        .void_bg        = { 0.039f, 0.039f, 0.102f },
        .star_m         = { 1.000f, 0.823f, 0.631f }, /* #ffd2a1 */
        .star_g         = { 1.000f, 0.957f, 0.917f }, /* #fff4ea */
        .star_b         = { 0.659f, 0.847f, 1.000f }, /* #a8d8ff */
        .nebula_violet  = { 0.20f,  0.10f,  0.40f  },
        .nebula_crimson = { 0.35f,  0.13f,  0.13f  },
        .hud            = { 0.000f, 1.000f, 0.533f }, /* #00ff88 */
        .hud_alert      = { 1.000f, 0.400f, 0.000f }, /* #ff6600 */
    };
}

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static int parse_hex_color(const char *s, Color *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '#') s++;
    if (strlen(s) < 6) return -1;
    int r1 = hex_nibble(s[0]), r2 = hex_nibble(s[1]);
    int g1 = hex_nibble(s[2]), g2 = hex_nibble(s[3]);
    int b1 = hex_nibble(s[4]), b2 = hex_nibble(s[5]);
    if (r1 < 0 || r2 < 0 || g1 < 0 || g2 < 0 || b1 < 0 || b2 < 0) return -1;
    out->r = (float)((r1 << 4) | r2) / 255.0f;
    out->g = (float)((g1 << 4) | g2) / 255.0f;
    out->b = (float)((b1 << 4) | b2) / 255.0f;
    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int palette_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(p);
        char *val = trim(eq + 1);

        Color c;
        if (parse_hex_color(val, &c) != 0) continue;

        if      (!strcmp(key, "void"))           g_palette.void_bg        = c;
        else if (!strcmp(key, "star_m"))         g_palette.star_m         = c;
        else if (!strcmp(key, "star_g"))         g_palette.star_g         = c;
        else if (!strcmp(key, "star_b"))         g_palette.star_b         = c;
        else if (!strcmp(key, "nebula_violet"))  g_palette.nebula_violet  = c;
        else if (!strcmp(key, "nebula_crimson")) g_palette.nebula_crimson = c;
        else if (!strcmp(key, "hud"))            g_palette.hud            = c;
        else if (!strcmp(key, "hud_alert"))      g_palette.hud_alert      = c;
        /* unknown keys: silently ignored */
    }
    fclose(f);
    return 0;
}

void palette_autoload(const char *cli_path) {
    palette_load_default();
    if (cli_path && palette_load_file(cli_path) == 0) return;

    char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/voidwatch/theme.conf", xdg);
        if (palette_load_file(path) == 0) return;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(path, sizeof path, "%s/.config/voidwatch/theme.conf", home);
        (void)palette_load_file(path);
    }
}
