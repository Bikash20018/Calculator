/*
 * editor.c - a small line editor in the spirit of readline, with no
 * dependencies beyond POSIX.
 *
 * While a line is being typed the terminal is in raw mode, so every key
 * arrives here as it is pressed and the echoing happens here too. The
 * original mode comes back after every line, at exit, around Ctrl+Z, and
 * before the process dies of a signal. The line takes a single row and
 * scrolls sideways when it is wider than the terminal.
 */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE /* for TIOCGWINSZ */
#endif
#endif

#include "editor.h"

#ifdef _WIN32

/* The Windows console edits lines itself, history included. */

bool editor_supported(void)
{
    return false;
}

editor_status editor_read_line(const char *prompt, char *buffer, size_t size)
{
    (void)prompt;
    if (buffer != NULL && size > 0)
        buffer[0] = '\0';
    return EDITOR_ERROR;
}

void editor_add_history(const char *line)
{
    (void)line;
}

void editor_set_completer(editor_completer completer)
{
    (void)completer;
}

#else /* POSIX */

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define HISTORY_SIZE 100

/* How long to wait for the rest of an escape sequence before deciding that
 * the Escape key was pressed on its own. */
#define ESCAPE_TIMEOUT_MS 50

enum key {
    KEY_CTRL_A = 1,
    KEY_CTRL_B = 2,
    KEY_CTRL_C = 3,
    KEY_CTRL_D = 4,
    KEY_CTRL_E = 5,
    KEY_CTRL_F = 6,
    KEY_CTRL_H = 8,
    KEY_TAB = 9,
    KEY_LINE_FEED = 10,
    KEY_CTRL_K = 11,
    KEY_CTRL_L = 12,
    KEY_ENTER = 13,
    KEY_CTRL_N = 14,
    KEY_CTRL_P = 16,
    KEY_CTRL_U = 21,
    KEY_CTRL_W = 23,
    KEY_CTRL_Z = 26,
    KEY_ESCAPE = 27,
    KEY_BACKSPACE = 127,

    /* Keys that arrive as escape sequences, and pseudo-keys. */
    KEY_UP = 256,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_WORD_LEFT,
    KEY_WORD_RIGHT,
    KEY_DELETE_WORD,
    KEY_UNKNOWN,
    KEY_EOF
};

typedef struct line {
    char *text;           /* what has been typed, NUL-terminated */
    size_t size;          /* bytes available at text */
    size_t length;        /* bytes typed */
    size_t cursor;        /* byte offset of the cursor, on a character */
    const char *prompt;
    size_t prompt_width;  /* columns the prompt takes */
    size_t history_index; /* 0 for the new line, n for the n-th latest */
    char *draft;          /* the new line, kept while browsing history */
} line;

static struct termios original_mode;
static volatile sig_atomic_t in_raw_mode = 0;
static bool set_up = false;

static char *history[HISTORY_SIZE];
static size_t history_count = 0;

static editor_completer completer = NULL;

/* ------------------------------------------------------ terminal modes */

static void leave_raw_mode(void)
{
    if (in_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_mode);
        in_raw_mode = 0;
    }
}

/* Puts the terminal back as it was, then dies of `signal_number` as the
 * process would have without this handler. */
static void on_fatal_signal(int signal_number)
{
    leave_raw_mode();
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void free_history(void)
{
    while (history_count > 0)
        free(history[--history_count]);
}

static void set_up_once(void)
{
    static const int fatal_signals[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
    struct sigaction action;
    size_t i;

    if (set_up)
        return;
    set_up = true;
    atexit(free_history);
    atexit(leave_raw_mode);

    memset(&action, 0, sizeof action);
    action.sa_handler = on_fatal_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    for (i = 0; i < sizeof fatal_signals / sizeof fatal_signals[0]; i++) {
        struct sigaction previous;

        /* Signals that were ignored, as under nohup, stay ignored. */
        if (sigaction(fatal_signals[i], NULL, &previous) == 0 &&
            previous.sa_handler != SIG_IGN)
            sigaction(fatal_signals[i], &action, NULL);
    }
}

static bool enter_raw_mode(void)
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &original_mode) == -1)
        return false;
    raw = original_mode;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    /* Flag first, so that a signal arriving meanwhile still restores the
     * mode. TCSANOW, because TCSAFLUSH would drop keys typed ahead, and it
     * and TCSADRAIN wait for output to drain, which on some systems (macOS)
     * means waiting for the terminal to read it: they could hang. */
    in_raw_mode = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1) {
        in_raw_mode = 0;
        return false;
    }
    return true;
}

