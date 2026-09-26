/*
 * main.c - the calci command: arguments, sessions, and how results and
 * errors are shown. The arithmetic lives in calc.c, line editing in
 * editor.c and terminal detection in term.c.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "calc.h"
#include "editor.h"
#include "term.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_NAME "calci"
#define VERSION "1.0.0"

/* Exit status for invalid command-line options. */
#define EXIT_USAGE 2

/* The prompt of an interactive session takes this many columns. */
#define PROMPT_WIDTH 2

/* Input lines may be up to LINE_SIZE - 1 bytes long. */
#define LINE_SIZE 4096

/* ANSI styles, used only where term_supports_color() allows. */
typedef struct style {
    const char *bold;
    const char *dim;
    const char *accent; /* the prompt and hints */
    const char *good;   /* results */
    const char *bad;    /* errors */
    const char *reset;
} style;

static const style colored = {
    "\033[1m", "\033[2m", "\033[1;36m", "\033[1;32m", "\033[1;31m", "\033[0m"
};
static const style plain = { "", "", "", "", "", "" };

/* The styles of standard output and standard error. */
static const style *out = &plain;
static const style *err = &plain;

typedef struct session {
    bool interactive;          /* reading from a terminal */
    bool editing;              /* with the line editor */
    bool results_to_terminal;  /* standard output is a terminal */
    bool can_clear;            /* the screen can be cleared */
    unsigned long line_number; /* current input line; 0 for arguments */
    double ans;                /* the previous result */
    bool failed;               /* an expression could not be evaluated */
} session;

/* ---------------------------------------------------------- help texts */

static void print_usage(FILE *f, const style *st)
{
    fprintf(f,
            "%sUsage:%s calci [EXPRESSION...]\n"
            "       calci --help | --version\n"
            "\n"
            "Evaluates arithmetic expressions.\n"
            "\n"
            "With an EXPRESSION, prints its value and exits. Quote the "
            "expression so\n"
            "that the shell leaves characters such as * and ( ) alone:\n"
            "\n"
            "  calci \"(2 + 3) * 4\"\n"
            "\n"
            "Without one, reads expressions from standard input, one per "
            "line: an\n"
            "interactive session on a terminal, otherwise a file or pipe of "
            "them.\n"
            "\n"
            "%sOptions:%s\n"
            "  -h, --help      show this help and exit\n"
            "  -v, --version   show the version and exit\n"
            "\n"
            "Terminals get colors unless NO_COLOR is set. The exit status is "
            "0 on\n"
            "success, 1 if an expression failed and 2 for an invalid "
            "option.\n"
            "\n",
            st->bold, st->reset, st->bold, st->reset);
}

static void print_operator(FILE *f, const style *st, const char *symbols,
                           const char *meaning, const char *example,
                           const char *value)
{
    fprintf(f, "  %-7s  %-28s  %s%-11s = %s%s\n", symbols, meaning, st->dim,
            example, value, st->reset);
}

/* Lists the names of the functions, or else of the constants, wrapped to
 * fit 76 columns after a 12-column heading. */
static void print_names(FILE *f, bool functions)
{
    const char *name;
    bool is_function = false;
    size_t column = 12;
    size_t i;

    for (i = 0; (name = calc_name(i, &is_function)) != NULL; i++) {
        const size_t width = strlen(name);

        if (is_function != functions)
            continue;
        if (column > 12 && column + 2 + width > 76) {
            fputs("\n            ", f);
            column = 12;
        }
        if (column > 12) {
            fputs("  ", f);
            column += 2;
        }
        fputs(name, f);
        column += width;
    }
    fputc('\n', f);
}

