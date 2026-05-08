#include <math.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "body.h"
#include "hud.h"
#include "palette.h"

/* Eight-step partial block characters: U+2581..U+2588 (▁▂▃▄▅▆▇█). */
static const char *BAR_LEVELS[9] = {
    " ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83",
         "\xE2\x96\x84", "\xE2\x96\x85", "\xE2\x96\x86",
         "\xE2\x96\x87", "\xE2\x96\x88",
};

/* HUD colours come from g_palette (default #00ff88 / #ff6600). The integer
 * 0..255 form is computed once per frame via these helpers. */
static int p_byte(float v) {
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return i;
}
#define HUD_R    p_byte(g_palette.hud.r)
#define HUD_G    p_byte(g_palette.hud.g)
#define HUD_B    p_byte(g_palette.hud.b)
#define ALERT_R  p_byte(g_palette.hud_alert.r)
#define ALERT_G  p_byte(g_palette.hud_alert.g)
#define ALERT_B  p_byte(g_palette.hud_alert.b)

/* Scan rotates one body per SCAN_DWELL seconds. */
#define SCAN_DWELL 6.0

/* Event log: small ring buffer of recent transients. */
#define EVENT_CAP 5
#define EVENT_TTL 8.0       /* seconds visible after firing */
#define EVENT_RATE_LIMIT 1.5 /* min seconds between log pushes */
#define TRANSIENT_LOG_TH 0.45f

typedef struct {
    char  text[28];
    double t;          /* monotonic timestamp from hud_draw's `t` */
} Event;

static Event events[EVENT_CAP];
static int   event_head = 0;     /* next write slot */
static double last_event_t = -1e9;

static void set_fg(FILE *out, int r, int g, int b) {
    fprintf(out, "\x1b[38;2;%d;%d;%dm", r, g, b);
}

static const char *kind_short(BodyKind k) {
    switch (k) {
        case BODY_STAR:         return "STAR";
        case BODY_PLANET:       return "PL";
        case BODY_NEUTRON_STAR: return "NS";
        case BODY_BLACK_HOLE:   return "BH";
        case BODY_NEBULA_CORE:  return "NEB";
    }
    return "?";
}

static const char *kind_long(BodyKind k) {
    switch (k) {
        case BODY_STAR:         return "MAIN-SEQ";
        case BODY_PLANET:       return "PLANETOID";
        case BODY_NEUTRON_STAR: return "NEUTRON-S";
        case BODY_BLACK_HOLE:   return "BLACK-HOLE";
        case BODY_NEBULA_CORE:  return "NEBULA";
    }
    return "?";
}

/* Per-kind running counter so labels read STAR-001, PL-002 etc. instead
 * of leaking the raw global index. Stable across frames because indices
 * don't change. */
static int kind_index(const BodySystem *bs, int target) {
    BodyKind k = bs->bodies[target].kind;
    int n = 0;
    for (int i = 0; i <= target; i++) {
        if (bs->bodies[i].kind == k) n++;
    }
    return n;
}

static void format_label(char *buf, size_t cap,
                         const BodySystem *bs, int i) {
    snprintf(buf, cap, "%s-%03d",
             kind_short(bs->bodies[i].kind),
             kind_index(bs, i));
}

static void hud_topleft(FILE *out, int cols,
                        float cam_x, float cam_y, double t) {
    if (cols < 36) return;

    int sec_x = (int)(cam_x / 64.0f);
    int sec_y = (int)(cam_y / 64.0f);

    int sec_total = (int)t;
    int hh = sec_total / 3600;
    int mm = (sec_total / 60) % 60;
    int ss = sec_total % 60;

    set_fg(out, HUD_R, HUD_G, HUD_B);
    fprintf(out, "\x1b[1;1H");
    fprintf(out, "VW \xE2\x97\x8B SECTOR %+05d.%+05d \xE2\x97\x8B "
                 "T+%02d:%02d:%02d",
            sec_x, sec_y, hh, mm, ss);
}

static void event_push(double t, const char *text) {
    if (t - last_event_t < EVENT_RATE_LIMIT) return;
    last_event_t = t;
    Event *e = &events[event_head];
    e->t = t;
    snprintf(e->text, sizeof e->text, "%s", text);
    event_head = (event_head + 1) % EVENT_CAP;
}

void hud_log_event(double t, const char *text) {
    if (text) event_push(t, text);
}

