#!/usr/bin/env python3
"""ETCC end-to-end test suite (Windows x64)."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ETCC = Path(os.environ.get("ETCC", ROOT / "build" / "etcc.exe"))
WORK = Path(os.environ.get("ETCC_TEST_WORK", ROOT / "build" / "tests"))
LLVM_READOBJ = shutil.which("llvm-readobj")

passed = 0
failed = 0
failures: list[str] = []


def run(argv, cwd=ROOT, timeout=120):
    return subprocess.run(
        [str(a) for a in argv],
        cwd=str(cwd),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )


def norm(text: str) -> str:
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    return "\n".join(line.rstrip() for line in text.split("\n")).strip("\n")


def check(name: str, condition: bool, detail: str = "") -> None:
    global passed, failed
    if condition:
        passed += 1
        print(f"PASS {name}")
    else:
        failed += 1
        failures.append(name)
        print(f"FAIL {name}")
        if detail:
            for line in detail.rstrip().splitlines():
                print(f"     {line}")


def expect_equal(name: str, actual: str, expected: str) -> None:
    if norm(actual) != norm(expected):
        check(
            name,
            False,
            f"expected:\n{norm(expected)}\nactual:\n{norm(actual)}",
        )
    else:
        check(name, True)


def compile_source(name: str, source: Path, extra=None):
    exe = WORK / f"{name}.exe"
    argv = [ETCC, source, "-o", exe]
    if extra:
        argv[1:1] = extra
    r = run(argv)
    if r.returncode != 0:
        check(name + " compile", False, r.stderr + r.stdout)
        return None
    return exe


def compile_and_run(name: str, source: Path, extra=None, args=None):
    exe = compile_source(name, source, extra)
    if exe is None:
        return None
    r = run([exe] + (args or []), cwd=WORK)
    if r.returncode != 0:
        check(name + " run", False, r.stderr + r.stdout)
        return None
    check(name + " compile", True)
    return r.stdout


def main() -> int:
    if not ETCC.exists():
        print(f"error: compiler not found at {ETCC}; run the CMake build first")
        return 1

    if WORK.exists():
        shutil.rmtree(WORK)
    WORK.mkdir(parents=True, exist_ok=True)

    # 1. The milestone: Hello, World with ETCC's own C library.
    out = compile_and_run("hello", ROOT / "tests" / "hello.c")
    if out is not None:
        expect_equal("hello output", out, "Hello, World!\n")

    # 1b. Prove the EXE does not import a system CRT.
    if LLVM_READOBJ:
        rr = run([LLVM_READOBJ, "--coff-imports", WORK / "hello.exe"])
        imports = (rr.stdout + rr.stderr).lower()
        check(
            "hello own runtime only",
            rr.returncode == 0
            and "kernel32.dll" in imports
            and not any(x in imports for x in ("vcruntime", "ucrtbase", "msvcrt")),
            rr.stdout + rr.stderr,
        )

    # 1c. Backend registry and version.
    r = run([ETCC, "--version"])
    check("version", r.returncode == 0 and "etcc " in r.stdout, r.stdout + r.stderr)
    r = run([ETCC, "--backend", "nope", ROOT / "tests" / "hello.c", "-o",
             WORK / "nope.exe"])
    check(
        "unknown backend",
        r.returncode != 0 and "unknown backend" in r.stderr,
        r.stderr,
    )
    exe = WORK / "hello_llvm.exe"
    r = run([ETCC, "--backend", "llvm", ROOT / "tests" / "hello.c", "-o", exe])
    check("explicit llvm backend", r.returncode == 0 and exe.exists(),
          r.stderr + r.stdout)

    # 2. argv support from the freestanding runtime entry point.
    out = compile_and_run(
        "args",
        ROOT / "tests" / "args.c",
        args=["one", "two words", "three"],
    )
    if out is not None:
        lines = norm(out).splitlines()
        check("args argc", lines[0] == "argc=4", "\n".join(lines))
        check("args argv1", len(lines) > 2 and lines[2] == "argv[1]=one",
              "\n".join(lines))
        check("args argv2", len(lines) > 3 and lines[3] == "argv[2]=two words",
              "\n".join(lines))
        check("args argv3", len(lines) > 4 and lines[4] == "argv[3]=three",
              "\n".join(lines))

    # 3. Feature programs.
    feature_expectations = {
        "features2": """\
3 7 global hi
0 2 4
0 1 1 10
size=4 1 4
bool=1
a b c
b
value=200
""",
        "features3": """\
