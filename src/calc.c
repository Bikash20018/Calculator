/*
 * calc.c - recursive-descent evaluator for arithmetic expressions.
 *
 * Grammar, from lowest to highest precedence:
 *
 *   expression := term (('+' | '-') term)* '='?
 *   term       := unary (('*' | '/' | '%') unary)*
 *   unary      := ('+' | '-') unary | power
 *   power      := primary ('^' unary)?
 *   primary    := NUMBER | NAME | NAME '(' expression ')' | '(' expression ')'
 *
 * so '^' is right-associative and binds tighter than a sign, as in
 * mathematics: 2^3^2 = 2^(3^2) = 512 and -2^2 = -(2^2) = -4.
 *
 * Syntax errors abort the parse at once. Math errors (division by zero,
 * overflow, ...) are only recorded and parsing goes on, so that a syntax
 * error later in the expression still gets reported in preference.
 */
#include "calc.h"

#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CALC_PI 3.14159265358979323846
#define CALC_E  2.71828182845904523536

/* Doubles represent every integer up to 2^53 exactly. */
#define MAX_EXACT_INTEGER 9007199254740992.0

typedef enum token_kind {
    TOKEN_END,
    TOKEN_NUMBER,
    TOKEN_NAME,
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_STAR,
    TOKEN_SLASH,
    TOKEN_PERCENT,
    TOKEN_CARET,
    TOKEN_LPAREN,
    TOKEN_RPAREN
} token_kind;

typedef struct token {
    token_kind kind;
    size_t offset; /* where the token starts in the expression */
    size_t length; /* length of the token in bytes */
    double number; /* value of a TOKEN_NUMBER */
} token;

typedef struct parser {
    const char *text;   /* the whole expression */
    size_t pos;         /* offset of the first character not yet scanned */
    token tok;          /* the current token */
    size_t prev_end;    /* where the token before the current one ended */
    double ans;         /* value of the name "ans" */
    int depth;          /* current nesting depth, see CALC_MAX_DEPTH */
    calc_result result; /* the error found so far, if status is not CALC_OK */
} parser;

typedef double (*math_function)(double);

/* Wrappers rather than pointers to the library functions themselves, whose
 * addresses are not guaranteed to be constant (they may live in a DLL). */
static double fn_abs(double x)   { return fabs(x); }
static double fn_sqrt(double x)  { return sqrt(x); }
static double fn_cbrt(double x)  { return cbrt(x); }
static double fn_exp(double x)   { return exp(x); }
static double fn_ln(double x)    { return log(x); }
static double fn_log(double x)   { return log10(x); }
static double fn_log2(double x)  { return log2(x); }
static double fn_sin(double x)   { return sin(x); }
static double fn_cos(double x)   { return cos(x); }
static double fn_tan(double x)   { return tan(x); }
static double fn_asin(double x)  { return asin(x); }
static double fn_acos(double x)  { return acos(x); }
static double fn_atan(double x)  { return atan(x); }
static double fn_floor(double x) { return floor(x); }
static double fn_ceil(double x)  { return ceil(x); }
static double fn_round(double x) { return round(x); }

static const struct {
    const char *name;
    math_function apply;
} functions[] = {
    { "abs",   fn_abs   }, { "sqrt",  fn_sqrt  }, { "cbrt",  fn_cbrt  },
    { "exp",   fn_exp   }, { "ln",    fn_ln    }, { "log",   fn_log   },
    { "log10", fn_log   }, { "log2",  fn_log2  }, { "sin",   fn_sin   },
    { "cos",   fn_cos   }, { "tan",   fn_tan   }, { "asin",  fn_asin  },
    { "acos",  fn_acos  }, { "atan",  fn_atan  }, { "floor", fn_floor },
    { "ceil",  fn_ceil  }, { "round", fn_round },
};

static const struct {
    const char *name;
    double value;
} constants[] = {
    { "pi",  CALC_PI },
    { "e",   CALC_E  },
    { "ans", 0.0     }, /* stands for whatever the caller passes as ans */
};

