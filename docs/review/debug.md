# The review of M6 step 32's std.debug unit: Breakpoint and IsDebuggerAttached

2 confirmed findings: 0 high, 1 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## debug-1 (medium): On Linux, Breakpoint ends a program run under strace or ltrace

Status: fixed in a6e033f5: on Linux and macOS, fb_debug_break puts an empty SIGTRAP handler in place around the raise and the old one back after, under a lock, so the tracer still stops at the trap and one that hands it back, as strace does, runs the empty handler; debug_tests traces a child that calls two breakpoints, under a tracer that swallows each trap and under one that hands it back, and checks two stops and an exit of 0 (run in WSL; without the fix the second child dies of SIGTRAP)

Where: runtime/src/debug.cpp:71

On Linux, a non-zero TracerPid means any ptrace tracer is attached, not only a debugger. Under strace, ltrace, or any other tracer that hands each signal back to the program, IsDebuggerAttached() is True. fb_debug_break then calls std::raise(SIGTRAP), and the tracer passes the signal on. SIGTRAP's default action kills the process and dumps core. docs/stdlib.md, std/debug/debugger.fb and fb/debug.hpp all promise the opposite: Breakpoint does nothing without a debugger, 'so one left in a program doesn't end it'. Running a program under strace to look at its I/O is common, and a Breakpoint left in the program kills it. I can't build fb for Linux here, so I compiled runtime/src/debug.cpp unchanged with g++ 15 -std=c++23 -fno-exceptions in WSL Ubuntu, with a main that prints fb_debug_attached() and then calls fb_debug_break(). I ran it plain, under a minimal ptrace tracer that passes every signal back (what strace does), and under one that swallows SIGTRAP (what gdb does by default). Plain: 'attached: 0 / went on'. Under the passing tracer: 'attached: 1', then '[tracer] stopped by Trace/breakpoint trap' and '[tracer] killed by Trace/breakpoint trap'. Under the swallowing tracer: 'attached: 1 / went on'. Windows is fine: under gdb it stops in KERNELBASE!DebugBreak both times and continues to the end.

```
Import std.core
Import std.debug

Print(IsDebuggerAttached())
Breakpoint()
Print("went on")

// On Linux: fb build trap.fb -o trap && strace -o /dev/null ./trap
```

Expected: True
went on
(the program exits 0; strace only watches it)

Actual: True
then the process is killed by SIGTRAP: 'Trace/breakpoint trap (core dumped)', exit status 133. Reproduced with the runtime's debug.cpp under a strace-like ptrace tracer in WSL: '[tracer] killed by Trace/breakpoint trap'.

Suggested fix: Around the raise on Linux and macOS, put an empty SIGTRAP handler in place with sigaction and restore the old one afterwards. The tracer still gets the signal-delivery stop first, so gdb and lldb stop as they do now. A tracer that hands the signal back now runs the empty handler instead of the default action. I tested this in WSL: both the passing and the swallowing tracer stop at the trap, and the program prints 'went on' and exits 0. Also add a Linux runtime test that forks under a tracer which passes signals back and checks the child exits 0.

Skeptic: I copied rev-5's runtime/src/debug.cpp and fb/debug.hpp unchanged to rv-debug/v1/linux/ and built them in WSL Ubuntu with g++ 15.2 (-std=c++23 -fno-exceptions). The main prints fb_debug_attached() and then calls fb_debug_break(). I wrote my own tracer.cpp, which forks, runs PTRACE_TRACEME and exec, sets PTRACE_O_TRACESYSGOOD, then PTRACE_CONTs after every stop. In pass mode it hands each signal back, as strace does; in swallow mode it drops SIGTRAP, as gdb does by default. Results: plain run, 'attached: 0 / went on', exit 0. Pass mode: 'attached: 1', '[tracer] stopped by Trace/breakpoint trap', '[tracer] killed by Trace/breakpoint trap'. Swallow mode: 'attached: 1 ... went on ... exited 0'. So when a tracer that isn't a debugger is attached, TracerPid is non-zero, fb_debug_break raises SIGTRAP with the default action still in place, and the process dies. That breaks stdlib.md's promise that 'one left in a program doesn't end it' (also in debugger.fb and debug.hpp). FB's Breakpoint is a direct call to fb_debug_break. The suggested fix also checks out: with an empty sigaction handler installed around raise and restored after (debug_fixed.cpp), pass mode prints 'went on' and exits 0, swallow mode still stops at the trap and exits 0, and the plain run is unchanged. WSL has no strace or gdb and I can't build fb for Linux, so this used the runtime's source under my own tracer. On Windows, fb run and fb run --release of the finding's program print 'False / went on' and exit 0.