s=12 fact=120
j=3
k=1 k=3
inner=20
outer=10
cmp=1 0 1
bits=2 7 5
shift=16 8
unary=-5 -1 0
sizeof=8
u=4000000000
big=1234567890123
""",
        "features4": """\
1 2 7
9 9
11
24 12 4
44
44
4464
4464
""",
        "features5": """\
hello etcc
42
33
inner=0
i=99
""",
        "features6": """\
5 5 5
comma=3
short
side=0
r=20 y=20
post=5 pre=7 dec=7
1 2 3
ptr=1
""",
        "strings": """\
tab\there
quote"slash\\
X
concatenated strings A
octal=65
""",
        "printf": """\
[-12][+12][   42][42   ][00042]
[42][52][beef][BEEF][0xbeef]
[abc][   ab][ab   ][abc]
[Z][%][0x0]
[123456][1234567890123][99]
""",
        "structs": """\
3 4 8
10 20
10 20 99 20
1 2 3 4 16
4 1
nodes=1 2
""",
        "structs2": """\
6
1 3
3
20
51
""",
        "structs3": """\
11
1 2
3 4 5 6 7
8 20
3 99
""",
        "structs4": """\
1 2 3 4
3 8
9 8 1
9 10
9 10
2
""",
        "aggregates": """\
7 8
1 4 5
hello hi 6
1 2 8
9 10 12
world 6 abc
16909060 4
11223344
4 4
abc 42
""",
        "aggregates2": """\
1 0 2
3 0 0 0
4 0 5
ab 0 0
""",
        "unions": """\
1020304 8 16
4 9
""",
        "control": """\
0 5 6
100 101 105 999
two
three
i=0
n=3
""",
        "control2": """\
mode=3
f=10 20
total=4321
nested
after nested
two
""",
        "goto": """\
0 1 2 3
done 3
pick=3
j=2
""",
        "enums": """\
0 5 6 11 4
5 6 0
10 15
99
7
99
1
""",
        "aggregate_abi": """\
pair=2 3 5
pair_call=15
big=10 11 12 33
big_call=63
one=Z Q
""",
        "aggregate_abi2": """\
eight=3 4 7
eight_after=3 4
twelve=10 11 12 33
twelve_after=10
three=198 6
sixteen=10000000000 20000000000
sixteen_sum=30000000000
mixed=x 42 8
union=D C B A
long=9000000000
two=1234
four=10
union_int=11223344
""",
        "aggregate_abi3": """\
pair=1 2 -> 2 2
big=5 6 7 -> 6 6 7
one=A -> B
mix=96
rvalue=30 52
""",
        "type_inference": """\
4294967297 4294967296
8 4 8 8
1 2
8 8
8 8 4 4 8 8 4
""",
        "conversions": """\
1 1 1 0 1 1 1 0
0
""",
        "const_short": """\
two
0 42 1 -1 1
""",
        "compound_shift": """\
