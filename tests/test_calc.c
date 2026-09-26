/*
 * test_calc.c - unit tests for the expression evaluator in src/calc.c.
 *
 * Prints every failed check and exits with status 1 if there was any.
 */
#include "calc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846
#define E  2.71828182845904523536

static int checks;
static int failures;

static void check_value(int line, const char *expression, double ans,
                        double expected)
{
    const calc_result result = calc_evaluate(expression, ans);

    checks++;
    if (result.status != CALC_OK) {
        failures++;
        printf("test_calc.c:%d: \"%s\": expected %.17g, got error \"%s\"\n",
               line, expression, expected,
               calc_status_message(result.status));
    } else if (!(fabs(result.value - expected) <=
                 1e-12 * fmax(1.0, fabs(expected)))) {
        failures++;
        printf("test_calc.c:%d: \"%s\": expected %.17g, got %.17g\n", line,
               expression, expected, result.value);
    }
}

static void check_error(int line, const char *expression, calc_status status,
                        size_t offset)
{
    const calc_result result = calc_evaluate(expression, 0.0);

    checks++;
    if (result.status != status || result.error_offset != offset) {
        failures++;
        printf("test_calc.c:%d: \"%s\": expected \"%s\" at offset %lu, "
               "got \"%s\" at offset %lu\n",
               line, expression, calc_status_message(status),
               (unsigned long)offset, calc_status_message(result.status),
               (unsigned long)result.error_offset);
    }
}

/* Checks the whole location of an error: the offending token, and the
 * construct around it. */
static void check_span(int line, const char *expression, calc_status status,
                       size_t offset, size_t length, size_t start, size_t end)
{
    const calc_result r = calc_evaluate(expression, 0.0);

    checks++;
    if (r.status != status || r.error_offset != offset ||
        r.error_length != length || r.error_start != start ||
        r.error_end != end) {
        failures++;
        printf("test_calc.c:%d: \"%s\": expected \"%s\" at %lu+%lu in "
               "[%lu, %lu), got \"%s\" at %lu+%lu in [%lu, %lu)\n",
               line, expression, calc_status_message(status),
               (unsigned long)offset, (unsigned long)length,
               (unsigned long)start, (unsigned long)end,
               calc_status_message(r.status), (unsigned long)r.error_offset,
               (unsigned long)r.error_length, (unsigned long)r.error_start,
               (unsigned long)r.error_end);
    }
}

static void check_suggestion(int line, const char *word, const char *expected)
{
    const char *suggestion = calc_suggest_name(word, strlen(word));

    checks++;
    if (expected == NULL ? suggestion != NULL
                         : suggestion == NULL ||
                               strcmp(suggestion, expected) != 0) {
        failures++;
        printf("test_calc.c:%d: suggestion for \"%s\": expected %s, got %s\n",
               line, word, expected != NULL ? expected : "none",
               suggestion != NULL ? suggestion : "none");
    }
}

static void check_format(int line, double value, const char *expected)
{
    char text[CALC_FORMAT_SIZE];

    calc_format(value, text, sizeof text);
    checks++;
    if (strcmp(text, expected) != 0) {
        failures++;
        printf("test_calc.c:%d: formatting %.17g: expected \"%s\", got "
               "\"%s\"\n",
               line, value, expected, text);
    }
}

static void check(int line, int condition, const char *description)
{
    checks++;
    if (!condition) {
        failures++;
        printf("test_calc.c:%d: %s\n", line, description);
    }
}

#define EXPECT_VALUE(expression, expected) \
    check_value(__LINE__, expression, 0.0, expected)
#define EXPECT_VALUE_WITH_ANS(expression, ans, expected) \
    check_value(__LINE__, expression, ans, expected)
#define EXPECT_ERROR(expression, status, offset) \
    check_error(__LINE__, expression, status, offset)
#define EXPECT_SPAN(expression, status, offset, length, start, end) \
    check_span(__LINE__, expression, status, offset, length, start, end)
#define EXPECT_SUGGESTION(word, expected) \
    check_suggestion(__LINE__, word, expected)
#define EXPECT_FORMAT(value, expected) \
    check_format(__LINE__, value, expected)
#define EXPECT(condition) check(__LINE__, (condition), #condition)