/* The syntax, and for a session also its commands and keys. */
static void print_reference(FILE *f, const style *st, bool in_session,
                            bool editing)
{
    fprintf(f, "%sOperators%s, from highest to lowest precedence:\n",
            st->bold, st->reset);
    print_operator(f, st, "( )", "grouping", "(2 + 3) * 4", "20");
    print_operator(f, st, "^", "power, right-associative", "2 ^ 3 ^ 2",
                   "512");
    print_operator(f, st, "+ -", "sign", "-2 ^ 2", "-4");
    print_operator(f, st, "* / %", "multiply, divide, remainder", "7 % 3",
                   "1");
    print_operator(f, st, "+ -", "add, subtract", "10 - 4 - 3", "3");
    fputc('\n', f);

    fprintf(f, "%sNumbers%s     42   3.14   .5   6.022e23\n", st->bold,
            st->reset);
    fprintf(f, "%sConstants%s   ", st->bold, st->reset);
    print_names(f, false);
    fprintf(f, "%sFunctions%s   ", st->bold, st->reset);
    print_names(f, true);
    fprintf(f,
            "            %sAngles are in radians; ans is the previous "
            "result.%s\n",
            st->dim, st->reset);

    if (in_session) {
        fprintf(f, "%sCommands%s    help  clear  quit\n", st->bold, st->reset);
        if (editing)
            fprintf(f,
                    "%sKeys%s        Up/Down history   Tab complete   "
                    "Ctrl+L clear   Ctrl+D quit\n",
                    st->bold, st->reset);
        else
            fprintf(f, "%sKeys%s        Up/Down history\n", st->bold,
                    st->reset);
    }
}

static void print_banner(void)
{
    fprintf(stderr, "%scalci%s %s%s - command-line calculator%s\n", err->bold,
            err->reset, err->dim, VERSION, err->reset);
    fprintf(stderr,
            "%sEnter an expression. Type \"help\" for the reference or "
            "\"quit\" to exit.%s\n\n",
            err->dim, err->reset);
}

/* ---------------------------------------------------------- diagnostics */

/* Tells whether the `length` bytes at `word` are `name`, ignoring case. */
static bool word_is(const char *word, size_t length, const char *name)
{
    size_t i;

    for (i = 0; i < length; i++) {
        if (name[i] == '\0' || tolower((unsigned char)word[i]) != name[i])
            return false;
    }
    return name[i] == '\0';
}

static bool is_known_name(const char *word, size_t length)
{
    const char *name;
    size_t i;

    for (i = 0; (name = calc_name(i, NULL)) != NULL; i++) {
        if (word_is(word, length, name))
            return true;
    }
    return false;
}

/* Advice on fixing the error in `text`, or NULL if there is none worth
 * giving. Advice that quotes the expression is built in `buffer`. */
static const char *hint_for(const char *text, calc_result r, char *buffer,
                            size_t size)
{
    const char *token = text + r.error_offset;
    const size_t length = r.error_length;
    const char *dot;
    size_t digits = 0;
    size_t start, end;

    switch (r.status) {
    case CALC_ERR_UNKNOWN_NAME: {
        const char *suggestion;

        if (word_is(token, length, "mod"))
            return "use % for the remainder, as in 7 % 3";
        if (word_is(token, length, "pow"))
            return "use ^ for powers, as in 2 ^ 10";
        suggestion = calc_suggest_name(token, length);
        if (suggestion == NULL)
            return NULL;
        snprintf(buffer, size, "did you mean %s?", suggestion);
        return buffer;
    }

    case CALC_ERR_EXPECTED_OPERATOR:
        return "use * to multiply";

    case CALC_ERR_INVALID_NUMBER:
        if (length >= 2 && token[0] == '0' &&
            tolower((unsigned char)token[1]) == 'x')
            return "only decimal numbers are supported";
        if (memchr(token, '_', length) != NULL)
            return "write numbers without separators, as in 1000000";
        dot = memchr(token, '.', length);
        if (dot != NULL && memchr(dot + 1, '.', length - (size_t)(dot + 1 - token)))
            return "a number has at most one decimal point";

        /* A number with a name stuck to it, as in "2pi" or "1e". */
        while (digits < length &&
               (isdigit((unsigned char)token[digits]) || token[digits] == '.'))
            digits++;
        if (digits == 0 || digits == length)
            return NULL;
        if (word_is(token + digits, length - digits, "e"))
            return "an exponent needs digits, as in 1e3";
        if (!is_known_name(token + digits, length - digits))
            return NULL;
        snprintf(buffer, size, "use * to multiply: %.*s * %.*s", (int)digits,
                 token, (int)(length - digits), token + digits);
        return buffer;

    case CALC_ERR_INVALID_CHAR:
        switch (*token) {
        case ',':
            return "use . as the decimal point";
        case '[': case ']': case '{': case '}':
            return "use ( and ) for grouping";
        case '=':
            return "= may only end the expression";
        default:
            return NULL;
        }

    case CALC_ERR_EXPECTED_LPAREN:
        /* The function's name is the word before the error. */
        end = r.error_offset;
        while (end > 0 && isspace((unsigned char)text[end - 1]))
            end--;
        start = end;
        while (start > 0 && (isalnum((unsigned char)text[start - 1]) ||
                             text[start - 1] == '_'))
            start--;
        if (length > 0 && isalnum((unsigned char)*token))
            snprintf(buffer, size, "write %.*s(%.*s)", (int)(end - start),
                     text + start, (int)length, token);
        else
            snprintf(buffer, size, "write %.*s(...)", (int)(end - start),
                     text + start);
        return buffer;

    case CALC_ERR_DOMAIN:
        if (*token == '^')
            return "for real cube roots, use cbrt(x)";
        return NULL;

    default:
        return NULL;
    }
}

