/*
 * calc.c - recursive-descent evaluator for arithmetic expressions.
 *
 * Grammar, from lowest to highest precedence:
 *
 *   expression := term (('+' | '-') term)*
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
#include <math.h>
#include <stdbool.h>
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
    const char *text;    /* the whole expression */
    size_t pos;          /* offset of the first character not yet scanned */
    token tok;           /* the current token */
    double ans;          /* value of the name "ans" */
    int depth;           /* current nesting depth, see CALC_MAX_DEPTH */
    calc_status status;  /* first error found, or CALC_OK */
    size_t error_offset; /* where that error is */
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
    { "log2",  fn_log2  }, { "sin",   fn_sin   }, { "cos",   fn_cos   },
    { "tan",   fn_tan   }, { "asin",  fn_asin  }, { "acos",  fn_acos  },
    { "atan",  fn_atan  }, { "floor", fn_floor }, { "ceil",  fn_ceil  },
    { "round", fn_round },
};

/* ---------------------------------------------------------------- errors */

/* Records a syntax error, replacing any math error found so far, and returns
 * false so that callers can write `return syntax_error(...)`. */
static bool syntax_error(parser *p, calc_status status, size_t offset)
{
    p->status = status;
    p->error_offset = offset;
    return false;
}

/* Records a math error unless an earlier one was already recorded. Returns a
 * placeholder value that lets parsing carry on. */
static double math_error(parser *p, calc_status status, size_t offset)
{
    if (p->status == CALC_OK) {
        p->status = status;
        p->error_offset = offset;
    }
    return 0.0;
}

/* Passes `value`, the result of the operation at `offset`, through if it is a
 * real number and records a math error otherwise. */
static double checked(parser *p, double value, size_t offset)
{
    if (isnan(value))
        return math_error(p, CALC_ERR_DOMAIN, offset);
    if (isinf(value))
        return math_error(p, CALC_ERR_RANGE, offset);
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
    /* Reject a lone ".", and numbers that run straight into letters or dots
     * such as "1.2.3", "0x1F", "2pi" or "1e". */
    if (!has_digits || is_name_char(*s) || *s == '.')
        return syntax_error(p, CALC_ERR_INVALID_NUMBER, p->pos);

    value = strtod(start, &end);
    if (end != s) /* only possible if a locale changed the decimal point */
        return syntax_error(p, CALC_ERR_INVALID_NUMBER, p->pos);

    p->tok.kind = TOKEN_NUMBER;
    p->tok.length = (size_t)(s - start);
    p->tok.number = checked(p, value, p->pos); /* e.g. 1e999 */
    p->pos += p->tok.length;
    return true;
}

/* Advances to the next token. */
static bool next_token(parser *p)
{
    const char *s = p->text;
    char c;

    while (isspace((unsigned char)s[p->pos]))
        p->pos++;

    c = s[p->pos];
    p->tok.offset = p->pos;
    p->tok.length = 1;
    p->tok.number = 0.0;

    if (c == '\0') {
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
        return syntax_error(p, CALC_ERR_INVALID_CHAR, p->pos);
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

/* Applies the binary operator `op` to its operands. */
static double apply_operator(parser *p, const token *op, double left,
                             double right)
{
    switch (op->kind) {
    case TOKEN_PLUS:
        return checked(p, left + right, op->offset);
    case TOKEN_MINUS:
        return checked(p, left - right, op->offset);
    case TOKEN_STAR:
        return checked(p, left * right, op->offset);
    case TOKEN_SLASH:
        if (right == 0.0)
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op->offset);
        return checked(p, left / right, op->offset);
    case TOKEN_PERCENT:
        if (right == 0.0)
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op->offset);
        return checked(p, fmod(left, right), op->offset);
    case TOKEN_CARET:
        if (left == 0.0 && right < 0.0) /* 0^-n = 1 / 0^n */
            return math_error(p, CALC_ERR_DIVISION_BY_ZERO, op->offset);
        return checked(p, pow(left, right), op->offset);
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
        return syntax_error(p, CALC_ERR_EXPECTED_RPAREN, p->tok.offset);
    return next_token(p);
}

/* A constant, or a function applied to a parenthesized argument. */
static bool parse_name(parser *p, double *value)
{
    const size_t offset = p->tok.offset;
    math_function function = NULL;
    double argument = 0.0;
    size_t i;

    for (i = 0; i < sizeof functions / sizeof functions[0]; i++) {
        if (token_is(p, functions[i].name)) {
            function = functions[i].apply;
            break;
        }
    }

    if (function == NULL) {
        if (token_is(p, "pi"))
            *value = CALC_PI;
        else if (token_is(p, "e"))
            *value = CALC_E;
        else if (token_is(p, "ans"))
            *value = checked(p, p->ans, offset);
        else
            return syntax_error(p, CALC_ERR_UNKNOWN_NAME, offset);
        return next_token(p);
    }

    if (!next_token(p))
        return false;
    if (p->tok.kind != TOKEN_LPAREN)
        return syntax_error(p, CALC_ERR_EXPECTED_LPAREN, p->tok.offset);
    if (!parse_parenthesized(p, &argument))
        return false;
    *value = checked(p, function(argument), offset);
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
        return syntax_error(p, CALC_ERR_EXPECTED_OPERAND, p->tok.offset);
    }
}

