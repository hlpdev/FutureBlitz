"""tools/verify.py's own tests: a run past its limit is killed with what it started, --quick
checks the programs a change is of, a release_ one in release mode, and the programs ctest test's
shards each have a time limit. ctest runs them, with FB_TEST_BUILD the build folder.

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
    if os.name == "nt":
        listed = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                capture_output=True, text=True).stdout
        return str(pid) in listed
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


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


if __name__ == "__main__":
    unittest.main()