## debug-2 (low): No test covers a debugger being attached, so the OS checks are never exercised

Status: fixed in a6e033f5 and 1d22b34f: the traced child on Linux and macOS checks fb_debug_attached is 1 before its breakpoints, and on Windows a test runs the test program again under DEBUG_ONLY_THIS_PROCESS, as a hidden test that checks it's attached and calls two breakpoints, and checks the debugger saw the loader's breakpoint and the program's two, and an exit of 0

Where: tests/runtime/debug_tests.cpp:5

Both tests, debug_tests.cpp and tests/programs/debug_debugger.fb, run only with no debugger attached. They only check that fb_debug_attached() is 0 or False and that Breakpoint returns. If fb_debug_attached's whole body were replaced with 'return 0;', every test would still pass on all three OSes. The macOS sysctl/P_TRACED path, the Linux TracerPid parsing and IsDebuggerPresent are never checked to report True. On macOS a failing sysctl call also returns 0, so a wrong MIB or flag would go unnoticed. Breakpoint stopping in the debugger and then continuing is untested too. The behaviour itself is correct on Windows: I built the program below and ran it under CLion's gdb ('gdb -batch -ex run -ex bt -ex continue -ex bt -ex continue attached.exe'). gdb stopped twice with SIGTRAP in KERNELBASE!DebugBreak, called from fb_debug_break, then fb.std.debug.debugger.Breakpoint(), then fb[main]. The program printed 'attached: True / after first / after second' and exited normally. Nothing in tests/ runs this case.

```
Import std.core
Import std.debug

Print("attached: {IsDebuggerAttached()}")
Breakpoint()
Print("after first")
Breakpoint()
Print("after second")

// fb build attached.fb -o attached.exe, then run it under a debugger (gdb -batch -ex run -ex continue -ex continue attached.exe)
```

Expected: A test that runs this under a debugger or tracer on each OS and checks: two SIGTRAP or breakpoint stops, 'attached: True', 'after first', 'after second', exit 0.

Actual: No such test exists. The suite passes even if IsDebuggerAttached always returns False. The manual gdb run on Windows does print 'attached: True / after first / after second' with two SIGTRAP stops in KERNELBASE!DebugBreak.

Suggested fix: Add a runtime test that traces a child process. On Linux and macOS: fork, have the child call ptrace(PTRACE_TRACEME / PT_TRACE_ME) and raise(SIGSTOP), then CHECK fb_debug_attached() == 1, call fb_debug_break() and exit 0. The parent runs a waitpid loop that continues with signal 0 on SIGTRAP and checks it saw one SIGTRAP and a clean exit. On Windows: CreateProcess the test binary in a child mode with DEBUG_ONLY_THIS_PROCESS, run a WaitForDebugEvent loop that answers the program's EXCEPTION_BREAKPOINT with DBG_CONTINUE, and check that the child reports attached and exits 0.

Skeptic: Both tests only check the case where no debugger is attached. debug_tests.cpp CHECKs fb_debug_attached() == 0 and calls fb_debug_break twice, and debug_debugger.out expects 'False / went on'. Mutation check: I built rev-5's tests/runtime/debug_tests.cpp against a stub.cpp in which fb_debug_attached returns 0 and fb_debug_break does nothing, linked with the build's libCatch2Maind.a and libCatch2d.a under MinGW g++. It printed 'All tests passed (2 assertions in 1 test case)' and exited 0. The .fb program's output would match too, since it only prints False. So the suite can't tell the real Windows, macOS and Linux checks from a stub. The breakpoint path when a debugger is attached is never run either, and that path is where debug-1's real kill-under-tracer bug went unnoticed. milestones.md row 32 requires each module 'documented and tested'. A ptrace-based child test like the debug-1 fix's works: my WSL tracer harness shows it detects attached == 1 and both outcomes of the stop.
