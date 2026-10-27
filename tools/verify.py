#!/usr/bin/env python3
"""Checks a step before it's called done: the build, every test, the test programs built in
release mode, the concurrent programs many times over, and clang-format and clang-tidy on what
changed since the base. CI runs it without the last two.

    python tools/verify.py --build <build folder> [--base origin/master] [--no-format] [--no-tidy]
                           [--quick] [--jobs N]

--quick is a lane's own check: the unit tests, and only the test programs that changed since the
base, in debug, release, and many times over; the landing CI runs the whole of it. The programs
build and run --jobs at a time, each run in an empty folder of its own, and each step's failures
come out by program once it's done.

clang-format and clang-tidy come from $CLANG_FORMAT and $CLANG_TIDY, or else from PATH.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor
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

# The HTTP test server an http_ program runs under, in the build folder.
http_server: Path | None = None

# How many programs build and run at once.
jobs = 1

# The longest a test program runs before it counts as hung, in seconds.
RUN_LIMIT = 600

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


class programs_t:
    """The test programs' builds, in a scratch folder, each made once and kept for the steps
    after, and the empty folder each run is in, so a program that writes a file where it runs
    doesn't meet another run's, or write into tests/programs."""

    def __init__(self, fb: Path, scratch: Path) -> None:
        self.fb = fb
        self.scratch = scratch
        self.lock = threading.Lock()
        self.builds: dict[tuple[str, bool], Path | None] = {}
        self.runs = 0

    def build(self, name: str, release: bool, problems: list[str]) -> Path | None:
        """Builds tests/programs/<name>.fb from its own folder, so its panics name it as the
        program tests do, or gives the build made already. A build that fails adds a problem."""
        with self.lock:
            if (name, release) in self.builds:
                return self.builds[(name, release)]
        output = executable(self.scratch / ("release" if release else "debug") / name)
        output.parent.mkdir(exist_ok=True)
        command = [str(self.fb), "build", f"{name}.fb", "-o", str(output)]
        if release:
            command.insert(2, "--release")
        built = run(command, cwd=PROGRAMS)
        program = output if built.returncode == 0 else None
        if program is None:
            problems.append(f"{name} ({'release' if release else 'debug'}) doesn't build:\n"
                            + built.stdout.decode(errors="replace"))
        with self.lock:
            self.builds[(name, release)] = program
        return program

    def folder(self) -> Path:
        """A new empty folder for a run."""
        with self.lock:
            self.runs += 1
            folder = self.scratch / "runs" / str(self.runs)
        folder.mkdir(parents=True)
        return folder

    def check(self, program: Path, name: str, label: str, problems: list[str]) -> bool:
        """Runs a program, and gives whether it printed its .out and exited as its name says,
        adding a problem when it didn't."""
        # A program with a .in file reads it as its input, as the program tests give it. An
        # http_ one runs under the HTTP test server, which gives it the server's URLs first.
        given = PROGRAMS / f"{name}.in"
        command = [str(program)]
        stdin = given.read_bytes() if given.exists() else None
        if name.removeprefix("release_").startswith("http_"):
            command.insert(0, str(http_server))
            stdin = stdin or b""
        folder = self.folder()
        try:
            ran = subprocess.run(command, cwd=folder, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, input=stdin, timeout=RUN_LIMIT)
        except subprocess.TimeoutExpired as hung:
            shutil.rmtree(folder, ignore_errors=True)
            printed = (hung.stdout or b"").decode(errors="replace")
            problems.append(f"{name} ({label}) was still running after {RUN_LIMIT} seconds, "
                            f"having printed:\n{printed}")
            return False
        shutil.rmtree(folder, ignore_errors=True)
        expected = (PROGRAMS / f"{name}.out").read_bytes()
        if ran.stdout == expected and ran.returncode == expected_code(name):
            return True
        problems.append(f"{name} ({label}) printed:\n{ran.stdout.decode(errors='replace')}"
                        f"(exit code {ran.returncode}, expected {expected_code(name)})")
        return False


def each_program(names: list[str], work: Callable[[str, list[str]], None]) -> None:
    """Does `work` for each program, jobs at a time, and then fails with what each one found,
    in the programs' order."""
    problems: dict[str, list[str]] = {name: [] for name in names}
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        for done in [pool.submit(work, name, problems[name]) for name in names]:
            done.result()
    for name in names:
        for problem in problems[name]:
            fail(problem)


# The programs checked: every one, or with --quick those that changed since the base.
only: set[str] | None = None


def programs() -> list[str]:
    return sorted(path.stem for path in PROGRAMS.glob("*.fb") if only is None or path.stem in only)


def check_debug(built: programs_t) -> None:
    step("changed test programs built in debug mode")
    names = [name for name in programs() if not name.startswith("release_")]

    def work(name: str, problems: list[str]) -> None:
        program = built.build(name, False, problems)
        if program is not None:
            built.check(program, name, "debug", problems)

    each_program(names, work)
    print(f"{len(names)} programs")


def check_release(built: programs_t) -> None:
    step("test programs built in release mode")
    names = [name for name in programs() if not name.startswith("release_")
             and not name.startswith("leak_") and name not in RELEASE_DIFFERS]

    def work(name: str, problems: list[str]) -> None:
        program = built.build(name, True, problems)
        if program is not None:
            built.check(program, name, "release", problems)

    each_program(names, work)
    print(f"{len(names)} programs")


def check_concurrent(built: programs_t) -> None:
    step(f"concurrent programs, {REPEATS} times each way")
    names = [name for name in programs()
             if CONCURRENT.search((PROGRAMS / f"{name}.fb").read_text(encoding="utf-8"))]

    # A program's runs are one after another, and the programs side by side.
    def work(name: str, problems: list[str]) -> None:
        modes = [False] if name.startswith("leak_") or name in RELEASE_DIFFERS else [False, True]
        if name.startswith("release_"):
            modes = [True]
        for release in modes:
            program = built.build(name, release, problems)
            label = "release" if release else "debug"
            for _ in range(REPEATS):
                if program is None or not built.check(program, name, label, problems):
                    break

    each_program(names, work)
    print(f"{len(names)} programs")


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
    parser.add_argument("--quick", action="store_true")
    options = parser.parse_args()
    if options.quick:
        global only
        only = {path.stem for path in changed_files(options.base)
                if path.parent == PROGRAMS and path.suffix in (".fb", ".out", ".in")
                and (PROGRAMS / f"{path.stem}.fb").exists()}
    build = options.build.resolve()
    fb = executable(build / "cli" / "fb")
    global http_server, jobs
    http_server = executable(build / "tests" / "native" / "fb_http_test_server")
    jobs = max(options.jobs, 1)

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
        command = ["ctest", "--test-dir", str(build), "-j", str(options.jobs),
                   "--output-on-failure"]
        if options.quick:
            command += ["-E", "programs print what they should"]
        tested = subprocess.run(command)
        if tested.returncode != 0:
            fail("ctest fails")

    if not options.no_programs:
        with tempfile.TemporaryDirectory(prefix="fb-verify-") as scratch:
            built = programs_t(fb, Path(scratch))
            if options.quick:
                check_debug(built)
            check_release(built)
            check_concurrent(built)

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
