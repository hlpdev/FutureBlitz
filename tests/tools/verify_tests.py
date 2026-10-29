"""tools/verify.py's own tests: a run past its limit is killed with what it started, --quick
checks the programs a change is of, a release_ one in release mode, a panic's stack trace is
compared only where the program's name says, and the programs ctest test's shards each have a
time limit. ctest runs them, with FB_TEST_BUILD the build folder.

    python -m unittest verify_tests
"""

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
import verify  # noqa: E402

# A program that starts another, which keeps what it prints open, as the HTTP test server starts
# an http_ program, then prints and sleeps, and writes the other's id to the file it's given.
STARTS_ANOTHER = textwrap.dedent("""\
    import subprocess, sys, time
    other = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
    open(sys.argv[1], "w").write(str(other.pid))
    print("started", flush=True)
    time.sleep(60)
""")


def running(pid: int) -> bool:
    """Whether the process is still running. A killed one that's ended but not been reaped yet, a
    zombie, isn't: its parent, the process that took in orphans, as a CI runner's can, may wait
    for it late, or never, and kill(pid, 0) still finds it."""
    if os.name == "nt":
        listed = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                capture_output=True, text=True).stdout
        return str(pid) in listed
    state = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True,
                           text=True).stdout.strip()
    return state != "" and not state.startswith("Z")


class limit_tests_t(unittest.TestCase):
    def test_a_run_past_its_limit_is_killed_with_what_it_started(self) -> None:
        with tempfile.TemporaryDirectory() as folder:
            script = Path(folder) / "starts_another.py"
            script.write_text(STARTS_ANOTHER)
            other = Path(folder) / "other.txt"
            start = time.monotonic()
            code, output = verify.run_limited([sys.executable, str(script), str(other)],
                                              Path(folder), None, 3)
            took = time.monotonic() - start
            self.assertIsNone(code)
            self.assertIn(b"started", output)
            self.assertLess(took, 20)
            self.assertFalse(running(int(other.read_text())))

    def test_a_hung_http_program_fails_after_the_limit(self) -> None:
        # The "server" is Python, and the "program" the script, which starts another as the
        # server starts an http_ program.
        with tempfile.TemporaryDirectory() as folder:
            programs = Path(folder) / "programs"
            programs.mkdir()
            (programs / "http_hang.out").write_bytes(b"started\n")
            script = Path(folder) / "starts_another.py"
            script.write_text(STARTS_ANOTHER.replace("sys.argv[1]", repr(str(Path(folder) /
                                                                              "other.txt"))))
            saved = (verify.PROGRAMS, verify.http_server, verify.RUN_LIMIT)
            verify.PROGRAMS, verify.http_server, verify.RUN_LIMIT = programs, Path(sys.executable), 3
            try:
                built = verify.programs_t(Path(sys.executable), Path(folder))
                problems: list[str] = []
                start = time.monotonic()
                self.assertFalse(built.check(script, "http_hang", "debug", problems))
                self.assertLess(time.monotonic() - start, 20)
            finally:
                verify.PROGRAMS, verify.http_server, verify.RUN_LIMIT = saved
            self.assertEqual(len(problems), 1)
            self.assertIn("was still running after 3 seconds", problems[0])


class quick_tests_t(unittest.TestCase):
    def setUp(self) -> None:
        self.folder = tempfile.TemporaryDirectory()
        self.saved = verify.PROGRAMS, verify.only
        verify.PROGRAMS = Path(self.folder.name)
        for name in ["embed", "plain", "release_wrap"]:
            (verify.PROGRAMS / f"{name}.fb").write_text("Import std.core\n")
        (verify.PROGRAMS / "embed").mkdir()
        (verify.PROGRAMS / "embed" / "hello.txt").write_text("hello\n")

    def tearDown(self) -> None:
        verify.PROGRAMS, verify.only = self.saved
        self.folder.cleanup()

    def test_a_change_in_a_programs_own_folder_is_a_change_to_it(self) -> None:
        programs = verify.PROGRAMS
        changed = [programs / "embed" / "hello.txt", programs / "plain.out",
                   programs / "notes.txt", programs / "gone" / "data.bin",
                   programs.parent / "elsewhere.fb"]
        self.assertEqual(verify.changed_programs(changed), {"embed", "plain"})

    def test_a_changed_release_program_is_checked_in_release_mode(self) -> None:
        # What the steps build, without building it.
        class built_t:
            def __init__(self) -> None:
                self.builds: list[tuple[str, bool]] = []

            def build(self, name: str, release: bool, problems: list[str]) -> None:
                self.builds.append((name, release))
                return None

        built = built_t()
        verify.only = {"release_wrap", "plain"}
        verify.check_debug(built)  # type: ignore[arg-type]
        self.assertEqual(sorted(built.builds), [("plain", False), ("release_wrap", True)])


class trace_tests_t(unittest.TestCase):
    def test_a_panics_trace_is_left_out_but_where_the_name_keeps_it(self) -> None:
        printed = (b"    at the start\n\x1b[?25hpanic: boom at a.fb:2:1\n    at F (a.fb:2)\n"
                   b"    at top-level code (a.fb:5)\n")
        self.assertEqual(verify.without_trace(printed),
                         b"    at the start\n\x1b[?25hpanic: boom at a.fb:2:1\n")
        uncaught = b"uncaught Error: x at a.fb:1:1\n"
        self.assertEqual(verify.without_trace(uncaught + b"    at G (a.fb:1)\n"), uncaught)
        self.assertTrue(verify.keeps_trace("release_panic_trace_inlined"))
        self.assertTrue(verify.keeps_trace("uncaught_trace_thrown"))
        self.assertFalse(verify.keeps_trace("panic_overflow"))


class shard_tests_t(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("FB_TEST_BUILD"), "needs FB_TEST_BUILD")
    def test_each_shard_of_the_programs_test_has_a_time_limit(self) -> None:
        shown = subprocess.run(["ctest", "--test-dir", os.environ["FB_TEST_BUILD"],
                                "--show-only=json-v1"], capture_output=True, check=True)
        shards = [test for test in json.loads(shown.stdout)["tests"]
                  if test["name"].startswith("programs print what they should ")]
        self.assertEqual(len(shards), 32)
        for shard in shards:
            limits = [p["value"] for p in shard.get("properties", []) if p["name"] == "TIMEOUT"]
            self.assertEqual(len(limits), 1, shard["name"])
            self.assertGreater(float(limits[0]), 0)


if __name__ == "__main__":
    unittest.main()
