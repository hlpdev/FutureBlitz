# The review of M6 step 18's rest: Lazy, the Parallel algorithms, Event.Subscribe(owner, handler), ThrowingTaskSource and throwing WhenAll/WhenAny, Cancellation and its waits, Selector over Channels, std.log, and two small compiler additions (ch.IsClosed(), ch.TryReceive(): T?)

8 confirmed findings: 0 high, 2 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## concurrent-1 (medium): ParallelSort panics on an array of a nullable type when a Null lands in a right-hand part

Status: fixed in 260dd4a2: Merged merges by index, so no item goes through a T?; concurrent_parallel.fb ParallelSorts 20000 Int? items with Nulls among them against Sort

Where: std/concurrent/parallel.fb:131

Merged reads each part through an iterator, so x and y are T? (Int?? for an Int?[]). In the Else branch the condition `y == Null || ...` has failed, so y is narrowed to T. When T is itself optional (Int?), `y.Expect(...)` then resolves to the inner Int?'s Expect, which panics on a Null element. The If branch's x isn't narrowed, because the condition is a disjunction, so only the right part breaks. Any ParallelSort(items, before) over a T? array with 4096 or more items and a Null in a right part of some merge panics, though items.Sort(before) sorts the same array fine. This happens in both debug and release builds. concurrent_parallel.fb never sorts a nullable element type.

```
Import std.core
Import std.collections
Import std.concurrent

// ParallelSort of 4096 Int? items, with a Null among them, by a before that puts Null first.
Local items: Int?[] = []
For i = 0 Until 4096
    items.Add(4096 - i)
End For
items[4095] = Null
Local expected = items.Clone()
expected.Sort(Function(a: Int?, b: Int?) => (a ?? -1) < (b ?? -1))
ParallelSort(items, Function(a: Int?, b: Int?) => (a ?? -1) < (b ?? -1))
Print(items[0] == Null, items[1] ?? -1, items[4095] ?? -1, expected[1] ?? -1)
```

Expected: True 1 4096 1

Actual: panic: the right part has a value here at std/concurrent/parallel.fb:131:24 (exit code 101, debug and --release alike)

Suggested fix: Stop going through T? in Merged. Merge by index instead (i < first.Count, j < second.Count, take first[i] unless before(second[j], first[i])) so no element is ever wrapped and unwrapped. Add a test to concurrent_parallel.fb that ParallelSorts 4096 or more Int? items with Nulls in them and compares against Sort(before).

Skeptic: The suggested build-rev2/cli/fb.exe is stale: it was built at 18:17, but the rev-crypto sources were checked out at 20:11, and it fails to compile std/concurrent/channels.fb (no IsClosed or TryReceive(): T?). So I used C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/build-lane6-verify/cli/fb.exe instead. It was built from lane6-verify, which is at 33e24764, and its std, compiler and runtime have no diff against 33e24764. The finding's program (rv-step18b/v2/p1.fb) printed 'panic: the right part has a value here at std/concurrent/parallel.fb:131:24' and exited 101 in both debug and --release. The program's expected line has a small mistake: index 4095 holds 1 before it's set to Null, so expected[1] is 2, not 1. That doesn't affect the point. A control (p2.fb) showed items.Sort(before) on the same 4096 Int? items working ('True 2 4096'), and ParallelSort of 3 Int? items, which falls back to Sort, working too ('True 1 3'). The code matches the finding: in Merged (std/concurrent/parallel.fb:117-135), x and y are T? from Iterate().Next(). In the Else branch, y is narrowed by the failed `y == Null ||` test, so for T = Int? y.Expect is the inner Int?'s Expect and panics on a Null element. ParallelSort accepts any Sendable T, and Int? is one, so this is a real crash on valid input.

## log-1 (medium): Logger.With rejects most integer types: UInt8, Int16, and UInt32 are ambiguous, and UInt64 has no overload

Status: fixed in 6dc0e8da: With has an overload for each of the eight integer types; log_output.fb logs the widest of each, UInt64's 18446744073709551615 too

Where: std/log/logger.fb:230

