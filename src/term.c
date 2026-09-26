/*
 * term.c - terminal detection and, on Windows, console setup.
 */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS /* getenv is used safely */
#define WIN32_LEAN_AND_MEAN
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* for fileno() and isatty() */
#endif

#include "term.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifdef _MSC_VER
#pragma warning(push, 0) /* system headers are not all clean at /W4 */
#endif
#include <io.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#define fileno _fileno
#define isatty _isatty
#else
#include <unistd.h>
#endif

bool term_is_terminal(FILE *stream)
{
    return isatty(fileno(stream)) != 0;
}

static bool term_is_dumb(void)
{
    const char *term = getenv("TERM");

#ifdef _WIN32
    /* The Windows console does not set TERM; other terminals may. */
    return term != NULL && strcmp(term, "dumb") == 0;
#else
    return term == NULL || term[0] == '\0' || strcmp(term, "dumb") == 0;
#endif
}

#ifdef _WIN32

/* The console modes of stdout and stderr before calci changed them. */
static struct {
    HANDLE handle;
    DWORD mode;
    bool changed;
} original[2];

static void restore_console_modes(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (original[i].changed)
            SetConsoleMode(original[i].handle, original[i].mode);
    }
}

/* Makes the console behind `stream` interpret ANSI escape sequences, and
 * tells whether it does. */
static bool enable_virtual_terminal(FILE *stream)
{
    static bool registered = false;
    const int which = stream == stderr ? 1 : 0;
    const HANDLE handle =
        GetStdHandle(which == 1 ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;

    if (handle == NULL || handle == INVALID_HANDLE_VALUE ||
        !GetConsoleMode(handle, &mode))
        return false;
    if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
        return true;
    if (!SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        return false; /* a console older than Windows 10 */

    if (!original[which].changed) {
        original[which].handle = handle;
        original[which].mode = mode;
        original[which].changed = true;
    }
    if (!registered) {
        registered = atexit(restore_console_modes) == 0;
    }
    return true;
}

#endif /* _WIN32 */

bool term_supports_ansi(FILE *stream)
{
    if (!term_is_terminal(stream) || term_is_dumb())
        return false;
#ifdef _WIN32
    return enable_virtual_terminal(stream);
#else
    return true;
#endif
}

bool term_supports_color(FILE *stream)
{
    const char *no_color = getenv("NO_COLOR");

    if (no_color != NULL && no_color[0] != '\0')
        return false;
    return term_supports_ansi(stream);
}
