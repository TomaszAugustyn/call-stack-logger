#!/usr/bin/env python3
"""The versions of src/main.cpp that the demo GIF shows, derived from the real file.

The GIF types parts of the demo in live, so it needs the file as it looks at
each moment of the story. Every version is derived from the repo's real
src/main.cpp. Anchor lines are found by their content, not by fixed line
numbers, so an edit elsewhere in the file (one more comment line) only shifts
the numbers and breaks nothing:

  initial       class A, both A::foo() calls and fibonacci removed (GIF opening)
  intermediate  class A and the A::foo() calls typed back, fibonacci still
                missing (the first build: cmake .. && make run)
  full          the real file (the LOG_ELAPSED build)
  exceptions    the real file plus the exception demo functions above main(),
                fibonacci(6) commented out and the calls to the exception demo
                right below it (the LOG_EXCEPTIONS build)

render.py types the transitions between these versions; capture_inputs.sh
builds and runs them, so every trace line number in the GIF is the real one
for the code on screen at that moment.

Usage: python3 demo_source.py VERSION [SOURCE] > file
       (SOURCE defaults to the repo's src/main.cpp)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOURCE = os.path.join(HERE, "..", "..", "src", "main.cpp")

# The exception demo, typed right after cube(). The names and the shape follow
# README's "Exceptions in the trace tree" example: a throw that leaves two
# frames, with a destructor calling a traced helper on the way out, caught one
# level up; then a throw that nobody catches.
EXC_FUNCS = r"""void exc_helper() {}

struct ExcGuard {
    ~ExcGuard() { exc_helper(); }
};

void exc_thrower() {
    throw std::runtime_error("bad header");
}

void exc_mid() {
    ExcGuard guard;
    exc_thrower();
}

void exc_catcher() {
    try {
        exc_mid();
    } catch (const std::exception& e) {
        std::cout << "caught: " << e.what() << "\n";
    }
}

void uncaught_leaf() {
    throw std::logic_error("nobody catches this");
}

void uncaught_outer() {
    uncaught_leaf();
}""".split("\n")

# Typed right below the commented-out fibonacci(6);, so they are the first
# statements after b.foo(). The uncaught exception terminates the program:
# print() and cube() below never run, and their missing trace lines show
# that the program really ended there.
EXC_CALLS = [
    "    // Test logging an exception that is caught",
    "    exc_catcher();",
    "    // Test logging an exception nobody catches (terminates)",
    "    uncaught_outer();",
]


class Demo:
    """Anchor line numbers (1-based, in the real file) and the file versions."""

    def __init__(self, path=DEFAULT_SOURCE):
        self.full = open(path).read().rstrip("\n").split("\n")
        find = self._find
        self.a_first = find(lambda l: l == "class A {")
        self.a_blank = self.a_first - 1
        self.a_last = find(lambda l: l == "};", self.a_first)
        self.b_first = find(lambda l: l == "class B {")
        self.a_call_b = find(lambda l: l.strip() == "A::foo();", self.b_first)
        self.fib_first = find(lambda l: l.startswith("constexpr unsigned fibonacci"))
        self.fib_blank = self.fib_first - 1
        self.fib_last = find(lambda l: l == "}", self.fib_first)
        self.cube_first = find(lambda l: l.startswith("inline int cube("))
        self.cube_last = find(lambda l: l == "}", self.cube_first)
        self.main_first = find(lambda l: l == "int main() {")
        self.a_call_comment = find(
            lambda l: l.strip().startswith("// Test logging static member"), self.main_first)
        self.a_call_main = self.a_call_comment + 1
        self.fib_call_comment = find(
            lambda l: l.strip().startswith("// Test logging constexpr"), self.main_first)
        self.fib_call = self.fib_call_comment + 1
        self.cube_call = find(lambda l: l.strip() == "cube(3);", self.main_first)

        line = self.line
        checks = [
            (line(self.a_blank) == "", "no blank line above class A"),
            (line(self.a_call_main).strip() == "A::foo();",
             "the A::foo() call in main() does not follow its comment"),
            (line(self.fib_blank) == "", "no blank line above fibonacci"),
            (line(self.fib_call).strip() == "fibonacci(6);",
             "fibonacci(6); does not follow its comment"),
            (self.a_last < self.b_first < self.a_call_b < self.fib_blank
             < self.fib_last < self.cube_first < self.cube_last < self.main_first
             < self.a_call_comment < self.fib_call_comment < self.cube_call,
             "demo sections are in an unexpected order"),
            (self.full[-1] == "}", "main.cpp does not end with main()'s closing brace"),
        ]
        for ok, what in checks:
            if not ok:
                raise SystemExit("demo_source.py: src/main.cpp changed shape: " + what)

    def _find(self, pred, start=1):
        for n in range(start, len(self.full) + 1):
            if pred(self.full[n - 1]):
                return n
        raise SystemExit("demo_source.py: anchor line not found in src/main.cpp "
                         "(after line %d) — update Demo.__init__" % start)

    def line(self, n):
        return self.full[n - 1]

    def lines(self, first, last):
        """Lines first..last (1-based, inclusive) of the real file."""
        return self.full[first - 1:last]

    @staticmethod
    def _without(lines, ranges):
        out = list(lines)
        for first, last in sorted(ranges, reverse=True):
            del out[first - 1:last]
        return out

    def full_version(self):
        return list(self.full)

    def intermediate(self):
        return self._without(self.full, [(self.fib_call_comment, self.fib_call),
                                         (self.fib_blank, self.fib_last)])

    def initial(self):
        return self._without(self.full, [(self.fib_call_comment, self.fib_call),
                                         (self.a_call_comment, self.a_call_main),
                                         (self.fib_blank, self.fib_last),
                                         (self.a_call_b, self.a_call_b),
                                         (self.a_blank, self.a_last)])

    # Lines the exceptions act inserts above main(); the call site line
    # numbers of the exceptions version shift by this much.
    exc_shift = 1 + len(EXC_FUNCS)

    def commented_fib_call(self):
        text = self.line(self.fib_call)
        indent = len(text) - len(text.lstrip())
        return text[:indent] + "// " + text[indent:]

    def exceptions(self):
        # Bottom-up, so each edit leaves the line numbers above it valid.
        out = list(self.full)
        out[self.fib_call - 1] = self.commented_fib_call()
        out[self.fib_call:self.fib_call] = EXC_CALLS
        out[self.cube_last:self.cube_last] = [""] + EXC_FUNCS
        return out


def main():
    versions = ("initial", "intermediate", "full", "exceptions")
    if len(sys.argv) not in (2, 3) or sys.argv[1] not in versions:
        raise SystemExit("usage: demo_source.py {%s} [SOURCE]" % "|".join(versions))
    demo = Demo(sys.argv[2] if len(sys.argv) == 3 else DEFAULT_SOURCE)
    method = {"full": demo.full_version}.get(sys.argv[1]) or getattr(demo, sys.argv[1])
    sys.stdout.write("\n".join(method()) + "\n")


if __name__ == "__main__":
    main()