#define FUNCTION_COUNT (sizeof functions / sizeof functions[0])
#define CONSTANT_COUNT (sizeof constants / sizeof constants[0])

/* Symbols accepted in UTF-8 besides the ASCII ones. */
static const struct {
    const char *utf8;
    token_kind kind;
} symbols[] = {
    { "\xC3\x97",     TOKEN_STAR   }, /* U+00D7 MULTIPLICATION SIGN */
    { "\xC3\xB7",     TOKEN_SLASH  }, /* U+00F7 DIVISION SIGN */
    { "\xE2\x88\x92", TOKEN_MINUS  }, /* U+2212 MINUS SIGN */
    { "\xCF\x80",     TOKEN_NUMBER }, /* U+03C0 GREEK SMALL LETTER PI */
};

/* The isnan() and isinf() macros of some C libraries (MinGW's among them)
 * draw -Wconversion warnings, so these say the same with plain comparisons. */
static bool is_nan(double x)
{
    return x != x;
}

static bool is_infinite(double x)
{
    return x > DBL_MAX || x < -DBL_MAX;
}

/* ---------------------------------------------------------------- errors */

static void set_error(parser *p, calc_status status, const token *at,
                      size_t start, size_t end)
{
    p->result.status = status;
    p->result.error_offset = at->offset;
    p->result.error_length = at->length;
    p->result.error_start = start;
    p->result.error_end = end;
}

/* Records a syntax error at the token `at`, replacing any math error found
 * so far, and returns false so that callers can `return syntax_error(...)`. */
static bool syntax_error(parser *p, calc_status status, const token *at)
{
    set_error(p, status, at, at->offset, at->offset + at->length);
    return false;
}

/* Records a math error in the operation from `start` to `end` performed by
 * the operator or function `at`, unless an earlier one was recorded already.
 * Returns a placeholder value that lets parsing carry on. */
static double math_error(parser *p, calc_status status, const token *at,
                         size_t start, size_t end)
{
    if (p->result.status == CALC_OK)
        set_error(p, status, at, start, end);
    return 0.0;
}

/* Passes `value`, the result of the operation described by the other
 * arguments, through if it is a real number and records an error if not. */
static double checked(parser *p, double value, const token *at, size_t start,
                      size_t end)
{
    if (is_nan(value))
        return math_error(p, CALC_ERR_DOMAIN, at, start, end);
    if (is_infinite(value))
        return math_error(p, CALC_ERR_RANGE, at, start, end);
    return value;
}

/* ----------------------------------------------------------------- lexer */

static bool is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static bool is_blank(const char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    return *s == '\0';
}

/* Scans the number at p->pos: digits with at most one '.', and an optional
 * exponent such as "e-3". */
static bool scan_number(parser *p)
{
    const char *start = p->text + p->pos;
    const char *s = start;
    char *end;
    bool has_digits = false;
    double value;

    while (isdigit((unsigned char)*s)) {
        s++;
        has_digits = true;
    }
    if (*s == '.') {
        s++;
        while (isdigit((unsigned char)*s)) {
            s++;
            has_digits = true;
        }
    }
    if (has_digits && (*s == 'e' || *s == 'E')) {
        const char *exponent = s + 1;

        if (*exponent == '+' || *exponent == '-')
            exponent++;
        if (isdigit((unsigned char)*exponent)) {
            while (isdigit((unsigned char)*exponent))
                exponent++;
            s = exponent;
        }
    }

    p->tok.kind = TOKEN_NUMBER;
    p->tok.length = (size_t)(s - start);

    /* Reject a lone ".", and numbers that run straight into letters or dots
     * such as "1.2.3", "0x1F", "2pi" or "1e"; all of that is the bad token. */
    if (!has_digits || is_name_char(*s) || *s == '.') {
        while (is_name_char(*s) || *s == '.')
            s++;
        p->tok.length = (size_t)(s - start);
        return syntax_error(p, CALC_ERR_INVALID_NUMBER, &p->tok);
    }

    value = strtod(start, &end);
    if (end != s) /* only possible if a locale changed the decimal point */
        return syntax_error(p, CALC_ERR_INVALID_NUMBER, &p->tok);

    p->pos += p->tok.length;
    p->tok.number = checked(p, value, &p->tok, p->tok.offset,
                            p->tok.offset + p->tok.length); /* e.g. 1e999 */
    return true;
}

