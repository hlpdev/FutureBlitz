# The review of M6 step 32's integration program moved to HTTPS (ListenTls) and wss://

1 confirmed findings: 0 high, 0 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## tls-1 (low): When readings.fb fails partway, its HTTPS servers keep running, so the program prints its failure and then never exits, and the site's TempDir is left behind

Status: fixed in 6303cd84 (readings: "failing: TLS, then its servers stopped, and its folder removed True"): Run defers each server's Stop right after its Start, the site's with Try?, which also removes its folder, so a failure partway stops them, and the program ends. The new Failing run starts both servers, then fetches from a client that doesn't trust the test CA, and checks the site's folder is gone; without its Defers it prints False, and leaves the folder.

Where: tests/programs/readings.fb:54

Run() starts the feed with `feed.Serve()` (line 55) and the site with `site.Serve()` (line 107). It only calls `feed.Stop()` and `site.Stop()` at the end of the happy path (lines 66 and 144). A `Try` that throws in between leaves the HttpServer's Serve task running. That could be the trusted fetch, a GetAsync, the wss:// connect, Json.Read, or Image.Decode. The top-level Catch prints `failed: ...`, but the process never exits. The site's `readings-XXXXXX` TempDir is also never removed: Site.Stop is what calls Folder.Close, and killing the hung process leaves the folder in %TEMP%. So a regression in TLS trust, the routes, or ws now hangs ctest until its timeout and leaves files behind, where it should show a diff.

This commit makes the feed side worse. At f9649d66 the feed was a bare TlsListener, and the same failure (deleting `client.TrustOnly(TestRoots())` at line 64) printed `failed: mbedTLS: ...` and exited 0. At f55f77f3 the same edit prints it and then hangs (`timeout 60` gives rc 124). Deleting `web.TrustOnly(TestRoots())` at line 109 also hangs, and leaves `%TEMP%/readings-FVF0MN` behind. The site side already hung at the base commit: a failing GetAsync after site.Serve() hung there too.

The standalone program below shows the pattern. It imports `./readings/feed` for TestServer, so run it from a folder that holds a copy of tests/programs/readings/. Debug and release behave the same. On the happy path the program passes 15 of 15 debug runs and 15 of 15 release runs, leaves no folder behind, and its output matches readings.out.

```
Import std.core
Import std.net
Import std.net.http
Import "./readings/feed"

Async Function Hello(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("hi"))
End Function

Async Function Run() Throws Error
    Local server = HttpServer()
    server.Get("/", Function(request: HttpRequest) => Hello(request))
    Try server.ListenTls(SocketAddress(IpAddress.Loopback, 0), TestServer())
    Local serving = server.Serve()
    // As readings.fb's fetches do when one fails: a client that doesn't trust the test CA.
    Local got = Try Await HttpClient().GetAsync("https://127.0.0.1:{server.LocalAddress.Port}/")
    server.Stop()
    Await serving
End Function

Try
    Await Run()
Catch e As Error
    Print("failed:", e.Message)
End Try
Print("ended")
```

Expected: failed: mbedTLS: The certificate is not correctly signed by the trusted CA
ended
(then the process exits with code 0; in readings.fb the site's readings-* folder is removed)

Actual: failed: mbedTLS: The certificate is not correctly signed by the trusted CA
ended
(then the process hangs until it's killed, rc 124 under `timeout 30`, in debug and release; readings.fb with line 64 or 109 removed does the same, and with 109 removed leaves %TEMP%/readings-XXXXXX behind)

Suggested fix: Stop each server on every way out of Run. Put `Defer feed.Stop()` right after `FeedServer.Start` (HttpServer.Stop is idempotent). Put `Defer Try? site.Stop()` right after `Site.Start`, which also removes the TempDir. Keep the explicit Stop and Await calls on the happy path. I tried both Defers: the two failing variants then print `failed: ...` and exit 0, leave no folder behind, and the unchanged program still matches readings.out. A test that checks a failing fetch ends the program would keep this from regressing.

Skeptic: Reproduced with the build-rev fb, working in scratchpad/rv-readings-tls/tls1/ with a copy of tests/programs/readings/. (1) The reviewer's hang.fb printed "failed: mbedTLS: The certificate is not correctly signed by the trusted CA" and then "ended". After that it hung until `timeout 60` killed it (rc=124, exe code 143), in both debug and release. (2) I made no109.fb, which is readings.fb with line 109 (`web.TrustOnly(TestRoots())`) deleted. It printed every line up to "from hour 30", then the same "failed: mbedTLS ...", then hung until killed (rc=124). It also left C:\Users\Hunter\AppData\Local\Temp\readings-CDuXHh behind; there was none before the run, and I removed it afterwards. (3) The unchanged readings.fb exited with rc=0 and its output matched readings.out. The code agrees: Run() calls feed.Stop() only on line 66 and site.Stop() only on line 144. Site.Stop is the only caller of Folder.Close (site.fb), so any Try that throws between lines 55 and 66, or between 107 and 144, leaves a Serve task running. docs/spec/02-types.md:120 says the program ends only "once its coroutines and jobs are done", so the hang is what the spec says will happen. The runtime is not at fault; the program never stops its servers when it fails. The happy path is correct and a regression still fails ctest, though only by timeout, so I rate it low. The cost is that a failure hangs until ctest's timeout instead of showing a diff, and leaves a temp folder behind. The suggested fix is right: Defer the Stops right after each Start.