static void test_numbers(void)
{
    EXPECT_VALUE("0", 0);
    EXPECT_VALUE("42", 42);
    EXPECT_VALUE("3.14", 3.14);
    EXPECT_VALUE(".5", 0.5);
    EXPECT_VALUE("5.", 5);
    EXPECT_VALUE("007", 7);
    EXPECT_VALUE("1e3", 1000);
    EXPECT_VALUE("1E3", 1000);
    EXPECT_VALUE("2.5e-3", 0.0025);
    EXPECT_VALUE("1e+2", 100);
    EXPECT_VALUE("1.e2", 100);
    EXPECT_VALUE("6.02214076e23", 6.02214076e23);
    EXPECT_VALUE("1e-400", 0); /* underflow to zero is not an error */
    EXPECT_VALUE("  7  ", 7);
    EXPECT_VALUE("\t1\t+\t2\r\n", 3);
}

static void test_operators(void)
{
    EXPECT_VALUE("1 + 2", 3);
    EXPECT_VALUE("7 - 10", -3);
    EXPECT_VALUE("6 * 7", 42);
    EXPECT_VALUE("7 / 2", 3.5);
    EXPECT_VALUE("7 % 3", 1);
    EXPECT_VALUE("-7 % 3", -1);
    EXPECT_VALUE("7 % -3", 1);
    EXPECT_VALUE("7.5 % 2", 1.5);
    EXPECT_VALUE("2 ^ 10", 1024);
    EXPECT_VALUE("2 ^ 0.5", sqrt(2.0));
    EXPECT_VALUE("4 ^ -0.5", 0.5);
    EXPECT_VALUE("(-2) ^ 3", -8);
    EXPECT_VALUE("0 ^ 0", 1);
    EXPECT_VALUE("2147483647 + 1", 2147483648.0); /* no int overflow */
    EXPECT_VALUE("-2147483648 / -1", 2147483648.0);
}

static void test_precedence(void)
{
    EXPECT_VALUE("2 + 3 * 4", 14);
    EXPECT_VALUE("(2 + 3) * 4", 20);
    EXPECT_VALUE("1 + 2 * 3 - 4 / 2", 5);
    EXPECT_VALUE("10 - 4 - 3", 3);
    EXPECT_VALUE("100 / 10 / 5", 2);
    EXPECT_VALUE("8 % 3 * 2", 4);
    EXPECT_VALUE("2 ^ 3 ^ 2", 512);
    EXPECT_VALUE("2 ^ 3 * 2", 16);
    EXPECT_VALUE("2 * 3 ^ 2", 18);
    EXPECT_VALUE("2 * (3 + 4) ^ 2", 98);
    EXPECT_VALUE("-2 ^ 2", -4);
    EXPECT_VALUE("(-2) ^ 2", 4);
    EXPECT_VALUE("2 ^ -1", 0.5);
    EXPECT_VALUE("-2 ^ -2", -0.25);
    EXPECT_VALUE("2 ^ -3 ^ 2", 1.0 / 512);
    EXPECT_VALUE("2 * -3", -6);
    EXPECT_VALUE("2 - -3", 5);
    EXPECT_VALUE("--3", 3);
    EXPECT_VALUE("+-+3", -3);
    EXPECT_VALUE("-(2 + 3)", -5);
    EXPECT_VALUE("((((1))))", 1);
}

