#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

static struct termios       orig_termios;
static volatile sig_atomic_t resized    = 1;
static volatile sig_atomic_t quit_flag  = 0;
static int                  termios_saved = 0;

/* Input ring: read_byte / peek_byte share a single buffer so escape-
 * sequence parsing can lookahead one byte without committing. The buffer
 * is refilled lazily — when in_cur catches in_len, one read() pulls
 * whatever's queued (up to 256 bytes). With VMIN=0/VTIME=0 this is
 * non-blocking. */
static unsigned char inbuf[256];
static int           in_len = 0;
static int           in_cur = 0;

static int pending_mouse = 0;
static int pending_mouse_col = 0;
static int pending_mouse_row = 0;

static void on_winch(int sig) { (void)sig; resized = 1; }
static void on_quit(int sig)  { (void)sig; quit_flag = 1; }

void term_get_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

void term_setup(void) {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == 0) {
        termios_saved = 1;
        struct termios raw = orig_termios;
        raw.c_lflag    &= ~(unsigned)(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }

    fputs("\x1b[?25l",   stdout); /* hide cursor      */
    fputs("\x1b[?1049h", stdout); /* alt screen       */
    fputs("\x1b[2J",     stdout); /* clear            */
    /* Mouse: any-button press/release (1000) + SGR encoding (1006).
     * 1006 lets us parse plain ASCII numerics instead of clamped legacy
     * 0xFF bytes — and works for terminals beyond 80×24. */
    fputs("\x1b[?1000h", stdout);
    fputs("\x1b[?1006h", stdout);
    fflush(stdout);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_winch;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = on_quit;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

void term_teardown(void) {
    fputs("\x1b[?1006l", stdout);
    fputs("\x1b[?1000l", stdout);
    fputs("\x1b[0m",     stdout);
    fputs("\x1b[?1049l", stdout);
    fputs("\x1b[?25h",   stdout);
    fflush(stdout);
    if (termios_saved) tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

int term_consume_resize(void) {
    int r = resized;
    resized = 0;
    return r;
}

int term_should_quit(void) {
    return quit_flag;
}

/* ---- Input plumbing ---------------------------------------------------- */

static int read_byte(void) {
    if (in_cur >= in_len) {
        ssize_t n = read(STDIN_FILENO, inbuf, sizeof inbuf);
        in_len = (n > 0) ? (int)n : 0;
        in_cur = 0;
    }
    if (in_cur < in_len) return inbuf[in_cur++];
    return -1;
}

static int peek_byte(void) {
    if (in_cur >= in_len) {
        ssize_t n = read(STDIN_FILENO, inbuf, sizeof inbuf);
        in_len = (n > 0) ? (int)n : 0;
        in_cur = 0;
    }
    return (in_cur < in_len) ? inbuf[in_cur] : -1;
}

/* Parse an SGR mouse sequence after the leading "\e[<" has been consumed.
 * Format: <btn>;<col>;<row>(M|m). M = press, m = release. We only stash
 * left-button presses (btn & 0x03) == 0. Modifier bits in btn (Shift=4,
 * Meta=8, Ctrl=16) are ignored. */
static void parse_sgr_mouse(void) {
    int fields[3] = {0, 0, 0};
    int fi = 0;
    int ch;
    int final = 0;
    while ((ch = read_byte()) >= 0) {
        if (ch >= '0' && ch <= '9') {
            if (fi < 3) fields[fi] = fields[fi] * 10 + (ch - '0');
        } else if (ch == ';') {
            if (fi < 2) fi++;
        } else if (ch == 'M' || ch == 'm') {
            final = ch;
            break;
        } else {
            /* malformed — bail without committing */
            return;
        }
    }
    if (final == 'M' && (fields[0] & 0x03) == 0) {
        pending_mouse     = 1;
        pending_mouse_col = fields[1];
        pending_mouse_row = fields[2];
    }
}

int term_poll_key(void) {
    for (;;) {
        int b = read_byte();
        if (b < 0) return 0;

        if (b != 0x1b) return b;

        /* Esc: peek next. Bare Esc returns 27. */
        int n = peek_byte();
        if (n < 0)   return 27;
        if (n != '[') {
            /* Some other Esc-prefixed thing (Alt+key etc.) — surface
             * the Esc and let the next call see the rest. Production
             * doesn't use Alt-combos. */
            return 27;
        }

        /* CSI — consume '[' and inspect what follows. */
        (void)read_byte();
        int x = peek_byte();
        if (x == '<') {
            (void)read_byte();
            parse_sgr_mouse();
            continue;     /* event consumed; look for the next thing */
        }

        /* Unknown CSI: swallow until the final byte (0x40..0x7E) so we
         * don't dribble it out as fake key presses. */
        int ch;
        while ((ch = read_byte()) >= 0) {
            if (ch >= 0x40 && ch <= 0x7E) break;
        }
    }
}

int term_poll_mouse(int *out_col, int *out_row) {
    if (!pending_mouse) return 0;
    if (out_col) *out_col = pending_mouse_col;
    if (out_row) *out_row = pending_mouse_row;
    pending_mouse = 0;
    return 1;
}
