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
 * 0 if no key is queued. ESC is reported as 27.
 */
int  term_poll_key(void);

#endif
