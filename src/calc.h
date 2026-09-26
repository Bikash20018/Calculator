/*
 * calc.h - evaluator for arithmetic expressions such as "(2 + 3) * 4".
 *
 * The evaluator has no global state and performs no I/O, so it is safe to
 * call from anywhere and easy to test in isolation.
 */
#ifndef CALC_H
#define CALC_H

#include <stdbool.h>
#include <stddef.h>

/* Outcome of an evaluation. Every error comes with the location of the part
 * of the expression that caused it (see calc_result). */
typedef enum calc_status {
    CALC_OK = 0,
    CALC_ERR_EMPTY,             /* the expression is empty or all blanks */
    CALC_ERR_INVALID_CHAR,      /* a character that is not part of any token */
    CALC_ERR_INVALID_NUMBER,    /* a malformed number, e.g. "1.2.3" or "2x" */
    CALC_ERR_UNKNOWN_NAME,      /* a name that is not a function or constant */
    CALC_ERR_EXPECTED_OPERAND,  /* e.g. "2 +" or "* 3" */
    CALC_ERR_EXPECTED_OPERATOR, /* e.g. "2 3" or "2(3)" */
    CALC_ERR_EXPECTED_LPAREN,   /* a function name without "(", e.g. "sqrt 4" */
    CALC_ERR_EXPECTED_RPAREN,   /* a "(" that is never closed */
    CALC_ERR_UNMATCHED_RPAREN,  /* a ")" that was never opened */
    CALC_ERR_TOO_DEEP,          /* nesting deeper than CALC_MAX_DEPTH */
    CALC_ERR_DIVISION_BY_ZERO,  /* x / 0, x % 0 or 0 ^ -n */
    CALC_ERR_DOMAIN,            /* the result is not a real number: sqrt(-1) */
    CALC_ERR_RANGE              /* a value too large for a double: 10 ^ 400 */
} calc_status;

/* When status is not CALC_OK, the error_* fields locate the problem as byte
 * offsets into the expression: the offending token, where a caret belongs,
 * and the whole construct involved, which may be underlined. For "8 / 0" the
 * token is the "/" and the construct is all of "8 / 0". */
typedef struct calc_result {
    calc_status status;
    double value;        /* the result, when status is CALC_OK */
    size_t error_offset; /* start of the offending token */
    size_t error_length; /* its length; 0 at the end of the expression */
    size_t error_start;  /* start of the construct, <= error_offset */
    size_t error_end;    /* end of the construct, >= error_offset + length */
} calc_result;

/* How many levels deep parentheses, function calls, signs and powers may
 * nest; each of them adds a level. The limit keeps pathological input such as
 * "((((((...1" from exhausting the stack. */
#define CALC_MAX_DEPTH 256

/* Evaluates `expression`, a NUL-terminated string (NULL counts as empty).
 * `ans` is the value the name "ans" stands for, normally the previous result.
 *
 * Besides ASCII, the expression may use these symbols in UTF-8: U+00D7
 * MULTIPLICATION SIGN, U+00F7 DIVISION SIGN, U+2212 MINUS SIGN and U+03C0
 * GREEK SMALL LETTER PI. It may end with "=", as on a pocket calculator.
 *
 * Syntax errors take precedence over math errors: "1/0 + )" reports the
 * misplaced ")", not the division by zero. Among math errors, the first one
 * in evaluation order is reported. */
calc_result calc_evaluate(const char *expression, double ans);

/* Returns a short, human-readable, statically allocated description of
 * `status`, such as "division by zero". */
const char *calc_status_message(calc_status status);

/* Returns the index-th name that calc_evaluate understands, lowercase, or
 * NULL when index is past the last one: first the functions, then the
 * constants. If `is_function` is not NULL, it is set to tell which. */
const char *calc_name(size_t index, bool *is_function);

/* Returns the known name that the `length` bytes at `word` most likely
 * misspell ("sqr" gives "sqrt", "sine" gives "sin"), or NULL if no name is
 * close enough to be a plausible suggestion. */
const char *calc_suggest_name(const char *word, size_t length);

/* A buffer of this many bytes holds any string calc_format produces. */
#define CALC_FORMAT_SIZE 32

/* Writes `value` to `buffer` the way a calculator displays it: whole numbers
 * up to 2^53 in full, anything else rounded to 15 significant digits so that
 * 0.1 + 0.2 shows as 0.3. The output is truncated to fit `size` bytes and
 * always NUL-terminated; nothing is written if `size` is 0. */
void calc_format(double value, char *buffer, size_t size);

#endif /* CALC_H */