/* Length of the UTF-8 character starting at `s`, at least 1 even if the
 * encoding is broken, and never past the end of the string. */
static size_t char_length(const char *s)
{
    const unsigned char lead = (unsigned char)s[0];
    const size_t expected = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3
                          : lead >= 0xC0 ? 2 : 1;
    size_t length = 1;

    while (length < expected && ((unsigned char)s[length] & 0xC0) == 0x80)
        length++;
    return length;
}

/* Advances to the next token. */
static bool next_token(parser *p)
{
    const char *s = p->text;
    size_t i;
    char c;

    p->prev_end = p->tok.offset + p->tok.length;
    while (isspace((unsigned char)s[p->pos]))
        p->pos++;

    c = s[p->pos];
    p->tok.offset = p->pos;
    p->tok.length = 1;
    p->tok.number = 0.0;

    /* A final "=" merely ends the expression, as on a pocket calculator. */
    if (c == '\0' || (c == '=' && is_blank(s + p->pos + 1))) {
        p->tok.kind = TOKEN_END;
        p->tok.length = 0;
        return true;
    }
    if (isdigit((unsigned char)c) || c == '.')
        return scan_number(p);
    if (is_name_start(c)) {
        size_t end = p->pos + 1;

        while (is_name_char(s[end]))
            end++;
        p->tok.kind = TOKEN_NAME;
        p->tok.length = end - p->pos;
        p->pos = end;
        return true;
    }

    for (i = 0; i < sizeof symbols / sizeof symbols[0]; i++) {
        const size_t length = strlen(symbols[i].utf8);

        if (strncmp(s + p->pos, symbols[i].utf8, length) == 0) {
            p->tok.kind = symbols[i].kind;
            p->tok.length = length;
            if (symbols[i].kind == TOKEN_NUMBER)
                p->tok.number = CALC_PI;
            p->pos += length;
            return true;
        }
    }

    switch (c) {
    case '+': p->tok.kind = TOKEN_PLUS;    break;
    case '-': p->tok.kind = TOKEN_MINUS;   break;
    case '*': p->tok.kind = TOKEN_STAR;    break;
    case '/': p->tok.kind = TOKEN_SLASH;   break;
    case '%': p->tok.kind = TOKEN_PERCENT; break;
    case '^': p->tok.kind = TOKEN_CARET;   break;
    case '(': p->tok.kind = TOKEN_LPAREN;  break;
    case ')': p->tok.kind = TOKEN_RPAREN;  break;
    default:
        p->tok.length = char_length(s + p->pos);
        return syntax_error(p, CALC_ERR_INVALID_CHAR, &p->tok);
    }
    p->pos++;
    return true;
}

/* Tells whether the current token spells `name` (lowercase), ignoring case. */
static bool token_is(const parser *p, const char *name)
{
    const char *text = p->text + p->tok.offset;
    size_t i;

    for (i = 0; i < p->tok.length; i++) {
        if (name[i] == '\0' || tolower((unsigned char)text[i]) != name[i])
            return false;
    }
    return name[i] == '\0';
}

/* ---------------------------------------------------------------- parser */

static bool parse_expression(parser *p, double *value);
static bool parse_unary(parser *p, double *value);

/* Applies the binary operator `op` to its operands. `start` and `end` span
 * the whole operation, operands included. */
