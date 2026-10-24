#!/usr/bin/env python3
"""Checks a step before it's called done: the build, every test, the test programs built in
release mode, the concurrent programs many times over, and clang-format and clang-tidy on what
changed since the base. CI runs it without the last two.

    python tools/verify.py --build <build folder> [--base origin/master] [--no-format] [--no-tidy]

clang-format and clang-tidy come from $CLANG_FORMAT and $CLANG_TIDY, or else from PATH.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROGRAMS = ROOT / "tests" / "programs"

# Programs whose release build prints something else on purpose, and why.
RELEASE_DIFFERS = {
    "panic_overflow": "release builds wrap on overflow",
    "panic_shift": "release builds don't check shift amounts",
    "panic_struct": "release builds don't check the overflow it panics on",
    "panic_null_pointer": "release builds don't check a Ptr for Null",
    "panic_null_callback": "release builds don't check a Callback for Null, so it crashes",
    "panic_debug_assert": "release builds leave DebugAssert out",
    "panic_math_abs": "release builds wrap on overflow",
    "panic_math_fixed32_add": "release builds wrap on overflow",
    "panic_math_fixed32_multiply": "release builds leave DebugAssert out",
    "panic_math_fixed64_multiply": "release builds leave DebugAssert out",
    "panic_math_fixed64_to_fixed32": "release builds leave DebugAssert out",
    "panic_math_gcd": "release builds wrap on overflow",
    "panic_math_lcm": "release builds wrap on overflow",
    "panic_math_vec2i_overflow": "release builds wrap on overflow",
    "panic_math_vec3i_distance": "release builds wrap on overflow",
    "panic_duration_add": "release builds wrap on overflow",
    "panic_duration_negate": "release builds wrap on overflow",
    "panic_duration_scale": "release builds wrap on overflow",
    "panic_instant_add": "release builds wrap on overflow",
}

# A program is concurrent when it says one of these, and runs many times over.
CONCURRENT = re.compile(r"\b(Spawn|Parallel For|Channel|Async|Await|Thread\.Start|Mutex|Atomic)\b")
REPEATS = 10

failures: list[str] = []


def step(title: str) -> None:
    print(f"\n== {title}", flush=True)


def fail(message: str) -> None:
    failures.append(message)
    print(f"FAIL {message}", flush=True)


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def tool(variable: str, name: str) -> str | None:
    return os.environ.get(variable) or shutil.which(name)


def executable(path: Path) -> Path:
    return path.with_suffix(".exe") if os.name == "nt" else path


def expected_code(name: str) -> int:
    """What a program's name says it exits with, as the program tests read it."""
    ending = name.removeprefix("release_")
    if ending.startswith("panic_"):
        return 101
    if ending.startswith("uncaught_"):
        return 1
    return 0


def build_program(fb: Path, name: str, release: bool, folder: Path) -> Path | None:
    """Builds tests/programs/<name>.fb from its own folder, so its panics name it as the
    program tests do."""
    output = executable(folder / name)
    command = [str(fb), "build", f"{name}.fb", "-o", str(output)]
    if release:
        command.insert(2, "--release")
    built = run(command, cwd=PROGRAMS)
    if built.returncode != 0:
        fail(f"{name} ({'release' if release else 'debug'}) doesn't build:\n"
             + built.stdout.decode(errors="replace"))
        return None
    return output


def check_run(program: Path, name: str, expected: bytes, label: str) -> bool:
    # A program with a .in file reads it as its input, as the program tests give it.
    given = PROGRAMS / f"{name}.in"
    ran = subprocess.run([str(program)], cwd=PROGRAMS, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT,
                         input=given.read_bytes() if given.exists() else None)
    if ran.stdout == expected and ran.returncode == expected_code(name):
        return True
    fail(f"{name} ({label}) printed:\n{ran.stdout.decode(errors='replace')}"
         f"(exit code {ran.returncode}, expected {expected_code(name)})")
    return False


def programs() -> list[str]:
    return sorted(path.stem for path in PROGRAMS.glob("*.fb"))


def check_release(fb: Path, scratch: Path) -> None:
    step("test programs built in release mode")
    checked = 0
    for name in programs():
        if name.startswith("release_") or name.startswith("leak_") or name in RELEASE_DIFFERS:
            continue
        program = build_program(fb, name, True, scratch)
        if program is not None:
            check_run(program, name, (PROGRAMS / f"{name}.out").read_bytes(), "release")
        checked += 1
    print(f"{checked} programs")