static void test_names(void)
{
    EXPECT_VALUE("pi", PI);
    EXPECT_VALUE("e", E);
    EXPECT_VALUE("PI", PI);
    EXPECT_VALUE("Pi * 2", 2 * PI);
    EXPECT_VALUE("e ^ 2", E * E);
    EXPECT_VALUE_WITH_ANS("ans", 42, 42);
    EXPECT_VALUE_WITH_ANS("ans * 2", 21, 42);
    EXPECT_VALUE_WITH_ANS("ANS + 1", -1, 0);

    EXPECT_VALUE("abs(-5)", 5);
    EXPECT_VALUE("sqrt(16)", 4);
    EXPECT_VALUE("sqrt(2)", sqrt(2.0));
    EXPECT_VALUE("cbrt(-27)", -3);
    EXPECT_VALUE("exp(0)", 1);
    EXPECT_VALUE("exp(1)", E);
    EXPECT_VALUE("ln(e)", 1);
    EXPECT_VALUE("log(1000)", 3);
    EXPECT_VALUE("log2(1024)", 10);
    EXPECT_VALUE("sin(0)", 0);
    EXPECT_VALUE("sin(pi / 2)", 1);
    EXPECT_VALUE("cos(0)", 1);
    EXPECT_VALUE("cos(pi)", -1);
    EXPECT_VALUE("tan(pi / 4)", 1);
    EXPECT_VALUE("asin(1)", PI / 2);
    EXPECT_VALUE("acos(1)", 0);
    EXPECT_VALUE("atan(1)", PI / 4);
    EXPECT_VALUE("floor(-2.5)", -3);
    EXPECT_VALUE("ceil(-2.5)", -2);
    EXPECT_VALUE("round(2.5)", 3);
    EXPECT_VALUE("round(-2.5)", -3);
    EXPECT_VALUE("SQRT(4)", 2);
    EXPECT_VALUE(" sqrt ( 4 ) ", 2);
    EXPECT_VALUE("sqrt(sqrt(16))", 2);
    EXPECT_VALUE("sqrt(3^2 + 4^2)", 5);
    EXPECT_VALUE("-sqrt(4)", -2);
    EXPECT_VALUE("sqrt(4) ^ 2", 4);
    EXPECT_VALUE("abs(-2) * 3", 6);
}

static void test_syntax_errors(void)
{
    EXPECT_ERROR("", CALC_ERR_EMPTY, 0);
    EXPECT_ERROR("   ", CALC_ERR_EMPTY, 3);
    EXPECT_ERROR(NULL, CALC_ERR_EMPTY, 0);

    EXPECT_ERROR("2 $ 3", CALC_ERR_INVALID_CHAR, 2);
    EXPECT_ERROR("2,5", CALC_ERR_INVALID_CHAR, 1);
    EXPECT_ERROR("5 \xE2\x82\xAC 3", CALC_ERR_INVALID_CHAR, 2); /* euro sign */

    EXPECT_ERROR("1.2.3", CALC_ERR_INVALID_NUMBER, 0);
    EXPECT_ERROR("1 + .", CALC_ERR_INVALID_NUMBER, 4);
    EXPECT_ERROR("1e", CALC_ERR_INVALID_NUMBER, 0);
    EXPECT_ERROR("1e+", CALC_ERR_INVALID_NUMBER, 0);
    EXPECT_ERROR("0x1F", CALC_ERR_INVALID_NUMBER, 0);
    EXPECT_ERROR("2pi", CALC_ERR_INVALID_NUMBER, 0);
    EXPECT_ERROR("1_000", CALC_ERR_INVALID_NUMBER, 0);

    EXPECT_ERROR("x", CALC_ERR_UNKNOWN_NAME, 0);
    EXPECT_ERROR("1 + foo(2)", CALC_ERR_UNKNOWN_NAME, 4);

    EXPECT_ERROR("2 +", CALC_ERR_EXPECTED_OPERAND, 3);
    EXPECT_ERROR("2 ^", CALC_ERR_EXPECTED_OPERAND, 3);
    EXPECT_ERROR("-", CALC_ERR_EXPECTED_OPERAND, 1);
    EXPECT_ERROR("* 3", CALC_ERR_EXPECTED_OPERAND, 0);
    EXPECT_ERROR("()", CALC_ERR_EXPECTED_OPERAND, 1);
    EXPECT_ERROR(")", CALC_ERR_EXPECTED_OPERAND, 0);
    EXPECT_ERROR("2 + )", CALC_ERR_EXPECTED_OPERAND, 4);

    EXPECT_ERROR("2 3", CALC_ERR_EXPECTED_OPERATOR, 2);
    EXPECT_ERROR("2 (3)", CALC_ERR_EXPECTED_OPERATOR, 2);
    EXPECT_ERROR("(1)(2)", CALC_ERR_EXPECTED_OPERATOR, 3);
    EXPECT_ERROR("pi(2)", CALC_ERR_EXPECTED_OPERATOR, 2);

    EXPECT_ERROR("sqrt 4", CALC_ERR_EXPECTED_LPAREN, 5);
    EXPECT_ERROR("sqrt", CALC_ERR_EXPECTED_LPAREN, 4);

    EXPECT_ERROR("(2 + 3", CALC_ERR_EXPECTED_RPAREN, 6);
    EXPECT_ERROR("(2 3)", CALC_ERR_EXPECTED_RPAREN, 3);
    EXPECT_ERROR("sqrt(4", CALC_ERR_EXPECTED_RPAREN, 6);

    EXPECT_ERROR("2)", CALC_ERR_UNMATCHED_RPAREN, 1);
    EXPECT_ERROR("(2))", CALC_ERR_UNMATCHED_RPAREN, 3);
}