/* power := primary ('^' unary)? */
static bool parse_power(parser *p, double *value)
{
    token op;
    double exponent = 0.0;

    if (!parse_primary(p, value))
        return false;
    if (p->tok.kind != TOKEN_CARET)
        return true;

    op = p->tok;
    if (!next_token(p) || !parse_unary(p, &exponent))
        return false;
    *value = apply_operator(p, &op, *value, exponent);
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
        return syntax_error(p, CALC_ERR_TOO_DEEP, p->tok.offset);
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
    if (!parse_unary(p, value))
        return false;

    while (p->tok.kind == TOKEN_STAR || p->tok.kind == TOKEN_SLASH ||
           p->tok.kind == TOKEN_PERCENT) {
        const token op = p->tok;
        double right = 0.0;

        if (!next_token(p) || !parse_unary(p, &right))
            return false;
        *value = apply_operator(p, &op, *value, right);
    }
    return true;
}

/* expression := term (('+' | '-') term)* */
static bool parse_expression(parser *p, double *value)
{
    if (!parse_term(p, value))
        return false;

    while (p->tok.kind == TOKEN_PLUS || p->tok.kind == TOKEN_MINUS) {
        const token op = p->tok;
        double right = 0.0;

        if (!next_token(p) || !parse_term(p, &right))
            return false;
        *value = apply_operator(p, &op, *value, right);
    }
    return true;
}

/* ------------------------------------------------------------ public API */

calc_result calc_evaluate(const char *expression, double ans)
{
    parser p;
    calc_result result;
    double value = 0.0;

    p.text = expression != NULL ? expression : "";
    p.pos = 0;
    p.ans = ans;
    p.depth = 0;
    p.status = CALC_OK;
    p.error_offset = 0;

    if (next_token(&p)) {
        if (p.tok.kind == TOKEN_END) {
            syntax_error(&p, CALC_ERR_EMPTY, p.tok.offset);
        } else if (parse_expression(&p, &value) && p.tok.kind != TOKEN_END) {
            syntax_error(&p,
                         p.tok.kind == TOKEN_RPAREN ? CALC_ERR_UNMATCHED_RPAREN
                                                    : CALC_ERR_EXPECTED_OPERATOR,
                         p.tok.offset);
        }
    }

    result.status = p.status;
    if (p.status == CALC_OK) {
        result.value = value == 0.0 ? 0.0 : value; /* no negative zero */
        result.error_offset = 0;
    } else {
        result.value = 0.0;
        result.error_offset = p.error_offset;
    }
    return result;
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

    if (isnan(value)) {
        snprintf(text, sizeof text, "nan");
    } else if (isinf(value)) {
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