/* -------------------------------------------------------------- output */

static void write_bytes(const char *data, size_t length)
{
    while (length > 0) {
        const ssize_t written = write(STDERR_FILENO, data, length);

        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return;
        data += written;
        length -= (size_t)written;
    }
}

static void write_text(const char *text)
{
    write_bytes(text, strlen(text));
}

static void beep(void)
{
    write_text("\a");
}

/* A growing buffer, so that a redraw reaches the terminal in one write. */
typedef struct output {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} output;

static void append(output *out, const char *data, size_t length)
{
    if (out->failed)
        return;
    if (out->length + length > out->capacity) {
        const size_t capacity = 2 * out->capacity + length + 64;
        char *grown = realloc(out->data, capacity);

        if (grown == NULL) {
            out->failed = true;
            return;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    memcpy(out->data + out->length, data, length);
    out->length += length;
}

static size_t terminal_width(void)
{
#ifdef TIOCGWINSZ
    struct winsize size;

    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return size.ws_col;
#endif
    return 80;
}

/* ------------------------------------------------------------ UTF-8 text */

static bool is_continuation(char c)
{
    return ((unsigned char)c & 0xC0) == 0x80;
}

/* Where the character before the one at `pos` starts. */
static size_t previous_char(const char *text, size_t pos)
{
    if (pos == 0)
        return 0;
    do
        pos--;
    while (pos > 0 && is_continuation(text[pos]));
    return pos;
}

/* Where the character after the one at `pos` starts. */
static size_t next_char(const char *text, size_t length, size_t pos)
{
    if (pos >= length)
        return length;
    do
        pos++;
    while (pos < length && is_continuation(text[pos]));
    return pos;
}

/* Columns taken by the `length` bytes at `text`, one per character. */
static size_t columns(const char *text, size_t length)
{
    size_t count = 0;
    size_t i;

    for (i = 0; i < length; i++) {
        if (!is_continuation(text[i]))
            count++;
    }
    return count;
}

/* Columns taken by `text` on screen, where escape sequences take none. */
static size_t display_width(const char *text)
{
    size_t width = 0;

    while (*text != '\0') {
        if (text[0] == '\033' && text[1] == '[') {
            text += 2;
            while (*text != '\0' && (*text < '@' || *text > '~'))
                text++;
            if (*text != '\0')
                text++;
        } else {
            if (!is_continuation(*text))
                width++;
            text++;
        }
    }
    return width;
}

/* Numbers and names are words; blanks, operators and parentheses are not. */
static bool is_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '.' ||
           (unsigned char)c >= 0x80;
}

static bool is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* ------------------------------------------------------------- drawing */

/* Draws the prompt and the line. Normally only the part that fits is shown,
 * scrolled so that the cursor is in view. With `whole`, used when the line
 * is accepted, all of it is written out, wrapping as long as it is. */
static void redraw(const line *ln, bool whole)
{
    const size_t width = terminal_width();
    const size_t room =
        width > ln->prompt_width + 1 ? width - ln->prompt_width - 1 : 1;
    size_t first = 0;
    size_t last = ln->length;
    output out = { NULL, 0, 0, false };

    if (!whole) {
        size_t before = columns(ln->text, ln->cursor);
        size_t shown = 0;

        for (; before > room; before--)
            first = next_char(ln->text, ln->length, first);
        for (last = first; last < ln->length && shown < room; shown++)
            last = next_char(ln->text, ln->length, last);
    }

    append(&out, "\r", 1);
    append(&out, ln->prompt, strlen(ln->prompt));
    append(&out, ln->text + first, last - first);
    if (!whole) {
        const size_t cursor_column =
            ln->prompt_width + columns(ln->text + first, ln->cursor - first);
        char move[32];

        append(&out, "\033[0K\r", 5); /* clear the rest, back to column 0 */
        if (cursor_column > 0) {
            snprintf(move, sizeof move, "\033[%luC",
                     (unsigned long)cursor_column);
            append(&out, move, strlen(move));
        }
    }
    if (!out.failed)
        write_bytes(out.data, out.length);
    free(out.data);
}

/* ------------------------------------------------------------- editing */

static void insert(line *ln, const char *text, size_t length)
{
    if (ln->length + length >= ln->size) {
        beep();
        return;
    }
    memmove(ln->text + ln->cursor + length, ln->text + ln->cursor,
            ln->length - ln->cursor + 1);
    memcpy(ln->text + ln->cursor, text, length);
    ln->length += length;
    ln->cursor += length;
}

/* Deletes the bytes from `from` to `to`, and leaves the cursor at `from`. */
static void delete_range(line *ln, size_t from, size_t to)
{
    if (from >= to) {
        beep();
        return;
    }
    memmove(ln->text + from, ln->text + to, ln->length - to + 1);
    ln->length -= to - from;
    ln->cursor = from;
}

/* Replaces the whole line with `text`, cut short if need be. */
static void replace_line(line *ln, const char *text)
{
    size_t length = strlen(text);

    if (length >= ln->size) {
        length = ln->size - 1;
        while (length > 0 && is_continuation(text[length]))
            length--;
    }
    memcpy(ln->text, text, length);
    ln->text[length] = '\0';
    ln->length = length;
    ln->cursor = length;
}

static size_t word_start(const line *ln, size_t pos)
{
    while (pos > 0 && !is_word_char(ln->text[pos - 1]))
        pos--;
    while (pos > 0 && is_word_char(ln->text[pos - 1]))
        pos--;
    return pos;
}

static size_t word_end(const line *ln, size_t pos)
{
    while (pos < ln->length && !is_word_char(ln->text[pos]))
        pos++;
    while (pos < ln->length && is_word_char(ln->text[pos]))
        pos++;
    return pos;
}

/* Moves through the history: `older` towards the first entry, otherwise
 * back towards the line being typed, which is kept aside meanwhile. */
static void browse_history(line *ln, bool older)
{
    if (older ? ln->history_index >= history_count : ln->history_index == 0) {
        beep();
        return;
    }
    if (ln->history_index == 0) {
        free(ln->draft);
        ln->draft = malloc(ln->length + 1);
        if (ln->draft != NULL)
            memcpy(ln->draft, ln->text, ln->length + 1);
    }
    ln->history_index = older ? ln->history_index + 1 : ln->history_index - 1;
    if (ln->history_index == 0)
        replace_line(ln, ln->draft != NULL ? ln->draft : "");
    else
        replace_line(ln, history[history_count - ln->history_index]);
}

/* Completes the name before the cursor as far as all the candidates agree,
 * and lists them if that is not far enough to settle it. */
static void complete(line *ln)
{
    const size_t cursor = ln->cursor;
    size_t start = cursor;
    const char *first = NULL;
    const char *match;
    size_t count = 0;
    size_t common = 0;
    size_t i;

    while (start > 0 && is_name_char(ln->text[start - 1]))
        start--;
    if (completer == NULL || start == cursor ||
        isdigit((unsigned char)ln->text[start])) {
        beep();
        return;
    }

    for (i = 0; (match = completer(ln->text + start, cursor - start, i));
         i++) {
        if (first == NULL) {
            first = match;
            common = strlen(match);
        } else {
            size_t j = 0;

            while (j < common && first[j] == match[j])
                j++;
            common = j;
        }
        count++;
    }

    if (count == 0) {
        beep();
    } else if (common > cursor - start) {
        /* Retype the word too, which fixes its case: "SQ" becomes "sqrt(". */
        delete_range(ln, start, cursor);
        insert(ln, first, common);
    } else if (count == 1) {
        beep();
    } else {
        write_text("\n");
        for (i = 0; (match = completer(ln->text + start, cursor - start, i));
             i++) {
            size_t length = strlen(match);

            if (length > 0 && match[length - 1] == '(')
                length--;
            write_bytes(match, length);
            write_text("  ");
        }
        write_text("\n");
    }
}

/* Ctrl+Z: stops like any program would, and carries on when resumed. */
static bool suspend(void)
{
    write_text("^Z");
    leave_raw_mode();
    raise(SIGTSTP);
    return enter_raw_mode();
}

/* ------------------------------------------------------------- input */

/* Reads a byte; -1 at the end of input or on an error. */
static int read_byte(void)
{
    unsigned char c;

    for (;;) {
        const ssize_t n = read(STDIN_FILENO, &c, 1);

        if (n == 1)
            return c;
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
}

/* Reads a byte if one arrives within ESCAPE_TIMEOUT_MS; -1 otherwise. */
static int read_byte_soon(void)
{
    struct pollfd input;

    input.fd = STDIN_FILENO;
    input.events = POLLIN;
    input.revents = 0;
    if (poll(&input, 1, ESCAPE_TIMEOUT_MS) <= 0)
        return -1;
    return read_byte();
}

/* Decodes "ESC [ parameters final", the form most keys take. A modifier
 * parameter, as in "1;5C" for Ctrl+Right, turns arrows into word moves. */
static int decode_csi(const char *parameters, int final)
{
    const bool modified = strchr(parameters, ';') != NULL;

    switch (final) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return modified ? KEY_WORD_RIGHT : KEY_RIGHT;
    case 'D': return modified ? KEY_WORD_LEFT : KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '~':
        switch (atoi(parameters)) {
        case 1: case 7: return KEY_HOME;
        case 4: case 8: return KEY_END;
        case 3: return KEY_DELETE;
        default: return KEY_UNKNOWN;
        }
    default:
        return KEY_UNKNOWN;
    }
}

/* Reads a key press: a byte, or one of the KEY_ codes above 255. */
static int read_key(void)
{
    char parameters[16];
    size_t count = 0;
    int c = read_byte();

    if (c < 0)
        return KEY_EOF;
    if (c != KEY_ESCAPE)
        return c;

    c = read_byte_soon();
    switch (c) {
    case 'b':
        return KEY_WORD_LEFT; /* Alt+B */
    case 'f':
        return KEY_WORD_RIGHT; /* Alt+F */
    case KEY_BACKSPACE:
    case KEY_CTRL_H:
        return KEY_DELETE_WORD; /* Alt+Backspace */
    case 'O':
        switch (read_byte_soon()) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        default: return KEY_UNKNOWN;
        }
    case '[':
        for (;;) {
            c = read_byte_soon();
            if (c < 0)
                return KEY_UNKNOWN;
            if (c >= '@' && c <= '~')
                break;
            if (count < sizeof parameters - 1)
                parameters[count++] = (char)c;
        }
        parameters[count] = '\0';
        return decode_csi(parameters, c);
    default:
        return KEY_UNKNOWN; /* Escape on its own, or a sequence unknown */
    }
}

