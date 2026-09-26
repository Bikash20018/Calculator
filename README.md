# Calculator

`calci` is a command-line calculator written in portable C99. Type an
expression, get the answer:

```console
$ calci "(2 + 3) * 4"
20
```

It follows the rules of arithmetic (precedence, parentheses, powers), handles
decimals and scientific notation, and points at mistakes instead of crashing
on them:

```console
$ calci "10 / (5 - 5)"
calci: error: division by zero
    10 / (5 - 5)
       ^
```

## Building

There are no dependencies beyond a C compiler and its standard library.

```sh
make            # builds ./calci
make test       # runs the unit and command-line tests
make install    # optional: copies calci to /usr/local/bin (set PREFIX to change)
```

Without `make`, compile the two source files directly:

```sh
cc -std=c99 -O2 -o calci src/main.c src/calc.c -lm    # Linux, macOS, MinGW
cl /O2 /Fecalci.exe src\main.c src\calc.c             # Visual Studio prompt
```

## Usage

### Interactive

Run `calci` on its own for a session. `ans` always holds the previous result.

```console
$ calci
calci - type an expression such as (2 + 3) * 4, "help" for help,
or "quit" to exit.
> 2 ^ 10
1024
> ans / 3
341.333333333333
> 2 + * 3
      ^ error: expected a number
> quit
```

`help` lists everything calci understands. Leave with `quit`, `exit`, or
end of input (Ctrl+D; on Windows, Ctrl+Z then Enter).

### A single expression

```console
$ calci "2 ^ 0.5"
1.4142135623731
$ calci 2 + 3
5
```

Quote the expression: unquoted, the shell treats characters such as `*`, `(`
and `)` specially.

### A file or pipe of expressions

When standard input is not a terminal, calci evaluates it line by line and
prints one result per line, which makes it easy to use from scripts. Blank
lines are skipped. An error is reported on standard error with its line
number; the lines after it are still evaluated.

```console
$ printf '1 + 1\n1 / 0\n3\n' | calci
2
calci: line 2: error: division by zero
    1 / 0
      ^
3
```

The exit status is 0 if everything was evaluated, and 1 otherwise. (In an
interactive session a typo is not a failure, so the status stays 0.)

## Reference

| Operator    | Meaning                         | Example                  |
| ----------- | ------------------------------- | ------------------------ |
| `( )`       | grouping                        | `(2 + 3) * 4` is 20      |
| `^`         | power, right-associative        | `2 ^ 3 ^ 2` is 512       |
| `+` `-`     | sign                            | `-2 ^ 2` is -4           |
| `*` `/` `%` | multiply, divide, remainder     | `7 % 3` is 1             |
| `+` `-`     | add, subtract                   | `10 - 4 - 3` is 3        |

Operators are listed from highest to lowest precedence; those on the same row
are evaluated from left to right, except `^`.

- **Numbers:** `42`, `3.14`, `.5`, `6.022e23`
- **Constants:** `pi`, `e`, and `ans`, the previous result (0 at first)
- **Functions:** `abs`, `sqrt`, `cbrt`, `exp`, `ln` (natural logarithm),
  `log` (base 10), `log2`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`,
  `floor`, `ceil`, `round`

A few details:

- Angles are in radians: `sin(30 * pi / 180)` is 0.5.
- `%` is the remainder of a division that truncates toward zero, as in C, so
  it has the sign of the dividend: `-7 % 3` is -1.
- There is no implicit multiplication: write `2 * pi`, not `2pi`.
- Names are case-insensitive: `PI` and `Sqrt(2)` work too.
- `round` rounds halves away from zero: `round(-2.5)` is -3.

Results are computed in double precision and shown with up to 15 significant
digits, so `0.1 + 0.2` shows as `0.3`. Whole numbers are shown in full up to
2^53 = 9007199254740992, the range in which a double holds every integer
exactly; beyond it they are rounded too: `2 ^ 64` shows as
`1.84467440737096e+19`.

## Errors

calci stops at the first problem and marks where it is. Syntax errors are
reported in preference to math errors, so `1 / 0 )` complains about the `)`.

| Message                            | Examples                                  |
| ---------------------------------- | ----------------------------------------- |
| `division by zero`                 | `1 / 0`, `5 % 0`, `0 ^ -1`                |
| `result is not a real number`      | `sqrt(-1)`, `ln(-2)`, `(-8) ^ (1/3)`      |
| `result is out of range`           | `10 ^ 400`, `exp(1000)`, `ln(0)`          |
| `expected a number`                | `2 +`, `* 3`, `()`                        |
| `expected an operator`             | `2 3`, `2 (3)`                            |
| `expected ')'`                     | `(2 + 3`                                  |
| `unmatched ')'`                    | `2 + 3)`                                  |
| `expected '(' after function name` | `sqrt 4`                                  |
| `unknown function or constant`     | `x + 1`, `foo(2)`                         |
| `invalid number`                   | `1.2.3`, `0x1F`, `2pi`                    |
| `invalid character`                | `2 $ 3`, `2,5`                            |
| `expression is nested too deeply`  | more than 256 levels of `(`, signs or `^` |

For a real cube root of a negative number, use `cbrt`: `cbrt(-8)` is -2.

## Project layout

| Path                        | Contents                                              |
| --------------------------- | ----------------------------------------------------- |
| `src/calc.h`, `src/calc.c`  | the evaluator: a recursive-descent parser with no I/O |
| `src/main.c`                | the `calci` command: arguments, sessions, messages    |
| `tests/test_calc.c`         | unit tests for the evaluator                          |
| `tests/cli.sh`              | end-to-end tests for the command                      |

The evaluator can be reused on its own: `calc_evaluate("2 + 2", 0.0)` returns
the value, or the kind of error and its position in the string. See
[`src/calc.h`](src/calc.h).