static bool is_continuation(char c)
{
    return ((unsigned char)c & 0xC0) == 0x80;
}

/* Prints, from column `indent`, the marks that go under an erroneous
 * expression: '^' under the first character of the offending token, '~'
 * under the rest of it and of the construct around it, except for the
 * blanks on either side of the token, so that "8 / 0" gets "~ ^ ~". Tabs are
 * copied and UTF-8 continuation bytes skipped to keep the marks in line
 * with the text. Returns the column where the marks end. */
static size_t print_marks(FILE *f, const char *text, calc_result r,
                          size_t indent)
{
    const size_t length = strlen(text);
    const size_t token_end = r.error_offset + r.error_length;
    const size_t end = r.error_end > token_end ? r.error_end : token_end;
    size_t gap_start = r.error_offset;
    size_t gap_end = token_end;
    size_t column = indent;
    size_t i;

    while (gap_start > r.error_start &&
           isspace((unsigned char)text[gap_start - 1]))
        gap_start--;
    while (gap_end < end && isspace((unsigned char)text[gap_end]))
        gap_end++;

    fprintf(f, "%*s", (int)indent, "");
    for (i = 0; i <= r.error_offset || i < end; i++) {
        const char c = i < length ? text[i] : ' ';
        char mark = ' ';

        if (is_continuation(c))
            continue;
        if (i == r.error_offset)
            mark = '^';
        else if (i >= r.error_start && i < end &&
                 (i < gap_start || i >= gap_end ||
                  (i > r.error_offset && i < token_end)))
            mark = '~';
        if (c == '\t' && mark != '^')
            mark = '\t';
        putc(mark, f);
        column = mark == '\t' ? (column / 8 + 1) * 8 : column + 1;
    }
    return column;
}

/* Prints `text` with control characters replaced, so that they can
 * neither disturb the terminal nor shift the marks printed below. */
static void print_expression(FILE *f, const char *text)
{
    for (; *text != '\0'; text++) {
        const unsigned char c = (unsigned char)*text;

        putc((c < 0x20 && c != '\t') || c == 0x7f ? '?' : c, f);
    }
}

/* Starts an error message: "calci: line 3: error: ", or in an interactive
 * session just "error: ". */
static void begin_error(const session *s)
{
    fflush(stdout); /* keep results and errors in order in a shared log */
    if (!s->interactive) {
        fputs(PROGRAM_NAME ": ", stderr);
        if (s->line_number > 0)
            fprintf(stderr, "line %lu: ", s->line_number);
    }
    fprintf(stderr, "%serror:%s ", err->bad, err->reset);
}

