/*
 * editor.h - interactive line input with editing, history and completion.
 *
 * On POSIX terminals the editor handles the keys itself while a line is
 * being typed: arrows, Home and End, history, Tab completion and the usual
 * Ctrl shortcuts. Where editor_supported() is false, notably in the Windows
 * console, which has its own line editing and history, callers read lines
 * the ordinary way.
 */
#ifndef EDITOR_H
#define EDITOR_H

#include <stdbool.h>
#include <stddef.h>

typedef enum editor_status {
    EDITOR_LINE,      /* a line was read */
    EDITOR_CANCELLED, /* Ctrl+C abandoned the line */
    EDITOR_EOF,       /* Ctrl+D on an empty line, or the end of input */
    EDITOR_ERROR      /* the terminal could not be used */
} editor_status;

/* Returns the index-th completion of the `length` bytes at `word`, which
 * start a name, or NULL when there are no more. A completion is the whole
 * name, possibly with a suffix to insert along with it, such as "(". The
 * returned strings must stay valid while the program runs. */
typedef const char *(*editor_completer)(const char *word, size_t length,
                                        size_t index);

/* Tells whether line editing is available: standard input and standard
 * error are both terminals, and TERM names one that is not "dumb". */
bool editor_supported(void);

/* Shows `prompt` on standard error, which may contain ANSI color sequences,
 * and reads a line of at most size - 1 bytes into `buffer`, without the line
 * terminator. On anything but EDITOR_LINE, `buffer` is left empty. */
editor_status editor_read_line(const char *prompt, char *buffer, size_t size);

/* Remembers `line` for recall with the Up key, unless it is blank or the
 * same as the previous one. */
void editor_add_history(const char *line);

/* Sets the function that Tab uses to complete names, or NULL for none. */
void editor_set_completer(editor_completer completer);

#endif /* EDITOR_H */