static double apply_operator(parser *p, const token *op, double left,
                             double right, size_t start, size_t end)
{
    switch (op->kind) {
    case TOKEN_PLUS:
        return checked(p, left + right, op, start, end);
    case TOKEN_MINUS:
        return checked(p, left - right, op, start, end);
    case TOKEN_STAR:
        return checked(p, left * right, op, start, end);
    case TOKEN_SLASH:
        if (right == 0.0)
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op, start, end);
        return checked(p, left / right, op, start, end);
    case TOKEN_PERCENT:
        if (right == 0.0)
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op, start, end);
        return checked(p, fmod(left, right), op, start, end);
    case TOKEN_CARET:
        if (left == 0.0 && right < 0.0) /* 0^-n = 1 / 0^n */
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op, start, end);
        return checked(p, pow(left, right), op, start, end);
    default:
        break;
    }
    assert(0); /* callers only ever pass the binary operators above */
    return 0.0;
}

/* '(' expression ')', with the current token being the '('. */
static bool parse_parenthesized(parser *p, double *value)
{
    if (!next_token(p) || !parse_expression(p, value))
        return false;
    if (p->tok.kind != TOKEN_RPAREN)
        return syntax_error(p, CALC_ERR_EXPECTED_RPAREN, &p->tok);
    return next_token(p);
}

/* A constant, or a function applied to a parenthesized argument. */
static bool parse_name(parser *p, double *value)
{
    const token name = p->tok;
    math_function function = NULL;
    double argument = 0.0;
    size_t i;

    for (i = 0; i < CONSTANT_COUNT; i++) {
        if (token_is(p, constants[i].name)) {
            if (strcmp(constants[i].name, "ans") == 0)
                *value = checked(p, p->ans, &name, name.offset,
                                 name.offset + name.length);
            else
                *value = constants[i].value;
            return next_token(p);
        }
    }
    for (i = 0; i < FUNCTION_COUNT && function == NULL; i++) {
        if (token_is(p, functions[i].name))
            function = functions[i].apply;
    }
    if (function == NULL)
        return syntax_error(p, CALC_ERR_UNKNOWN_NAME, &name);

    if (!next_token(p))
        return false;
    if (p->tok.kind != TOKEN_LPAREN)
        return syntax_error(p, CALC_ERR_EXPECTED_LPAREN, &p->tok);
    if (!parse_parenthesized(p, &argument))
        return false;
    *value = checked(p, function(argument), &name, name.offset, p->prev_end);
    return true;
}

/* primary := NUMBER | NAME | NAME '(' expression ')' | '(' expression ')' */
static bool parse_primary(parser *p, double *value)
{
    switch (p->tok.kind) {
    case TOKEN_NUMBER:
        *value = p->tok.number;
        return next_token(p);
    case TOKEN_NAME:
        return parse_name(p, value);
    case TOKEN_LPAREN:
        return parse_parenthesized(p, value);
    default:
        return syntax_error(p, CALC_ERR_EXPECTED_OPERAND, &p->tok);
    }
}

/* power := primary ('^' unary)? */
static bool parse_power(parser *p, double *value)
{
    const size_t start = p->tok.offset;
    token op;
    double exponent = 0.0;

    if (!parse_primary(p, value))
        return false;
    if (p->tok.kind != TOKEN_CARET)
        return true;

    op = p->tok;
    if (!next_token(p) || !parse_unary(p, &exponent))
        return false;
    *value = apply_operator(p, &op, *value, exponent, start, p->prev_end);
    return true;
}

/* unary := ('+' | '-') unary | power
 *
 * Every level of nesting passes through here, so this is where the depth
 * limit is enforced. The outermost call is depth 1, not a nesting level. */
static bool parse_unary(parser *p, double *value)
{
    bool ok;

    if (p->depth > CALC_MAX_DEPTH)
        return syntax_error(p, CALC_ERR_TOO_DEEP, &p->tok);
    p->depth++;

    if (p->tok.kind == TOKEN_PLUS || p->tok.kind == TOKEN_MINUS) {
        const bool negate = p->tok.kind == TOKEN_MINUS;

        ok = next_token(p) && parse_unary(p, value);
        if (ok && negate)
            *value = -*value;
    } else {
        ok = parse_power(p, value);
    }

    p->depth--;
    return ok;
}

