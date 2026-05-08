#ifndef VOIDWATCH_TERM_H
#define VOIDWATCH_TERM_H

void term_setup(void);
void term_teardown(void);
void term_get_size(int *cols, int *rows);

/* Returns 1 once when SIGWINCH has fired since the last call. */
int  term_consume_resize(void);

/* Returns 1 if SIGINT/SIGTERM fired since startup. Does NOT consume stdin. */
int  term_should_quit(void);

/*
 * Non-blocking poll for one keypress. Returns the byte read (1..255) or
 * 0 if no key is queued. ESC is reported as 27. Escape sequences (CSI,
 * SGR mouse) are consumed internally — they do not surface as raw bytes.
 */
int  term_poll_key(void);

/*
 * Non-blocking poll for one mouse click. Returns 1 if a left-button
 * press was decoded since the last call and writes 1-indexed terminal
 * coordinates into *out_col / *out_row. Returns 0 otherwise.
 *
 * Mouse reporting is enabled in term_setup via SGR mode (DEC 1000 +
 * 1006). Releases and right/middle clicks are silently dropped — only
 * left presses surface.
 */
int  term_poll_mouse(int *out_col, int *out_row);

#endif
