# The review of M6 step 30's Workers (server.Work routes on a pool of blocking worker threads) and its two compiler fixes (a Static ReadOnly Field of a Sendable type read from any thread; a Struct's New starting a Func Field at a null function value)

5 confirmed findings: 0 high, 0 medium, 5 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## compiler-1 (low): sema's comment says a Static ReadOnly Field has its value before any work starts, but work started from an earlier Global's initializer can reach it first

Status: fixed in 425583e1: note_thread_use's comment says the field never changes once its initializer runs, in source order with the Globals; comment only, so no test

Where: compiler/src/sema.cpp:10541

note_thread_use's new comment says a Static ReadOnly Field of a Sendable type 'keeps the value it starts with, before any work starts, so any thread reads it'. Static Fields start in source order with the Globals (spec 06), so a Global initializer above the type can start work that reads the field before its initializer runs. The behavior itself matches spec 02 and a Sendable Const: debug panics with 'used before its initializer has run', and release reads the String's zero. Only the comment's 'before any work starts' is wrong. Spec 10's new wording ('which keeps the value it starts with') is fine.

```
Import std.core

Function Make(): String
    Return "status {200}"
End Function

Function Start(): Task<String>
    Local t = Spawn(Function() => "worker reads '{Status.Name}'")
    t.Wait()
    Return t
End Function

Global early = Start()
Print(early.Wait())

Struct Status
    Static ReadOnly Field Name: String = Make()
End Struct
```

Expected: Per the comment, the worker always sees the field's value: worker reads 'status 200'

Actual: debug: panic: 'Status.Name' is used before its initializer has run, since Globals initialize in source order at i2.fb:8:51 (exit 101); release: worker reads ''

Suggested fix: Reword the comment: such a field never changes after its initializer runs, which happens in source order with the Globals, so reading it from any thread is as safe as reading a Sendable Const. Drop 'before any work starts'.

Skeptic: I ran the reviewer's i2.fb (in scratchpad/rv-workers/cf/). In debug it exits 101 with: "panic: 'Status.Name' is used before its initializer has run, since Globals initialize in source order at i2.fb:8:51". In release it prints "worker reads ''". The Global `early` above the Struct starts a Spawn that reads Status.Name before that field's initializer has run. That contradicts the new note_thread_use comment at compiler/src/sema.cpp:10541-10543 ("keeps the value it starts with, before any work starts"). The behavior is consistent with the source-order rule for Globals: debug panics and release reads the zero value. Spec 10's new wording ("which keeps the value it starts with") makes no claim about timing. So only the comment is wrong, and the fix is to drop 'before any work starts' and say the field never changes once its initializer runs, in source order with the Globals.

## compiler-2 (low): The error for a Static ReadOnly Field that isn't Sendable doesn't say why the new exception doesn't apply

Status: fixed in 425583e1: worker code's use of a Static ReadOnly Field whose type isn't Sendable says "which holds T, which isn't Sendable", as a Const's does; the sema test's Boss.Main case says so, and a new one reads a Static ReadOnly Int[]

Where: compiler/src/sema.cpp:10574

Worker code may now read a Static ReadOnly Field of a Sendable type. When the field's type isn't Sendable, the error is still the generic one for a mutable Static Field: 'which only the main thread can, unless it's an Atomic or a Mutex'. It doesn't say that the field is ReadOnly but its type isn't Sendable. The Const case just below names the type and says 'which isn't Sendable'. The new sema test pins the unhelpful message ('Boss.Main').

```
Import std.core

Struct Status
    Static ReadOnly Field Codes: Int[] = [200, 201]
End Struct

Local t = Spawn(Function() => Status.Codes[0])
Print(Await t)
```

Expected: An error that says the field's type isn't Sendable, like the Const case's: "... uses the Static Field 'Status.Codes', which holds Int32[], which isn't Sendable"

Actual: error: this work runs on another thread, but it uses the Static Field 'Status.Codes', which only the main thread can, unless it's an Atomic or a Mutex

Suggested fix: In the GLOBAL case, when used.owner is set and the field is ReadOnly but its type is unsendable, use main_only with the type and ", which isn't Sendable", as the CONSTANT case does. Update the sema test's expected 'Boss.Main' message to match.

Skeptic: I ran the reviewer's c2.fb (Static ReadOnly Field Codes: Int[] read inside Spawn). Debug and release both fail with: "this work runs on another thread, but it uses the Static Field 'Status.Codes', which only the main thread can, unless it's an Atomic or a Mutex". In sema.cpp:10572-10579 the new `fixed` check only suppresses the error. When a field is ReadOnly but its type is unsendable, it falls through to the generic mutable-field message. That message points to the wrong fix (Atomic/Mutex), when the real cause is that the type isn't Sendable, and it ignores the exception spec 10 now lists ('a Static ReadOnly Field of a Sendable type'). The CONSTANT case just below already names the type and says ", which isn't Sendable". tests/compiler/sema_tests.cpp's new Boss.Main case locks in the generic message. Spec 10 doesn't require a wording for this case, so this is only a low-severity diagnostic issue. Fix: in the GLOBAL case, when used.owner is set, the field is ReadOnly and its type is unsendable, use main_only with the type and ", which isn't Sendable", and update the Boss.Main expectation.

