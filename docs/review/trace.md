# The review of M6 the Linux/macOS pass's last fixes: FileLock retrying flock on macOS until it's free, and the HTTP test server's opt-in trace

2 confirmed findings: 0 high, 1 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## lock-1 (medium): Test servers running at once overwrite each other's FB_HTTP_TEST_LOG trace

Status: fixed in 93a8810a: the trace's file is opened to add to its end only (FILE_APPEND_DATA on Windows, O_APPEND elsewhere), each line one write, starting after its time with its server's process; program_tests' "the HTTP test servers of tests that run at once share a trace" runs two servers at once on one file and checks each server's accepts, threads, and requests are all there, by process, which failed with four processes' worth of mixed lines before

Where: tests/native/http_server/http_server.cpp:1268

main opens FB_HTTP_TEST_LOG with fopen(log, "w"). Each fb_http_test_server process truncates the file and then writes from its own offset 0. The trace lines also carry no process id. The lane's note says to use the trace under the full ctest load, where many http_ program tests (and so many servers) run at once with one env var. In that case the servers truncate and overwrite each other's lines, and the lines that survive can't be told apart by process because socket numbers repeat across processes. Lost lines also produce false signs of a hang, such as an "accept" with no "serve", which is exactly what the note says to look for. Reproduced on Windows: build the program below with `fb build gets.fb -o gets.exe`. Then run two servers on it 0.7 s apart, both with FB_HTTP_TEST_LOG set to the same two.log. Each server writes 80 lines (20 each of accept, serve, request, and close).

```
// Makes 20 requests, each on a new client, to the test server's plain port, after waiting 1.5 s.
Import std.core
Import std.time
Import std.net.http

Local base = Input() ?? ""
Sleep(1500.Milliseconds)
For i = 1 To 20
    Local client = HttpClient()
    Try
        Print(client.Get("{base}/bytes/10").Status)
    Catch e As NetError
        Print("NetError")
    Catch e As TimeoutError
        Print("TimeoutError")
    End Try
End For
```

Expected: Both programs print 200 twenty times. two.log has 160 lines (40 each of accept, serve, request, and close), and each line can be traced to its server.

Actual: Both print 200 twenty times, but two.log has 81 lines: 20 each of accept, serve, request, and close from one server, plus a stray empty line where the other's longer write was cut. The other server's 80 lines are gone, and nothing says which server wrote the lines that are left.

Suggested fix: Open the file in append mode ("a", one fprintf+fflush per line, which O_APPEND keeps whole on POSIX). Put the process id on each line, or expand a %p-style placeholder in FB_HTTP_TEST_LOG to the pid so each server writes its own file. Document whichever you choose in the comment on trace_file.

Skeptic: I built the finding's gets.fb (C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-lock/trace/gets.fb) with fb build. I ran it under build-rev's fb_http_test_server.exe, which was built Oct 27 20:48 and contains the FB_HTTP_TEST_LOG trace. One run used FB_HTTP_TEST_LOG=one.log. For the other, I started two servers 0.7 s apart, both with FB_HTTP_TEST_LOG=two.log and stdin from /dev/null. Both programs printed '200' 20 times. one.log had 80 lines. two.log had 81 lines: 20 each of accept, serve, request, and close, plus one empty line. The other server's 80 lines were gone, and no surviving line says which process wrote it. The cause is that main (http_server.cpp:1267-1268) opens the file with fopen(log, "w"): each server truncates it and writes from offset 0. This breaks the trace's stated use. The lane's note says to set FB_HTTP_TEST_LOG under the full load and look for an 'accept' with no 'serve'. tests/CMakeLists.txt:41-47 runs the program tests in shards that ctest runs side by side, so several http_ programs, each with its own server, run at once with one env var. Lines lost that way look exactly like the hang signs the note says to look for. Only test tooling is affected, nothing shipped, but the tool fails at the one job it was added for and can mislead the hang hunt, so I kept medium.

## lock-2 (low): The trace's times count from its first line, not from when the server started

Status: fixed in 93a8810a: the start time is set in main as the trace opens, so a line's time counts from the server's start; the same test's program waits 300 ms before its first request, and each server's first line is checked to be at 300 or more

Where: tests/native/http_server/http_server.cpp:101

trace()'s comment says each line starts with "the milliseconds since the server started". But `started` is a function-local static, so it's set on the first trace() call, which is the first accept. Every server set-up and program start-up before that first connection is left out. In the run below, the program sleeps 1.5 s before its first request, yet the first trace line reads 0. For looking into a hang, this hides how long the server waited before its first accept. Run it with `fb build gets.fb -o gets.exe`, then `FB_HTTP_TEST_LOG=one.log fb_http_test_server.exe ./gets.exe`, then read the first line of one.log.

```
// Makes 20 requests, each on a new client, to the test server's plain port, after waiting 1.5 s.
Import std.core
Import std.time
Import std.net.http

Local base = Input() ?? ""
Sleep(1500.Milliseconds)
For i = 1 To 20
    Local client = HttpClient()
    Try
        Print(client.Get("{base}/bytes/10").Status)
    Catch e As NetError
        Print("NetError")
    Catch e As TimeoutError
        Print("TimeoutError")
    End Try
End For
```

Expected: The first line of one.log starts at about 1500 or more (ms since the server started), e.g. "1530 <thread> accept 424 on 280".

Actual: The first line of one.log is "0 8670141377090704656 accept 424 on 280".

Suggested fix: Set the start time in main, at the same place the file is opened (a namespace-scope `started` assigned there), or change the comment to say the times count from the first line traced.

Skeptic: Same run with FB_HTTP_TEST_LOG=one.log: the program sleeps 1.5 s before its first request, yet the first line of one.log is '0 8670141377090704656 accept 356 on 320', and the second also starts at 0. In http_server.cpp:100-108, trace()'s comment says each line starts with 'the milliseconds since the server started'. But `started` is a function-local static inside trace() (line 107), so it is set on the first trace() call, the first accept, not when the server starts. The code plainly contradicts its own comment, and the trace hides how long the server waited before its first connection. Low: it only affects diagnostics. Either set the start time in main where the file is opened, or reword the comment.