static void test_math_errors(void)
{
    EXPECT_ERROR("1 / 0", CALC_ERR_DIVISION_BY_ZERO, 2);
    EXPECT_ERROR("1 / -0", CALC_ERR_DIVISION_BY_ZERO, 2);
    EXPECT_ERROR("1 / (2 - 2)", CALC_ERR_DIVISION_BY_ZERO, 2);
    EXPECT_ERROR("1 % 0", CALC_ERR_DIVISION_BY_ZERO, 2);
    EXPECT_ERROR("0 ^ -1", CALC_ERR_DIVISION_BY_ZERO, 2);

    EXPECT_ERROR("sqrt(-1)", CALC_ERR_DOMAIN, 0);
    EXPECT_ERROR("ln(-1)", CALC_ERR_DOMAIN, 0);
    EXPECT_ERROR("asin(2)", CALC_ERR_DOMAIN, 0);
    EXPECT_ERROR("acos(-1.5)", CALC_ERR_DOMAIN, 0);
    EXPECT_ERROR("(-8) ^ (1/3)", CALC_ERR_DOMAIN, 5);

    EXPECT_ERROR("1e999", CALC_ERR_RANGE, 0);
    EXPECT_ERROR("2 * 1e999", CALC_ERR_RANGE, 4);
    EXPECT_ERROR("10 ^ 400", CALC_ERR_RANGE, 3);
    EXPECT_ERROR("exp(1000)", CALC_ERR_RANGE, 0);
    EXPECT_ERROR("1e308 * 10", CALC_ERR_RANGE, 6);
    EXPECT_ERROR("-1e308 - 1e308", CALC_ERR_RANGE, 7);
    EXPECT_ERROR("ln(0)", CALC_ERR_RANGE, 0);
    EXPECT_ERROR("log(0)", CALC_ERR_RANGE, 0);

    /* "ans" is only ever a real number, whatever a caller passes in. */
    EXPECT(calc_evaluate("ans", strtod("inf", NULL)).status == CALC_ERR_RANGE);
    EXPECT(calc_evaluate("ans + 1", strtod("nan", NULL)).status ==
           CALC_ERR_DOMAIN);
}

static void test_error_precedence(void)
{
    /* Syntax errors win over math errors, wherever they are. */
    EXPECT_ERROR("1/0)", CALC_ERR_UNMATCHED_RPAREN, 3);
    EXPECT_ERROR("1/0 + $", CALC_ERR_INVALID_CHAR, 6);
    EXPECT_ERROR("sqrt(-1) + (", CALC_ERR_EXPECTED_OPERAND, 12);
    EXPECT_ERROR("1e999 2", CALC_ERR_EXPECTED_OPERATOR, 6);

    /* Otherwise the first math error in evaluation order is reported... */
    EXPECT_ERROR("1/0 + sqrt(-1)", CALC_ERR_DIVISION_BY_ZERO, 1);
    EXPECT_ERROR("sqrt(-1) + 1/0", CALC_ERR_DOMAIN, 0);
    EXPECT_ERROR("1/0 * 0", CALC_ERR_DIVISION_BY_ZERO, 1);

    /* ...even when later operations would hide it. */
    EXPECT_ERROR("sqrt(-1) ^ 0", CALC_ERR_DOMAIN, 0);
}