## work-1 (low): A work-route request already being answered when Stop comes gets a 503, while an Async route's request is answered

Status: fixed in b70d4ea5: Serve no longer closes the Workers threads as it gives; they end once Serve has given and the last connection it was answering has ended (Ended and Left), and a work call no longer refuses for Stopping; http_server_work_limits holds an Async route's request and a work route's in middleware until Stop has come, and both are 200s

Where: std/net/http/server.fb:526

Serve's comment says it 'gives once it stops taking them; requests being answered then are answered'. Serve runs `Defer Pool?.Close()` as soon as its accept loop ends, which is right after Stop closes the listener. Connections that are still being answered keep going after that. A request that was accepted before Stop, but reaches Worked only after it (because it is still in middleware, still reading its body, and so on), finds the pool closed (`Run` gives Null) or finds `Stopping` set on the first-call path, and gets a 503 Service Unavailable. An Async route in the same position is answered normally. This happens whether or not the pool has already started (first-call path and started-pool path both give 503). stdlib.md's 'a call after [Serve gives] is a 503' describes this, but it contradicts Serve's own promise, and it makes a work route behave differently from an Async route during a graceful stop.

```
// Requests the server is answering when it stops are answered, a work route's as an Async
// route's: both are in a middleware that waits when Stop comes.
Import std.core
Import std.net
Import std.net.http
Import std.time

Local server = HttpServer()
server.Workers = 1
server.Use(Async Function(request: HttpRequest, next: HttpHandler): HttpResponse
    Await RealWait(300.Milliseconds)
    Return Await next(request)
End Function)
server.Get("/async", Async Function(request: HttpRequest) => HttpResponse.Of(Body.Text("async")))
server.Work("/work", Function(request: WorkRequest) => HttpResponse.Of(Body.Text("work")))

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local client = HttpClient()
    Local a = client.GetAsync("{base}/async")
    Local w = client.GetAsync("{base}/work")
    Await RealWait(100.Milliseconds)
    server.Stop()
    Local ra = Await a
    Local rw = Await w
    Print("async:", ra.Status, ra.Text)
    Print("work:", rw.Status, rw.Text)
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: async: 200 async
work: 200 work

Actual: async: 200 async
work: 503 
(same in debug and release; also 503 when an earlier work call has already started the pool)

Suggested fix: Keep the pool open while connections are still being answered. For example, in Worked, start or use the pool whenever the request's connection is still open, and do not refuse on `Stopping`. Then close the pool once Serve has given and the last connection's Answer has ended (in Answer's Defer, when Connections is empty and Stopping is set), rather than in Serve's Defer. Then make stdlib.md's '503 after Serve gives' wording match: only a call that arrives after every connection has ended should get a 503. Add the program above as a test.

Skeptic: I ran the finding's program, rv-workers/refute/w1.fb, and it printed 'async: 200 async' then 'work: 503 ' in both debug and release. The cause is in std/net/http/server.fb. Serve runs `Defer Pool?.Close()` and gives as soon as Stop closes the listener. Worked returns a 503 when `Stopping` is set and the pool hasn't started, or when `pool.Run` gives Null because the channel is closed. So a request the server accepted before Stop, but which reaches Worked only after it (here it was still in middleware), gets a 503, while an Async route in the same position is answered. Serve's comment says 'requests being answered then are answered', and Stop's says it lets those close 'once it's answered'. Work calls that were already running or queued when Stop came are answered: rv-workers/refute/w1b.fb, with Workers = 1 and one call running plus one queued, gave 200/200 in both builds, because the channel drains. stdlib.md's 'a call after [Serve gives] is a 503' does cover this literally, and the window is only during a stop, with a well-formed 503 sent. That is why I rate it low rather than medium. It still breaks Serve's own promise and makes work routes behave differently from Async routes.

## work-2 (low): WorkRequest has no Params, though its comment says it has what HttpRequest has but the connection

Status: fixed in b70d4ea5: WorkRequest has Params, a new Map each time; http_server_work's echo prints its Count

Where: std/net/http/work.fb:23

WorkRequest's comment says it is 'what HttpRequest has but the connection, as Sendable values'. HttpRequest has `Params` (each bound path parameter, by name), and WorkRequest does not. It keeps the bound map privately as `Bound` and exposes only `Param(name)`. So a work handler can't list its route's parameters, and code that reads `request.Params` doesn't move from an Async route to a work route. The lane's proposal also listed 'params' as part of the copy.

```
// A work handler reads each path parameter by name, as an Async one does through Params.
Import std.core
Import std.net.http

