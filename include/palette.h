#ifndef VOIDWATCH_PALETTE_H
#define VOIDWATCH_PALETTE_H

#include "config.h"

/*
 * Runtime-mutable theme palette. Default values come from the original
 * compile-time constants; load_file() lets a wallust template (or any
 * key=value `.theme` file) override them on startup.
 *
 * Format (lines, '#' starts a comment):
 *     void          = #0a0a1a
 *     star_m        = #ffd2a1
 *     star_g        = #fff4ea
 *     star_b        = #a8d8ff
 *     nebula_violet = #331a66
 *     nebula_crimson= #592222
 *     hud           = #00ff88
 *     hud_alert     = #ff6600
 *
 * Unknown keys are tolerated; missing keys keep the default. RGB channels
 * are converted to linear-space floats so they slot into the framebuffer
 * pipeline directly.
 */
typedef struct {
    Color void_bg;
    Color star_m;
    Color star_g;
    Color star_b;
    Color nebula_violet;
    Color nebula_crimson;
    Color hud;
    Color hud_alert;
} Palette;

extern Palette g_palette;

void palette_load_default(void);

/* Returns 0 on success, -1 if the file can't be opened. Per-line parse
 * errors are silently ignored — partial themes are valid. */
int  palette_load_file(const char *path);

/* Tries `--theme` (when non-NULL), else $XDG_CONFIG_HOME/voidwatch/theme.conf,
 * else ~/.config/voidwatch/theme.conf. Quiet on miss. */
void palette_autoload(const char *cli_path);

#endif
