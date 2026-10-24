# The review of M6 step 17's std.os: lane 4's Env, .env files if any, system info, Folders, OpenDefault, Process.Exit and Process.Id (git diff c0962b80 42c226ee), and lane 8's processes and signals on it: Command, Child, piped streams as step 13's Readers and Writers, Process.Arguments, OnInterrupt, OnTerminate (git diff 46d48446 47d18376); the commits between 42c226ee and 46d48446 are a temporary copy of step 13, not under review

18 confirmed findings: 2 high, 8 medium, 8 low. This file has lane 8's 16, of the processes and signals; signals-os-3 (Env.Expand and Env.Load) and signals-os-4 (Folders.Temp's comment) are lane 4's, in its own. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## spawn-1 (high): On Windows, a .bat or .cmd program's arguments go through cmd.exe, so an argument can run commands

Status: fixed in 0e8edfa: on Windows, a program found as a .bat or .cmd file (fb::os::is_batch_file(), any case, dots and spaces after it) runs as `cmd.exe /e:ON /v:OFF /d /c` with UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS and fb::os::batch_arguments(), Rust's append_bat_arg escaping (quotes for anything but letters, digits, and #$*+-./:?@\_, a quote doubled, every % as %%cd:~,%); an argument with a line break throws an IOError (failure 3). tests/runtime/process_tests.cpp's batch cases, and the CLI test "a .bat or .cmd file gets each argument as cmd.exe can carry it, and nothing runs" (the injections above, %PATH%, .cmd, SHOW.CMD, show.bat. ., and a line break)

Where: runtime/src/process.cpp:700

start_child gives libuv the program and arguments as they are. libuv quotes them by the MSVCRT rules, leaving an argument with no space, tab or quote unquoted and writing a quote as \". When the program is a .bat or .cmd file, CreateProcessW runs it through cmd.exe, which doesn't follow those rules: it acts on &, |, <, >, ^ and %VAR%, and doesn't treat \" as an escaped quote. So Command("x.bat").Arg(untrusted) is command injection (the BatBadBut bug: CVE-2024-24576 in Rust, CVE-2024-27980 in Node). This contradicts Arg's doc ('the program gets it as it is: nothing splits it, or reads quotes or anything else in it') and stdlib.md's 'No shell'.

```
Import std.core
Import std.os

// A batch file that prints its first argument.
Local folder = Folders.Temp.Expect("a temp folder") + "\\fb-bat"
Try Command("cmd").Args(["/c", "mkdir", folder]).Output()
Local script = folder + "\\show.bat"
Try Command("cmd").Args(["/c", "echo @echo first=[%1]> " + script]).Output()

// Each argument should reach the script as one argument, with nothing run.
Write((Try Command(script).Arg("safe&echo,INJECTED").Output()).Text)
Write((Try Command(script).Arg("x\" & echo INJECTED-TOO").Output()).Text)
```