static void test_friendly_input(void)
{
    /* A final "=", as on a pocket calculator. */
    EXPECT_VALUE("2 + 2 =", 4);
    EXPECT_VALUE("2 + 2 =  ", 4);
    EXPECT_VALUE("(1 + 2) * 3=", 9);
    EXPECT_ERROR("=", CALC_ERR_EMPTY, 0);
    EXPECT_ERROR("2 + =", CALC_ERR_EXPECTED_OPERAND, 4);
    EXPECT_ERROR("2 = 2", CALC_ERR_INVALID_CHAR, 2);
    EXPECT_ERROR("2 ==", CALC_ERR_INVALID_CHAR, 2);

    /* Typographic symbols, as pasted from documents. */
    EXPECT_VALUE("6 \xC3\x97 7", 42);          /* multiplication sign */
    EXPECT_VALUE("6\xC3\x97" "7", 42);
    EXPECT_VALUE("84 \xC3\xB7 2", 42);         /* division sign */
    EXPECT_VALUE("50 \xE2\x88\x92 8", 42);     /* minus sign */
    EXPECT_VALUE("\xE2\x88\x92" "5", -5);
    EXPECT_VALUE("\xCF\x80", PI);               /* pi */
    EXPECT_VALUE("2 \xC3\x97 \xCF\x80", 2 * PI);
    EXPECT_ERROR("2\xCF\x80", CALC_ERR_EXPECTED_OPERATOR, 1);
    EXPECT_ERROR("1 \xC3\xB7 0", CALC_ERR_DIVISION_BY_ZERO, 2);

    EXPECT_VALUE("log10(1000)", 3);
}

static void test_error_spans(void)
{
    /* Syntax errors: the offending token. */
    EXPECT_SPAN("sqr(2)", CALC_ERR_UNKNOWN_NAME, 0, 3, 0, 3);
    EXPECT_SPAN("1 + 1.2.3", CALC_ERR_INVALID_NUMBER, 4, 5, 4, 9);
    EXPECT_SPAN("2pi", CALC_ERR_INVALID_NUMBER, 0, 3, 0, 3);
    EXPECT_SPAN("1e+5e", CALC_ERR_INVALID_NUMBER, 0, 5, 0, 5);
    EXPECT_SPAN("2 34", CALC_ERR_EXPECTED_OPERATOR, 2, 2, 2, 4);
    EXPECT_SPAN("2 +", CALC_ERR_EXPECTED_OPERAND, 3, 0, 3, 3);
    EXPECT_SPAN("sqrt 4", CALC_ERR_EXPECTED_LPAREN, 5, 1, 5, 6);
    EXPECT_SPAN("5 \xE2\x82\xAC 3", CALC_ERR_INVALID_CHAR, 2, 3, 2, 5);
    EXPECT_SPAN("\xE2\x82", CALC_ERR_INVALID_CHAR, 0, 2, 0, 2); /* cut short */

    /* Math errors: the operator or function, and all of the operation. */
    EXPECT_SPAN("10 / (5 - 5)", CALC_ERR_DIVISION_BY_ZERO, 3, 1, 0, 12);
    EXPECT_SPAN("1 + 2 * 3 / 0", CALC_ERR_DIVISION_BY_ZERO, 10, 1, 4, 13);
    EXPECT_SPAN("-(1 % 0)", CALC_ERR_DIVISION_BY_ZERO, 4, 1, 2, 7);
    EXPECT_SPAN("2 * 10 ^ 400", CALC_ERR_RANGE, 7, 1, 4, 12);
    EXPECT_SPAN("(-2) ^ 0.5", CALC_ERR_DOMAIN, 5, 1, 0, 10);
    EXPECT_SPAN("1 + sqrt(-1)", CALC_ERR_DOMAIN, 4, 4, 4, 12);
    EXPECT_SPAN("1 + sqrt ( -1 ) * 2", CALC_ERR_DOMAIN, 4, 4, 4, 15);
    EXPECT_SPAN("2 * 1e999", CALC_ERR_RANGE, 4, 5, 4, 9);
    EXPECT_SPAN("6 \xC3\xB7 0", CALC_ERR_DIVISION_BY_ZERO, 2, 2, 0, 6);
}