/* term := unary (('*' | '/' | '%') unary)* */
static bool parse_term(parser *p, double *value)
{
    const size_t start = p->tok.offset;

    if (!parse_unary(p, value))
        return false;

    while (p->tok.kind == TOKEN_STAR || p->tok.kind == TOKEN_SLASH ||
           p->tok.kind == TOKEN_PERCENT) {
        const token op = p->tok;
        double right = 0.0;

        if (!next_token(p) || !parse_unary(p, &right))
            return false;
        *value = apply_operator(p, &op, *value, right, start, p->prev_end);
    }
    return true;
}

/* expression := term (('+' | '-') term)* */
static bool parse_expression(parser *p, double *value)
{
    const size_t start = p->tok.offset;

    if (!parse_term(p, value))
        return false;

    while (p->tok.kind == TOKEN_PLUS || p->tok.kind == TOKEN_MINUS) {
        const token op = p->tok;
        double right = 0.0;

        if (!next_token(p) || !parse_term(p, &right))
            return false;
        *value = apply_operator(p, &op, *value, right, start, p->prev_end);
    }
    return true;
}

/* ------------------------------------------------------------ public API */

calc_result calc_evaluate(const char *expression, double ans)
{
    parser p;
    double value = 0.0;

    p.text = expression != NULL ? expression : "";
    p.pos = 0;
    p.tok.kind = TOKEN_END;
    p.tok.offset = 0;
    p.tok.length = 0;
    p.tok.number = 0.0;
    p.prev_end = 0;
    p.ans = ans;
    p.depth = 0;
    p.result.status = CALC_OK;
    p.result.value = 0.0;
    p.result.error_offset = 0;
    p.result.error_length = 0;
    p.result.error_start = 0;
    p.result.error_end = 0;

    if (next_token(&p)) {
        if (p.tok.kind == TOKEN_END) {
            syntax_error(&p, CALC_ERR_EMPTY, &p.tok);
        } else if (parse_expression(&p, &value) && p.tok.kind != TOKEN_END) {
            syntax_error(&p,
                         p.tok.kind == TOKEN_RPAREN ? CALC_ERR_UNMATCHED_RPAREN
                                                    : CALC_ERR_EXPECTED_OPERATOR,
                         &p.tok);
        }
    }

    if (p.result.status == CALC_OK)
        p.result.value = value == 0.0 ? 0.0 : value; /* no negative zero */
    return p.result;
}

const char *calc_status_message(calc_status status)
{
    switch (status) {
    case CALC_OK:                    return "success";
    case CALC_ERR_EMPTY:             return "empty expression";
    case CALC_ERR_INVALID_CHAR:      return "invalid character";
    case CALC_ERR_INVALID_NUMBER:    return "invalid number";
    case CALC_ERR_UNKNOWN_NAME:      return "unknown function or constant";
    case CALC_ERR_EXPECTED_OPERAND:  return "expected a number";
    case CALC_ERR_EXPECTED_OPERATOR: return "expected an operator";
    case CALC_ERR_EXPECTED_LPAREN:   return "expected '(' after function name";
    case CALC_ERR_EXPECTED_RPAREN:   return "expected ')'";
    case CALC_ERR_UNMATCHED_RPAREN:  return "unmatched ')'";
    case CALC_ERR_TOO_DEEP:          return "expression is nested too deeply";
    case CALC_ERR_DIVISION_BY_ZERO:  return "division by zero";
    case CALC_ERR_DOMAIN:            return "result is not a real number";
    case CALC_ERR_RANGE:             return "result is out of range";
    }
    return "unknown error";
}

const char *calc_name(size_t index, bool *is_function)
{
    if (index < FUNCTION_COUNT) {
        if (is_function != NULL)
            *is_function = true;
        return functions[index].name;
    }
    index -= FUNCTION_COUNT;
    if (index < CONSTANT_COUNT) {
        if (is_function != NULL)
            *is_function = false;
        return constants[index].name;
    }
    return NULL;
}

/* Longest word that calc_suggest_name compares letter by letter. */
#define MAX_SUGGEST_LENGTH 16

/* Number of edits between the `length` bytes at `word`, compared without
 * regard to case, and the lowercase `name`: insertions, deletions,
 * substitutions and swaps of adjacent letters ("optimal string alignment").
 * Both must be at most MAX_SUGGEST_LENGTH long. */