Local server = HttpServer()
server.Work("/a/:x/:y", Function(request: WorkRequest) => HttpResponse.Of(Body.Text("{request.Params.Count}")))
server.Get("/b/:x/:y", Async Function(request: HttpRequest) => HttpResponse.Of(Body.Text("{request.Params.Count}")))
Print("compiled")
```

Expected: compiled

Actual: params.fb:6:95: error: WorkRequest has no member named 'Params'

Suggested fix: Add `Public Property Params: Map<String, String>` to WorkRequest, with `Get => Bound.ToMap()` (or however ImmutableMap converts), so each call gets a new Map. Add it to stdlib.md's list of WorkRequest's parts and to http_server_work.fb. Or, if leaving it out is intended, change the comment so it doesn't claim WorkRequest has everything HttpRequest has.

Skeptic: I ran rv-workers/refute/w2.fb and it failed to compile with 'w2.fb:6:95: error: WorkRequest has no member named 'Params''. HttpRequest's public members are Verb, Target, Path, Query, Headers, Bytes, Version, RemoteAddress, Text, Param, Params, and Form(). WorkRequest (std/net/http/work.fb) has all of them except Params. It copies the bound map privately as `Bound` and exposes it only through Param(name). WorkRequest's comment says it has 'what HttpRequest has but the connection', and the user-chosen proposal C listed params as part of the copy. stdlib.md's list of WorkRequest's parts leaves Params out, so the docs and the code agree there. The fix is either to add Params or to correct the comment.

## work-3 (low): Neither of the documented 503s (1024 waiting, or after Serve gives) is tested

Status: fixed in b70d4ea5: the queue's length is a server's MaxQueuedWork (1024 unless it's set; panics below 1 or once the threads start: panic_http_max_queued_work and panic_http_max_queued_work_started), so http_server_work_limits holds one thread's call, queues two, and the fourth is a 503; the 503 after Serve gives can't come any more, as work-1's fix keeps the threads while connections are answered, and no request comes once they've all ended; http_server_many makes a hundred work calls, fifty at once, with no leaks

Where: tests/programs/http_server_work.fb:1

stdlib.md and Worked's comment both promise a 503 Service Unavailable when 1024 calls are already waiting for a Workers thread, and when a work route is called after Serve gives. No test exercises either. http_server_work never sees a non-2xx status from a work route, and the panic programs cover only Workers' setter and the method token. The queue limit does work as documented: with Workers = 1 and the thread held, 1 call runs, 1024 wait, and the rest get a 503. Also, http_server_many's no-leaks-after-many-connections check (part of the milestones done-when) never goes through a work route.

```
// With one Workers thread held, 1024 calls wait, and the next is a 503.
Import std.core
Import std.collections
Import std.concurrent
Import std.net
Import std.net.http
Import std.time

Const release = Channel<Bool>(4096)
Global started: Atomic<Int> = Atomic<Int>(0)

Local server = HttpServer()
server.Workers = 1
server.Work("/w", Function(request: WorkRequest): HttpResponse
    started.Add(1)
    release.Receive()
    Return HttpResponse.Of(204)
End Function)

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local client = HttpClient()
    Local tasks: (Task<HttpResponse> Throws NetError, TimeoutError)[] = []
    For i = 1 To 1030
        tasks.Add(client.GetAsync("{base}/w"))
        If i % 50 == 0 Then Await RealWait(200.Milliseconds)
    End For
    Local deadline = Deadline.After(30.Seconds)
    While started.Load() < 1 && !deadline.HasPassed
        Await RealWait(10.Milliseconds)
    End While
    Await RealWait(3.Seconds)
    For i = 1 To 1030
        release.Send(True)
    End For
    Local counts = Map<Int, Int>()
    For task In tasks
        Local r = Await task
        counts[r.Status] = (counts.Get(r.Status) ?? 0) + 1
    End For
    For status, count In counts
        Print(status, count)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
    For i = 1 To 1030
        release.Send(True)
    End For
End Try
```

Expected: 204 1025
503 5
(and a test in tests/programs that checks it, plus one for the 503 after Serve gives)

Actual: 204 1025
503 5 when run by hand. No program in tests/programs checks either 503 path, and none checks work routes for leaks across many connections.

Suggested fix: Add a test for the queue limit, as above or with WAITING made smaller for tests. Add a test for a work call after Serve gives (or the in-flight case from work-1, once that is settled). Add work routes to http_server_many's many-connections leak check.

Skeptic: Neither 503 path has a test. `grep 503|SERVICE_UNAVAILABLE` finds nothing in tests/programs/http_server*.fb or panic_http_work*.fb. http_server_work.out shows only 2xx statuses from work routes. Among the http_server* programs, only http_server_work.fb calls `Work(`, so http_server_many's no-leaks check never goes through a work route. The behavior itself is correct: rv-workers/refute/w3.fb (one Workers thread held, 1030 calls) printed '204 1025' and '503 5' in debug, as documented. So this is a coverage gap only. Under the project's rule that each fix comes with a test, the queue-limit 503, the 503 after Serve gives, and leaks across many work-route connections each need one.