static void test_name_list(void)
{
    const char *name;
    bool is_function = false;
    size_t i, functions = 0, constants = 0;

    for (i = 0; (name = calc_name(i, &is_function)) != NULL; i++) {
        const calc_result r = calc_evaluate(name, 1.0);

        /* Every listed name must be understood: constants on their own,
         * functions with an argument. */
        if (is_function) {
            char call[32];

            functions++;
            EXPECT(r.status == CALC_ERR_EXPECTED_LPAREN);
            snprintf(call, sizeof call, "%s(1)", name);
            EXPECT(calc_evaluate(call, 0.0).status == CALC_OK);
        } else {
            constants++;
            EXPECT(r.status == CALC_OK);
        }
    }
    EXPECT(functions == 17);
    EXPECT(constants == 3);
    EXPECT(calc_name(0, NULL) != NULL && strcmp(calc_name(0, NULL), "abs") == 0);
    EXPECT(calc_name(i, NULL) == NULL);
    EXPECT(calc_name((size_t)-1, NULL) == NULL);
}

static void test_suggestions(void)
{
    EXPECT_SUGGESTION("sqr", "sqrt");
    EXPECT_SUGGESTION("SQR", "sqrt");
    EXPECT_SUGGESTION("sqtr", "sqrt");
    EXPECT_SUGGESTION("sqroot", "sqrt");
    EXPECT_SUGGESTION("flor", "floor");
    EXPECT_SUGGESTION("pie", "pi");
    EXPECT_SUGGESTION("arcsin", "asin");
    EXPECT_SUGGESTION("sine", "sin");
    EXPECT_SUGGESTION("cosine", "cos");
    EXPECT_SUGGESTION("tangent", "tan");
    EXPECT_SUGGESTION("absolute", "abs");
    EXPECT_SUGGESTION("logarithm", "log");
    EXPECT_SUGGESTION("x", NULL);
    EXPECT_SUGGESTION("ex", NULL);
    EXPECT_SUGGESTION("xyz", NULL);
    EXPECT_SUGGESTION("pow", NULL); /* two edits from "log": too far */
    EXPECT_SUGGESTION("mod", NULL);
    EXPECT_SUGGESTION("squareroot", NULL);
    EXPECT_SUGGESTION("abcdefghijklmnopqrstuvwxyz", NULL);
    EXPECT(calc_suggest_name(NULL, 3) == NULL);
    /* Only `length` bytes count, and a known name suggests itself. */
    EXPECT(calc_suggest_name("sqrt(2)", 4) != NULL &&
           strcmp(calc_suggest_name("sqrt(2)", 4), "sqrt") == 0);
}

static void test_nesting_limit(void)
{
    static char text[4 * CALC_MAX_DEPTH + 16];
    size_t n;

    /* CALC_MAX_DEPTH levels of parentheses are fine, one more is not. */
    n = CALC_MAX_DEPTH;
    memset(text, '(', n);
    text[n] = '1';
    memset(text + n + 1, ')', n);
    text[2 * n + 1] = '\0';
    EXPECT_VALUE(text, 1);

    n = CALC_MAX_DEPTH + 1;
    memset(text, '(', n);
    text[n] = '1';
    memset(text + n + 1, ')', n);
    text[2 * n + 1] = '\0';
    EXPECT_ERROR(text, CALC_ERR_TOO_DEEP, CALC_MAX_DEPTH + 1);

    /* The same goes for signs. */
    n = CALC_MAX_DEPTH;
    memset(text, '-', n);
    text[n] = '1';
    text[n + 1] = '\0';
    EXPECT_VALUE(text, CALC_MAX_DEPTH % 2 == 0 ? 1 : -1);

    n = CALC_MAX_DEPTH + 1;
    memset(text, '-', n);
    text[n] = '1';
    text[n + 1] = '\0';
    EXPECT_ERROR(text, CALC_ERR_TOO_DEEP, CALC_MAX_DEPTH + 1);

    /* Far deeper input is rejected without exhausting the stack. */
    n = sizeof text - 1;
    memset(text, '(', n);
    text[n] = '\0';
    EXPECT_ERROR(text, CALC_ERR_TOO_DEEP, CALC_MAX_DEPTH + 1);
}

static void test_no_negative_zero(void)
{
    /* copysign rather than signbit, whose macro some libraries (MinGW's)
     * define in a way that draws -Wconversion warnings. */
    EXPECT(copysign(1.0, calc_evaluate("-0", 0.0).value) > 0.0);
    EXPECT(copysign(1.0, calc_evaluate("0 * -1", 0.0).value) > 0.0);
    EXPECT(copysign(1.0, calc_evaluate("round(-0.4)", 0.0).value) > 0.0);
    EXPECT(copysign(1.0, -0.0) < 0.0); /* the check itself works */
}