docs/stdlib.md says With(name, value) takes "a String, an integer, a Double, or a Bool", but only Int (32 bits) and Int64 overloads exist beside Float64. A UInt8, Int8, Int16, UInt16, or UInt32 argument widens equally well to Int, Int64, and Float64, so the call is ambiguous, and a UInt64 matches no overload. Int32 is the only narrow type that compiles. (Note for the run: build-rev2's fb.exe is from 5cb051e8 and has no runtime/src/log.cpp, so every std.log program fails to link there with undefined fb_log_setting and fb_log_colors. I ran everything with C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/build-lane6-verify/cli/fb.exe, which was built at 33e24764 and passes log_filter and log_output in debug and release.)

```
Import std.core
Import std.collections
Import std.time
Import std.log

Const o = LogOutput(LogFilter.At(LogLevel.INFO))
Local id: UInt32 = 4000000000
Local big: UInt64 = 18446744073709551615
Local l = Logger("m", o).With("id", id).With("big", big)
Print(JsonLine(LogRecord(Timestamp.FromUnixMilliseconds(0), LogLevel.INFO, "m", "x", l.Fields)))
```

Expected: {"time":"1970-01-01T00:00:00.000Z","level":"info","module":"m","message":"x","id":4000000000,"big":18446744073709551615}

Actual: error: the call to 'With' is ambiguous (could be With(String, Int64) or With(String, Float64)) for the UInt32. With a UInt64: error: no overload of 'With' takes (String, UInt64). A UInt8 or Int16 is ambiguous between the Int, Int64, and Float64 overloads.

Suggested fix: Add an overload for each integer type, or at least UInt64 plus overloads that make the narrow types resolve (e.g. Int8/Int16/UInt8/UInt16/UInt32 widening to Int64), each writing "{value}" as a number. Add a log_output case for each type.

Skeptic: I used build-lane6-verify's fb.exe, built at 33e24764, because build-rev2's has no log.cpp. A UInt32 passed to With fails with "the call to 'With' is ambiguous", with notes pointing at the Int64 overload (logger.fb:88). A UInt64 fails with "no overload of 'With' takes (String, UInt64)". UInt8 and Int16 fail as ambiguous too, with notes pointing at the Int overload (logger.fb:84). std/log/logger.fb:80-108 has With overloads for String, Int, Int64, Float64 and Bool only. docs/stdlib.md's std.log bullet says With takes "a String, an integer, a Double, or a Bool", so the code contradicts the doc for most integer types.

## channels-1 (low): TryReceive's new overload loses the 'write ByRef' hint, and spec 10's own example doesn't compile

Status: fixed in 08cfce10: when no overload fits, the one that would fit but for a ByRef the call leaves off or adds is the one meant, so its call says to write or remove ByRef, as a lone function's does; sema tests for TryReceive(value), and for an overloaded function either way; spec 10's example reads `ch.TryReceive(ByRef pkt)`

Where: compiler/src/sema.cpp:11315

Adding the no-argument TryReceive() overload turned ch.TryReceive into an overload set. When the ByRef is left off, the error no longer says what to fix. The pre-change compiler (build-rev2's fb.exe, at 5cb051e8) said "parameter 'value' is ByRef; write 'ByRef pkt'". 33e24764 only says "no overload of 'TryReceive' takes (Int32)". The Communication example in docs/spec/10-concurrency.md (line 242, `While ch.TryReceive(pkt)`) is just above the Channel list this change edits. It leaves the ByRef off, so it hits exactly this error.

```
Import std.core
Const ch = Channel<Int>(4)
ch.Send(1)
Local pkt = 0
While ch.TryReceive(pkt)
    Print(pkt)
End While
```

Expected: t4.fb:5:21: error: parameter 'value' is ByRef; write 'ByRef pkt' (what the compiler said before this change), and spec 10's example should read `While ch.TryReceive(ByRef pkt)`

Actual: t4.fb:5:7: error: no overload of 'TryReceive' takes (Int32)

Suggested fix: When no overload matches by arity and types, but one would match if a plain variable argument were passed ByRef, give the ByRef hint as before. A Channel-specific check in channel_methods' call path also works. Add a sema test for it, and change spec 10's example to `ch.TryReceive(ByRef pkt)`.

Skeptic: build-rev2's fb.exe turned out to be stale: it was built at 18:17, before rev-crypto moved to 33e24764, and it fails on std/concurrent/channels.fb. So I used the scratchpad's b18c/cli/fb.exe, which is configured from rev-crypto and was built at 20:20, after the commit. The finding's program, debug and release: the stale build says "t1.fb:5:21: error: parameter 'value' is ByRef; write 'ByRef pkt'", and 33e24764 says "t1.fb:5:7: error: no overload of 'TryReceive' takes (Int32)". The cause is the second method("TryReceive", nullable(element), {}) in sema.cpp's channel_methods (around line 11315), which makes TryReceive an overload set, so the ByRef hint at sema.cpp:22928 is never reached. Spec 10:242 still reads `While ch.TryReceive(pkt)`, which hits this error. Its missing ByRef predates the change, but the diff edits the Channel list right below it, and the regressed message no longer says how to fix it.

## channels-2 (low): concurrent_channels.fb never cancels a Receive or Send while it waits

Status: fixed in 205d575d: the thread that receives, and the one that sends to a full Channel, say they're about to wait, and the Cancel comes 20 ms after, so each is waiting by then; the output says neither found it cancelled before

Where: tests/programs/concurrent_channels.fb:82

Receiving() and Sending() start the waiting Receive/Send on another thread (Thread.Start / Spawn) and call cancel.Cancel() right after, with no wait between. So the receiver always finds the Cancellation already set when Receive begins, and takes the 'before' path. The 'while it waits' path is never run: fb_pool_wait woken by fb_cancel_set's fb_pool_signal, then the loop's IsCancelled check. The comment says it 'stops when it's cancelled, while it waits or before', and the milestone's done-when says 'a Cancellation stops a blocking read', but neither is checked for Receive or Send. The program below copies the test's pattern and counts how often the Cancel came first. My own program, which drains and then cancels idle receivers, shows the in-wait path does work. The test just doesn't cover it. Only the Selector case sleeps before cancelling.

```
Import std.core
Import std.concurrent

Shared Class Counts
    Field Before: Atomic<Int>
    Field Caught: Atomic<Int>
End Class

// concurrent_channels.fb's pattern: start a Receive on another thread, then Cancel at once.
Const counts = Counts()
For round = 1 To 50
    Const waiting = Channel<Int>(2)
    Const cancel = Cancellation()
    Local stopped = Thread.Start(Function()
        If cancel.IsCancelled Then counts.Before.Add(1)
        Try
            Receive(waiting, cancel)
        Catch e As CancelledError
            counts.Caught.Add(1)
        End Try
    End Function)
    cancel.Cancel()
    stopped.Join()
End For
Print("cancelled before Receive began:", counts.Before.Load(), "of", counts.Caught.Load())
```

Expected: The test should cancel a Receive (and a Send on a full Channel) that is already waiting: a count well below 50 here, ideally 0 with a pause before Cancel

Actual: cancelled before Receive began: 50 of 50 (debug and release)

Suggested fix: In Receiving() and Sending(), let the other thread reach its wait before cancel.Cancel(): a Sleep(10.Milliseconds) as the Selector case does, or better, a handshake such as the thread sending on a 'ready' Channel just before it calls Receive/Send.

Skeptic: I ran the finding's program, which copies the test's pattern (Thread.Start, then Cancel at once). It printed 'cancelled before Receive began: 50 of 50' in both debug and release. In concurrent_channels.fb, Receiving() (lines 87-95) and Sending() (lines 126-134) call cancel.Cancel() straight after starting the thread or Spawn. So the receive and send always find the Cancellation already set when they begin, and the path where fb_pool_wait is woken by a Cancel is never run for Receive or Send. Only the Selector case sleeps first (line 174). No other test cancels a Receive or Send while it waits: grep shows only panic_concurrent_send_closed.fb, which doesn't wait. The comment on lines 61-62 claims 'while it waits or before', and the milestone's done-when says 'a Cancellation stops a blocking read'.

## channels-3 (low): concurrent_channels.fb compiles with a warning

Status: fixed in 205d575d: the dead '?? 0' is gone, and the test builds without a warning

Where: tests/programs/concurrent_channels.fb:72

`totals.Sum.Add((value ?? 0) As Int64)` comes after `If value == Null Then Break`, so value is narrowed and the compiler warns that '??' always gives it. Every build of the test prints this warning.

```
Import std.core
Import std.concurrent

Const values = Channel<Int>(2)
Const cancel = Cancellation()
values.Send(4)
values.Close()
Local sum: Int64 = 0
Try
    Loop
        Local value = Try Receive(values, cancel)
        If value == Null Then Break
        sum += (value ?? 0) As Int64
    End Loop
Catch e As CancelledError
End Try
Print(sum)
```

Expected: No warnings, and 4 printed

Actual: warning: 'value' can't be Null here, so '??' always gives it (the test itself prints the same warning at concurrent_channels.fb:72:33), then 4

Suggested fix: Write `totals.Sum.Add(value As Int64)` on line 72.

Skeptic: I copied tests/programs/concurrent_channels.fb and ran it with the 33e24764 build. It prints "cc.fb:72:33: warning: 'value' can't be Null here, so '??' always gives it" and then the expected output. The finding's smaller program prints the same warning, then 4, in debug and release. Line 72 comes after `If value == Null Then Break`, so the `?? 0` is dead.

## concurrent-2 (low): A Lazy maker that reads its own Value panics with the internal Mutex's message

Status: fixed in c9ac1d06: the maker's thread writes its number (the runtime's new fb_thread_number) while it makes the value, and a read on that thread panics with "the Lazy's maker reads its own Value, which would wait for itself forever"; panic_concurrent_lazy_self.out has it, and a runtime test checks the numbers

Where: std/concurrent/lazy.fb:24

lazy.fb and docs/stdlib.md say a maker that reads its own Lazy's Value panics because it would wait for itself forever. The panic that comes out is the built-in Lock's message, about a Mutex and a Lock the user never wrote. Other std.concurrent types catch this case and give their own message, for example Once: "the Once's work calls its own Do, which would wait for itself forever". panic_concurrent_lazy_self.out locks in the Mutex text, so the test doesn't catch it.

```
Import std.core
Import std.concurrent

Global self = Mutex<Lazy<Int>?>(Null)

Function Maker(): Int
    Local lazy: Lazy<Int>? = Null
    Lock self As held
        lazy = held
    End Lock
    Return (lazy?.Value ?? 0) + 1
End Function

Const lazy = Lazy<Int>(Maker)
Lock self As held
    held = lazy
End Lock
Print(lazy.Value)
```

Expected: panic: a message about the Lazy, e.g. "the Lazy's maker reads its own Value, which would wait for itself forever at l1.fb:11:13"

Actual: panic: this thread holds the Mutex already, so Lock would wait for itself forever at l1.fb:11:13

Suggested fix: In Value, check whether this thread is already making the Lazy before taking the Lock, for example with a maker-thread Atomic set inside the lock or a TryLock-style check, and Panic with a Lazy-specific message as Once.Do does. Update panic_concurrent_lazy_self.out to match.

Skeptic: Ran the finding's program (rv-step18b/v2/l1.fb) with build-lane6-verify/cli/fb.exe, which is at 33e24764. Debug and --release both printed 'panic: this thread holds the Mutex already, so Lock would wait for itself forever at l1.fb:11:13' and exited 101. It does panic at the caller's line as docs/stdlib.md:690-693 says. But the message is the built-in Lock's, about the Lazy's private Mutex field (std/concurrent/lazy.fb:24, `Lock Made As made`), and nothing checks for a maker reading its own Value first. That leaks an internal detail, and it misleads when the program has its own Mutex, as this one does (`self`). std.concurrent's other types each give a message of their own for the same case (Once.Do at locks.fb:188, RwLock.Read and Write at locks.fb:229/243). tests/.../panic_concurrent_lazy_self.out locks in the Mutex text. The behavior is right, so this is a diagnostics defect: low.

## log-3 (low): The console line writes field names raw, so a name with a newline splits the line and an ESC reaches the terminal

Status: fixed in 6dc0e8da: LogLine quotes and escapes a field's name as it does a value; log_output.fb has a name with a space and a line break

Where: std/log/sinks.fb:156

LogLine escapes the module and message ("so a line stays one line") and quotes values that need it, but writes field.Name as-is. A name with a newline breaks the one-line output, a name with a control character such as ESC goes straight to the terminal, and a name with a space or '=' can't be read back as name=value. JsonLine escapes the same name correctly.

```
Import std.core
Import std.collections
Import std.time
Import std.log

Local f = ImmutableArray<LogField>().Add(LogField("a b\nc", "v"))
Print(LogLine(LogRecord(Timestamp.FromUnixMilliseconds(0), LogLevel.INFO, "m", "x", f), False))
Print("end")
```

Expected: One console line with the name escaped, for example: INFO  m: x "a b\nc"=v
end

Actual: INFO  m: x a b
c=v
end   (the line is split in two)

Suggested fix: Write the name through Escaped(name, ...): quote and escape it, as with values, when NeedsQuotes says so. Add a log_output case with a name that has a space and a newline.

Skeptic: The program prints "INFO  m: x a b", then "c=v", then "end", so the record's line splits in two. std/log/sinks.fb:50 writes the name raw (line += " {field.Name}={value}"). Line 46 escapes the module and message, and the doc comment at lines 29-33 says the escaping is there "so a line stays one line" and that fields are written as name=value. JsonLine (line 67) runs the same name through Escaped(field.Name, True). So the console line breaks its own one-line contract when a field's name has a control character.

## log-4 (low): On Windows FB_LOG is read with getenv in the ANSI code page, so a non-ASCII module name never matches

Status: fixed in 24960463: the runtime reads environment variables through one helper, _wgetenv and UTF-8 on Windows, for FB_LOG, NO_COLOR, and tz.cpp's TZ; log_tests.cpp sets FB_LOG to "café=debug" with _wputenv_s and reads it back as UTF-8

Where: runtime/src/log.cpp:14

fb_log_setting uses std::getenv, which on Windows gives the variable in the process's ANSI code page, not UTF-8. fb_string_from_cstring then turns the non-ASCII byte into U+FFFD, so FB_LOG="café=debug" gives the filter "info,caf�=debug", and Logger("café")'s debug lines are dropped while an ASCII module named in the same setting works. The invalid-FB_LOG message would show the same mangled text.

```
Import std.core
Import std.log

// Run with: FB_LOG="café=debug,plain=debug" ./e.exe   (Windows)
Print("{DefaultLogOutput().Filter}")
Logger("café").Debug("café's debug line")
Logger("plain").Debug("plain's debug line")
```

Expected: info,café=debug,plain=debug
<time> DEBUG café: café's debug line
<time> DEBUG plain: plain's debug line

Actual: info,caf�=debug,plain=debug   (bytes EF BF BD in place of é)
<time> DEBUG plain: plain's debug line   (café's line is missing)