Expected: first=[safe&echo,INJECTED]
first=[x" & echo INJECTED-TOO]
(or an IOError that refuses to start a batch file)

Actual: first=[safe]
INJECTED
first=["x\"]
INJECTED-TOO"

Suggested fix: On Windows, check the program after lookup, ignoring case and any trailing dots or spaces. When it ends in .bat or .cmd, either refuse it with an IOError (Node's approach), or do what Rust does: run cmd.exe /e:ON /v:OFF /d /c with a command line built by cmd's own escaping rules, passed with UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS, and refuse arguments that can't be escaped (CR or LF). Add a test with a .bat.

Skeptic: Ran rv-step17/proc/s1.fb (debug). show.bat is '@echo first=[%1]'. Command(script).Arg("safe&echo,INJECTED") printed 'first=[safe]' then 'INJECTED'. Arg("x\" & echo INJECTED-TOO") printed 'first=["x\"]' then 'INJECTED-TOO"'. A plain argument printed 'first=[plain]'. So cmd.exe ran the injected commands. start_child (process.cpp:700-722) gives uv_spawn the program and arguments with no .bat/.cmd handling. That contradicts Arg's doc ('nothing splits it, or reads quotes or anything else in it') and stdlib.md's 'No shell: a program and its arguments, the same on every OS'.

## std-1 (high): On Windows, a .bat or .cmd program's arguments go through cmd.exe, which reads %VAR% and quotes and runs injected commands

Status: fixed in 0e8edfa, with spawn-1, which it is: the same CLI test has %PATH% and the quote-and-ampersand argument, and Arg's comment and stdlib.md's Process bullet say what a batch file gets

Where: runtime/src/process.cpp:700

Command promises no shell, so each argument reaches the program as it is. On Windows, libuv's uv_spawn hands a .bat or .cmd file to CreateProcessW, and CreateProcessW runs it through cmd.exe with the command line libuv quoted for C programs. cmd.exe doesn't follow those rules: it expands %VAR% even inside quotes, and a '"' inside an argument closes cmd's quoting, so an '&' after it starts a new command. This is the BatBadBut issue (CVE-2024-24576 in Rust, CVE-2024-27980 in Node). libuv doesn't guard against it, and start_child and fb_os_process_spawn don't either. Any program that passes outside text to a batch file can be made to run commands. Only Windows is affected: libuv only adds .com and .exe to a bare name, so Command("show.bat") or a path ending in .bat/.cmd is what reaches cmd.exe. Not decided anywhere in docs/decisions.md.

```
Import std.core
Import std.os

// show.bat, in the temp folder, is these three lines:
//   @echo off
//   echo arg1=[%1]
//   echo arg2=[%2]
// No shell reads a Command's arguments, so each reaches the program as it is.
Local bat = Folders.Temp.Expect("a temp folder") + "\\show.bat"
Local out = Try Command(bat).Arg("%USERNAME%").Arg("a\"&echo INJECTED").Output()
Write(out.Text)
```

Expected: arg1=[%USERNAME%] and arg2 holding the whole a"&echo INJECTED argument, with nothing else run. Or an IOError refusing the batch file, as Node does.

Actual: arg1=[Hunter]
arg2=["a\"]
INJECTED"
(The variable was expanded, and the echo INJECTED that came in through the argument ran as its own command. Same in debug and --release.)

Suggested fix: On Windows, in fb_os_process_spawn or start_child, check whether the program ends in .bat or .cmd. Compare case-insensitively, after dropping trailing dots and spaces, which Windows ignores. Then either throw an IOError saying batch files need an explicit cmd.exe Command, as Node does, or launch it the way Rust 1.77.2 does: cmd.exe /e:ON /v:OFF /d /c with UV_PROCESS_WINDOWS_VERBATIM_ARGUMENTS, cmd-specific escaping, and a refusal for arguments that can't be escaped (newlines, for one). Add a Windows-only case to os_process.fb, and say so in Command's comment and in stdlib.md's Process bullet.

Skeptic: Wrote %TEMP%\show.bat (@echo off / echo arg1=[%1] / echo arg2=[%2]) and ran rv-step17/std-r/s1.fb: Command(bat).Arg("%USERNAME%").Arg("a\"&echo INJECTED").Output(). It printed, in both debug and --release: arg1=[Hunter] / arg2=["a\"] / INJECTED". So cmd.exe expanded the variable and ran the injected echo as a command of its own. start_child (runtime/src/process.cpp:660-723) passes the program and its arguments straight to uv_spawn, with no check for .bat or .cmd files. That breaks stdlib.md's promise ('No shell: a program and its arguments, the same on every OS') and the milestones row's done-when ('with no shell'). docs/decisions.md says nothing about it. This is command injection whenever a program passes outside text to a batch file.

## signals-os-1 (medium): A program whose first OnInterrupt/OnTerminate comes after its code is done never ends, and the handler then catches the signal

Status: fixed in 6405b3a: run_code_end() marks the code done before it runs the slots, and at_code_end() runs what it's given at once when the code is done (both seq_cst, so one sees the other; end_signals() takes a second call), so a late watch ends its waits and watches nothing; tests/programs/os_signals_late.fb, a Thread.Start thread's OnInterrupt after the code, which hung, ends

Where: runtime/src/runtime.cpp:236

at_code_end only stores the function in a slot, and nothing records that run_code_end has already run. Suppose the first Process.OnInterrupt or OnTerminate is called once fb_scheduler_finish has run run_code_end, for example from a Thread.Start thread that the code started and that pool finish is still waiting for. Then processes_t::end_signal_waits never runs. m_code_ended stays false, start_watching (process.cpp:246 watch -> start_watching) starts the libuv watchers, and Watch's Thread.Start thread waits in fb_os_signal_wait forever. fb_pool_finish waits for that thread, so the program hangs. A Ctrl+C or Ctrl+Break then runs the handler instead of ending the program. This breaks the doc's promise that once the code is done 'none runs, and the signal ends the program again'. A call racing run_code_end on another thread hits the same thing. If the code installed any handler earlier, the late one gets m_code_ended and returns (checked: that variant ends with exit 0). Debug and release behave the same.

```
// A thread the code started asks for a handler after the code is done.
Import std.core
Import std.os
Import std.time

Thread.Start(Function()
    Sleep(200.Milliseconds)
    Process.OnInterrupt(Function()
        Print("interrupted")
    End Function)
    Print("watching")
End Function)
Print("done")
```

Expected: done
watching
(then the program ends with exit code 0; a Ctrl+Break sent after 'watching' ends it)

Actual: done
watching
(then it never ends: `timeout 20 fb run` kills it in debug and in --release. A variant that sends Ctrl+Break after 'watching' prints 'interrupted' and still hangs until it's killed.)

Suggested fix: Have run_code_end set a flag (an atomic, release) after running the slots. at_code_end, after its CAS, checks the flag and runs `done` itself when the code has already ended, or watch() asks a fb::runtime::code_ended() first and returns without watching. Either way Watch's thread gets 0 at once. Add a program test like the one above (it must end) to tests/programs.

Skeptic: I ran rv-step17/sig-os/late_handler.fb, the finding's program, with `timeout 20 fb run`. Debug and --release both print 'done' and 'watching', then hang until timeout kills them (exit 124). I also built late_break.fb, which installs the handler late and then has a driver send Ctrl+Break from a new process group once 'watching, send' is printed. The Ctrl+Break does not end it; it was still running after 15 s and the driver had to TerminateProcess it. The code matches the finding. at_code_end (runtime.cpp) only CASes the function into a slot, and nothing records that run_code_end has already run. So a first watch() after fb_scheduler_finish never gets end_signals: m_code_ended stays false, start_watching starts the libuv watchers, and Watch's Thread.Start thread blocks in wait_for_signal. fb_pool_finish then waits for that thread forever. This contradicts the doc comment in process_signals.fb and stdlib.md: handlers apply only until the code is done, after which the signal ends the program again.

## signals-os-2 (medium): With a signal handler watching, a program with stuck coroutines never ends

Status: fixed in 6405b3a and 0e8edfa: a thread waiting in fb_os_signal_wait counts in the pool's m_signal_waits (add_signal_waits(), uncounted by what wakes it, on_signal() or end_signals()), and stuck() takes the pool as idle when only those wait (idle_but_for_signals()); while one waits, the Tasks a TaskSource holds (start_source_task(), from fb_signal_task) are coming, so awaiting what a handler gives still waits. tests/programs/os_signals_stuck.fb (the finding's program ends), and the CLI test "a coroutine waiting for what a handler gives waits for the signal"

Where: runtime/src/thread.cpp:588

Watch (std/os/process_signals.fb:33) runs each handler's loop on a Thread.Start thread, and that thread counts as pending pool work until run_code_end. scheduler_t::stuck() returns false whenever pool().idle() is false. So fb_scheduler_finish's run_until(empty || stuck) can't finish while a handler watches if a main-thread coroutine is stuck, for example awaiting a coroutine on a Scheduler nothing ticks. run_code_end, which would release the handler threads, only runs after that loop, so the program hangs forever. The same program without the handler ends at once. This contradicts os_signals.fb's own comment: handlers for signals that never come leave the program ending 'once its code is done, its coroutines too'. Note the trade-off: awaiting a TaskSource that the handler finishes on Ctrl+C works today because of this (checked: wait_ctrl prints 'stopping' and exits 0), so a fix must keep that.

```
// A coroutine waits for a Scheduler that nothing ticks: with a handler for Ctrl+C, the program
// should end as it does without one.
Import std.core
Import std.time
Import std.os

Process.OnInterrupt(Function()
    Print("interrupted")
End Function)

Global paused = Scheduler()

Async Function Inner(): Int
    Await Wait(1.Seconds)
    Return 1
End Function

Async Function Outer()
    Local inner = Inner()
    paused.Run(inner)
    Print("never {Await inner}")
End Function

Outer()
Print("done")
```

Expected: done
(then the program ends with exit code 0, as the same program without the OnInterrupt call does)

Actual: done
(then it never ends; `timeout 20 fb run` kills it. Without the OnInterrupt call it prints 'done' and exits 0.)

Suggested fix: Don't count signal-watch threads parked in wait_for_signal as work that could finish a Task. For example, processes_t keeps a count of threads waiting there, and stuck() treats the pool as idle when m_pending equals that count. While any watch exists, a Task given by a signal (fb_signal_task, as TaskSource uses) can stay among the 'coming' ones, so awaiting Ctrl+C through a TaskSource still waits. Add this program as a test that must end.

Skeptic: I ran rv-step17/sig-os/stuck_coro.fb, the finding's program. Debug and --release both print 'done' and then hang until `timeout 20` kills them (exit 124). stuck_coro_nohandler.fb is the same program without OnInterrupt; it prints 'done' and exits 0 in both builds. The cause is in thread.cpp. pool_t::start_thread does m_pending++ for every Thread.Start thread, Watch's handler thread included, and scheduler_t::stuck() returns false whenever !pool().idle(). So fb_scheduler_finish's run_until(empty || stuck) can never finish while a handler thread waits, and run_code_end, which would release that thread, only runs after the loop. This contradicts os_signals.fb's own promise that handlers for signals that never come leave the program ending once its code is done, its coroutines too.

## spawn-2 (medium): On Windows, a program given by name is looked for in the current folder or Folder() before PATH

Status: fixed in 0e8edfa: on Windows, find_program() finds the program itself before uv_spawn, which gets its full path with UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME: a path from the child's folder, and a name alone only in the child's PATH's folders (fb::os::path_entries(), as libuv reads them, with no empty one), with .com and .exe after it, never in a current folder. The CLI test "a program's name is found on PATH, never in the folder it runs in" (a planted whoami, with NoDefaultCurrentDirectoryInExePath unset; ./whoami still finds it), and process_tests' PATH cases

Where: runtime/src/process.cpp:702

options.file is the bare name, and libuv's Windows search_path looks in the cwd before PATH: the child's Folder() when one is set, else this program's current folder. It does this unless the rarely set NoDefaultCurrentDirectoryInExePath variable is present. So Command("git").Folder(repo) runs repo\git.exe if one is there, which is the classic planted-binary hole (Rust stopped searching the cwd in 1.58, Go in 1.19). Linux and macOS (execvp) never search the cwd. That contradicts Command's doc ('found as a path, or by its name on PATH'), process.hpp ('looked up ... on PATH'), and 'the same on every OS'.

```
Import std.core
Import std.os

// As the copy in the folder, it says so.
If Env.Get("FB_PLANTED") != Null
    Print("the whoami.exe in the folder ran")
    Process.Exit(0)
End If

// A copy of this program named whoami.exe, in a folder of its own.
Local self = Folders.Executable.Expect("the program's path")
Local folder = Folders.Temp.Expect("a temp folder") + "\\fb-planted"
Try Command("cmd").Args(["/c", "mkdir", folder]).Output()
Try Command("cmd").Args(["/c", "copy", "/y", self, folder + "\\whoami.exe"]).Output()
Env.Set("FB_PLANTED", "1")

// whoami is on PATH, as C:\Windows\System32\whoami.exe.
Write((Try Command("whoami").Output()).Text)
Write((Try Command("whoami").Folder(folder).Output()).Text)
```

Expected: PATH's whoami twice, e.g.
machine\user
machine\user

Actual: Hunter
the whoami.exe in the folder ran
(the first line is the whoami found first on this shell's PATH; run with NoDefaultCurrentDirectoryInExePath unset, which is Windows' default)

Suggested fix: On Windows, when the program has no path separator, look it up on PATH yourself before uv_spawn: the child's PATH when Env changed it, else this program's. Try the name as given when it has an extension, then with .com and .exe appended, and never the cwd or Folder. Pass the absolute path with UV_PROCESS_WINDOWS_FILE_PATH_EXACT_NAME, and give NOT_FOUND when nothing matches.

Skeptic: The tool's shell sets NoDefaultCurrentDirectoryInExePath=1. With it set, s2.fb printed 'Hunter' twice. With it unset, which is Windows' default, the same program printed 'Hunter' and then 'the whoami.exe in the folder ran': Command("whoami").Folder(folder) ran the planted copy. libuv-src/src/win/process.c search_path checks the cwd before PATH when NeedCurrentDirectoryForExePathW(L"") is true, and that cwd is options.cwd. process.cpp:702 passes the bare name. This contradicts Command's doc ('found as a path, or by its name on PATH') and 'the same on every OS'.

## spawn-3 (medium): What the program printed before a spawn comes out after the child's output when stdout is a pipe or file

Status: fixed in 0e8edfa: fb_os_process_spawn calls fb_flush() before it starts the child; os_process.fb's shared mode, whose Status child prints between its parent's lines through a pipe

Where: runtime/src/process.cpp:846

fb_os_process_spawn starts the child without flushing this program's stdout. Print goes through C stdio, which buffers fully when stdout isn't a terminal (logs, CI, | and >). A child that inherits stdout (Status and Spawn's default) writes straight to the shared handle. So it appears before lines the parent printed earlier. The ordering is wrong in debug and release.

```
Import std.core
Import std.os

Local self = Folders.Executable.Expect("the program's path")
Local arguments = Process.Arguments()
If arguments.Count > 0 && arguments[0] == "child"
    Print("child")
ElseIf arguments.Count > 0 && arguments[0] == "parent"
    // Its stdout is a pipe, as in a log or a CI job, and the child shares it.
    Print("parent before")
    Print(Try Command(self).Arg("child").Status())
    Print("parent after")
Else
    Write((Try Command(self).Arg("parent").Output()).Text)
End If
```

Expected: parent before
child
exit code 0
parent after

Actual: child
parent before
exit code 0
parent after

Suggested fix: Flush the runtime's stdout and stderr on the calling thread (under the output lock Print uses, as fb_exit does) before posting the spawn, at least when any stream is INHERIT. Add the case above to os_process.fb.

Skeptic: Ran s3.fb with the parent's stdout a pipe (Output). Debug printed 'child / parent before / exit code 0 / parent after'. Release printed the same. So the parent's earlier Print comes out after the inheriting child's output. fb_os_process_spawn (process.cpp:800-855) never flushes stdout or stderr before uv_spawn. The rest of the runtime keeps output in order on purpose: fb_write flushes stdout before writing stderr, and panics and fb_exit flush.

## spawn-4 (medium): On Windows a child still running is killed when the program ends, unlike on Linux and macOS and unlike the docs

Status: fixed in 0e8edfa: third_party/CMakeLists.txt changes libuv's win/process.c, as it was fetched, so uv_spawn no longer puts a child in its kill-on-close job (`if (0 && ...)`, checked at configure time); os_process.fb's leaver mode, whose child prints after its parent ends

Where: runtime/src/process.cpp:419

libuv's Windows uv_spawn puts every child not started with UV_PROCESS_DETACHED into a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, and the job handle closes when this program exits. So any child still running then is terminated, after a normal end or after Process.Exit. On Linux and macOS it keeps running. Several docs say otherwise: Child's doc ('runs beside this one until it ends, whether or not this program waits for it ... doesn't end it'), process.hpp's fb_os_process_release ('nothing kills it'), and close_all's comment ('A child that still runs goes on running').

```
Import std.core
Import std.os
Import std.time

Local self = Folders.Executable.Expect("the program's path")
Local arguments = Process.Arguments()
If arguments.Count > 0 && arguments[0] == "child"
    Sleep(2.Seconds)
    Print("the child ran on")
ElseIf arguments.Count > 0 && arguments[0] == "parent"
    // Starts the child, which shares this stdout, and ends without waiting for it.
    Local child = Try Command(self).Arg("child").Spawn()
    Print("the parent ends")
Else
    // Reads the parent's stdout to its end, which comes once the child is done with it too.
    Write((Try Command(self).Arg("parent").Output()).Text)
End If
```

Expected: the parent ends
the child ran on
(as on Linux and macOS)

Actual: the parent ends

Suggested fix: UV_PROCESS_DETACHED isn't a fix: it also adds DETACHED_PROCESS and a new process group, so the child loses the console and Ctrl+C. Either patch libuv (a FetchContent PATCH_COMMAND) to leave children out of its job, or say in Child's doc, process.hpp, close_all's comment and stdlib.md that on Windows a child still running ends when the program does.

Skeptic: s4.fb printed only 'the parent ends', and the top-level Output returned after 1.2 s, not the child's 2 s. To rule out other causes, s4b.exe, run directly from bash, spawns a child that sleeps 2 s and then writes a mark file; after waiting 4 s there was no mark. As a control, s4c.exe does the same but the parent sleeps 3 s, and its mark file was written. libuv's win/process.c puts every non-detached child into a job with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (lines 69-96, 1085-1087), so the child dies when the parent exits. That contradicts Child's doc ('runs beside this one until it ends, whether or not this program waits for it ... doesn't end it'), process.hpp ('nothing kills it') and close_all's comment.

## spawn-6 (medium): OnInterrupt or OnTerminate called after the program's code is done makes the program hang at its end

Status: fixed in 6405b3a, with signals-os-1, which it is: os_signals_late.fb is the finding's program

Where: runtime/src/process.cpp:253

watch() registers end_signal_waits with at_code_end, which only fb_scheduler_finish's single run_code_end ever runs. A Thread.Start thread can keep running after the top-level code and coroutines are done, which is the spec's pattern for long-lived work. If it calls OnInterrupt for the first time then, the registration comes after run_code_end has run, so m_code_ended is never set. The handler's own Thread.Start thread then waits in fb_os_signal_wait forever, and fb_pool_finish waits forever for that thread.

```
Import std.core
Import std.os
Import std.time

Thread.Start(Function()
    Sleep(300.Milliseconds)
    Process.OnInterrupt(Function()
        Print("interrupted")
    End Function)
    Print("worker done")
End Function)
Print("main done")
```

Expected: main done
worker done
(then it ends with code 0, as it does when OnInterrupt comes before main's code ends)

Actual: main done
worker done
(then it never ends; killed by a 10 s timeout, exit 124)

Suggested fix: Have run_code_end set an atomic flag before it runs the enders. Have at_code_end, after storing `done`, call done() itself when that flag is set (end_signals is idempotent, and start_watching already returns once m_code_ended is set). Or have watch() check the flag first. Add the program above as a test.

Skeptic: In s6.fb a Thread.Start thread calls OnInterrupt 300 ms after main's code ends. It printed 'main done / worker done' and then hung until timeout 10 killed it (exit 124). The release build hung the same way. The control, s6b.fb, calls OnInterrupt before main ends, and it exits 0. Reading the code: run_code_end (runtime.cpp:246) runs once from fb_scheduler_finish. watch() registers at_code_end afterwards, and nothing then calls end_signals, so m_code_ended stays false. The handler thread waits forever in wait_for_signal, and fb_pool_finish waits for that thread.

## std-2 (medium): On Windows, a program that wrote to a child's stdin that the child never read can't end until the child does

Status: fixed in 0e8edfa: close_input() no longer calls uv_shutdown, which on Windows waits for the child to read the pipe; it closes stdin with uv_close once the writes under way are done (stream_t::writes); os_process.fb's writer mode, which leaves a line in a deaf child's stdin, ends in under 3 seconds

Where: runtime/src/process.cpp:532

close_input closes stdin with uv_shutdown, both from ChildInput.Close and from fb_os_process_release when the last holder lets go. On a Windows pipe, libuv's uv__pipe_shutdown runs FlushFileBuffers on a thread-pool thread whenever the pipe still holds bytes. FlushFileBuffers returns only once the child has read every byte or has ended. At program end, close_all calls uv_close on that pipe, but the close waits for the pending shutdown, so uv_run in the process thread doesn't return, and processes_t::stop's join blocks fb_finish. So the program hangs at exit for as long as the child runs, which is forever for a server-like child. If nothing was written, the shutdown takes libuv's short path and the program ends at once: the same program without the WriteText ended in 0.05 s. The child is supposed to run on alone ('A child that still runs goes on running', and letting go of a Child doesn't end it). Rust just closes the handle.

```
Import std.core
Import std.os
Import std.time

Local arguments = Process.Arguments()
If arguments.Count > 0
    // Never reads its stdin, and runs for 20 seconds.
    Sleep(20.Seconds)
    Process.Exit(0)
End If

// The parent ends at once, and the child goes on running, as letting go of a Child does.
Local child = Try Command(Folders.Executable.Expect("self")).Arg("child").Stdin(Stdio.PIPED).Spawn()
Local into = child.Stdin.Expect("stdin")
Try into.WriteText("unread\n")
into.Close()
Print("code done at", Instant.Now().Nanoseconds / 1000000000)
```

Expected: The parent prints its line and its process exits within a fraction of a second, while the child goes on sleeping.

Actual: The parent prints its line but its process exits only after 20.07 s, when the child ends (timed with the parent's own WaitForExit). Without the WriteText, the same program exits in 0.05 s.

Suggested fix: Close stdin with uv_close once its in-flight writes have completed, not with uv_shutdown, at least on Windows. Keep a count of pending writes on the stream_t. Close and release set closing, and call close_stream right away when the count is 0, or from the last write's callback. Bytes already in the pipe stay readable by the child after the parent's handle closes. Add a test that a parent with unread bytes in a sleeping child's stdin ends promptly, for example a cli test with a timeout.

Skeptic: Built rv-step17/std-r/s2.fb (WriteText then Close), s2c.fb (WriteText, never closed, so letting go of the Child closes stdin), and s2b.fb (Close with nothing written), each in debug and release, with a child that sleeps 20 s with stdout and stderr NONE. Timing each parent with Start-Process -PassThru and WaitForExit: s2 20.05 s, s2r 20.04 s, s2c 20.04 s, s2cr 20.22 s, but s2b 0.054 s and s2br 0.022 s. All of them print 'code done' right away. Cause: close_input (process.cpp:532) calls uv_shutdown. libuv's win/pipe.c uv__pipe_shutdown skips the flush only when OutboundQuota == WriteQuotaAvailable; otherwise it queues FlushFileBuffers (pipe.c:585), which blocks until the child reads the bytes or ends, and close_all and stop's join wait behind it. That contradicts the lane's own rule that letting go of a Child leaves it running, and close_all's comment that 'A child that still runs goes on running': the parent can't exit.

## signals-os-5 (low): On POSIX, a signal the program started ignoring kills it once the code is done

Status: fixed in 0e8edfa: on Linux and macOS, start_watching() doesn't watch a signal the program started out ignoring (sigaction), so it stays ignored, with a handler and after; the doc comments say so. The CLI test "a signal a program started out ignoring stays ignored, with a handler or after" (POSIX only: SIGHUP while the code runs and once it's done)

Where: runtime/src/process.cpp:378

close_watchers runs at the code's end and calls uv_signal_stop and uv_close. libuv then sets the signal to SIG_DFL (uv__signal_unregister_handler, libuv-src/src/unix/signal.c:249), not to whatever the program started with. A program started with SIGHUP ignored (nohup) or SIGINT ignored (a background job in a non-interactive shell) that calls OnTerminate or OnInterrupt is then killed by that signal while pool finish waits for its threads or the Globals are let go of. Without the handler it would have gone on. I couldn't run fb on Linux here. To show the mechanism, I built libuv 1.52.1 from the build tree's _deps on WSL Ubuntu and ran the same calls start_watching and close_watchers make (signal(SIGHUP, SIG_IGN); uv_signal_init/start(SIGHUP); uv_signal_stop; uv_close; raise(SIGHUP)). Without the watcher it printed 'SIGHUP is ignored / still running after SIGHUP' and exited 0. With it, it printed 'SIGHUP is at its default' and died: 'Hangup', exit 129.

```
// Run on Linux or macOS as: nohup ./ignored_hup > out.txt & sleep 1; kill -HUP $!
// nohup starts it with SIGHUP ignored; the SIGHUP comes once the code is done, while the thread
// still works.
Import std.core
Import std.os
Import std.time

Process.OnTerminate(Function()
    Print("terminated")
End Function)
Thread.Start(Function()
    Sleep(3.Seconds)
    Print("the thread's work is done")
End Function)
Print("done")
```

Expected: done
the thread's work is done
(exit 0: SIGHUP stays ignored once the handler is gone, as it is without OnTerminate)

Actual: By libuv's code, shown on WSL with the same calls: SIGHUP is at SIG_DFL after close_watchers, so the program dies of SIGHUP (exit 129) before the thread's line. The .fb program itself was only type-checked here, not run on Linux.

Suggested fix: In start_watching, read each signal's disposition with sigaction first. In close_watchers, after uv_signal_stop, put SIG_IGN back for the ones that started ignored (POSIX only), or don't watch a signal that started ignored. Test it in cli_tests on POSIX with the signal ignored in the spawn attributes.

Skeptic: In the build tree's libuv-src/src/unix/signal.c, uv__signal_unregister_handler sets sa_handler = SIG_DFL (line 249), and uv__signal_register_handler has the comment '/* XXX save old action so we can restore it later on? */'. On WSL Ubuntu I built that libuv with gcc and ran rv-step17/sig-os/wsl/ign.c. It ignores SIGHUP, then makes the same calls start_watching and close_watchers make: uv_signal_init/start, then uv_signal_stop and uv_close. Without the watcher it printed 'start: SIGHUP is ignored / still running after SIGHUP' and exited 0. With it, it printed 'watching: SIGHUP is handled', then 'closed: SIGHUP is default', and died with 'Hangup'. close_watchers (process.cpp:378) runs at the code's end, so a program started under nohup, or as a background job with SIGINT ignored, that installs OnTerminate or OnInterrupt can be killed by that signal while pool finish or the Globals' release runs. Without the handler, the signal stays ignored. Saying the signal ends the program 'again' implies a return to the starting disposition. The window is narrow and POSIX-only, hence low.

## signals-os-6 (low): No test checks that signals end the program again after its code, or the extra call for signals during a handler

Status: fixed in 0e8edfa: the CLI signal tests share drive(), which builds a program, starts it in a process group of its own, and sends signals after what it prints; "a signal ends a program again once its code is done, while a thread runs on" and "a handler runs once more for the signals that come while it runs" (three signals during a run make one more), and the first test counts two 'interrupted' lines on Windows

Where: tests/cli/cli_tests.cpp:890

The CLI test only sends signals while the code is still waiting in Receive. tests/programs/os_signals.fb sends none. Two of the lane's documented behaviors are never checked. (1) Once the code is done, the waits return and Ctrl+C, SIGINT, or SIGTERM ends the program again. (2) A handler runs one call at a time, and once more for the signals that came while it ran. Both work on Windows: I checked them by driving built programs with Ctrl+Break from a new process group. after_end gives exit 0xC000013A. Three breaks during a 500 ms handler give 'runs: 2'. A regression in either would pass every test. Also, on Windows the test's last two CHECKs look for the same 'interrupted\n' twice, so only the exit code shows the second break was handled.

```
// The code has a handler and ends, but a thread runs on: a Ctrl+Break then ends the program.
// (Driven by a parent that sends Ctrl+Break to its process group when it reads 'send' on stderr.)
Import std.core
Import std.os
Import std.time

Process.OnInterrupt(Function()
    Print("interrupted")
End Function)
Thread.Start(Function()
    Sleep(300.Milliseconds)
    PrintError("send")
    Sleep(3.Seconds)
    Print("the thread ran to its end")
End Function)
Print("done")
```

Expected: A test that checks: 'done', 'send', no 'interrupted' and no 'the thread ran to its end', and an exit by the signal (0xC000013A on Windows, SIGINT or SIGTERM on POSIX). Plus one that sends several signals during a slow handler and checks that it ran exactly twice.

Actual: The behavior is right today (done / send, exit 0xc000013a; coalesce: runs: 2), but no test in tests/cli or tests/programs checks either.

Suggested fix: Extend the CLI signal test, or add a second one: after the second signal, let the code end while a Thread.Start thread sleeps, send one more signal, and check that the program ended by it. Add a case with a slow handler that gets several signals and counts its runs. On Windows, make the second CHECK look for two 'interrupted' lines.

Skeptic: grep finds OnInterrupt/OnTerminate tested only in tests/cli/cli_tests.cpp:890 and tests/programs/os_signals.fb. The CLI test sends signals only while the code waits in Receive, and os_signals.fb sends none. Neither checks that a signal ends the program once its code is done, or that a handler runs only once more for signals that came while it ran. I checked both behaviors with a Ctrl+Break driver (rv-step17/sig-os/driver.exe). after_end.fb gave 'done', 'send', exit 0xc000013a. coalesce.fb, sent 3 breaks during a 500 ms handler, gave 'runs: 2', exit 0. So the behavior is correct today; this is only a coverage gap, and a regression would pass every test. The Windows branch's `second` is 'interrupted\n', the same string as the first CHECK, so only exit_code == 0 shows the second break was handled.

## spawn-5 (low): Windows variable names match the program's environment ignoring case only for ASCII

Status: fixed in 0e8edfa: on Windows, same_variable() compares names as UTF-16 with CompareStringOrdinal(..., TRUE), as Windows and libuv do; os_process.fb's unicode mode, with Env and Unset of äfb_case beside the parent's ÄFB_CASE

Where: runtime/src/process.cpp:610

same_variable lowercases only A-Z. Windows compares environment names case-insensitively over all of Unicode (Env.Get("fb_äö") finds FB_ÄÖ), and so does libuv's own env_strncmp, which uses CompareStringOrdinal with ignoreCase. So Command.Env("fb_äö", ...) or Unset("fb_äö") leaves the parent's FB_ÄÖ in the child's block next to the new entry. The child then reads the parent's value. This contradicts 'On Windows a name is the same in any case'.

```
Import std.core
Import std.os

Local arguments = Process.Arguments()
If arguments.Count > 0
    Print(Env.Get("FB_ÄÖ") ?? "Null", Env.Get("fb_äö") ?? "Null")
    Local all = Env.All()
    Print(all.Contains("FB_ÄÖ"), all.Contains("fb_äö"))
    Process.Exit(0)
End If

Env.Set("FB_ÄÖ", "upper")
Print(Env.Get("fb_äö") ?? "Null")
Local self = Folders.Executable.Expect("path")
Write((Try Command(self).Arg("x").Env("fb_äö", "lower").Output()).Text)
Write((Try Command(self).Arg("x").Unset("fb_äö").Output()).Text)
```

Expected: upper
lower lower
False True
Null Null
False False

Actual: upper
upper upper
True True
upper upper
True False

Suggested fix: On Windows, compare the names as UTF-16 with CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL, as libuv's make_program_env does. Add a non-ASCII case to os_process.fb.

Skeptic: Fixed the program so the child prints the names it is given, then ran s5.fb. The parent's Env.Get("fb_äö") gives 'upper', because Windows matches the case. With Env("fb_äö","lower"), the child printed 'upper upper / True True': both entries are in its block and Windows takes the parent's. With Unset("fb_äö"), it printed 'upper upper / True False'. The ASCII control works: FB_AO/fb_ao gave 'lower lower / False True' and 'Null Null / False False'. same_variable (process.cpp:610-617) lowercases only A-Z, which contradicts 'On Windows a name is the same in any case'. Low because non-ASCII variable names are rare.

## spawn-8 (low): process.hpp says a NUL in any text gives failure 1, but a NUL in a variable's name gives 2

Status: fixed in 0e8edfa: fb_os_process_spawn's comment in os_process.hpp (renamed from process.hpp, apart from the compiler's) says 1 is a NUL in the program, an argument, a value, or the folder, 2 a name that's empty or has an '=' or a NUL, and 3 a batch file's argument with a line break; the comment has no test

Where: runtime/include/fb/process.hpp:25

fb_os_process_spawn's comment says failure is '1 there when a text has a NUL, which none can, or 2 when a name is empty or has an '='. The code checks names first and gives 2 for a name with a NUL (change.first.contains('=') || has_nul(change.first)), so Command panics with the names message. The panic is right; the header comment isn't.

```
Import std.core
Import std.os

Local out = Try Command("cmd").Env("A\0B", "v").Output()
```

Expected: The header to describe failure 2 as a name that's empty or has an '=' or a NUL (what the code and the panic do)

Actual: panic: a Command takes variables' names that aren't empty and have no '=' or NUL at namenul.fb:4:17 (failure 2), while the header puts every NUL under 1

Suggested fix: Reword the comment: '... or 1 there when the program, an argument, a value, or the folder has a NUL, or 2 when a name is empty or has an '=' or a NUL, and nothing starts.'

Skeptic: Ran s8.fb, which calls Command("cmd").Env("A\0B","v").Output(). It printed 'panic: a Command takes variables' names that aren't empty and have no '=' or NUL at s8.fb:4:17' (exit 101), so it took failure 2. process.cpp:833-838 checks names, NUL included, before the failure-1 NUL check. process.hpp:25-26 says '1 there when a text has a NUL ... or 2 when a name is empty or has an '='', which puts a NUL in a name under 1. The behavior is right; only the header comment is wrong.

## std-3 (low): A write to a child that ended mid-write gives UNEXPECTED_END "end of file" on Windows, but OTHER "broken pipe" otherwise

Status: fixed in 0e8edfa: write_error() turns libuv's UV_EOF for a write, Windows' ERROR_BROKEN_PIPE, into UV_EPIPE, in the write's callback and when uv_write fails at once; os_process.fb writes 2,000,000 bytes to a child that reads one line, OTHER broken pipe during and after

Where: runtime/src/process.cpp:770

libuv's Windows uv_translate_sys_error maps ERROR_BROKEN_PIPE to UV_EOF. When the child ends while a uv_write to its stdin is in flight, the write callback gets UV_EOF. write_input passes that on, and fb_os_error_kind turns UV_EOF into UNEXPECTED_END, with the text "end of file". UNEXPECTED_END is the reading kind (a stream that ended too soon), not a write's. A write to the same gone child a moment later gets UV_EPIPE, which is OTHER "broken pipe", and so does every such write on Linux and macOS. So one failure gets a different Kind depending on the OS and on timing, and code that catches broken pipes by Kind misses it. os_process.fb only tests writing after Close.

```
Import std.core
Import std.io
Import std.os

Local arguments = Process.Arguments()
If arguments.Count > 0
    // Reads one line of its stdin, and ends.
    Print(Input() ?? "none")
    Process.Exit(0)
End If

Local self = Folders.Executable.Expect("self")

// Writing more than the child reads: the write fails once the child has gone.
Local child = Try Command(self).Arg("one").Stdin(Stdio.PIPED).Stdout(Stdio.NONE).Spawn()
Local into = child.Stdin.Expect("stdin")
Local bytes: UInt8[] = []
For i = 0 Until 2000000
    bytes.Add(i % 10 == 9 ? 10 : 65)
End For
Try
    Try into.WriteBytes(bytes)
Catch e As IOError
    Print("during:", e.Kind, e.Message)
End Try

// The same child gone, written to again.
Try
    Try into.WriteText("again\n")
Catch e As IOError
    Print("after:", e.Kind, e.Message)
End Try
```

Expected: during: OTHER broken pipe
after: OTHER broken pipe

Actual: during: UNEXPECTED_END end of file
after: OTHER broken pipe
(On Windows, every run, debug and --release.)

Suggested fix: In write_input, change a UV_EOF status to UV_EPIPE before answering, both in the uv_write callback and on the immediate-error path, so a write to a gone child is OTHER "broken pipe" on every OS. Add the during-write case to os_process.fb.

Skeptic: Ran rv-step17/std-r/s3.fb: the child reads one line and exits while the parent writes 2,000,000 bytes, then the parent writes again. Debug, release, and three more runs of the built exe all print: 'during: UNEXPECTED_END end of file' / 'after: OTHER broken pipe'. libuv's win/error.c maps ERROR_BROKEN_PIPE to UV_EOF in uv_translate_sys_error (line 157). write_input (process.cpp:770 and 774) passes the status on unchanged, and fb_os_error_kind maps UV_EOF to UNEXPECTED_END. So on Windows the same failure, a write to a child that has gone, has two Kinds depending on timing, and an 'end of file' message on a write. POSIX gives EPIPE, which is OTHER. std is meant to behave the same on every OS. Low, because the docs don't name the Kind for this case.

## std-4 (low): Output's drained stderr is handed out by erasing from the front of one string, which is quadratic in its size

Status: fixed in 0e8edfa: a stream's held bytes are a fb::os::byte_queue_t, which takes from an offset and moves what's left to the front only once more is taken than is left; process_tests' "taking a child's held bytes takes time in proportion to them" (128 MiB, 64 KiB at a time, under 2 seconds; erasing from the front each time took far longer, checked)

Where: runtime/src/process.cpp:507

Output and OutputAsync drain stderr into stream_t::held while they read stdout. ReadAll then takes held 64 KiB at a time, and each read in deliver() does held.erase(0, count), which moves everything left in the buffer. For a drained stream holding N bytes, that is N/65536 moves averaging N/2 bytes, so the time grows with N squared, and it all runs on the one process thread, so every other child's I/O and the signal counting wait behind it. The same loop in C++ (-O2) takes 0.94 s for 50 MB, 3.39 s for 100 MB and 13.87 s for 200 MB, about 4x per doubling, so 1 GB of stderr would spend several minutes in memmove. In fb, Output of 200 MB to stderr takes 66 s against 49 s for the same 200 MB to stdout. Most of both times is ReadAll's per-byte Add, which is step 13's code, not this step's. The extra ~17 s is the erase.

```
Import std.core
Import std.os
Import std.time

Local arguments = Process.Arguments()
If arguments.Count > 0
    // 200 MB to one stream: stderr, or stdout.
    Local chunk = "0123456789"
    For i = 0 Until 17
        chunk += chunk
    End For
    For i = 0 Until 160
        If arguments[0] == "err" Then WriteError(chunk) Else Write(chunk)
    End For
    Process.Exit(0)
End If

Local self = Folders.Executable.Expect("self")
Local start = Instant.Now()
Local out = Try Command(self).Arg("out").Output()
Print("stdout", out.Stdout.Count, (Instant.Now() - start).TotalSeconds)
start = Instant.Now()
Local err = Try Command(self).Arg("err").Output()
Print("stderr", err.Stderr.Count, (Instant.Now() - start).TotalSeconds)
```

Expected: stderr takes about as long as stdout, since both carry the same bytes and handing them out should be linear.

Actual: stdout 209715200 49.0297072
stderr 209715200 66.3816024
(--release. deliver()'s erase-from-front loop alone takes 0.94 / 3.39 / 13.87 s for 50 / 100 / 200 MB.)

Suggested fix: Hand out held bytes from a read offset (std::size_t taken) instead of erasing from the front, and compact only when the offset passes half of held's size or held empties. Or keep held as a deque of chunks.

Skeptic: deliver() (process.cpp:504-507) takes at most read.count bytes (ReadAll's ChunkSize, 65536, std/io/streams.fb:73) and erases them from the front of stream.held. A stderr that Output drained holds the child's whole stderr, so handing it out costs O(N^2/65536). A C++ copy of the loop (-O2, MinGW, run alone) took 0.214 / 0.846 / 3.354 s for 25 / 50 / 100 MB, about 4x per doubling, all of it on the process thread. My fb run of Output with stdout vs stderr at 50/100/200 MB in release (rv-step17/std-r/s4.fb) gave extra stderr time of 1.9 / 17.9 / 3.4 s: too noisy to show the term, because ReadAll's per-byte Add dominates. So the quadratic cost is real in code under review, but it only matters for output of hundreds of MB. Hence low.

## std-5 (low): On Windows, Command.Env with a non-ASCII name gives the child two variables instead of replacing this program's

Status: fixed in 0e8edfa, with spawn-5, which it is: the same unicode case checks the child has one variable for both names on Windows

Where: runtime/src/process.cpp:610

same_variable folds only A-Z when it compares environment names on Windows. Windows itself, and libuv, compare names case-insensitively across Unicode (CompareStringOrdinal with ignore-case, which is what Env.Get in the parent uses). With the parent's ÄFB_CASE set, Command.Env("äfb_case", ...) doesn't remove it, and the child's environment block holds both entries. Command.Env's comment says 'On Windows a name is the same in any case, so "Path" is "PATH"'. The child's Env.All shows two variables for one name, and which value a lookup finds depends on libuv's unstable qsort of the block.

```
Import std.core
Import std.os

Local arguments = Process.Arguments()
If arguments.Count > 0
    Print(Env.Get("ÄFB_CASE") ?? "Null", Env.Get("äfb_case") ?? "Null")
    For name In Env.All().Keys()
        If name == "ÄFB_CASE" || name == "äfb_case" Then Print("has {name}")
    End For
    Process.Exit(0)
End If

Local self = Folders.Executable.Expect("self")

// On Windows a variable's name is the same in any case, so the Command's value replaces this
// program's.
Env.Set("ÄFB_CASE", "upper")
Print(Env.Get("äfb_case") ?? "Null")
Local out = Try Command(self).Arg("x").Env("äfb_case", "lower").Output()
Write(out.Text)
```

Expected: upper
lower lower
has äfb_case

Actual: upper
lower lower
has äfb_case
has ÄFB_CASE

Suggested fix: On Windows, compare names the way the OS does: convert both to UTF-16 and use CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL, as libuv's own environment code does. Add a non-ASCII case to os_process.fb's case test.

Skeptic: Ran rv-step17/std-r/s5.fb in debug and release. After Env.Set("ÄFB_CASE","upper"), the parent's Env.Get("äfb_case") gives 'upper', so the OS treats the two names as one variable. Command.Env("äfb_case","lower") gave the child 'lower lower', but its Env.All() lists both 'äfb_case' and 'ÄFB_CASE'. Worse, Command.Unset("äfb_case") leaves the child with ÄFB_CASE=upper ('upper upper / has ÄFB_CASE'). same_variable (process.cpp:610-617) folds only A-Z. That contradicts Command.Env's comment (process_command.fb:86, 'On Windows a name is the same in any case') and the parent's own Env.Get. Low, because non-ASCII variable names are rare.

## Refuted by the skeptics (no action)

- spawn-7: No test writes to a child that has ended (the SIGPIPE block) or checks that the child runs in Folder(). The claims about the tests are accurate: os_process.fb's only failing write is after Close, which write_input refuses before touching the pipe, and the Folder test (line 133) prints only the status. But there is no wrong behavior. s7.fb on Windows printed 'exit code 0 / IOError OTHER / True / still running', which is right. Reading the POSIX path: uv_write runs on the loop thread, which blocks SIGPIPE (process.cpp:339-346), so the thread-directed SIGPIPE can't end the program. This is a test-coverage request, not a bug in the code or a contradiction of the docs.
