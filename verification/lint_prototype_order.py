"""Catches the Arduino auto-prototype ordering bug.

The Arduino IDE synthesises a prototype for every top-level function and injects
the whole block immediately before the FIRST function definition in the sketch.
A prototype whose signature names a struct, enum or class declared later in the
file therefore refers to an undeclared type, and the build fails with
"'X' does not name a type" -- pointing at the function, not at the real cause.

Plain g++ never reproduces this, because it does not synthesise prototypes. So a
sketch can pass a -fsyntax-only check and still fail in the IDE. This lint closes
that gap: every user-defined type named in a top-level function signature must be
declared before the first function definition.
"""
import re, sys

KEYWORDS = {"if", "for", "while", "switch", "else", "do", "return", "struct",
            "enum", "class", "union", "namespace", "extern", "typedef"}


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"),
                  text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def main(path):
    src = strip_comments(open(path, encoding="utf-8").read())
    lines = src.split("\n")

    types = {}
    for i, ln in enumerate(lines):
        m = re.match(r"\s*(?:typedef\s+)?(?:struct|enum|class|union)\s+([A-Za-z_]\w*)", ln)
        if m:
            types.setdefault(m.group(1), i)

    funcs = []
    for i, ln in enumerate(lines):
        if not ln or ln[0].isspace():
            continue
        m = re.match(r"([A-Za-z_][\w\s\*&:]*?)\s+\*?([A-Za-z_]\w*)\s*\(", ln)
        if not m:
            continue
        if m.group(2) in KEYWORDS or m.group(1).split()[0] in KEYWORDS:
            continue
        sig = ln
        j = i
        while ")" not in sig and j + 1 < len(lines):
            j += 1
            sig += " " + lines[j]
        if not re.search(r"\)\s*\{?\s*$", sig.strip()):
            continue
        if sig.rstrip().endswith(";"):
            continue
        funcs.append((i, m.group(2), sig))

    if not funcs:
        print("lint: no top-level functions found -- check the parser")
        return 1
    first = funcs[0][0]

    bad = []
    for line_no, name, sig in funcs:
        head = sig.split(")")[0]
        for tname, tline in types.items():
            if re.search(r"\b%s\b" % re.escape(tname), head) and tline >= first:
                bad.append((line_no + 1, name, tname, tline + 1))

    if bad:
        print("LINT FAILED -- Arduino will inject prototypes at line %d" % (first + 1))
        for line_no, name, tname, tline in bad:
            print("  %s() at line %d names type '%s', declared at line %d"
                  % (name, line_no, tname, tline))
        print("  Move each of these type declarations above line %d." % (first + 1))
        return 1

    print("  pass Arduino prototype ordering (%d functions, %d types, "
          "prototypes inject at line %d)" % (len(funcs), len(types), first + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
