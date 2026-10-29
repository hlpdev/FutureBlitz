# The review of M8 step 7 (testing)

19 confirmed findings, each with its Status.

## run-1 (high): Work a test starts on the pool belongs to the test only when the pool happens to run it on the test's thread, so leak results flip from run to run and Expect sometimes panics

runtime/src/test.cpp:205. run_test sets the thread-local t_test and t_owner only on the test's own thread. A Spawn or Parallel For pass started by a test is the test's only if the worker happens to run it in its own wait (run_own). If another worker steals it, the pass has no test and no owner. So (a) a leak made in a Spawn the test waits for fails the test on some runs and passes on others. When it passes, the child's own leak report still prints to stderr, yet fb test says ok and exits 0. (b) A failed Expect in a Spawn or Parallel For body counts as an ordinary failure on some passes and panics on others, sometimes within a single run. stdlib.md:2014 says work a test starts on another thread panics, but a Spawn that runs on the test's thread reports normally instead.

Shown by: Programs in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/. spawnleak.fb: class Node with Field next: Node?; MakeCycle() makes n with n.next = n; @Test LeaksOnAnotherThread does Local t = Spawn(Function() => MakeCycle()) then Expect(t.Wait() == 1). I ran `fb test spawnleak.fb --color never` six times. pfexpect.fb: @Test with `Parallel For i = 0 Until 64` around `Expect(i < 0, "pass {i}")`. spawnexpect.fb: Expect(1 == 2) inside a Spawn'd function.

Expected: Every run gives the same result. Work a test starts and waits for is attributed to that test: its leaks fail it, and its Expects report to it. If that isn't the design, it should at least behave the same every run, as the docs say.

Actual: spawnleak, six runs: FAILED, ok, FAILED, FAILED, ok, FAILED. On the 'ok' runs, 'leak: 1 Node object was never released' and the cycle line still print, and the exit code is 0. pfexpect: one run listed 34 ordinary 'expect failed' messages, then a 'panic: expect failed: i < 0 ... pass 12 (only a test's own thread can report to fb test)'. spawnexpect: reported as an ordinary failure, not the panic stdlib.md describes.

Skeptic: Copied the reviewer's programs to scratchpad/review-m8s7/verify. `fb test spawnleak.fb --color never` six times gave ok, FAILED, FAILED, FAILED, ok, FAILED. Every run printed the raw 'leak: 1 Node object was never released' and the cycle line, including the runs that said ok. pfexpect.fb, three runs: 45, 47, and 28 'expect failed' lines, each mixed with 'panic: expect failed: i < 0 at pfexpect.fb:7:9' lines. spawnexpect.fb, three runs: always an ordinary failure ('expect failed: 1 == 2 ... in a spawned function'), never the panic stdlib.md's std.test section promises for 'work a test starts on another thread'. In runtime/src/test.cpp, run_test (line 205) sets the thread-local t_test and set_object_owner only on the test's own thread, and fb_job_start/run_job never carry them into jobs. So whether Spawn or Parallel For work belongs to the test depends on which worker picks it up. Same test, different results from run to run.

Fix: Carry the starting thread's test (t_test and t_owner) into the jobs it starts. For example, fb_job_start and fb_task_start capture them and run_job and run_task restore them around the work, so Spawn and Parallel For work is the test's wherever it runs. Or fail the run whenever an unowned leak is reported in a test build. Update stdlib.md and spec 09 to match.

