/*
 * term.h - what the terminal around calci can do.
 */
#ifndef TERM_H
#define TERM_H

#include <stdbool.h>
#include <stdio.h>

/* Tells whether `stream` is a terminal rather than a file or a pipe. */
bool term_is_terminal(FILE *stream);

/* Tells whether ANSI escape sequences, for cursor control and the like, may
 * be written to `stream`, which must be stdout or stderr: it has to be a
 * terminal whose TERM is not "dumb". On Windows this also switches the
 * console to interpret the sequences (Windows 10 and later can), and back
 * again at exit. */
bool term_supports_ansi(FILE *stream);

/* Tells whether `stream` may be colored: term_supports_ansi(stream) holds and
 * the user has not asked for no color by setting NO_COLOR (no-color.org). */
bool term_supports_color(FILE *stream);

#endif /* TERM_H */