def check_concurrent(fb: Path, scratch: Path) -> None:
    step(f"concurrent programs, {REPEATS} times each way")
    checked = 0
    for name in programs():
        if not CONCURRENT.search((PROGRAMS / f"{name}.fb").read_text(encoding="utf-8")):
            continue
        checked += 1
        expected = (PROGRAMS / f"{name}.out").read_bytes()
        modes = [False] if name.startswith("leak_") or name in RELEASE_DIFFERS else [False, True]
        if name.startswith("release_"):
            modes = [True]
        for release in modes:
            program = build_program(fb, f"{name}", release, scratch)
            label = "release" if release else "debug"
            for _ in range(REPEATS):
                if program is None or not check_run(program, name, expected, label):
                    break
    print(f"{checked} programs")


def changed_files(base: str) -> list[Path]:
    """What differs from the base, committed or not, and new files git doesn't ignore."""
    names = run(["git", "diff", "--name-only", base], cwd=ROOT).stdout.decode().split()
    names += run(["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT).stdout.decode().split()
    return sorted({ROOT / name for name in names if (ROOT / name).is_file()})


def changed_lines(base: str, path: Path) -> set[int] | None:
    """The lines of a file that differ from the base, or None when it's new."""
    tracked = run(["git", "ls-files", "--error-unmatch", str(path)], cwd=ROOT).returncode == 0
    if not tracked:
        return None
    diff = run(["git", "diff", "-U0", base, "--", str(path)], cwd=ROOT).stdout.decode(errors="replace")
    lines: set[int] = set()
    for match in re.finditer(r"^@@ -\S+ \+(\d+)(?:,(\d+))? @@", diff, re.M):
        start, count = int(match.group(1)), int(match.group(2) or "1")
        lines.update(range(start, start + count))
    return lines


def is_cpp(path: Path) -> bool:
    return path.suffix in {".cpp", ".hpp", ".h", ".c"} and "tests/programs" not in path.as_posix() \
        and "tests/native" not in path.as_posix()


def check_format(base: str) -> None:
    step("clang-format on what changed")
    clang_format = tool("CLANG_FORMAT", "clang-format")
    if clang_format is None:
        fail("clang-format isn't on PATH; set CLANG_FORMAT")
        return
    for path in filter(is_cpp, changed_files(base)):
        checked = run([clang_format, "--dry-run", "--Werror", str(path)], cwd=ROOT)
        if checked.returncode != 0:
            fail(f"{path.relative_to(ROOT).as_posix()} isn't formatted")


def check_tidy(base: str, build: Path) -> None:
    step("clang-tidy on the lines that changed")
    clang_tidy = tool("CLANG_TIDY", "clang-tidy")
    if clang_tidy is None:
        fail("clang-tidy isn't on PATH; set CLANG_TIDY")
        return
    changed = {os.path.normcase(str(path)): changed_lines(base, path)
               for path in filter(is_cpp, changed_files(base))}
    warning = re.compile(r"^(.*?):(\d+):\d+: warning: (.*)$", re.M)
    for path in (Path(name) for name in changed if name.endswith(".cpp")):
        output = run([clang_tidy, "-p", str(build), "--quiet", str(path)], cwd=ROOT)
        for match in warning.finditer(output.stdout.decode(errors="replace")):
            file = os.path.normcase(str(Path(match.group(1)).resolve()))
            line = int(match.group(2))
            if file in changed and (changed[file] is None or line in changed[file]):
                fail(f"{Path(match.group(1)).name}:{line}: {match.group(3)}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--base", default="origin/master")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-tests", action="store_true")
    parser.add_argument("--no-programs", action="store_true")
    parser.add_argument("--no-format", action="store_true")
    parser.add_argument("--no-tidy", action="store_true")
    options = parser.parse_args()
    build = options.build.resolve()
    fb = executable(build / "cli" / "fb")

    # A base git doesn't know would give an empty diff, which lint would pass.
    if (not options.no_format or not options.no_tidy) and \
            run(["git", "rev-parse", "--verify", "--quiet", f"{options.base}^{{commit}}"],
                cwd=ROOT).returncode != 0:
        print(f"FAIL the base, {options.base}, isn't a commit git knows")
        return 1

    if not options.no_build:
        step("build")
        if run(["cmake", "--build", str(build)]).returncode != 0:
            fail("the build fails")
            return 1

    if not options.no_tests:
        step("ctest")
        tested = subprocess.run(["ctest", "--test-dir", str(build), "-j", str(options.jobs),
                                 "--output-on-failure"])
        if tested.returncode != 0:
            fail("ctest fails")

    if not options.no_programs:
        with tempfile.TemporaryDirectory(prefix="fb-verify-") as scratch:
            check_release(fb, Path(scratch))
            check_concurrent(fb, Path(scratch))

    if not options.no_format:
        check_format(options.base)

    if not options.no_tidy:
        check_tidy(options.base, build)

    step("done")
    if failures:
        print(f"{len(failures)} failure(s)")
        return 1
    print("everything passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