Status: fixed: a job now carries the object owner of the thread that starts it (fb_job_t::object_owner, set around its work by thread.cpp's work_of), and fb test's runner takes a thread's test from that owner, so Spawn, Thread, and Parallel For work is the test's wherever it runs: its leaks, Expects, and panics. Tests: thread_tests' "a job's work has the object owner of the thread that started it, wherever it runs", and cli_tests' "fb test gives a test the work it starts, and fails a run that fails outside its tests", which runs a Spawn leak, a Parallel For of Expects, and a Spawn that panics three times over; spec 09 and stdlib.md say so.

## run-2 (high): A panic once no test is running (a Global's Destroy, or work a test left running) is ignored: tests pass, the leak check never runs, and fb test exits 0

cli/src/commands.cpp:848. When a run ends without the 'done' record and no test is going, `keeps` is true (`!panicked && going.empty()`), so every test that ended is kept as passed. Nothing records the run's own panic: the test-less 'panic' text is dropped unless exactly one test is going, and the end of the loop only fails tests when none ended. The run ended before check_leaks, so the kept tests' leaks were never checked even in a debug build, and a leaking test passes.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/destroypanic.fb: class Holder whose Destroy does Panic("Destroy broke"); Global holder = Holder(); @Test Leaks makes a Node cycle; @Test Fine does Expect(1 == 1). Run with `fb test destroypanic.fb --color never`. Also late/late.fb: @Test that does Spawn(Function() => Later()) without waiting, where Later sleeps 300ms and then panics.

Expected: The run fails: the panic is reported, as a failure of the run or of a test, and the exit code is nonzero. In a debug build, Leaks fails for its Node cycle, as it does without the Holder.

Actual: destroypanic: 'panic: Destroy broke at destroypanic.fb:7:5', then 'test destroypanic.Leaks ... ok', 'test destroypanic.Fine ... ok', 'test result: ok. 2 passed', exit 0. late.fb: 'panic: the work this test started broke', then 'test late.StartsWorkItDoesntWaitFor ... ok', 'test result: ok', exit 0.

Skeptic: destroypanic.fb printed 'panic: Destroy broke at destroypanic.fb:7:9', then 'test destroypanic.Leaks ... ok', 'Fine ... ok', 'test result: ok. 2 passed', exit=0. As a control I removed only the Holder (noholder.fb): 'test noholder.Leaks ... FAILED' with its leak, exit=1. So the panic hides a leak that spec 09 says a debug build finds. late/late.fb printed 'panic: the work this test started broke', then 'StartsWorkItDoesntWaitFor ... ok', 'test result: ok', exit=0. In cli/src/commands.cpp run_tests, a test-less panic only goes into `panic`. `keeps` is true when `!panicked && going.empty()`. The ended.empty() bail-out doesn't fire because tests ended. Nothing marks the run as failed, so fb test passes a run that panicked.

Fix: When a run ends without 'done', count a recorded test-less panic, or a nonzero exit, as a failure even when no test is going. For example, report it as a failure of the whole run, so the result says FAILED and fb test exits 1. In debug builds, don't keep ended tests whose leaks weren't checked unless one more run can check them.

Status: fixed: a run that ends with no test going, after a test-less panic or any other early end, fails the run: fb test lists it under "---- the tests' program ----", with a line saying the finished tests' leaks weren't checked in a debug build, and exits 1; a leak no test made, and a String left, fail the run the same way. The late Spawn's panic is now its test's (run-1). Test: the same cli test's destroy.fb and unowned.fb.

## docs-1 (medium): fb test can't build a program package that has a second main file

compiler/src/driver.cpp:2618. Spec 11 lets any .fb file in a package build as its main file ("A .fb file with a project.toml in its folder or above builds in that package, as its main file"), so a package can have a tool script with top-level code next to its main.fb. When testing, compile_package adds every file from library_files(own) as a plain module, and only the one main file's top-level code is left out. Any other entry point then fails with "only the main module's top-level code runs", so that package can't run fb test at all, even though check, build, and run all work. Spec 09/11 say fb test builds "each of its .fb files", but nothing says a package with more than one entry point can't be tested.

Shown by: Scratch dir C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/docs/prog2: project.toml (name prog2, no type), main.fb (Import std.core, std.test; Print("game"); one @Test Function Works with Expect(1 == 1)), tools/gen.fb (Import std.core; Print("a tool...")). Commands: fb run prog2 prints game; fb run prog2/tools/gen.fb prints the tool line; fb check prog2 passes; fb test prog2 and fb test prog2/tools/gen.fb both fail.

Expected: fb test runs prog2.main.Works and leaves out the top-level code of the other entry point too (or only takes files that hold tests). Failing that, the docs should say a package's other main files stop fb test.

Actual: prog2/tools/gen.fb:2:1: error: only the main module's top-level code runs, so this module only declares things ... error: couldn't build prog2 v0.1.0 (...)'s tests because of 1 error (exit 1)

Skeptic: Reran the reviewer's prog2 (project.toml with no type, main.fb with Print("game") and a @Test Works, tools/gen.fb with top-level Print). `fb run prog2` printed game, `fb run prog2/tools/gen.fb` printed the tool line, and `fb check prog2` passed (exit 0). `fb test prog2` failed with exit 1: "prog2/tools/gen.fb:2:1: error: only the main module's top-level code runs ... couldn't build prog2 v0.1.0 (...)'s tests because of 1 error". In compile_package (driver.cpp ~2618), when testing, extra = library_files(own) holds every package file and only the main file's path is erased, so any other entry point's top-level code is an error. Spec 11 lets any .fb file in a package build as its main file ("or after another main file it was given"), and nothing in spec 09/11 or tooling.md says such a package can't be tested.

Fix: When testing, either leave out the top-level code of every package file, as for the main file, or skip files that have top-level code and no @Test Functions. Then say which in spec 11, and add a driver test with a second main file.

Status: fixed with lang-2: in fb test's build, a package file with top-level code other than the main file is another main file (module_t::other_main), whose top-level code is left out, as the main file's is, and whose tests run. Test: driver_tests' "a test build leaves out other main files' code, and builds leave out tests/" (tools/gen.fb); spec 09 says so.

## docs-2 (medium): A tests/ folder with its own project.toml is taken as <name>.tests and its manifest is ignored

compiler/src/driver.cpp:2521. Spec 11 says a folder with a project.toml of its own is another package. tests_package() only checks that tests/ is a directory. It makes it "<name>.tests", depending on the package and its dependencies, and ignores tests/project.toml along with its dependencies and main file. A repo whose tests/ is already a separate package, one that depends on the library and on other packages, works with fb run but breaks under fb test. The error even says the module's package is lib3.tests.

Shown by: Scratch dir .../review-m8s7/docs: helper/ (library, util.fb: Public Function Two(): Int => 2); lib3/project.toml (library lib3), lib3/shapes.fb (Public Function One(): Int => 1); lib3/tests/project.toml (name lib3check, dependencies lib3 = ".." and helper = "../../helper"), lib3/tests/main.fb (Import std.core, lib3.shapes, helper.util; Print("{One() + Two()}")). fb run lib3/tests prints 3. fb test lib3 fails.

Expected: fb test lib3 treats tests/ as the separate package its project.toml makes it, either by leaving it out or by building it with its own manifest, as spec 11's rule on folders with a project.toml says.

Actual: lib3/tests/main.fb:3:8: error: there's no module helper.util: a module's name starts with std, ..., this package's name, lib3.tests, or a dependency's name in project.toml (lib3) ... plus 'only the main module's top-level code runs' at main.fb:4:1; exit 1

Skeptic: Reran lib3 (a library) with lib3/tests/project.toml (lib3check, depending on lib3 and ../../helper). `fb run lib3/tests` printed 3. `fb test lib3` exited 1 with "there's no module helper.util ... this package's name, lib3.tests, or a dependency's name in project.toml (lib3)" and "only the main module's top-level code runs" at main.fb:4:1. tests_package() (driver.cpp ~2519) only checks is_directory(root / "tests") and never looks for tests/project.toml, though library_files() and spec 11 treat a folder with a project.toml of its own as another package's.

Fix: In tests_package(), return nullopt (or report it) when tests/project.toml exists. Say so in spec 09's Tests section and add a driver test.

Status: fixed: tests_package() leaves a tests/ folder with a project.toml of its own alone. Test: the same driver test, whose tests/ has a project.toml and a main.fb that isn't code; spec 09 says so.

## lang-1 (medium): Skip() in a file that doesn't import std.test silently does nothing, and the test passes

compiler/src/sema.cpp:23845. Expect, Fail, and ExpectThrows used without Import std.test give "'X' is in std.test, which this file doesn't import". Skip doesn't. std.core's Attribute Skip is in scope, so `Skip()` resolves to building that Attribute value and throwing it away. The test runs on past it and is reported ok rather than skipped. `Skip("reason")` is rejected, but with "too many arguments to 'Skip'", which points nowhere near the missing import. The swap to the built-in in the callee code (the `is_test_builtin(name->name) && sees_test_builtins(...)` block) only happens when std.test is imported. Nothing reports Skip when it isn't. Skip is the one std.test name that std.core also has, so this is the one a user who forgets the import hits silently.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/k.fb:
  Import std.core
  @Test
  Function NotReady()
      Skip()
      Print("ran past Skip")
  End Function
Run: C:/Users/Hunter/Projects/build-m8-test/cli/fb.exe test k.fb
Also j.fb (no Import std.test): `Skip("y")` inside a Function; run fb.exe check j.fb

Expected: An error, "'Skip' is in std.test, which this file doesn't import", as Expect and Fail give, for Skip() and Skip(reason) alike.

Actual: k.fb builds. The output is "ran past Skip" then "test k.NotReady ... ok", and the summary says 1 passed, 0 skipped. In j.fb, Skip("y") gives "error: too many arguments to 'Skip'".

Skeptic: I wrote verify/k.fb (Import std.core only; @Test with Skip() then Print) and ran `fb test k.fb`. It printed "ran past Skip", then "test k.NotReady ... ok" and "1 passed; 0 failed; 0 skipped", exit 0. In verify/j.fb, Skip("y") inside a Function gave "error: too many arguments to 'Skip'". The same file with Expect(1 == 1) (verify/k3.fb) gives "'Expect' is in std.test, which this file doesn't import". So Skip is the only std.test built-in that silently falls through, to std.core's Attribute Skip. The intended call, that Skip as a call is the built-in, covers files that import std.test, not this case. A test the user meant to skip runs on and is reported as passing.

Fix: Where a call's callee resolves to std's Attribute Skip (a TYPE in std) and the file doesn't import std.test, report the std.test hint, as report_undeclared does for the other three. Add a sema test for Skip() and Skip(reason) without the import.

Status: fixed: a call of Skip that finds std's Attribute Skip, in a file that doesn't import std.test, gives "'Skip' is in std.test, which this file doesn't import", for Skip() and Skip(reason). Test: sema_tests' "std.test's built-ins are only its importers'".

## lang-2 (medium): fb test fails on an executable package that has another program's main file

compiler/src/driver.cpp:2619. For fb test, compile_package loads every .fb file of the package as a module (extra = library_files(own)), not only the main file and what it imports. Spec 11 lets a .fb file inside a package build "in that package, as its main file" (`fb build tools/gen.fb`), so an executable package can hold several programs with top-level code. In a test build, each of those is a non-main module, and sema rejects its top-level code. The package's tests can't build at all, though fb build and fb run both succeed.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/app/ (project.toml name = "app"; main.fb with @Test MainGlobal; game/shapes.fb; tests/outside.fb) plus tools/gen.fb:
  Import std.core
  Print("a second program")
(gen.fb is now saved as lang/gen.fb.bak; move it back to app/tools/gen.fb to reproduce.)
In app/: fb.exe build tools/gen.fb -o ../gen.exe (succeeds); fb.exe run (succeeds); fb.exe test

Expected: fb test builds and runs the package's three tests. Another main file's top-level code is left out, as the main file's is, or that file is skipped.

Actual: tools/gen.fb:2:1: error: only the main module's top-level code runs, so this module only declares things, then "couldn't build app v0.1.0 (...)'s tests because of 1 error". With gen.fb moved away, all 3 tests pass.

Skeptic: I copied lang/app to verify/app and put gen.fb.bak back as tools/gen.fb (top-level Print). `fb build tools/gen.fb -o ../gen.exe` succeeded, and `fb run` printed "main 4". `fb test` failed: "tools/gen.fb:2:1: error: only the main module's top-level code runs, so this module only declares things" and "couldn't build app v0.1.0 (...)'s tests because of 1 error". driver.cpp compile_package, when testing, sets extra = library_files(own), so every .fb file of the package becomes a module, other main files too. Spec 11 lets any .fb file in a package build as its main file. The result is a valid package whose tests can't be built.

Fix: In a test build of an executable package, either leave out the top-level code of files other than the main file (as the main file's is left out), or don't load a file with top-level code that the main file doesn't import. Add a driver test with a second main file.

Status: fixed with docs-1 (the same bug), by its fix and test.

## run-3 (medium): A panic in work a test leaves running is blamed on the tests running when it fires, and the test that started the work passes

cli/src/commands.cpp:833. Spawned work has no test (t_test isn't carried into jobs), so its panic is recorded with no test. fb test then blames whichever single test is going in the serial rerun. If the work outlives its test, each serial run blames the next innocent test. The test that started the work ends cleanly and passes. The root cause is shared with run-1.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/blame/blame.fb: @Test StartsWork does Spawn(Function() => Later()), where Later sleeps 300ms and panics. @Test Innocent and @Test AlsoInnocent each Sleep(1.Seconds). Run with `fb test blame.fb --color never`.

Expected: StartsWork fails with the panic. Innocent and AlsoInnocent pass.

Actual: 'test blame.Innocent ... FAILED', 'test blame.AlsoInnocent ... FAILED', 'test blame.StartsWork ... ok'. Both innocent tests list 'panic: the work StartsWork started broke at blame.fb:7:5', and the result is '1 passed; 2 failed'.

Skeptic: `fb test blame/blame.fb --color never` gave 'test blame.Innocent ... FAILED', 'test blame.AlsoInnocent ... FAILED', 'test blame.StartsWork ... ok'. Both innocent tests listed 'panic: the work StartsWork started broke at blame/blame.fb:7:5'. Result '1 passed; 2 failed', exit=1. The cause: a test-less panic is blamed on the single test going (commands.cpp, the going.size()==1 branch, which uses `panic`). Spawned work carries no t_test, the same root cause as run-1.

Fix: Record the starting test in jobs a test starts, as in run-1, so a panic in its work names that test. Alternatively, wait for a test's outstanding Spawns before recording its 'end'.

Status: fixed with run-1: work a test starts carries its test, so its panic names that test, and the tests going when it fires pass. Test: the cli test's StartsWorkThatPanics and Innocent.

## run-4 (medium): Ctrl+C doesn't stop fb test: the interrupted run counts as a crash, and fb test runs the remaining tests again

cli/src/commands.cpp:779. run_attached ignores Ctrl+C in fb while the child runs (Windows: SetConsoleCtrlHandler(ignore_interrupt, TRUE), process.cpp:235; POSIX: SIGINT and SIGQUIT set to SIG_IGN). This is right for fb run, which just returns the child's code. run_tests, however, treats the child's interrupt exit (0xC000013A on Windows, 130 on POSIX) like any other early end. It blames the test that was going, or switches to serial when several were, and starts the executable again for the rest. Pressing Ctrl+C once fails one test and lets the others keep running, so the user has to interrupt again and again.

Shown by: A real console Ctrl+C couldn't be sent from this shell (GenerateConsoleCtrlEvent had no console), so I simulated the interrupt's exit code. C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/ctrlc/interrupted.fb: @Test InterruptedHere sleeps 200ms, then Process.Exit(-1073741510) (STATUS_CONTROL_C_EXIT). @Test Slow1 and @Test Slow2 each sleep 1s and Print. Run with `fb test interrupted.fb --color never`.

Expected: fb test stops on an interrupt: it doesn't run more tests, and it exits with the interrupt's code, as cargo test does.

Actual: 'test interrupted.InterruptedHere ... FAILED' with 'the tests' program exited with code 0xC000013A, in this test'. Then 'Slow1 ran after the interrupt' and 'Slow2 ran after the interrupt' print, both tests pass, and fb test exits 1.

Skeptic: I sent a real Ctrl+C rather than the simulated exit code. A small helper (verify/ctrlc2/sendc.c) creates its own console, re-enables Ctrl+C, starts `fb test slow.fb` (three tests that each sleep 2s and Print), and calls GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) 1.8s in, during the run. fb did not stop. The child was killed, then all three tests ran again serially. Output: 'Slow1 ran', 'Slow2 ran', 'Slow3 ran', all ok, 'finished in 7.00s' (3 x 2s serial plus the first try), fb exit code 0x0, 6047 ms after the Ctrl+C. The interrupt was swallowed and the run reported success. With one test going it would be blamed instead, as the reviewer's simulation showed. run_attached ignores Ctrl+C in fb (compiler/src/process.cpp:235, and SIGINT/SIGQUIT SIG_IGN on POSIX). run_tests never checks for 0xC000013A or a SIGINT exit.

Fix: In run_tests, stop and return the interrupt's code when the run ended by Ctrl+C (exit code 0xC000013A, or STATUS_CONTROL_C_EXIT, on Windows; 128+SIGINT on POSIX). Also clean up the temporary folder.

Status: fixed: run_tests stops when a run ends by Ctrl+C (STATUS_CONTROL_C_EXIT on Windows, 128 + SIGINT elsewhere), and fb test says the tests were interrupted, deletes its folder, and exits with that code. Test: the cli test's interrupt.fb, whose test exits with that code; spec 09 says so.

## docs-3 (low): Import std.test As t gives bare Expect, and t.Expect is an error

compiler/src/sema.cpp:8778. Spec 11 says `Import X As C` brings X's names in only as `C.Name`. sees_test_builtins() only asks whether the file imports std.test at all, so an aliased import still gives bare Expect, Fail, and the rest. The qualified form that the alias should allow fails, with a message saying Expect isn't std.test's, which contradicts spec 09 and stdlib.md.

Shown by: .../review-m8s7/docs/q2.fb: `Import std.test As t`, then @Test Function T1() with Expect(1 == 3) and Fail("bare fail"); fb test q2.fb builds and reports both failures. b2.fb: Import std.core, Import std.test As t, @Test Function T1() calling t.Expect(1 == 2); fb test b2.fb fails to build. Compare q1.fb: Import std.collections As C, then Stack<Int>(), which gives "'Stack' is in std.collections, which this file imports as C, so it's C.Stack".

Expected: With an alias, t.Expect(...) works and bare Expect gets the 'imports as t, so it's t.Expect' error, as for any other module. Failing that, an alias is an error, or the docs say std.test's names can't be qualified.

Actual: q2.fb: bare Expect and Fail work. b2.fb: "b2.fb:6:7: error: 'Expect' isn't declared in std.test"

Skeptic: q2.fb (`Import std.test As t`, then bare Expect and Fail) builds under fb test and reports both failures. b2.fb's `t.Expect(1 == 2)` fails with "b2.fb:6:7: error: 'Expect' isn't declared in std.test". q1.fb (`Import std.collections As C`, then bare Stack) gives the expected "imports as C, so it's C.Stack". sees_test_builtins() (sema.cpp 8778) only checks imports_module, so it ignores the alias. Spec 11 line 67 says `Import X As C` brings names in only as C.Name. std/test.fb calls these "Its names", and sema's own message calls them "'Expect' is in std.test". That contradicts the "isn't declared in std.test" error for t.Expect.

Fix: Track how std.test is imported. Resolve t.Expect, t.Fail, t.Skip, and t.ExpectThrows through the alias, and reject the bare names when the import is aliased, or reject `As` on std.test with a clear message. Add sema tests.

Status: fixed with lang-4: `Import std.test As t` is an error that says std.test's names are built in, so a file has them all, with 'Import std.test'. Test: sema_tests' "std.test's built-ins are only its importers'"; spec 09 and stdlib.md say so.

## docs-4 (low): From std.test Import Expect says Expect isn't declared in std.test

compiler/src/sema.cpp:19027. stdlib.md and std/test.fb describe Expect, ExpectThrows, Fail, and Skip as std.test's names. A user who writes the From form that spec 11 offers gets an error saying std.test doesn't declare Expect. That's wrong by the docs and doesn't say to use `Import std.test`.

Shown by: .../review-m8s7/docs/b3.fb: Import std.core, `From std.test Import Expect`, @Test Function T1() with Expect(1 == 2) and Fail("x"); fb test b3.fb

Expected: Either From std.test Import Expect brings in that built-in, or the error says std.test's names are built in and come with `Import std.test`.

Actual: b3.fb:2:22: error: 'Expect' isn't declared in std.test

Skeptic: b3.fb (`From std.test Import Expect`) under fb test gives "b3.fb:2:22: error: 'Expect' isn't declared in std.test". stdlib.md and std/test.fb describe Expect and the others as std.test's names, and sema's own error for a file without the import says "'Expect' is in std.test, which this file doesn't import". So the From error contradicts both and doesn't point to `Import std.test`. It's minor, and it overlaps docs-3.

Fix: In From-import resolution, accept std.test's four built-in names (and give only those named), or give a specific message. Note the rule in spec 09's Tests section, and add a sema test.

Status: fixed with lang-4: `From std.test Import ...` is an error, as for As. Test: the same sema test.

## docs-5 (low): Spec 09's Tests example can't compile

docs/spec/09-errors.md:132. The example declares `Function ParsesALevel() Throws IOError`, does `Try LoadLevel("one.dat")`, and then checks `ExpectThrows<ParseError>(LoadLevel("broken.dat"))`. With spec 09's own std.core IOError and ParseError, which are unrelated Classes that each extend Error, this can't compile. If LoadLevel throws only IOError, ExpectThrows<ParseError> is an error. If it also throws ParseError, the Try in a Function that Throws only IOError is an error.

Shown by: .../review-m8s7/docs/l2.fb (LoadLevel Throws IOError, ParseError; test Throws IOError): "error: ParseError isn't caught; catch it in a Try block, or add it to the Throws of 'ParsesALevel'". l3.fb (LoadLevel Throws IOError only): "error: nothing in 'LoadLevel(\"broken.dat\")' throws ParseError". Ran with fb test l2.fb and fb test l3.fb.

Expected: The spec's example compiles as written, given a LoadLevel the reader can infer.

Actual: Each reading of LoadLevel's Throws gives a compile error.

Skeptic: Spec 09 says IOError and ParseError each extend Error. I wrote my own l4.fb, because the reviewer's l2.fb used a List type that doesn't exist: LoadLevel Throws IOError, ParseError, and the test Throws IOError exactly as in the spec example. fb test l4.fb gave "ParseError isn't caught; catch it in a Try block, or add it to the Throws of 'ParsesALevel'" at the `Try LoadLevel("one.dat")` line. l3.fb (LoadLevel Throws IOError only) gave "nothing in 'LoadLevel(\"broken.dat\")' throws ParseError". l5.fb, with the test changed to Throws IOError, ParseError, passed. So the example as written can't compile under either reading.

Fix: Make the test `Throws IOError, ParseError` (or `Throws Error`), or use a ParseError that extends IOError and say so.

Status: fixed: the example's test is `Throws IOError, ParseError`.

## docs-6 (low): @Test Async/Const/Unsafe Function says @Test only goes before a Function

compiler/src/parser.cpp:1389. Spec 09 says a test isn't Async, Const, or Unsafe. The parser rejects these by checking that the next token is `Function`, but the message it gives, "@Test only goes before a Function, which fb test runs", is the same one used for `@Test Class`. For `@Test Async Function A()` it's misleading, since A is a Function. The parser's own comment says "not an Async, Const, or Unsafe one", but the message doesn't.

Shown by: .../review-m8s7/docs/a3.fb: `@Test Async Function A()`, `@Test Const Function B()`, `@Test Unsafe Function C()`; fb check a3.fb

Expected: Something like "a @Test Function can't be Async" (or Const, or Unsafe), as sema's messages for generic, Extern, parameters, and visibility name the reason.

Actual: a3.fb:4:2: error: @Test only goes before a Function, which fb test runs (the same for lines 7 and 10)

Skeptic: fb check a3.fb printed "@Test only goes before a Function, which fb test runs" for `@Test Async Function A()`, `@Test Const Function B()` and `@Test Unsafe Function C()`, the same message as for `@Test Method M()` in a Class. parser.cpp ~1388 checks !at(KW_FUNCTION), and its comment says "not an Async, Const, or Unsafe one". Sema's other @Test errors name their reason (visibility, parameters, result, Extern), but this one wrongly implies A, B and C aren't Functions.

Fix: When the token after the attributes is Async, Const, or Unsafe, name it in the message. Add the cases to the @Test sema/parser test.

Status: fixed: `@Test Async`, `Const`, and `Unsafe Function` say "a @Test Function can't be Async, since fb test calls it as a plain Function" (or Const, or Unsafe). Test: sema_tests' "a @Test Function takes nothing and gives nothing, though it can throw".

## docs-9 (low): No tests for the library check/build tests/ exclusion, the interface file's omission of tests, or fb test on a file

compiler/src/interface.cpp:421. The step changed library_files() so fb check and fb build of a library skip its tests/ folder (spec 11's new text), and interface.cpp so a library's interface file leaves out @Test Functions (spec 09's new text). No test covers either. The driver test only builds a program package, which never loads tests/, and no test reads an interface file with a @Test in it. The intended "fb test also tests a lone .fb file" also has no test, and neither does fb test given a .fb file inside a package or no argument. Both behaviors work by hand.

Shown by: .../review-m8s7/docs/lib1 (library with shapes.fb holding @Test AreaWorks and tests/outside.fb). Adding tests/broken.fb with "This is not code": fb check lib1 and fb build lib1 pass, and fb test lib1 reports its errors. lib1/build/windows-x64/debug/lib1.fbi has no AreaWorks. grep -rn compile_tests tests/ shows only the package case in driver_tests.cpp.

Expected: Tests that pin the documented behavior: a library's check and build ignore tests/, its interface has no @Test, and fb test works on a lone .fb file and on a file in a package.

Actual: Removing the tests/ exclusion or the interface.cpp check would fail no test.

Skeptic: Partly confirmed. The added tests are the CLI case on tests/packages/tested, which always passes a package folder, and the driver test, which builds the program package 'app'. No test reads a library's .fbi for a @Test. I built lib1 by hand and its lib1.fbi has 0 matches for AreaWorks, so the interface.cpp is_test skip works but isn't pinned. No CLI or driver test runs `fb test` on a lone .fb file or on a .fb file inside a package. The claim that removing library_files()' tests/ exclusion fails no test is overstated: compile_tests() also uses library_files(own), so the driver test would then load tests/outside.fb twice and fail. Only the `fb check`/`fb build` of a library skipping tests/ has no test of its own. Under the project's rule that every documented behavior gets a test, the interface and lone-file gaps are real but small.

Fix: Add a driver test that checks a library with a broken tests/ file and a @Test, and reads its interface. Add CLI cases for `fb test x.fb` alone and inside a package.

Status: fixed: the driver test checks that a library's check ignores a broken tests/ file, and that its build's interface file and binary have its Public Function but not its test; cli_tests' fb test case runs a file in the package, and the new cli case runs lone files throughout.

## lang-3 (low): A library's binary keeps code that only its tests reach, which spec 09 says builds leave out

compiler/src/lower.cpp:295. Spec 09's Tests section says "`fb build`, `fb run`, and a library's binary and interface file leave tests out, and the code only they reach." The library build drops the @Test Functions (`!function.modifiers.is_test`), but it still lowers every other Function of its modules. So a Private helper called only from a test ends up in the .fblib, in debug and release builds. The codegen test "a build leaves out the code only tests reach" checks only a main file, where LLVM drops internal functions nothing calls, not a library build.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/lib1/ (type = "library"). core.fb has:
  Function OnlyForTestsXyz(): Int => 77
  @Test
  Function UsesPrivate()
      Expect(OnlyForTestsXyz() == 77)
  End Function
In lib1/: fb.exe build --release; then grep -a -o "fb\.lib1\.core\.[A-Za-z]*" build/windows-x64/release/lib1.fblib

Expected: The binary has no OnlyForTestsXyz, as the spec says, or the spec says only that tests are left out.

Actual: The grep prints fb.lib1.core.Add, fb.lib1.core.OnlyForTestsXyz, and fb.lib1.core.TestHelper. The tests (Adds, UsesPrivate) aren't there, but the helper only they call is.

Skeptic: I copied lang/lib1 to verify/lib1 and ran `fb build --release`. I stripped the .fblib header to get its LLVM bitcode, then disassembled it with clang -x ir -S -emit-llvm. The output has `define i32 @"fb.lib1.core.OnlyForTestsXyz()"()`, a full definition, though only the @Test UsesPrivate calls it. The tests themselves (Adds, UsesPrivate) aren't there, and the .fbi doesn't mention OnlyForTestsXyz. Spec 09's Tests section says a library's binary leaves out tests "and the code only they reach", so the code plainly contradicts the spec. The cost is only binary size.

Fix: Either lower a library's non-Public code by reachability from its Public and Package declarations and Global initializers, leaving out what only tests reach, or change spec 09's sentence to say only the tests are left out of a library's binary. Add a test for a library build.

Status: fixed in spec 09, the skeptic's second option: a library's binary keeps every one of its modules' Functions, exported, since a program's instances of its generic code can call its Private ones, so leaving out what only tests reach would need the interface writer's reachability in lowering; spec 09 now says a library's binary and interface file leave the tests out, and only fb build and fb run the code only they reach. The tests' absence is pinned by the driver test above.

## lang-4 (low): std.test's names ignore From and As imports, and From gives a wrong error

compiler/src/sema.cpp:8777. sees_test_builtins() only asks whether the module imports std.test at all. As a result, `From std.test Import Expect` is rejected with "'Expect' isn't declared in std.test", although spec 09 and stdlib.md present Expect as std.test's. `Import std.test As t` rejects `t.Expect(...)` the same way, yet it makes the unqualified `Expect(...)` work. Spec 11 says `Import X As C` brings names in only as `C.Name`, and other modules follow that: `Import std.collections As C` with a bare Stack gives "it's C.Stack".

Shown by: Both files are in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/.
hh.fb:
  Import std.core
  From std.test Import Expect
  @Test
  Function Selected()
      Expect(1 == 2)
  End Function
ii.fb: `Import std.test As t`, with `t.Expect(1 == 2)` and then `Expect(1 == 3)` in a @Test.
Run fb.exe test on each.

Expected: `From std.test Import Expect` brings in Expect, and `t.Expect` works under As while the bare name doesn't. Failing that, an error that says std.test's built-ins come only with a plain `Import std.test`.

Actual: hh.fb: "error: 'Expect' isn't declared in std.test". ii.fb: "'Expect' isn't declared in std.test" at t.Expect. With that line removed, the bare Expect(1 == 3) builds and reports its failure.

Skeptic: verify/hh.fb (`From std.test Import Expect`) failed with "hh.fb:2:22: error: 'Expect' isn't declared in std.test". verify/ii.fb (`Import std.test As t`, t.Expect) failed with "'Expect' isn't declared in std.test". verify/ii2.fb (As t, bare Expect(1 == 3)) built and reported the failure. For comparison, `Import std.collections As C` with a bare Stack gives "it's C.Stack". Spec 11 says `Import X As C` brings names in only as C.Name, and stdlib.md and spec 09 present Expect as std.test's. sees_test_builtins() (sema.cpp:8778) only checks imports_module, so From's selected names and the As qualifier are ignored. I also checked `From std.core Import Assert`: it fails the same way, but Assert is a language-wide built-in, not std.core's, so that isn't a precedent.

Fix: Have From's selected names and an As qualifier resolve to the std.test built-ins, and stop the bare names under As. Or reject From and As on std.test with an error that says so, and write that in spec 09 and stdlib.md.

Status: fixed: From and As on std.test are an error, which says std.test's names are built in, so a file has them all with 'Import std.test'. Test: sema_tests' "std.test's built-ins are only its importers'"; spec 09 and stdlib.md say so.

## lang-5 (low): Misleading diagnostics for ExpectThrows used as a value, or without the import

compiler/src/sema.cpp:21427. ExpectThrows is only recognized as a call's callee. In a file that imports std.test, `Local j = ExpectThrows` says "'ExpectThrows' isn't declared", while `Local k = Expect` gives "'Expect' is built in, so it can't be a value; use a lambda that calls it". Without the import, check_expect_throws runs check_alone(node.arguments) with no TRY_OPTIONAL handler. So after the right hint, "'ExpectThrows' is in std.test, which this file doesn't import", the argument gets a second, wrong error, "'Boom' can throw Error, so calling it needs Try, Try?, or a Try block". The malformed-call branch just below pushes a TRY_OPTIONAL handler to avoid exactly that.

Shown by: Both files are in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/.
r.fb (imports std.test):
  Function T()
      Local k = Expect
      Local a = Assert
      Local j = ExpectThrows
  End Function
j.fb (no std.test): `Local e = ExpectThrows(Boom())`, where Boom Throws Error.
Run fb.exe check on each.

Expected: r.fb: "'ExpectThrows' is built in, so it can't be a value...", as Expect and Assert say. j.fb: only the std.test import hint.

Actual: r.fb:7:15: error: 'ExpectThrows' isn't declared. j.fb:11:15 gives the import hint, then j.fb:11:28: error: 'Boom' can throw Error, so calling it needs Try, Try?, or a Try block.

Skeptic: verify/r.fb (imports std.test) gives "'Expect' is built in, so it can't be a value" for Expect, and the same for Assert and Fail. For ExpectThrows it gives only "'ExpectThrows' isn't declared". verify/j3.fb (no import, Boom(): Int Throws Error) gives the import hint, then a second, wrong error: "'Boom' can throw Error, so calling it needs Try, Try?, or a Try block". With a no-value Boom (j2.fb), a third appears: "'Boom' doesn't return a value". The same call with the import (j4.fb) builds and passes. In sema.cpp:22621, check_expect_throws's not-imported branch calls check_alone(node.arguments) without the TRY_OPTIONAL handler that the malformed branch just below pushes.

Fix: Give a bare ExpectThrows name the built-in-as-value error when std.test is imported. In the not-imported branch of check_expect_throws, check the arguments under a TRY_OPTIONAL handler, as the malformed branch does.

Status: fixed: `ExpectThrows` as a value says it's built in, so it can't be a value, as Expect does, and a call without the import, or with the wrong arguments, checks them as ExpectThrows would, under a Try?'s handler and taking no value, so only the hint shows. Test: the same sema test.

## lang-6 (low): fb build lets the package's own code import its tests/ folder, then fb test rejects the same program

compiler/src/driver.cpp:2519. Spec 09 says a package's tests/ folder is a package of its own, app.tests, and that "A package builds without its tests/ folder." fb build and fb run still let the package's main file `Import app.tests.helpers`. They load tests/helpers.fb as a module of package app, so it can use app's Package names. fb test loads the same file as a module of app.tests, where those Package names are out of reach, so a program that builds and runs fails its test build. The file's package depends on which command reads it.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/lang/app/ (game/shapes.fb has `Package Function Square(n: Int): Int => n * n`). Add tests/helpers.fb:
  Import app.game.shapes
  Public Function Helped(): Int => Square(2) + 1
and add `Import app.tests.helpers` to main.fb. In app/: fb.exe run, then fb.exe test. (Both edits were undone afterwards.)

Expected: fb build and fb run reject an Import into the tests/ folder from the package's own code, as they reject an Import into a folder with its own project.toml, so the package builds without its tests/ folder.

Actual: fb run builds and prints "main 4". fb test fails with "tests/helpers.fb:2:34: error: 'Square' is Package in game/shapes.fb, so only the package app can use it".

Skeptic: In verify/app6 I added tests/helpers.fb (imports app.game.shapes and uses the Package Function Square) and added `Import app.tests.helpers` to main.fb. `fb run` built and printed "main 4 5". `fb test` failed: "tests/helpers.fb:2:34: error: 'Square' is Package in game/shapes.fb, so only the package app can use it". Spec 09 says the tests/ folder is a package of its own, app.tests, and that "A package builds without its tests/ folder". So fb build and fb run, which pull tests/helpers.fb in as a module of app, contradict the spec, and the same program builds under one command and fails under another.

Fix: Make Import resolution treat the package's top-level tests/ folder as outside the package, for its own modules, in every build. Add a driver test where main imports a tests/ file.

Status: fixed: an Import in a package's own files of its tests/ folder, by name or by path, is an error in every build. Test: the driver test above; spec 09 says so.

## run-5 (low): After a panic, debug builds run tests that already finished again, but spec 09 and std/test.fb say only those that didn't finish run again

docs/spec/09-errors.md:156. In a debug build, `keeps` is false whenever a run ends early (to get leak checks), so every test that ended in that run runs again, not just the unfinished ones. Spec 09 says 'fb test starts the program again after a panic, for the tests that didn't finish', and std/test.fb:17 says 'fb test runs the ones it didn't finish again'. A test with side effects (Print, files) does them twice.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/twice/twice.fb: @Test Quick does Print("Quick ran"); @Test PanicsLater sleeps 100ms and panics. Run with `fb test twice.fb --color never` and with `--release`.

Expected: As the docs say, Quick runs once. Or the docs say that debug builds also run again the tests that finished, so their leaks can be checked.

Actual: In debug, 'Quick ran' prints twice: once before the panic and once in the rerun. With --release it prints once.

Skeptic: In debug, `fb test twice/twice.fb --color never` printed 'Quick ran', then the panic, 'PanicsLater ... FAILED', then 'Quick ran' again and 'Quick ... ok'. With --release, 'Quick ran' printed once. In commands.cpp, `keeps` is false in debug after an early end, so finished tests run again on purpose, for their leak check. Spec 09 says fb test starts again 'for the tests that didn't finish', and std/test.fb says it 'runs the ones it didn't finish again'. The docs and the behavior disagree, so a test's side effects happen twice.

Fix: Update spec 09 and std/test.fb (and tooling.md's line) to say that a debug build runs the run's finished tests again too, for their leak check. Or change the behavior to match the docs.

Status: fixed in the docs: spec 09, std/test.fb, and tooling.md say a debug build runs a crashed run's finished tests again, whose leaks it didn't check. Test: the cli test's twice.fb, whose test runs twice in debug and once in release.

## run-6 (low): A panic before any test runs is reported as 'exited with code 101, which a panic gives first', without the panic's text

cli/src/commands.cpp:866. The no-progress message appends ' first' after exited_with(), whose text for 101 already ends in ', which a panic gives'. The result reads as if 'first' belonged to the panic. at_panic is installed only in fb_test_run (test.cpp:333), after the Globals are set, so a panic in a Global's initializer never reaches the report. The failure therefore never shows what panicked, and each test's message just repeats the exit code.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s7/run/ginit/ginit.fb: Global start = Start(), where Start indexes [1] at 2; @Test A and @Test B. Run with `fb test ginit.fb --color never`.

Expected: A message like 'the tests' program panicked before any test ran: index 2 is out of range for 1 element at ginit.fb:6:12', or at least correct wording.

Actual: Both A and B list 'the tests' program exited with code 101, which a panic gives first'.

Skeptic: `fb test ginit/ginit.fb --color never`: both A and B list 'the tests' program exited with code 101, which a panic gives first'. exited_with (commands.cpp:439) returns '..., which a panic gives', and run_tests appends ' first', so the sentence reads wrong. The panic text ('index 2 is out of range for 1 element at ginit/ginit.fb:6:12') appears only in the child's raw stderr, not in the failures. at_panic is installed in fb_test_run (test.cpp:333), after the Globals are set, so a Global initializer's panic never reaches the report.

Fix: Word it as 'the tests' program {exited_with} before any test ran'. Install the panic hook, and open the report, before the Globals are set (for example, from fb_start in a test build), so the panic text is captured.

Status: fixed: fb_test_begin, which a test build's main calls before it sets the Globals, opens the report and installs the panic hook, so a Global initializer's panic is reported with its text, and a run that ends before any test does says "<the panic>\nbefore any test ran", or "the tests' program exited with code N\nbefore any test ran". Test: the cli test's early.fb.