-1 1073741824 -2 -2
""",
    }
    for name, expected in feature_expectations.items():
        out = compile_and_run(name, ROOT / "tests" / f"{name}.c")
        if out is not None:
            expect_equal(name + " output", out, expected)

    # 4. Include paths + include guards.
    out = compile_and_run(
        "include_test",
        ROOT / "tests" / "include_test.c",
        extra=["-I", ROOT / "tests" / "include"],
    )
    if out is not None:
        expect_equal("include_test output", out, "42 42\n")

    # 5. Command-line macros.
    macro_src = WORK / "macro_d.c"
    macro_src.write_text(
        "#include <stdio.h>\n"
        "int main(void) { printf(\"%d\\n\", ETCC_VALUE); return 0; }\n",
        encoding="utf-8",
    )
    out = compile_and_run(
        "macro_d", macro_src, extra=["-DETCC_VALUE=42"]
    )
    if out is not None:
        expect_equal("macro -D output", out, "42\n")

    # 6. Backend IR output.
    ir_path = WORK / "hello.ll"
    r = run([ETCC, "-S", ROOT / "tests" / "hello.c", "-o", ir_path])
    if r.returncode != 0:
        check("emit IR", False, r.stderr + r.stdout)
    else:
        text = ir_path.read_text(encoding="utf-8") if ir_path.exists() else ""
        check("emit IR file", bool(text), r.stderr)
        check(
            "emit IR triple",
            'target triple = "x86_64-pc-windows-msvc"' in text,
            text[:500],
        )
        check("emit IR main", "define i32 @main()" in text, text[:800])
        check("emit IR string", '@.str.0' in text and 'c"Hello, World!\\00"' in text,
              text[:800])

    # 7. Compile-only COFF object.
    obj_path = WORK / "hello.obj"
    r = run([ETCC, "-c", ROOT / "tests" / "hello.c", "-o", obj_path])
    if r.returncode != 0:
        check("compile object", False, r.stderr + r.stdout)
    else:
        check("compile object exists", obj_path.exists() and obj_path.stat().st_size > 0)
        if LLVM_READOBJ:
            rr = run([LLVM_READOBJ, "--file-headers", obj_path])
            check(
                "compile object format",
                rr.returncode == 0 and "COFF-x86-64" in rr.stdout,
                rr.stdout + rr.stderr,
            )

    # 8. Diagnostics.
    bad = WORK / "bad.c"
    bad.write_text("int main(void) {\n    return missing;\n}\n", encoding="utf-8")
    r = run([ETCC, bad, "-o", WORK / "bad.exe"])
    check("bad source rejected", r.returncode != 0, r.stdout)
    check(
        "bad source diagnostic",
        "use of undeclared identifier 'missing'" in r.stderr
        and "bad.c:2" in r.stderr,
        r.stderr,
    )

    # 8b. Aggregate diagnostics.
    aggregate_errors = [
        (
            "incomplete",
            "struct S;\n"
            "int main(void) { struct S s; return 0; }\n",
            "incomplete type",
        ),
        (
            "arg_mismatch",
            "struct A { int x; };\n"
            "struct B { int y; };\n"
            "int f(struct A a) { return a.x; }\n"
            "int main(void) { struct B b; return f(b); }\n",
            "incompatible struct/union argument type",
        ),
        (
            "return_mismatch",
            "struct A { int x; };\n"
            "struct B { int y; };\n"
            "struct A f(void) { struct B b; return b; }\n"
            "int main(void) { return 0; }\n",
            "returning an incompatible struct/union",
        ),
        (
            "too_long",
            "int main(void) { char s[2] = \"abc\"; return 0; }\n",
            "too long",
        ),
        (
            "redefined",
            "struct S { int x; };\n"
            "struct S { int y; };\n"
            "int main(void) { return 0; }\n",
            "redefinition of struct",
        ),
        (
            "missing_member",
            "struct S { int x; };\n"
            "int main(void) { struct S s; return s.y; }\n",
            "no member named 'y'",
        ),
        (
            "pointer_mismatch",
            "int main(void) {\n"
            "  int x = 0;\n"
            "  int *p = &x;\n"
            "  char *q = p;\n"
            "  return q != 0;\n"
            "}\n",
            "incompatible pointer types in conversion",
        ),
        (
            "void_to_int",
            "void f(void) { }\n"
            "int main(void) { int x = f(); return x; }\n",
            "cannot convert a void expression to a value",
        ),
        (
            "const_overflow",
            "enum { X = (-9223372036854775807LL - 1) / -1 };\n"
            "int main(void) { return 0; }\n",
            "signed integer overflow in constant expression",
        ),
        (
            "case_overflow",
            "int main(void) {\n"
            "  switch (0) {\n"
            "  case (-9223372036854775807LL - 1) / -1:\n"
            "    break;\n"
            "  }\n"
            "  return 0;\n"
            "}\n",
            "signed integer overflow in constant expression",
        ),
    ]
    for name, text, needle in aggregate_errors:
        src = WORK / f"{name}.c"
        src.write_text(text, encoding="utf-8")
        rr = run([ETCC, src, "-o", WORK / f"{name}.exe"])
        check(
            f"aggregate error {name}",
            rr.returncode != 0 and needle in rr.stderr,
            rr.stderr + rr.stdout,
        )

    # 8c. Control-flow and enum diagnostics.
    control_errors = [
        (
            "case_outside",
            "int main(void) { case 1: return 0; }\n",
            "'case' outside of a switch",
        ),
        (
            "duplicate_case",
            "int main(void) { switch (1) { case 1: break; case 1: break; } return 0; }\n",
            "duplicate case value",
        ),
        (
            "duplicate_default",
            "int main(void) { switch (1) { default: break; default: break; } return 0; }\n",
            "duplicate default label",
        ),
        (
            "case_nonconstant",
            "int main(void) { int x = 1; switch (1) { case x: break; } return 0; }\n",
            "case label is not an integer constant",
        ),
        (
            "switch_pointer",
            "int main(void) { int x = 0; switch (&x) { default: break; } return 0; }\n",
            "switch expression must have integer type",
        ),
        (
            "goto_missing",
            "int main(void) { goto nowhere; return 0; }\n",
            "goto to undefined label",
        ),
        (
            "duplicate_label",
            "int main(void) { again: ; again: ; return 0; }\n",
            "duplicate label",
        ),
        (
            "enum_redef",
            "enum { E_A, E_A };\nint main(void) { return 0; }\n",
            "redefinition of 'E_A'",
        ),
        (
            "break_outside",
            "int main(void) { break; return 0; }\n",
            "'break' outside of a loop or switch",
        ),
    ]
    for name, text, needle in control_errors:
        src = WORK / f"{name}.c"
        src.write_text(text, encoding="utf-8")
        rr = run([ETCC, src, "-o", WORK / f"{name}.exe"])
        check(
            f"control error {name}",
            rr.returncode != 0 and needle in rr.stderr,
            rr.stderr + rr.stdout,
        )

    # 9. Conditional compilation and #if 0 skipping.
    cond = WORK / "cond.c"
    cond.write_text(
        "#include <stdio.h>\n"
        "#define FEATURE 1\n"
        "#if FEATURE\n"
        "int main(void) { puts(\"enabled\"); return 0; }\n"
        "#else\n"
        "int main(void) { missing = 1; return 0; }\n"
        "#endif\n"
        "#if 0\n"
        "int hidden = also_missing;\n"
        "#endif\n",
        encoding="utf-8",
    )
    out = compile_and_run("conditional", cond)
    if out is not None:
        expect_equal("conditional output", out, "enabled\n")

    # 9b. Predefined target macros.
    builtins = WORK / "builtins.c"
    builtins.write_text(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "#if defined(__ETCC__) && defined(_WIN64) && __STDC__\n"
        "  puts(\"etcc-win64\");\n"
        "#else\n"
        "  puts(\"unknown\");\n"
        "#endif\n"
        "  return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    out = compile_and_run("builtins", builtins)
    if out is not None:
        expect_equal("builtin macros output", out, "etcc-win64\n")

    # 9c. #pragma once.
    incdir = WORK / "inc"
    incdir.mkdir(exist_ok=True)
    (incdir / "once.h").write_text(
        "#pragma once\nint once_value(void);\n", encoding="utf-8"
    )
    once_src = WORK / "once.c"
    once_src.write_text(
        "#include <stdio.h>\n"
        "#include \"once.h\"\n"
        "#include \"once.h\"\n"
        "int main(void) { puts(\"once\"); return 0; }\n",
        encoding="utf-8",
    )
    out = compile_and_run("once", once_src, extra=["-I", incdir])
    if out is not None:
        expect_equal("pragma once output", out, "once\n")

    # 10. Global arrays.
    glob = WORK / "global_array.c"
    glob.write_text(
        "#include <stdio.h>\n"
        "int values[3];\n"
        "int main(void) { values[1] = 7; printf(\"%d\\n\", values[1]); return 0; }\n",
        encoding="utf-8",
    )
    out = compile_and_run("global_array", glob)
    if out is not None:
        expect_equal("global array output", out, "7\n")

    # 11. Pointer/integer casts and pointer comparisons.
    casts = WORK / "casts.c"
    casts.write_text(
        "#include <stdio.h>\n"
        "int main(void) {\n"
        "  int x = 5; int *p = &x;\n"
        "  unsigned long long v = (unsigned long long)p;\n"
        "  int *q = (int *)v;\n"
        "  printf(\"%d %d\\n\", *q, q == p);\n"
        "  return 0;\n"
        "}\n",
        encoding="utf-8",
    )
    out = compile_and_run("casts", casts)
    if out is not None:
        expect_equal("pointer casts output", out, "5 1\n")

    # 12. Preprocessed output.
    pp_path = WORK / "hello.i"
    r = run([ETCC, "-E", ROOT / "tests" / "hello.c", "-o", pp_path])
    if r.returncode != 0:
        check("preprocess", False, r.stderr + r.stdout)
    else:
        text = pp_path.read_text(encoding="utf-8")
        check("preprocess file", "int main(void)" in text, text[:500])
        check("preprocess no include", "#include" not in text, text[:500])

    print()
    print(f"{passed} passed, {failed} failed")
    if failures:
        print("failed: " + ", ".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