void hud_help_overlay(FILE *out, int cols, int rows) {
    static const char *lines[] = {
        "  voidwatch \xE2\x80\x94 keys",
        "",
        "    h        toggle HUD",
        "    ?        toggle this help",
        "    q / Esc  quit",
        "",
        "  astro mode (--astro)",
        "    + / -    speed up / slow down (10x)",
        "    0        reset speed + scrub",
        "    , / .    scrub time -1h / +1h",
        "    g        toggle alt-az grid",
        "    l        toggle constellation lines",
        "    d        toggle deep-sky objects",
        "    a        toggle aurora",
        "    t        toggle planet trails",
        "    m        toggle geo / helio view",
        "    s        toggle decorative star backdrop",
        "    c        toggle object cursor (hjkl move)",
        "",
        "  CLI flags",
        "    --astro           real-ephemeris mode",
        "    --lat / --lon     observer location",
        "    --list-devices    enumerate ALSA sources",
        "    --device <name>   pick PCM source",
        "    --no-audio        skip audio capture",
        "    --theme <path>    load palette .conf",
        "",
        "  press ? to dismiss",
    };
    const int n_lines = (int)(sizeof lines / sizeof lines[0]);
    const int W = 42;
    const int H = n_lines + 2;
    if (cols < W + 2 || rows < H + 2) return;

    int x0 = (cols - W) / 2 + 1;
    int y0 = (rows - H) / 2 + 1;

    set_fg(out, HUD_R, HUD_G, HUD_B);

    /* Top border */
    fprintf(out, "\x1b[%d;%dH\xE2\x95\xAD", y0, x0);
    for (int i = 0; i < W - 2; i++) fputs("\xE2\x94\x80", out);
    fputs("\xE2\x95\xAE", out);

    /* Body */
    for (int i = 0; i < n_lines; i++) {
        fprintf(out, "\x1b[%d;%dH\xE2\x94\x82", y0 + 1 + i, x0);
        /* line, padded to W-2 cells */
        fprintf(out, "%-*s", W - 2, lines[i]);
        fputs("\xE2\x94\x82", out);
    }

    /* Bottom border */
    fprintf(out, "\x1b[%d;%dH\xE2\x95\xB0", y0 + H - 1, x0);
    for (int i = 0; i < W - 2; i++) fputs("\xE2\x94\x80", out);
    fputs("\xE2\x95\xAF", out);
}

static void hud_event_log(FILE *out, int cols, int rows, double t) {
    if (cols < 30 || rows < 8) return;

    /* Walk newest → oldest. event_head points at the next *write* slot,
     * so the newest record is at event_head-1. */
    int row = 3;
    for (int n = 0; n < EVENT_CAP; n++) {
        int idx = (event_head - 1 - n + EVENT_CAP) % EVENT_CAP;
        const Event *e = &events[idx];
        if (e->text[0] == 0) continue;
        double age = t - e->t;
        if (age > EVENT_TTL) continue;

        /* Fade brightness with age — fresh events read brighter. */
        float k = 1.0f - (float)(age / EVENT_TTL);
        if (k < 0.25f) k = 0.25f;
        int r = (int)(HUD_R * k);
        int g = (int)(HUD_G * k);
        int b = (int)(HUD_B * k);
        set_fg(out, r, g, b);
        fprintf(out, "\x1b[%d;1H", row);
        fprintf(out, "\xE2\x80\xA2 %s", e->text);  /* "• " bullet */
        row++;
    }
}

static int select_scan_target(const BodySystem *bs, double t) {
    if (bs->count <= 0) return -1;
    int i = ((int)(t / SCAN_DWELL)) % bs->count;
    return i;
}

static void hud_scan_readout(FILE *out, int cols,
                             const BodySystem *bs,
                             float cam_x, float cam_y, double t) {
    const int W = 26;
    if (cols < W + 2) return;
    int sel = select_scan_target(bs, t);
    if (sel < 0) return;

    const Body *b = &bs->bodies[sel];
    char id[16];
    format_label(id, sizeof id, bs, sel);

    float dx = b->wx - cam_x;
    float dy = b->wy - cam_y;
    float dist = sqrtf(dx * dx + dy * dy);
    float speed = sqrtf(b->vx * b->vx + b->vy * b->vy);

    int x0 = cols - W + 1; /* 1-indexed */
    set_fg(out, HUD_R, HUD_G, HUD_B);

    fprintf(out, "\x1b[1;%dH", x0);
    fprintf(out, "[ SCAN \xE2\x80\xBA %-16s ]", id);

    fprintf(out, "\x1b[2;%dH", x0);
    fprintf(out, "  KIND  %-18s",  kind_long(b->kind));

    fprintf(out, "\x1b[3;%dH", x0);
    fprintf(out, "  MASS  %-18.2e",  (double)b->mass);

    fprintf(out, "\x1b[4;%dH", x0);
    fprintf(out, "  VEL   %-18.2f",  (double)speed);

    fprintf(out, "\x1b[5;%dH", x0);
    fprintf(out, "  RNG   %-18.0f",  (double)dist);
}