Suggested fix: On _WIN32, read the variable with _wgetenv or GetEnvironmentVariableW and convert it to UTF-8 with WideCharToMultiByte(CP_UTF8) before making the String. Do the same for NO_COLOR; tz.cpp's TZ has the same pattern. Add a runtime test that sets a non-ASCII FB_LOG with _wputenv_s.

Skeptic: I built l4.fb and ran it with FB_LOG="café=debug,plain=debug" from Git Bash on Windows, in debug and in release. stdout shows "info,caf" followed by bytes 357 277 275 (EF BF BD, which is U+FFFD) and then "=debug,plain=debug". Only plain's DEBUG line prints; café's is dropped. runtime/src/log.cpp:14 uses std::getenv, which returns the ANSI code page's bytes on Windows, and fb_string_from_cstring turns the non-UTF-8 byte into U+FFFD. On POSIX the same setting works, so std behaves differently by platform. It only affects non-ASCII module names, so I rate it low.

## Refuted by the skeptics (no action)

- channels-4: A Selector with only a Timeout returns False at once and never runs the timeout. Reproduced: Selector().Timeout(10.Milliseconds, f).Wait() printed 'False 0' in debug and release. But this follows the documented rule. stdlib.md's bullet says Wait 'gives False once every Channel is closed and empty', and Selector's own doc comment in channels.fb says the same. In WaitFor, all-closed is checked before the deadline even when there are Channels: a closed and empty Selector gives False rather than running its timeout. A Selector with no Channels is vacuously all closed and empty, so giving False at once is the same rule applied consistently, and it matches the lane's call ('False once every Channel is closed and empty'). Nothing in the spec, stdlib.md, or the milestones row says a Selector with no Channels must wait out its timeout. It's an ambiguous edge case, not a contradiction, so I refute it.
- log-2: Logger.With of a Float32 writes the widened Double's digits, 0.10000000149011612 for 0.1. The behavior is as reported: the program prints 0.1 and then "INFO  m: x f=0.10000000149011612". But stdlib.md says With takes a Double, not a Float32. A Float32 widens to the Float64 overload, and 0.10000000149011612 is the exact value of Float32 0.1 written as a Double. That is the language's ordinary widening giving a correct Double, not a broken promise. Nothing in the spec, stdlib.md or the milestones row says a Float32 field keeps its short form.
- log-5: DefaultLogOutput's use of FB_LOG and fb_log_colors/NO_COLOR have no test. The behavior is right when run by hand. FB_LOG=verbose prints the 'isn't a log filter ... so it's left out' message once, then "info". A valid filter is read and applied. The milestones done-when ("logs filter by module") is covered by tests/programs/log_filter.fb and log_output.fb; log_output.fb also uses DefaultLogOutput. tests/runtime/log_tests.cpp tests fb_log_setting with FB_LOG set and unset. I saw no wrong behavior and no contradiction with the spec, stdlib.md or the row. More coverage of NO_COLOR and of DefaultLogOutput end to end would be nice to have, but its absence is not a bug.
