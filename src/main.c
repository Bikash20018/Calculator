/*
 * main.c - the calci command: argument handling, the interactive session
 * and error reporting. All of the arithmetic lives in calc.c.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* for fileno() and isatty() */
#endif

#include "calc.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define fileno _fileno
#define isatty _isatty
#else
#include <unistd.h>
#endif

#define PROGRAM_NAME "calci"
#define PROMPT "> "

/* Input lines may be up to LINE_SIZE - 1 characters long. */
#define LINE_SIZE 4096

static const char banner[] =
    "calci - type an expression such as (2 + 3) * 4, \"help\" for help,\n"
    "or \"quit\" to exit.\n";

static const char usage[] =
    "Usage: calci [EXPRESSION...]\n"
    "\n"
    "Evaluates arithmetic expressions.\n"
    "\n"
    "With an EXPRESSION, prints its value and exits. Quote the expression so\n"
    "that the shell leaves characters such as * and ( ) alone:\n"
    "\n"
    "  calci \"(2 + 3) * 4\"\n"
    "\n"
    "Without one, reads expressions from standard input, one per line: an\n"
    "interactive session on a terminal, otherwise a file or pipe of\n"
    "expressions.\n"
    "\n"
    "Options:\n"
    "  -h, --help   show this help and exit\n"
    "\n"
    "The exit status is 0 on success and 1 if an expression failed.\n"
    "\n";

static const char reference[] =
    "Operators, from highest to lowest precedence:\n"
    "  ( )     grouping\n"
    "  ^       power, right-associative: 2^3^2 = 2^9 = 512\n"
    "  + -     sign: -2^2 = -(2^2) = -4\n"
    "  * / %   multiply, divide, remainder: 7 % 3 = 1, -7 % 3 = -1\n"
    "  + -     add, subtract\n"
    "\n"
    "Numbers:    42   3.14   .5   6.022e23\n"
    "Constants:  pi   e   ans (the previous result)\n"
    "Functions:  abs  sqrt  cbrt  exp  ln  log (base 10)  log2\n"
    "            sin  cos  tan  asin  acos  atan (in radians)\n"
    "            floor  ceil  round\n"
    "Commands:   help  quit  exit\n";

typedef struct session {
    bool interactive;          /* reading from a terminal */
    unsigned long line_number; /* current input line; 0 for arguments */
    double ans;                /* the previous result */
    bool failed;               /* an expression could not be evaluated */
} session;

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
    while (isspace((unsigned char)*line))
        line++;
    return *line == '\0';
}

/* Tells whether `line` is `command` (lowercase), ignoring case and blanks
 * around it. */
static bool is_command(const char *line, const char *command)
{
    while (isspace((unsigned char)*line))
        line++;
    for (; *command != '\0'; line++, command++) {
        if (tolower((unsigned char)*line) != *command)
            return false;
    }
    return is_blank(line);
}

/* Starts an error message: "calci: line 3: error: " or, in an interactive
 * session, just "error: ". */
static void begin_error(const session *s)
{
    fflush(stdout); /* keep results and errors in order in a shared log */
    if (!s->interactive) {
        fputs(PROGRAM_NAME ": ", stderr);
        if (s->line_number > 0)
            fprintf(stderr, "line %lu: ", s->line_number);
    }
    fputs("error: ", stderr);
}

/* Prints `text`, with control characters replaced so that they can neither
 * disturb the terminal nor shift the caret printed below it. */
static void print_expression(const char *text)
{
    for (; *text != '\0'; text++) {
        const unsigned char c = (unsigned char)*text;

        putc((c < 0x20 && c != '\t') || c == 0x7f ? '?' : c, stderr);
    }
}

/* Prints `indent` spaces, then a '^' under byte `offset` of `text`. Tabs are
 * copied so that the caret lines up however wide the terminal shows them. */
static void print_caret(const char *text, size_t offset, int indent)
{
    size_t i;

    fprintf(stderr, "%*s", indent, "");
    for (i = 0; i < offset && text[i] != '\0'; i++)
        putc(text[i] == '\t' ? '\t' : ' ', stderr);
    putc('^', stderr);
}

static void report_error(const session *s, const char *expression,
                         calc_result result)
{
    const char *message = calc_status_message(result.status);

    if (s->interactive) {
        /* The expression is still on the screen, right after the prompt. */
        fflush(stdout);
        print_caret(expression, result.error_offset, (int)strlen(PROMPT));
        fprintf(stderr, " error: %s\n", message);
        return;
    }

    begin_error(s);
    fprintf(stderr, "%s\n    ", message);
    print_expression(expression);
    fputc('\n', stderr);
    print_caret(expression, result.error_offset, 4);
    fputc('\n', stderr);
}

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
    puts(text);
    fflush(stdout); /* show each result at once, even through a pipe */
}

/* Evaluates the expression formed by the `count` words in `words`. */
static int run_expression(int count, char **words)
{
    session s = { false, 0, 0.0, false };
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

    evaluate(&s, expression);
    free(expression);
    return s.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Evaluates standard input line by line until it ends or says "quit". The
 * banner and prompt go to stderr, along with error messages, so that
 * standard output carries only what was asked for: results and help. */
static int run_session(void)
{
    session s = { false, 0, 0.0, false };
    char line[LINE_SIZE];

    s.interactive = isatty(fileno(stdin)) != 0;
    if (s.interactive)
        fputs(banner, stderr);

    for (;;) {
        line_status status;

        if (s.interactive)
            fputs(PROMPT, stderr);
        status = read_line(stdin, line, sizeof line);
        if (status == LINE_EOF) {
            if (s.interactive)
                fputc('\n', stderr); /* end the line the prompt is on */
            break;
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
            fputs(reference, stdout);
            fflush(stdout);
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
    int first = 1;

    if (argc > 1 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fputs(usage, stdout);
        fputs(reference, stdout);
        return finish(EXIT_SUCCESS);
    }

    /* Anything else is an expression, even when it starts with '-' as in
     * "-5 + 3". A "--" in front is accepted and skipped. */
    if (argc > 1 && strcmp(argv[1], "--") == 0)
        first = 2;

    if (first < argc)
        return finish(run_expression(argc - first, argv + first));
    return finish(run_session());
}