static void hud_body_labels(FILE *out, int cols, int rows,
                            const BodySystem *bs,
                            float cam_x, float cam_y) {
    /* Reserve the corners for fixed widgets — don't draw labels there. */
    const int top_left_w  = 36;          /* sector header */
    const int top_right_w = 27;          /* scan readout */
    const int bot_right_w = 16;          /* audio meter (with margin) */
    const int bot_right_h = 5;

    /* World-space anchor: the central (heaviest, index 0) body is the
     * system origin. Fading by orbital depth from there reads as diegetic
     * — an outer companion stays dim even as the camera drifts past it. */
    float anchor_x = (bs->count > 0) ? bs->bodies[0].wx : cam_x;
    float anchor_y = (bs->count > 0) ? bs->bodies[0].wy : cam_y;
    float r_max_world = 0.0f;
    for (int i = 0; i < bs->count; i++) {
        float dx = bs->bodies[i].wx - anchor_x;
        float dy = bs->bodies[i].wy - anchor_y;
        float r2 = dx * dx + dy * dy;
        if (r2 > r_max_world) r_max_world = r2;
    }
    r_max_world = sqrtf(r_max_world);

    for (int i = 0; i < bs->count; i++) {
        const Body *b = &bs->bodies[i];

        /* Place label one cell up-right of the body centre. */
        float sub_x = b->wx - cam_x;
        float sub_y = b->wy - cam_y;
        int   cx    = (int)(sub_x / 2.0f) + 2;
        int   cy    = (int)(sub_y / 4.0f);

        if (cx < 1 || cy < 2 || cx > cols - 8 || cy > rows - 1) continue;

        /* Reserved-zone clipping. */
        if (cy == 1 && cx <= top_left_w)            continue;
        if (cy <= 5 && cx >= cols - top_right_w)    continue;
        if (cy >= rows - bot_right_h
            && cx >= cols - bot_right_w)            continue;

        /* Fade by world-space orbital radius from the central body. */
        float wdx = b->wx - anchor_x;
        float wdy = b->wy - anchor_y;
        float r   = sqrtf(wdx * wdx + wdy * wdy);
        float k = 1.0f - (r / (r_max_world + 1.0f)) * 0.55f;
        if (k < 0.3f) k = 0.3f;

        int rr = (int)(HUD_R * k);
        int gg = (int)(HUD_G * k);
        int bb = (int)(HUD_B * k);
        set_fg(out, rr, gg, bb);

        char label[16];
        format_label(label, sizeof label, bs, i);
        fprintf(out, "\x1b[%d;%dH%s", cy, cx, label);
    }
}

static void hud_band_meters(FILE *out, int cols, int rows,
                            const AudioSnapshot *snap) {
    const int W = 15, H = 4;
    if (cols < W + 2 || rows < H + 2) return;

    int x0 = cols - W;
    int y0 = rows - H + 1;

    set_fg(out, HUD_R, HUD_G, HUD_B);

    fprintf(out, "\x1b[%d;%dH", y0, x0);
    fputs("\xE2\x95\xAD AUDIO "
          "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
          "\xE2\x95\xAE", out);

    fprintf(out, "\x1b[%d;%dH", y0 + 1, x0);
    fputs("\xE2\x94\x82 ", out);
    for (int i = 0; i < BAND_COUNT; i++) {
        float v = snap->bands[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        int idx = (int)(v * 8.0f + 0.5f);
        if (idx > 8) idx = 8;
        fputs(BAR_LEVELS[idx], out);
        fputc(' ', out);
    }
    fputs("\xE2\x94\x82", out);

    fprintf(out, "\x1b[%d;%dH", y0 + 2, x0);
    fputs("\xE2\x94\x82 S B L M H T", out);
    if (snap->transient > 0.25f) {
        set_fg(out, ALERT_R, ALERT_G, ALERT_B);
        fputs("\xE2\x97\x8F", out);
        set_fg(out, HUD_R, HUD_G, HUD_B);
    } else {
        fputc(' ', out);
    }
    fputs("\xE2\x94\x82", out);

    fprintf(out, "\x1b[%d;%dH", y0 + 3, x0);
    fputs("\xE2\x95\xB0"
          "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
          "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
          "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"
          "\xE2\x95\xAF", out);
}

void hud_draw(FILE *out, int cols, int rows,
              const BodySystem *bs,
              const AudioSnapshot *snap,
              float cam_x, float cam_y, double t,
              int astro_mode) {
    if (!snap) return;

    if (snap->transient > TRANSIENT_LOG_TH
        && snap->transient < SUPERNOVA_THRESHOLD) {
        char msg[28];
        snprintf(msg, sizeof msg, "TRANSIENT %02d%%",
                 (int)(snap->transient * 100.0f));
        event_push(t, msg);
    }

    if (!astro_mode) {
        hud_topleft(out, cols, cam_x, cam_y, t);
        if (bs) {
            hud_scan_readout(out, cols, bs, cam_x, cam_y, t);
            hud_body_labels(out, cols, rows, bs, cam_x, cam_y);
        }
    }
    hud_event_log (out, cols, rows, t);
    hud_band_meters(out, cols, rows, snap);
    fputs("\x1b[0m", out);
}