static void report_error(const session *s, const char *expression,
                         calc_result r)
{
    char buffer[160];
    const char *hint = hint_for(expression, r, buffer, sizeof buffer);
    const char *message = calc_status_message(r.status);

    fflush(stdout);
    if (s->interactive) {
        /* The expression is on the screen already, after the prompt:
         *   > 8 / 0
         *     ~ ^ ~ error: division by zero */
        size_t column;

        fputs(err->bad, stderr);
        column = print_marks(stderr, expression, r, PROMPT_WIDTH);
        fprintf(stderr, " error:%s %s%s%s\n", err->reset, err->bold, message,
                err->reset);
        if (hint != NULL)
            fprintf(stderr, "%*s%shint:%s %s\n", (int)column + 1, "",
                    err->accent, err->reset, hint);
        return;
    }

    begin_error(s);
    fprintf(stderr, "%s%s%s\n    ", err->bold, message, err->reset);
    print_expression(stderr, expression);
    fprintf(stderr, "\n%s", err->bad);
    print_marks(stderr, expression, r, 4);
    fprintf(stderr, "%s\n", err->reset);
    if (hint != NULL)
        fprintf(stderr, "    %shint:%s %s\n", err->accent, err->reset, hint);
}

/* ------------------------------------------------------------ sessions */

/* Evaluates one expression and prints its value or what is wrong with it. */
static void evaluate(session *s, const char *expression)
{
    const calc_result result = calc_evaluate(expression, s->ans);
    char text[CALC_FORMAT_SIZE];

    if (result.status != CALC_OK) {
        report_error(s, expression, result);
        s->failed = true;
        return;
    }

    s->ans = result.value;
    calc_format(result.value, text, sizeof text);
    if (s->interactive && s->results_to_terminal)
        printf("%s=%s %s%s%s\n", out->dim, out->reset, out->good, text,
               out->reset);
    else
        printf("%s%s%s\n", out->good, text, out->reset);
    fflush(stdout); /* show each result at once, even through a pipe */
}