static size_t edit_distance(const char *word, size_t length, const char *name)
{
    size_t d[MAX_SUGGEST_LENGTH + 1][MAX_SUGGEST_LENGTH + 1];
    const size_t n = strlen(name);
    size_t i, j;

    assert(length <= MAX_SUGGEST_LENGTH && n <= MAX_SUGGEST_LENGTH);
    for (i = 0; i <= length; i++)
        d[i][0] = i;
    for (j = 0; j <= n; j++)
        d[0][j] = j;

    for (i = 1; i <= length; i++) {
        const int w = tolower((unsigned char)word[i - 1]);

        for (j = 1; j <= n; j++) {
            size_t best = d[i - 1][j - 1] + (w == name[j - 1] ? 0u : 1u);

            if (d[i - 1][j] + 1 < best)
                best = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < best)
                best = d[i][j - 1] + 1;
            if (i > 1 && j > 1 && w == name[j - 2] &&
                tolower((unsigned char)word[i - 2]) == name[j - 1] &&
                d[i - 2][j - 2] + 1 < best)
                best = d[i - 2][j - 2] + 1;
            d[i][j] = best;
        }
    }
    return d[length][n];
}

/* Tells whether the `length` bytes at `word` start with `name`, ignoring
 * case. */
static bool starts_with(const char *word, size_t length, const char *name)
{
    size_t i;

    for (i = 0; name[i] != '\0'; i++) {
        if (i >= length || tolower((unsigned char)word[i]) != name[i])
            return false;
    }
    return true;
}

const char *calc_suggest_name(const char *word, size_t length)
{
    const char *best = NULL;
    size_t best_score = MAX_SUGGEST_LENGTH + 1;
    const char *name;
    size_t i;

    /* Two letters leave too little to go on: "ex" could mean e or exp. */
    if (word == NULL || length < 3)
        return NULL;

    for (i = 0; (name = calc_name(i, NULL)) != NULL; i++) {
        const size_t n = strlen(name);
        size_t score = MAX_SUGGEST_LENGTH + 1;

        /* A typo or two, fewer for short words: "sqr", "flor", "arcsin". */
        if (length <= MAX_SUGGEST_LENGTH) {
            const size_t distance = edit_distance(word, length, name);

            if (distance <= (length <= 4 ? 1u : 2u))
                score = distance;
        }
        /* The name spelled out: "sine", "cosine", "absolute". */
        if (n >= 3 && length > n && starts_with(word, length, name) &&
            score > 2)
            score = 2;

        if (score < best_score) {
            best = name;
            best_score = score;
        }
    }
    return best;
}

/* Shortens a printf exponent to its minimal form: "1e-05" becomes "1e-5",
 * and the "1e+021" of older Windows C libraries becomes "1e+21". */
static void trim_exponent(char *text)
{
    char *digits = strchr(text, 'e');
    char *significant;

    if (digits == NULL || (digits[1] != '+' && digits[1] != '-'))
        return;
    digits += 2;
    significant = digits;
    while (significant[0] == '0' && significant[1] != '\0')
        significant++;
    memmove(digits, significant, strlen(significant) + 1);
}

void calc_format(double value, char *buffer, size_t size)
{
    char text[64];
    size_t length;

    if (buffer == NULL || size == 0)
        return;

    if (is_nan(value)) {
        snprintf(text, sizeof text, "nan");
    } else if (is_infinite(value)) {
        snprintf(text, sizeof text, "%s", value < 0 ? "-inf" : "inf");
    } else if (fabs(value) <= MAX_EXACT_INTEGER && value == floor(value)) {
        snprintf(text, sizeof text, "%.0f", value == 0.0 ? 0.0 : value);
    } else {
        snprintf(text, sizeof text, "%.15g", value);
        trim_exponent(text);
    }

    length = strlen(text);
    if (length >= size)
        length = size - 1;
    memcpy(buffer, text, length);
    buffer[length] = '\0';
}
