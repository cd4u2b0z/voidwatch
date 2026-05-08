#ifndef VOIDWATCH_VWCONFIG_H
#define VOIDWATCH_VWCONFIG_H

/*
 * Runtime config. A small TOML/INI-subset file populates a single global
 * struct (`g_config`) at startup. Knobs included here are the ones a
 * user might reasonably want to retune without rebuilding — physics
 * tuning constants and visibility thresholds. The vast majority of
 * compile-time #defines in config.h stay there.
 *
 * Format: `key = value` lines, optional `[section]` headers, `#` and
 * `;` start comments. Section names are descriptive only; keys are
 * matched fully-qualified (e.g. `astro.comet_mag_cutoff`).
 *
 * Resolution order (mirrors palette):
 *   1. `--config <path>`               (CLI)
 *   2. `$XDG_CONFIG_HOME/voidwatch/config.toml`
 *   3. `~/.config/voidwatch/config.toml`
 *   4. built-in defaults (from config.h)
 *
 * Inotify-watched hot reload is intentionally not in this version —
 * deferred until live tuning is asked for.
 */

typedef struct {
    /* [visual] */
    float fb_decay;              /* exponential decay factor per frame */

    /* [astro] */
    float star_mag_cutoff;       /* hide stars dimmer than this         */
    float comet_mag_cutoff;      /* hide comets dimmer than this        */
    float asteroid_mag_cutoff;   /* hide asteroids dimmer than this     */
    float kp_index;              /* geomagnetic activity, 0..9 — scales
                                  * aurora intensity. 0 = quiet (nothing),
                                  * 3 = baseline visible, 9 = severe storm */

    /* [sandbox] */
    float gravity_g;             /* N-body gravitational constant       */
} VWConfig;

extern VWConfig g_config;

/* Populate g_config with built-in defaults from config.h. Always called
 * before any file load so partial files don't leave fields zeroed. */
void vwconfig_init_defaults(void);

/* Load a single config file and apply onto g_config. Returns 0 on
 * success, -1 if the file can't be opened. Parse errors per-line are
 * silently skipped — we tolerate forward-compatible files. */
int  vwconfig_load_file(const char *path);

/* CLI > XDG > $HOME path > defaults. `cli_path` may be NULL. Always
 * succeeds; missing files are not an error. */
void vwconfig_autoload(const char *cli_path);

#endif