/* Evaluates the expression formed by the `count` words in `words`. */
static int run_expression(int count, char **words)
{
    session s = { false, false, false, false, 0, 0.0, false };
    size_t size = 1;
    char *expression;
    char *end;
    int i;

    /* Join the words with spaces, so that "calci 2 + 3" works unquoted. */
    for (i = 0; i < count; i++)
        size += strlen(words[i]) + 1;
    expression = malloc(size);
    if (expression == NULL) {
        fputs(PROGRAM_NAME ": error: out of memory\n", stderr);
        return EXIT_FAILURE;
    }

    end = expression;
    for (i = 0; i < count; i++) {
        const size_t length = strlen(words[i]);

        if (i > 0)
            *end++ = ' ';
        memcpy(end, words[i], length);
        end += length;
    }
    *end = '\0';

    s.results_to_terminal = term_is_terminal(stdout);
    evaluate(&s, expression);
    free(expression);
    return s.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

typedef enum line_status { LINE_OK, LINE_TOO_LONG, LINE_EOF } line_status;

/* Reads one line, without its line terminator, into `buffer`. A line that
 * does not fit is consumed entirely and reported as LINE_TOO_LONG. A last
 * line without a terminator counts as a line. */
static line_status read_line(FILE *stream, char *buffer, size_t size)
{
    size_t length = 0;
    bool too_long = false;
    int c;

    while ((c = getc(stream)) != EOF && c != '\n') {
        if (length + 1 < size)
            buffer[length++] = (char)c;
        else
            too_long = true;
    }
    buffer[length] = '\0';

    if (c == EOF && length == 0 && !too_long)
        return LINE_EOF;
    return too_long ? LINE_TOO_LONG : LINE_OK;
}

static bool is_blank(const char *line)
{
    while (*line != '\0' && isspace((unsigned char)*line))
        line++;
    return *line == '\0';
}

/* Tells whether `line` is `command` (lowercase), ignoring case and blanks
 * around it. */
static bool is_command(const char *line, const char *command)
{
    while (*line != '\0' && isspace((unsigned char)*line))
        line++;
    for (; *command != '\0'; line++, command++) {
        if (*line == '\0' || tolower((unsigned char)*line) != *command)
            return false;
    }
    return is_blank(line);
}

/* Completes names for the line editor's Tab key: the names that start with
 * `word`, followed by "(" for functions. */
static const char *complete_name(const char *word, size_t length,
                                 size_t index)
{
    static char completions[64][16];
    const char *name;
    bool is_function = false;
    size_t found = 0;
    size_t i, j;

    for (i = 0; i < 64 && (name = calc_name(i, &is_function)) != NULL; i++) {
        for (j = 0; j < length && name[j] != '\0' &&
                    tolower((unsigned char)word[j]) == name[j];
             j++)
            continue;
        if (j < length || found++ < index)
            continue;
        if (completions[i][0] == '\0')
            snprintf(completions[i], sizeof completions[i], "%s%s", name,
                     is_function ? "(" : "");
        return completions[i];
    }
    return NULL;
}

/* Evaluates standard input line by line until it ends or says "quit". The
 * banner and prompt go to stderr, along with error messages, so that
 * standard output carries only what was asked for: results and help. */
static int run_session(void)
{
    session s = { false, false, false, false, 0, 0.0, false };
    char line[LINE_SIZE];
    char prompt[32];

    s.interactive = term_is_terminal(stdin);
    s.editing = s.interactive && editor_supported();
    s.results_to_terminal = term_is_terminal(stdout);
    s.can_clear = s.interactive && term_supports_ansi(stderr);
    snprintf(prompt, sizeof prompt, "%s>%s ", err->accent, err->reset);
    editor_set_completer(complete_name);
    if (s.interactive)
        print_banner();

    for (;;) {
        line_status status = LINE_OK;

        if (s.editing) {
            const editor_status got =
                editor_read_line(prompt, line, sizeof line);

            if (got == EDITOR_CANCELLED)
                continue;
            if (got == EDITOR_ERROR) {
                s.editing = false; /* read the ordinary way from now on */
                continue;
            }
            if (got == EDITOR_EOF) {
                fputc('\n', stderr);
                break;
            }
            editor_add_history(line);
        } else {
            if (s.interactive)
                fputs(prompt, stderr);
            status = read_line(stdin, line, sizeof line);
            if (status == LINE_EOF) {
                if (s.interactive)
                    fputc('\n', stderr); /* end the line the prompt is on */
                break;
            }
        }
        s.line_number++;

        if (status == LINE_TOO_LONG) {
            begin_error(&s);
            fprintf(stderr, "line is longer than %d characters\n",
                    LINE_SIZE - 1);
            s.failed = true;
        } else if (is_command(line, "quit") || is_command(line, "exit")) {
            break;
        } else if (is_command(line, "help")) {
            print_reference(stdout, out, s.interactive, s.editing);
            fflush(stdout);
        } else if (is_command(line, "clear") || is_command(line, "cls")) {
            if (s.can_clear)
                fputs("\033[H\033[2J", stderr);
        } else if (!is_blank(line)) {
            evaluate(&s, line);
        }
    }

    if (ferror(stdin)) {
        fputs(PROGRAM_NAME ": error: cannot read standard input\n", stderr);
        return EXIT_FAILURE;
    }
    /* A typo in an interactive session is not a failure of the session. */
    return s.failed && !s.interactive ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Makes sure that everything written to stdout arrived, since a result that
 * silently went missing (on a full disk, say) would be worse than an error. */
static int finish(int status)
{
    if (fflush(stdout) != 0 || ferror(stdout)) {
        fputs(PROGRAM_NAME ": error: cannot write to standard output\n",
              stderr);
        return EXIT_FAILURE;
    }
    return status;
}

int main(int argc, char *argv[])
{
    const char *option = argc > 1 ? argv[1] : "";
    int first = 1;

    out = term_supports_color(stdout) ? &colored : &plain;
    err = term_supports_color(stderr) ? &colored : &plain;

    if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
        print_usage(stdout, out);
        print_reference(stdout, out, false, false);
        return finish(EXIT_SUCCESS);
    }
    if (strcmp(option, "-v") == 0 || strcmp(option, "--version") == 0) {
        printf("calci %s\n", VERSION);
        return finish(EXIT_SUCCESS);
    }
    if (option[0] == '-' && option[1] == '-' &&
        isalpha((unsigned char)option[2])) {
        fprintf(stderr,
                "%s: %serror:%s unknown option '%s'\n"
                "Try 'calci --help' for more information.\n",
                PROGRAM_NAME, err->bad, err->reset, option);
        return EXIT_USAGE;
    }

    /* Anything else is an expression, even when it starts with '-' as in
     * "-5 + 3". A "--" in front is skipped: "calci -- --pi" is pi. */
    if (strcmp(option, "--") == 0)
        first = 2;

    if (first < argc)
        return finish(run_expression(argc - first, argv + first));
    return finish(run_session());
}
