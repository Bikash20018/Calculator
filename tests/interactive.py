#!/usr/bin/env python3
"""End-to-end tests of calci's interactive sessions, in a pseudo-terminal.

Usage: python3 tests/interactive.py [PATH-TO-CALCI]

Types into calci key by key, the way a person would, and checks what ends up
on the screen. A tiny terminal emulator turns calci's output, escape
sequences and all, back into rows of text. Needs a POSIX system.
"""

import fcntl
import os
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

CALCI = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./calci")

UP, DOWN, RIGHT, LEFT = "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D"
HOME, END, DELETE = "\x1b[H", "\x1b[F", "\x1b[3~"
CTRL_WORD_LEFT = "\x1b[1;5D"
CTRL = {c: chr(ord(c) - ord("a") + 1) for c in "abcdefklnpuwz"}
BACKSPACE, TAB, ENTER = "\x7f", "\t", "\r"
SGR = re.compile(rb"\x1b\[[0-9;]*m")


class Screen:
    """Just enough of a VT100 to replay what calci writes."""

    def __init__(self, columns):
        self.columns = columns
        self.rows = [[]]
        self.row = 0
        self.column = 0
        self.raw = b""

    def feed(self, data):
        self.raw += data
        text = data.decode("utf-8", "replace")
        i = 0
        while i < len(text):
            match = re.match(r"\x1b\[([0-9;?]*)([@-~])", text[i:])
            if match:
                self._control(match.group(1), match.group(2))
                i += match.end()
                continue
            self._put(text[i])
            i += 1

    def _put(self, char):
        if char == "\r":
            self.column = 0
        elif char == "\n":
            self.row += 1
        elif char == "\t":
            self.column = (self.column // 8 + 1) * 8
        elif char in "\a\x1b":
            pass
        else:
            if self.column >= self.columns:  # automatic margin
                self.row += 1
                self.column = 0
            while len(self.rows) <= self.row:
                self.rows.append([])
            cells = self.rows[self.row]
            cells.extend(" " * (self.column + 1 - len(cells)))
            cells[self.column] = char
            self.column += 1
        while len(self.rows) <= self.row:
            self.rows.append([])

    def _control(self, parameters, final):
        count = int(parameters) if parameters.isdigit() else 1
        if final == "K":
            del self.rows[self.row][self.column:]
        elif final == "C":
            self.column = min(self.column + count, self.columns - 1)
        elif final == "D":
            self.column = max(self.column - count, 0)
        elif final == "J" and parameters == "2":
            self.rows = [[]]
            self.row = self.column = 0
        elif final == "H":
            self.row = self.column = 0

    def lines(self):
        return ["".join(cells).rstrip() for cells in self.rows]

    def text(self):
        return "\n".join(self.lines())


class Session:
    def __init__(self, env_changes=None, columns=80):
        env = dict(os.environ, TERM="xterm")
        env.pop("NO_COLOR", None)
        env.update(env_changes or {})
        self.master, self.slave = os.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", 24, columns, 0, 0))
        self.process = subprocess.Popen(
            [CALCI], stdin=self.slave, stdout=self.slave, stderr=self.slave,
            env=env, start_new_session=True)
        self.screen = Screen(columns)

    def pump(self, condition, timeout=5.0):
        """Reads output until `condition(screen)` holds; tells whether it
        did."""
        deadline = time.monotonic() + timeout
        while not condition(self.screen):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            ready, _, _ = select.select([self.master], [], [],
                                        min(remaining, 0.05))
            if ready:
                self.screen.feed(os.read(self.master, 65536))
        return True

    def prompts(self):
        return sum(line.startswith(">") for line in self.screen.lines())

    def type(self, keys):
        os.write(self.master, keys.encode())

    def enter(self, keys=""):
        """Types `keys` and Enter, and waits for the next prompt."""
        count = self.prompts()
        self.type(keys + ENTER)
        return self.pump(lambda _: self.prompts() > count)

    def current_line(self):
        return self.screen.lines()[self.screen.row]

    def shows(self, line):
        return self.pump(lambda _: self.current_line() == line)

    def finish(self, keys=CTRL["d"]):
        self.type(keys)
        try:
            return self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            return None
        finally:
            self.pump(lambda _: False, timeout=0.2)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
            self.process.wait()
        os.close(self.master)
        os.close(self.slave)


checks = 0
failures = 0


def check(description, condition, session=None):
    global checks, failures
    checks += 1
    if not condition:
        failures += 1
        print(f"FAIL: {description}")
        if session is not None:
            print("  screen:\n    " + "\n    ".join(session.screen.lines()))


def test(function):
    session = Session(**getattr(function, "options", {}))
    try:
        ready = session.pump(lambda s: s.lines()[-1:] == [">"])
        check(f"{function.__name__}: calci shows a prompt", ready, session)
        if ready:
            function(session)
    finally:
        session.close()
    return function


def options(**kwargs):
    def decorate(function):
        function.options = kwargs
        return function
    return decorate


@test
def banner_and_results(s):
    check("banner", s.screen.lines()[0].startswith("calci 1.0.0"), s)
    s.enter("2 ^ 10")
    s.enter("ans / 4")
    lines = s.screen.lines()
    check("results", "= 1024" in lines and "= 256" in lines, s)
    check("Ctrl+D quits cleanly", s.finish() == 0, s)


@test
def error_marks_and_hints(s):
    s.enter("10 / (5 - 5)")
    s.enter("sqr(2)")
    lines = s.screen.lines()
    check("marks line up under the expression",
          "  ~~ ^ ~~~~~~~ error: division by zero" in lines, s)
    check("hint lines up under the message",
          "  ^~~ error: unknown function or constant" in lines
          and "      hint: did you mean sqrt?" in lines, s)
    check("a typo does not fail the session", s.finish() == 0, s)


@test
def history(s):
    s.enter("6 * 7")
    s.type(UP)
    check("Up recalls the last line", s.shows("> 6 * 7"), s)
    s.type(UP + DOWN)
    check("Down comes back to the new line", s.shows(">"), s)
    s.type("1 +" + UP + DOWN)
    check("the new line is kept while browsing", s.shows("> 1 +"), s)
    s.type(CTRL["u"] + UP)
    s.enter()
    check("a recalled line runs again",
          s.screen.lines().count("= 42") == 2, s)


@test
def cursor_movement(s):
    s.type("2+3" + LEFT + LEFT + "0")
    check("Left moves back over characters", s.shows("> 20+3"), s)
    s.enter()
    s.type("3)" + HOME + "(1+")
    check("Home goes to the start", s.shows("> (1+3)"), s)
    s.type(END + "*2")
    check("End goes to the end", s.shows("> (1+3)*2"), s)
    s.type(CTRL["a"] + DELETE + CTRL["e"] + BACKSPACE)
    check("Delete and Backspace", s.shows("> 1+3)*"), s)
    s.type(CTRL["a"] + CTRL["k"] + "7" + CTRL["b"] + "4" + CTRL["f"] + "2")
    check("Ctrl+A/B/F/K", s.shows("> 472"), s)
    s.type(CTRL["u"] + "sqrt(16) + 99" + CTRL["w"] + "1")
    check("Ctrl+W deletes a word", s.shows("> sqrt(16) + 1"), s)
    s.type(CTRL_WORD_LEFT + CTRL_WORD_LEFT + "2 * ")
    check("Ctrl+Left jumps words", s.shows("> sqrt(2 * 16) + 1"), s)
    s.enter()
    check("edited lines evaluate", "= 23" in s.screen.lines() and
          "= 6.65685424949238" in s.screen.lines(), s)


@test
def tab_completion(s):
    s.type("sq" + TAB)
    check("Tab completes a function with its (", s.shows("> sqrt("), s)
    s.type("16) + P" + TAB)
    check("Tab completes a constant, fixing case", s.shows("> sqrt(16) + pi"),
          s)
    s.enter()
    s.type("a" + TAB)
    check("Tab lists the candidates",
          s.pump(lambda sc: any(l.startswith("abs  ") and "ans" in l
                                for l in sc.lines())), s)
    check("the line survives the list", s.shows("> a"), s)
    s.type("c" + TAB)
    check("completion continues", s.shows("> acos("), s)
    s.type(CTRL["u"] + "zz" + TAB)
    check("nothing to complete", s.shows("> zz"), s)


@test
def cancel_clear_and_suspend(s):
    s.type("1 +" + CTRL["c"])
    check("Ctrl+C abandons the line without evaluating it",
          s.pump(lambda sc: sc.lines()[-2:] == ["> 1 +^C", ">"]) and
          s.pump(lambda sc: "error" in sc.text(), timeout=0.3) is False, s)
    s.enter("1")
    s.type(CTRL["l"])
    check("Ctrl+L clears the screen", s.pump(lambda sc: sc.lines() == [">"]),
          s)
    s.enter("2")
    s.type("clear" + ENTER)
    check("clear clears the screen", s.pump(lambda sc: sc.lines() == [">"]),
          s)
    s.type("4" + CTRL["z"] + "2")
    check("Ctrl+Z leaves the line intact", s.shows("> 42"), s)
    s.enter()
    check("and carries on", "= 42" in s.screen.lines(), s)


@test
def help_and_quit(s):
    s.enter("help")
    check("help shows the keys",
          any(l.startswith("Keys") and "Tab complete" in l
              for l in s.screen.lines()), s)
    check("quit exits with 0", s.finish("quit\r") == 0, s)


@test
def unicode_input(s):
    s.enter("6 \u00d7 7")
    check("typographic symbols", "= 42" in s.screen.lines(), s)
    s.type("2\u00d73" + LEFT + BACKSPACE)
    check("Backspace removes a whole UTF-8 character", s.shows("> 23"), s)
    s.enter()
    check("and the rest evaluates", "= 23" in s.screen.lines(), s)


@test
@options(columns=30)
def long_lines_scroll(s):
    expression = "+".join(str(n) for n in range(1, 25))  # 1+2+...+24
    s.type(expression)
    check("a long line scrolls to keep the cursor in view",
          s.pump(lambda sc: len(sc.lines()[sc.row]) < 30
                 and sc.lines()[sc.row].endswith("+23+24")
                 and "1+2+3" not in sc.lines()[sc.row]), s)
    s.type(HOME)
    check("and scrolls back", s.shows("> " + expression[:27]), s)
    s.enter()
    check("the whole line is evaluated", "= 300" in s.screen.lines(), s)


@test
def colors_on_a_terminal(s):
    s.enter("1 + 1")
    s.enter("1 / 0")
    raw = s.screen.raw
    check("results are colored", b"\x1b[1;32m2\x1b[0m" in raw, s)
    check("errors are colored", b"\x1b[1;31m" in raw, s)


@test
@options(env_changes={"NO_COLOR": "1"})
def no_color(s):
    s.enter("1 + 1")
    s.enter("1 / 0")
    check("NO_COLOR turns colors off", not SGR.search(s.screen.raw), s)
    check("but editing still works", "= 2" in s.screen.lines(), s)


@test
@options(env_changes={"TERM": "dumb"})
def dumb_terminal(s):
    s.enter("3 * 3")
    check("a dumb terminal gets no escape sequences",
          b"\x1b" not in s.screen.raw, s)
    check("and still works", "= 9" in s.screen.lines(), s)


@test
def terminal_restored_after_a_signal(s):
    check("raw mode while typing",
          not termios.tcgetattr(s.slave)[3] & termios.ICANON, s)
    s.process.send_signal(signal.SIGTERM)
    status = s.process.wait(timeout=5)
    mode = termios.tcgetattr(s.slave)
    check("dies of the signal", status == -signal.SIGTERM, s)
    check("with the terminal back in normal mode",
          mode[3] & termios.ICANON and mode[3] & termios.ECHO, s)


print(f"{checks} checks, {failures} failed")
sys.exit(1 if failures else 0)