/* Inserts the character that starts with the byte `lead`, reading the rest
 * of it first if it is a multibyte UTF-8 character. */
static void insert_char(line *ln, int lead)
{
    char bytes[4];
    const size_t expected = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                          : lead >= 0xC0 ? 2 : 1;
    size_t count = 1;

    bytes[0] = (char)lead;
    while (count < expected) {
        const int c = read_byte_soon();

        if (c < 0 || (c & 0xC0) != 0x80)
            return; /* a broken character: drop it */
        bytes[count++] = (char)c;
    }
    insert(ln, bytes, count);
}

/* ---------------------------------------------------------- public API */

bool editor_supported(void)
{
    const char *term = getenv("TERM");

    return isatty(STDIN_FILENO) && isatty(STDERR_FILENO) && term != NULL &&
           term[0] != '\0' && strcmp(term, "dumb") != 0;
}

editor_status editor_read_line(const char *prompt, char *buffer, size_t size)
{
    editor_status status = EDITOR_LINE;
    bool done = false;
    line ln;

    if (buffer == NULL || size < 2)
        return EDITOR_ERROR;
    buffer[0] = '\0';
    fflush(stdout);
    fflush(stderr);
    set_up_once();
    if (!enter_raw_mode())
        return EDITOR_ERROR;

    ln.text = buffer;
    ln.size = size;
    ln.length = 0;
    ln.cursor = 0;
    ln.prompt = prompt != NULL ? prompt : "";
    ln.prompt_width = display_width(ln.prompt);
    ln.history_index = 0;
    ln.draft = NULL;
    redraw(&ln, false);

    while (!done) {
        const int key = read_key();
        bool changed = true;

        switch (key) {
        case KEY_ENTER:
        case KEY_LINE_FEED:
            redraw(&ln, true);
            write_text("\n");
            done = true;
            changed = false;
            break;
        case KEY_CTRL_C:
            write_text("^C\n");
            status = EDITOR_CANCELLED;
            done = true;
            changed = false;
            break;
        case KEY_EOF:
            status = EDITOR_EOF;
            done = true;
            changed = false;
            break;
        case KEY_CTRL_D:
            if (ln.length == 0) {
                status = EDITOR_EOF;
                done = true;
                changed = false;
            } else {
                delete_range(&ln, ln.cursor,
                             next_char(ln.text, ln.length, ln.cursor));
            }
            break;
        case KEY_BACKSPACE:
        case KEY_CTRL_H:
            delete_range(&ln, previous_char(ln.text, ln.cursor), ln.cursor);
            break;
        case KEY_DELETE:
            delete_range(&ln, ln.cursor,
                         next_char(ln.text, ln.length, ln.cursor));
            break;
        case KEY_LEFT:
        case KEY_CTRL_B:
            ln.cursor = previous_char(ln.text, ln.cursor);
            break;
        case KEY_RIGHT:
        case KEY_CTRL_F:
            ln.cursor = next_char(ln.text, ln.length, ln.cursor);
            break;
        case KEY_HOME:
        case KEY_CTRL_A:
            ln.cursor = 0;
            break;
        case KEY_END:
        case KEY_CTRL_E:
            ln.cursor = ln.length;
            break;
        case KEY_WORD_LEFT:
            ln.cursor = word_start(&ln, ln.cursor);
            break;
        case KEY_WORD_RIGHT:
            ln.cursor = word_end(&ln, ln.cursor);
            break;
        case KEY_UP:
        case KEY_CTRL_P:
            browse_history(&ln, true);
            break;
        case KEY_DOWN:
        case KEY_CTRL_N:
            browse_history(&ln, false);
            break;
        case KEY_CTRL_K:
            delete_range(&ln, ln.cursor, ln.length);
            break;
        case KEY_CTRL_U:
            delete_range(&ln, 0, ln.cursor);
            break;
        case KEY_CTRL_W:
        case KEY_DELETE_WORD:
            delete_range(&ln, word_start(&ln, ln.cursor), ln.cursor);
            break;
        case KEY_CTRL_L:
            write_text("\033[H\033[2J");
            break;
        case KEY_CTRL_Z:
            if (!suspend()) {
                status = EDITOR_ERROR;
                done = true;
                changed = false;
            }
            break;
        case KEY_TAB:
            complete(&ln);
            break;
        default:
            if ((key >= ' ' && key < KEY_BACKSPACE) ||
                (key >= 0xC0 && key < 0xF8))
                insert_char(&ln, key);
            else
                changed = false; /* other control keys are ignored */
            break;
        }
        if (changed)
            redraw(&ln, false);
    }

    leave_raw_mode();
    free(ln.draft);
    if (status != EDITOR_LINE)
        buffer[0] = '\0';
    return status;
}

void editor_add_history(const char *text)
{
    const char *p = text;
    size_t length;
    char *copy;

    if (text == NULL)
        return;
    while (isspace((unsigned char)*p))
        p++;
    if (*p == '\0' ||
        (history_count > 0 && strcmp(history[history_count - 1], text) == 0))
        return;

    length = strlen(text) + 1;
    copy = malloc(length);
    if (copy == NULL)
        return;
    memcpy(copy, text, length);
    if (history_count == HISTORY_SIZE) {
        free(history[0]);
        memmove(history, history + 1, (HISTORY_SIZE - 1) * sizeof history[0]);
        history_count--;
    }
    history[history_count++] = copy;
}

void editor_set_completer(editor_completer function)
{
    completer = function;
}

#endif /* POSIX */