static void test_format(void)
{
    char small[4];

    EXPECT_FORMAT(0.0, "0");
    EXPECT_FORMAT(-0.0, "0");
    EXPECT_FORMAT(14, "14");
    EXPECT_FORMAT(-3, "-3");
    EXPECT_FORMAT(3.5, "3.5");
    EXPECT_FORMAT(-0.001, "-0.001");
    EXPECT_FORMAT(0.0001, "0.0001");
    EXPECT_FORMAT(123456.789, "123456.789");
    EXPECT_FORMAT(0.1 + 0.2, "0.3");
    EXPECT_FORMAT(1.0 / 3, "0.333333333333333");
    EXPECT_FORMAT(2.0 / 3, "0.666666666666667");
    EXPECT_FORMAT(sqrt(2.0), "1.4142135623731");

    /* Whole numbers are shown in full as long as a double holds them exactly
     * (up to 2^53), and with 15 significant digits beyond. */
    EXPECT_FORMAT(1e15, "1000000000000000");
    EXPECT_FORMAT(9007199254740992.0, "9007199254740992");
    EXPECT_FORMAT(-9007199254740992.0, "-9007199254740992");
    EXPECT_FORMAT(9007199254740994.0, "9.00719925474099e+15");
    EXPECT_FORMAT(1152921504606846976.0, "1.15292150460685e+18");

    /* Exponents have no leading zeros. */
    EXPECT_FORMAT(1e21, "1e+21");
    EXPECT_FORMAT(1e100, "1e+100");
    EXPECT_FORMAT(1e-5, "1e-5");
    EXPECT_FORMAT(-1.5e-7, "-1.5e-7");
    EXPECT_FORMAT(1.7976931348623157e308, "1.79769313486232e+308");
    EXPECT_FORMAT(5e-324, "4.94065645841247e-324");

    EXPECT_FORMAT(strtod("inf", NULL), "inf");
    EXPECT_FORMAT(-strtod("inf", NULL), "-inf");
    EXPECT_FORMAT(strtod("nan", NULL), "nan");

    /* Output is truncated to the buffer, and always terminated. */
    calc_format(12345, small, sizeof small);
    EXPECT(strcmp(small, "123") == 0);
    calc_format(12345, small, 1);
    EXPECT(small[0] == '\0');
    calc_format(12345, NULL, 0); /* must not crash */
}

static void test_status_messages(void)
{
    static const calc_status statuses[] = {
        CALC_OK,
        CALC_ERR_EMPTY,
        CALC_ERR_INVALID_CHAR,
        CALC_ERR_INVALID_NUMBER,
        CALC_ERR_UNKNOWN_NAME,
        CALC_ERR_EXPECTED_OPERAND,
        CALC_ERR_EXPECTED_OPERATOR,
        CALC_ERR_EXPECTED_LPAREN,
        CALC_ERR_EXPECTED_RPAREN,
        CALC_ERR_UNMATCHED_RPAREN,
        CALC_ERR_TOO_DEEP,
        CALC_ERR_DIVISION_BY_ZERO,
        CALC_ERR_DOMAIN,
        CALC_ERR_RANGE,
    };
    const size_t count = sizeof statuses / sizeof statuses[0];
    size_t i, j;

    for (i = 0; i < count; i++) {
        const char *message = calc_status_message(statuses[i]);

        EXPECT(message != NULL && message[0] != '\0');
        for (j = 0; message != NULL && j < i; j++) {
            const char *other = calc_status_message(statuses[j]);

            EXPECT(other == NULL || strcmp(message, other) != 0);
        }
    }
    EXPECT(strcmp(calc_status_message((calc_status)99), "unknown error") == 0);
}

int main(void)
{
    test_numbers();
    test_operators();
    test_precedence();
    test_names();
    test_syntax_errors();
    test_math_errors();
    test_error_precedence();
    test_friendly_input();
    test_error_spans();
    test_name_list();
    test_suggestions();
    test_nesting_limit();
    test_no_negative_zero();
    test_format();
    test_status_messages();

    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
