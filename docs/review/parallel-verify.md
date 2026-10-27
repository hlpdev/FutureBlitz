# The review of M6 tooling: a parallel verify.py and the programs ctest test split into 32 shards

4 confirmed findings: 0 high, 1 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## verify-1 (medium): On Windows, the 600 s limit doesn't stop a hung http_ program: verify.py waits for it to end, forever if it never does

Status: fixed in 5b077ed (verify.py kills a run past its limit with what it started, which an http_ program's server holds its output open by), with tests/tools/verify_tests.py, which ctest runs: a script that starts another, as the test server starts an http_ program, is killed with it after a limit of 3 seconds, and programs_t.check fails an http_ program that does so after 3 seconds, where it waited 60; run_limited() kills the tree with taskkill /T on Windows and the process group elsewhere

Where: tools/verify.py:155

programs_t.check runs an http_ program as fb_http_test_server <program>. The server starts the program with CreateProcess and bInheritHandles TRUE, handing it the server's own stdout and stderr, which is verify's pipe. When subprocess.run times out, Python kills only the server. On Windows it then calls process.communicate() with no timeout to collect the output (Lib/subprocess.py, the _mswindows branch). That call blocks until every holder of the pipe's write end has closed it, and the program the server started is still running and still holds it. So the timeout path returns only once the grandchild exits on its own. For a real hang, the http_progress case in the lane's notes that commit c283704b says the limit now turns into a failure, verify.py hangs as before. The program is also left running, orphaned (on POSIX too). Shown by importing verify.py from the checkout with PROGRAMS set to my folder, RUN_LIMIT = 3, and the http test server from build-lane3-verify, then calling programs_t.build and programs_t.check for each program. Harness: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-parallel/verify-review/harness.py. The same program as plain_hang (no server) fails after 3.0 s. As http_hang it fails only after 25.0 s, when its Sleep ends, and the message says it was still running after 3 seconds while showing it printed both lines. verify.py has no test of the timeout path.

```
Import std.core
Import std.time

Print("started")
Sleep(25.Seconds)
Print("done")

' saved as both plain_hang.fb and http_hang.fb, each with a .out of "started\ndone\n", and checked through verify.programs_t.check with verify.RUN_LIMIT = 3
```

Expected: Both runs fail after about 3 s with "... was still running after 3 seconds", and nothing they started is left running

Actual: plain_hang failed after 3.0 s; http_hang failed after 25.0 s ['http_hang (debug) was still running after 3 seconds, having printed:\nstarted\ndone\n']. A program that never ends would hang verify.py for good.

Suggested fix: Kill the whole process tree on a timeout, not just the server. Use Popen with communicate(timeout=RUN_LIMIT). On TimeoutExpired, run `taskkill /F /T /PID <pid>` on Windows (or start the server in a Job Object), or use start_new_session=True with os.killpg on POSIX, then call communicate(). Another way is for the test server to put its child in a kill-on-close job, or kill it when it is killed itself. Add a test that runs a sleeping http_ program under a small RUN_LIMIT.

Skeptic: I copied the reviewer's programs and harness into rv-parallel/refute-verify, with plain_hang.fb and http_hang.fb (print, Sleep(25.Seconds), print) and RUN_LIMIT = 3. I ran programs_t.build/check from the c283704b checkout with build-rev2's fb and build-lane3-verify's fb_http_test_server. Output: "plain_hang failed after 3.0 s", then "http_hang failed after 25.0 s ['http_hang (debug) was still running after 3 seconds, having printed:\nstarted\ndone\n']". So the timeout never fires for an http_ program until the grandchild exits on its own. The server starts the program with CreateProcessA(..., TRUE, ...) at tests/native/http_server/http_server.cpp:1127, so the program inherits the pipe, and subprocess.run's post-kill communicate() waits for it. That means commit c283704b doesn't do what the lane's notes say: an http_progress-style hang still stalls verify.py with no end. I rate it medium, not high. The gate can't pass anything wrongly; a rare hang (about 1 in 340 runs) just stalls, as it did before. Still, the commit's stated fix doesn't work, and nothing tests the timeout path.

## shards-1 (low): The ctest shards have no time limit, so a hung program still stalls ctest and verify.py's ctest step

Status: fixed in 7a370ea (each shard of the programs test fails after 20 minutes, so a hung program doesn't stop ctest), with a verify_tests case that asks ctest for the 32 shards and finds a TIMEOUT on each; it's 1200 seconds, about ten times a shard's run 32 at once

Where: tests/CMakeLists.txt:47

c283704b puts a 600 s limit (RUN_LIMIT) on each run that verify.py makes itself, so that one hanging program doesn't stop the rest. The lane's notes say this "now turns such a hang into a failure". But the same debug programs also run in ctest, through the 32 shards registered in tests/CMakeLists.txt:45-55, and those shards get no TIMEOUT property. fb::run_process in build_and_run has no limit either. The project only calls enable_testing(), with no include(CTest) and no DartConfiguration.tcl, so ctest's default timeout applies, which is 10,000,000 s. In a full verify.py the ctest step runs before the programs steps, and CI's Test step runs the shards too. So a program that hangs, like the rare http_progress hang the lane saw, still stops the gate indefinitely: verify.py sits in its ctest step, and CI waits for its 120-minute job limit. To check this I built a CMake project shaped like tests/CMakeLists.txt (enable_testing plus an add_test with only ENVIRONMENT set). `ctest -V` printed "Test timeout computed to be: 10000000". The program below never ends under fb run; I stopped it with an external 20 s timeout, which gave exit 124. I couldn't run a real shard: build-rev2's fb_compiler_tests.exe dates from Oct 23 and predates the change. Its --list-tests still shows the test as [programs] only, not hidden.

```
Import std.core
Print("started")
While True
End While
```

Expected: Placed in tests/programs as hang_forever.fb with "started\n" as its .out, its shard ("programs print what they should i/32") should fail after a bounded time. That matches verify.py's "was still running after 600 seconds" failure for the same program, so ctest, verify.py's ctest step, and CI's Test step would all end.

Actual: The program prints "started" and never ends (fb run, stopped by an external 20 s timeout, exit 124). ctest computes a timeout of 10000000 s for a test registered like the shards, so the shard, and with it ctest and verify.py's ctest step, wait indefinitely.

Suggested fix: Give each shard a TIMEOUT in tests/CMakeLists.txt, for example set_tests_properties("${test}" PROPERTIES TIMEOUT 1200), comfortably above a shard's normal run time (about 121 s at 32 at once; allow for CI's 4-core runners). Or have verify.py's ctest command pass --timeout. Either way, a hang becomes a failure everywhere the programs run, as c283704b intends.

Skeptic: I read tests/CMakeLists.txt at c283704b (lines 41-55). Each of the 32 shards gets add_test plus ENVIRONMENT, and on MinGW ENVIRONMENT_MODIFICATION as well. None of them gets a TIMEOUT. The project has no include(CTest) and no DartConfiguration.tcl: build-rev2 has neither tests/DartConfiguration.tcl nor DartConfiguration.tcl. build_and_run calls fb::run_process with no limit (program_tests.cpp lines 129-155). verify.py's ctest step (lines 332-339) passes no --timeout. CI runs `ctest --test-dir build -j 4 --output-on-failure` with only a 120-minute job limit. To check ctest's default, I built a small CMake project with enable_testing() and an add_test that sets only ENVIRONMENT, then ran `ctest -C Debug -V`. It printed "Test timeout computed to be: 10000000". I wrote the finding's hang_forever.fb and ran it with fb run under an external 15 s timeout. It printed "started" and never ended (exit 124). So a hanging program in a shard holds up ctest, a full verify.py's ctest step, and CI's Test step with no bound. That contradicts the change's own claim that "a run past 600 s fails": RUN_LIMIT only covers runs that verify.py makes itself. This is not a weakening of the old gate, because before the change the one programs test had no timeout either. That is why it is low. Still, the 600 s rule that c283704b adds does not hold where most debug runs happen in a full verify and in CI. The fix is a TIMEOUT property on each shard, or --timeout in verify.py's ctest command.

## verify-2 (low): --quick checks a changed release_ program in no step at all

Status: fixed in e0fd866 (verify.py --quick checks a changed release_ program, in release mode), with a verify_tests case: with only release_wrap and plain changed, the --quick step builds release_wrap in release mode and plain in debug, as the program tests build them

Where: tools/verify.py:194

With --quick, ctest skips "programs print what they should" (-E), so only verify.py's own steps check the changed programs. check_debug drops names that start with release_, and check_release drops them too. check_concurrent takes a release_ program only when its source names a concurrency keyword. So a changed release_ program that isn't concurrent, for example release_wrap or release_globals_released, is never built or run under --quick, even with a wrong .out. The docstring and docs/m6-plan.md both say --quick checks the changed programs in debug and release. The serial version had the same gap, but this step was meant to keep the gate exactly as strict, and it still leaves one. Shown by importing verify.py with PROGRAMS set to my folder and verify.only = {name}, as --quick sets it, then calling check_debug, check_release, and check_concurrent. Harness: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-parallel/verify-review/harness2.py.

```
Import std.core

Print("what it prints")

' saved as release_wrong.fb, and as wrong.fb for comparison, each with a .out of "something else\n"
```

Expected: release_wrong fails once, as built in release mode, just as wrong fails in debug and release

Actual: For release_wrong, every step prints "0 programs" and the run ends with "0 failure(s)". For wrong, the debug and release steps each print FAIL, 2 failure(s) in all.

Suggested fix: In --quick, check a release_ program in release mode with its expected code: have check_debug build release_ names with release=True, or have check_release take release_ names when only is set, which is how the program tests treat them.

Skeptic: I ran harness2 with verify.only = {release_wrong}, where the .out says "something else" but the program prints "what it prints". The debug, release, and concurrent steps each printed "0 programs", ending in "0 failure(s)". With only = {wrong}, the debug and release steps both FAIL, 2 failure(s). The cause is in the code: check_debug (line 194) and check_release (line 207) both drop names that start with release_, and check_concurrent takes them only when CONCURRENT matches. That contradicts docs/m6-plan.md step 3 and the --quick docstring ("the changed programs in debug and release"), because --quick also leaves the programs ctest test out with -E. The gap was already there in the serial version, and the landing's full ctest still catches it. So it is only a hole in the lane's own --quick check: low.

## verify-3 (low): --quick doesn't count a change to a program's own folder (its modules, embeds, data) as a change to the program

Status: fixed in f5baef8 (verify.py --quick checks a program whose own folder changed), with a verify_tests case: a change to embed/hello.txt is a change to embed, a .out to its program, and a file beside them, or in the folder of no program, to none; changed_programs() does it

Where: tools/verify.py:309

The --quick filter keeps a changed path only when path.parent == PROGRAMS. A program's modules, embedded files, and data live in tests/programs/<name>/, for example embed/hello.txt for embed.fb or the archives in compress_archives/. A change to one of those leaves the program out of --quick, and since --quick also leaves the program tests out of ctest, nothing checks it. The serial version had the same filter. Shown by calling main() with --quick --no-build --no-tests --no-programs --no-format --no-tidy, with changed_files replaced to return only tests/programs/embed/hello.txt, then printing verify.only. Harness: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-parallel/verify-review/harness3.py.

```
' The repo's own tests/programs/embed.fb, which embeds embed/hello.txt:
Const Greeting = EmbedText("embed/hello.txt")
' with git reporting only tests/programs/embed/hello.txt as changed
```

Expected: checked: {'embed'}

Actual: checked: set()

Suggested fix: Also map a changed path under PROGRAMS/<name>/ to <name>, when PROGRAMS/<name>.fb exists. Take the first part of path.relative_to(PROGRAMS).

Skeptic: I ran harness3 against the checkout, with changed_files returning only tests/programs/embed/hello.txt. tests/programs/embed.fb line 6 has EmbedText("embed/hello.txt"). It printed "checked: set()". The filter at verify.py:309-311 keeps only paths whose parent is PROGRAMS and whose suffix is .fb, .out, or .in. So under --quick, a change to a program's own modules or data checks that program nowhere, since -E drops the programs ctest test too. That is the same as the serial version, and the landing's full run still catches it. Low.
