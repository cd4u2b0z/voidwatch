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

int term_poll_key(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) return (int)c;
    return 0;
}
