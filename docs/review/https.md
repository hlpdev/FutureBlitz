# The review of M6 step 30's last pieces: HttpServer.ListenTls, wss://, TlsServer.Identify, and automatic HTTPS (ListenAutoAsync over ACME, renewal, AcmeClient.InFolder)

9 confirmed findings: 0 high, 3 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## auto-1 (medium): A kept certificate that's due, or about to expire, isn't checked until a whole RenewalCheck has passed, so it's served expired

Status: fixed in 17c36556 (Renewing checks once before its first wait, so a certificate that's due, kept or new, is renewed as the server starts while it serves the one it has), with http_acme_auto: a first certificate that's due is renewed as the server starts though RenewalCheck is an hour, and a server made again with a folder that keeps a due certificate renews it as it starts though RenewalCheck is 12 hours

Where: std/net/http/server.fb:1095

FirstAsync reuses the folder's certificate whenever its NotAfter is still in the future, even when less than a third of its lifetime is left. Renewing then always waits a whole check() before its first check. After a restart, a kept certificate that's due for renewal is served unrenewed for up to RenewalCheck, which is 12 hours by default, and it expires in the meantime if it had less time than that left. certmagic checks for renewal when it starts. Run as a tests/programs/http_*.fb program, under fb_http_test_server. Same result in debug and release.

```
// A certificate the folder keeps that's due for renewal, or about to expire, when the server
// starts again isn't checked until a whole RenewalCheck has passed, so it's served expired.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

Async Function Run()
    Try
        Local folder = Try TempDir()
        Local root = Try Certificate.AllFromPem(Try Http.GetText("{base}/acme/root"))
        // The first run's certificate: valid from 100 seconds ago to 4 from now.
        Try Http.GetText("{base}/acme/validity?before=100&after=4")
        Local first = HttpServer()
        first.RenewalCheck = 1.Hours
        Local acme = Try AcmeClient.InFolder("{base}/acme/dir", folder.Path)
        Try Await first.ListenAutoAsync(acme, ["localhost"], folder.Path,
                                        SocketAddress(IpAddress.Loopback, 0),
                                        SocketAddress(IpAddress.Loopback, challengePort))
        Try Http.GetText("{base}/acme/validity")
        Local serving = first.Serve()
        first.Stop()
        Await serving

        // The run after, checking every 10 seconds.
        Local again = HttpServer()
        again.RenewalCheck = 10.Seconds
        Try Await again.ListenAutoAsync(Try AcmeClient.InFolder("{base}/acme/dir", folder.Path),
                                        ["localhost"], folder.Path,
                                        SocketAddress(IpAddress.Loopback, 0),
                                        SocketAddress(IpAddress.Loopback, challengePort))
        Local servingAgain = again.Serve()
        Await Timer.After(6.Seconds)
        Local notAfter = again.CertificateNotAfter
        Print("expired: {notAfter != Null && notAfter <= Timestamp.Now()}")
        Try
            Local client = TlsClient()
            client.TrustOnly(root)
            Local tcp = Try Await TcpStream.ConnectAsync("127.0.0.1", again.LocalAddress.Port)
            Local tls = Try Await client.ConnectAsync(tcp, "localhost")
            Print("connected")
            Try tls.Close()
        Catch e As NetError
            Print("NetError: {e.Message}")
        End Try

        again.Stop()
        Await servingAgain
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: expired: False
connected
(the kept certificate, already due, is renewed as the server starts)

Actual: expired: True
NetError: the server's certificate has expired

Suggested fix: Have Renewing check once before its first wait, or have ListenAutoAsync renew a kept certificate that's already due (less than a third of its lifetime left) before it listens, as certmagic does at startup.

Skeptic: I built refute-auto/a1.fb (the finding's program) and ran it under fb_http_test_server. Debug and release both print 'expired: True' and 'NetError: the server's certificate has expired'. In server.fb, FirstAsync returns Stored() whenever NotAfter is still ahead of now. Renewing then waits a whole Deadline.After(check()) before its first check. So no check happens at startup, and a kept certificate with less than RenewalCheck left is served past its expiry. It's worse for a server that restarts more often than every RenewalCheck (deploys every few hours, say): the first check never happens, so the certificate is never renewed. That contradicts stdlib.md's 'Serve renews the certificate once less than a third of its lifetime is left'.

## auto-2 (medium): A renewal that stops between key.pem and chain.pem leaves a folder that panics every run after

Status: fixed in 17c36556 (the folder keeps the key and its chain in one file, certificate.pem, written atomically, so a write that stops leaves both as they were; and Stored() uses a kept identity only when the key is the private key of the chain's first certificate, through IsKeyOf, and otherwise orders another), with http_acme_auto: a folder whose certificate.pem has another key before the chain gets a new order, serial 07, and no panic

Where: std/net/http/server.fb:1061

ObtainAsync writes key.pem, then chain.pem, then names.txt. Each write is atomic, but the three together aren't. Stored() never checks that the key belongs to the chain. When the chain.pem write fails, the folder keeps the new key with the old chain, and every later ListenAutoAsync panics in TlsServer(chain, key), until someone clears the folder by hand. A write can fail from a full disk, a permission error, or the process ending between the writes. I reproduced the real path. A first run filled a folder. I set chain.pem read-only (attrib +R), and a run's RenewAsync threw 'can't write .../chain.pem: operation not permitted' after it had already replaced key.pem. I cleared the flag, and the next run panicked with the message below. The program here is self-contained: it writes the key.pem an interrupted renewal leaves, then restarts. Run it as a tests/programs/http_*.fb program, under fb_http_test_server.

```
// A renewal stopped between key.pem and chain.pem (an IOError writing chain.pem, or the
// process ending) leaves the new key with the chain before; the next run panics on it.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

Async Function Run()
    Try
        Local folder = Try TempDir()
        Local acme = Try AcmeClient.InFolder("{base}/acme/dir", folder.Path)
        Local server = HttpServer()
        Try Await server.ListenAutoAsync(acme, ["localhost"], folder.Path,
                                         SocketAddress(IpAddress.Loopback, 0),
                                         SocketAddress(IpAddress.Loopback, challengePort))
        server.Stop()

        // What ObtainAsync's first write leaves when the second doesn't happen.
        Local key = Key(KeyKind.ECDSA_P256, True, EcdsaP256.NewPrivateKey())
        Try File.WriteTextAtomic(folder.Path / Path("key.pem"), key.ToPem())

        Local again = HttpServer()
        Try Await again.ListenAutoAsync(acme, ["localhost"], folder.Path,
                                        SocketAddress(IpAddress.Loopback, 0),
                                        SocketAddress(IpAddress.Loopback, challengePort))
        Print("made again: {again.CertificateNotAfter != Null}")
        again.Stop()
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: made again: True
(a kept key that isn't the chain's is treated like no kept certificate, and a new one is ordered)

Actual: panic: the key isn't the private key of the chain's first certificate, CN=localhost at mismatch.fb:41:19
(exit 101)

Suggested fix: Make the kept identity a single unit. One option is to write the key and chain to one file atomically. Another is to write them under new names and commit with names.txt last. Either way, have Stored() return Null when the key isn't the private key of chain[0], so a mismatched folder leads to a new order instead of a panic. Add a test for this case.

Skeptic: I ran refute-auto/a2.fb. Debug and release both print 'panic: the key isn't the private key of the chain's first certificate, CN=localhost at a2.fb:38:19' and exit 101. In server.fb, ObtainAsync writes key.pem, then chain.pem, then names.txt as three separate atomic writes. Stored() checks only the names and NotAfter, never that the key matches chain[0]. ListenAutoAsync then hands the mismatched pair to TlsServer(chain, key), which panics. A failure between the writes (an IOError, or the process ending) leaves the folder panicking on every later run, which breaks decisions.md's 'a run after uses them while they're valid'.

## auto-3 (medium): While ListenAutoAsync is still getting its first certificate, the HTTP listener sends 308s to port 0

Status: fixed in 17c36556 (the redirects' port is `https`'s from the start, and the server on HTTP answers what isn't a challenge with a 503 and Retry-After: 5 until HTTPS listens, through HttpsPort's Ready), with http_acme_auto: a request to the HTTP port while ListenAutoAsync is getting its first certificate gets "HTTP/1.1 503 Service Unavailable"

Where: std/net/http/server.fb:962

The HTTP server starts serving before the first ObtainAsync, which it has to so it can answer challenges. HttpsPort.Port stays 0 until ListenTls has run (line 596). A visitor during that window, which takes seconds against a real CA, gets 'https://host:0/...', even when `https` names its port, as 8443 does in the program below. A 308 is permanent, so browsers cache the broken redirect. Run as a tests/programs/http_*.fb program, under fb_http_test_server. Same result in debug and release.

```
// A request on HTTP while ListenAutoAsync still gets its first certificate is sent to port 0.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

// The first Location the HTTP port answers with.
Async Function Early(): String
    Local plain = HttpClient()
    plain.FollowRedirects = False
    For i = 1 To 500
        Try
            Local response = Try Await plain.GetAsync("http://localhost:{challengePort}/page")
            Return response.Headers.Get("Location") ?? "-"
        Catch e As Error
            Await Timer.After(1.Milliseconds)
        End Try
    End For
    Return "none"
End Function

Async Function Run()
    Try
        Local folder = Try TempDir()
        Local acme = Try AcmeClient.InFolder("{base}/acme/dir", folder.Path)
        Local early = Early()
        Local server = HttpServer()
        Try Await server.ListenAutoAsync(acme, ["localhost"], folder.Path,
                                         SocketAddress(IpAddress.Loopback, 8443),
                                         SocketAddress(IpAddress.Loopback, challengePort))
        Print("https at {server.LocalAddress.Port}")
        Print("early: {Await early}")
        server.Stop()
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: https at 8443
early: https://localhost:8443/page

Actual: https at 8443
early: https://localhost:0/page

Suggested fix: Set Secure.Port from https.Port when that isn't 0 before the HTTP server starts serving. Until HTTPS is listening, answer requests other than challenges with a 503 (with Retry-After), not a permanent redirect to an unknown port.

Skeptic: I ran refute-auto/a3.fb with https at 8443. Debug and release both print 'https at 8443' and 'early: https://localhost:0/page'. In server.fb, ListenAutoAsync calls auto.Plain.Listen(http) and Serve() before FirstAsync, and sets auto.Secure.Port only after ListenTls. Until then HttpsPort.Port is 0, and Challenged builds '{host}:{port}' with 0. So every non-challenge request in that window gets a permanent 308 to port 0, which a browser caches.

## auto-4 (low): Setting RenewalCheck after ListenAutoAsync has no effect until the 12-hour wait it began with ends

Status: fixed in 17c36556 (Renewing reads check() on each one-second tick, against the time since the check before, so a RenewalCheck set meanwhile counts at once), with http_acme_auto: with RenewalCheck an hour, setting it to 500 ms has a due certificate renewed within seconds

Where: std/net/http/server.fb:1097

Renewing reads check() once per round, in Deadline.After(check()), and then waits out that deadline one second at a time. ListenAutoAsync starts that loop, so a RenewalCheck set afterwards takes effect only after the default 12 hours have passed. Configuring a server after it listens is a natural order. The docs say renewal 'checks every RenewalCheck' and don't say it has to be set first. Here a certificate due at once is never renewed with RenewalCheck set to 500 ms. Run as a tests/programs/http_*.fb program, under fb_http_test_server.

```
// RenewalCheck set after ListenAutoAsync: the renewals keep waiting out the 12 hours they began
// with, so a certificate due for renewal isn't renewed every half second as it's set to be.
Import std.core
Import std.collections
Import std.crypto
Import std.io
Import std.net
Import std.net.http
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

Async Function Run()
    Try
        Local folder = Try TempDir()
        // Valid from 100 seconds ago to 40 from now: due at once.
        Try Http.GetText("{base}/acme/validity?before=100&after=40")
        Local server = HttpServer()
        Try Await server.ListenAutoAsync(Try AcmeClient.InFolder("{base}/acme/dir", folder.Path),
                                         ["localhost"], folder.Path,
                                         SocketAddress(IpAddress.Loopback, 0),
                                         SocketAddress(IpAddress.Loopback, challengePort))
        Try Http.GetText("{base}/acme/validity")
        server.RenewalCheck = 500.Milliseconds
        Local serving = server.Serve()
        Local first = server.CertificateNotAfter
        Await Timer.After(5.Seconds)
        Print("renewed: {server.CertificateNotAfter != first}")
        server.Stop()
        Await serving
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: renewed: True

Actual: renewed: False

Suggested fix: Re-read check() on each one-second tick, measured from when the round started (for example, compare Instant.Now() - start against check()), so a new RenewalCheck applies right away. Alternatively, document that it has to be set before ListenAutoAsync.

Skeptic: I ran refute-auto/a4.fb. Debug and release both print 'renewed: False'. In Renewing, Deadline.After(check()) is read once per round, and the first round starts in ListenAutoAsync with the 12-hour default. A RenewalCheck set afterwards applies only after those 12 hours. That contradicts the docs' 'checks every RenewalCheck', which set no ordering. It's low because the 12-hour default is safe and the setting takes effect after one round.

## auto-5 (low): A 308 for an absolute-form request target drops the query

Status: fixed in 17c36556 (TargetOf gives an absolute-form target's path and query, from Url.Parse), with http_acme_auto: "GET http://localhost/page?x=1" is redirected to https://localhost:<port>/page?x=1

Where: std/net/http/server.fb:1136

Challenged uses request.Target only when it starts with '/'. For an absolute-form target (RFC 9112 3.2.2), it falls back to request.Path, which has no query. 'GET http://localhost/page?x=1' is redirected to .../page, without ?x=1. The origin-form request in the same program keeps its query. Run as a tests/programs/http_*.fb program, under fb_http_test_server.

```
// Automatic HTTPS's 308s for requests with an absolute-form target, and with an empty Host.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

// The Location of the answer to `head`, sent as it is.
Async Function LocationOf(head: String): String Throws NetError, TimeoutError, IOError
    Local tcp = Try Await TcpStream.ConnectAsync("127.0.0.1", challengePort)
    Try Await tcp.WriteAsync(Buffer<UInt8>.From(head.Bytes))
    Local reader = BufferedReader(tcp)
    Local status = Try Await reader.ReadLineAsync() ?? "-"
    Loop
        Local line = Try Await reader.ReadLineAsync()
        If line == Null Then Break
        Local text: String = line
        If text == "" Then Break
        If text.StartsWith("Location: ") Then Return "{status} -> {text.Slice(10, text.Length)}"
    End Loop
    Return status
End Function

Async Function Run()
    Try
        Local folder = Try TempDir()
        Local server = HttpServer()
        Try Await server.ListenAutoAsync(Try AcmeClient.InFolder("{base}/acme/dir", folder.Path),
                                         ["localhost"], folder.Path,
                                         SocketAddress(IpAddress.Loopback, 0),
                                         SocketAddress(IpAddress.Loopback, challengePort))
        Local serving = server.Serve()
        Print("https at {server.LocalAddress.Port}")
        Print(Try Await LocationOf("GET /page?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n"))
        Print(Try Await LocationOf("GET http://localhost/page?x=1 HTTP/1.1\r\nHost: localhost\r\n\r\n"))
        Print(Try Await LocationOf("GET /page HTTP/1.1\r\nHost:\r\n\r\n"))
        Print(Try Await LocationOf("GET /page HTTP/1.0\r\n\r\n"))
        server.Stop()
        Await serving
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: https at N
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page?x=1
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page?x=1
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page

Actual: https at N
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page?x=1
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page
HTTP/1.1 308 Permanent Redirect -> https://:N/page
HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page

Suggested fix: For an absolute-form target, redirect to its path and query: take them from the parsed target URL, or use request.Path plus the query when there is one.

Skeptic: I ran refute-auto/a5.fb. Debug and release both answer 'GET http://localhost/page?x=1' with '308 -> https://localhost:N/page', which loses the query, while the origin-form request keeps ?x=1. In Challenged, 'request.Target.StartsWith("/") ? request.Target : request.Path' falls back to Path, which has no query. RFC 9112 requires servers to accept absolute-form, so the redirect is wrong. It's rare in practice.

## auto-6 (low): An empty Host field makes the 308's Location 'https://:port/...'

Status: fixed in 17c36556 (an empty Host's host is the first name, as a missing one's is), with http_acme_auto: a request with "Host: " is redirected to https://localhost:<port>/page

Where: std/net/http/server.fb:1134

Challenged falls back to names[0] only when the Host field is missing. An empty 'Host:' field, which is valid HTTP/1.1, makes HostOf return "", so Location is 'https://:N/page', which isn't a valid URL. This is the third request of the auto-5 program. Run as a tests/programs/http_*.fb program, under fb_http_test_server.

```
// Same program as auto-5; the third request is the one with an empty Host.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""
Global secure1 = Input() ?? ""
Global challengePort: UInt16 = 0
Loop
    Local line = Input()
    If line == Null Then Break
    Local port = UInt16.Parse(line)
    If port != Null
        challengePort = port
        Break
    End If
End Loop

Async Function LocationOf(head: String): String Throws NetError, TimeoutError, IOError
    Local tcp = Try Await TcpStream.ConnectAsync("127.0.0.1", challengePort)
    Try Await tcp.WriteAsync(Buffer<UInt8>.From(head.Bytes))
    Local reader = BufferedReader(tcp)
    Local status = Try Await reader.ReadLineAsync() ?? "-"
    Loop
        Local line = Try Await reader.ReadLineAsync()
        If line == Null Then Break
        Local text: String = line
        If text == "" Then Break
        If text.StartsWith("Location: ") Then Return "{status} -> {text.Slice(10, text.Length)}"
    End Loop
    Return status
End Function

Async Function Run()
    Try
        Local folder = Try TempDir()
        Local server = HttpServer()
        Try Await server.ListenAutoAsync(Try AcmeClient.InFolder("{base}/acme/dir", folder.Path),
                                         ["localhost"], folder.Path,
                                         SocketAddress(IpAddress.Loopback, 0),
                                         SocketAddress(IpAddress.Loopback, challengePort))
        Local serving = server.Serve()
        Print(Try Await LocationOf("GET /page HTTP/1.1\r\nHost:\r\n\r\n"))
        server.Stop()
        Await serving
        Try folder.Close()
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: HTTP/1.1 308 Permanent Redirect -> https://localhost:N/page

Actual: HTTP/1.1 308 Permanent Redirect -> https://:N/page

Suggested fix: Use the fallback (names[0]) when HostOf gives an empty host, not only when the Host field is missing. Alternatively, answer an empty Host with a 400.

Skeptic: The same run of refute-auto/a5.fb (debug and release) answers 'Host:' with an empty value with '308 -> https://:N/page', which isn't a valid URL. Challenged uses `request.Headers.Get("Host") ?? fallback`, so the fallback applies only when the field is missing, and HostOf("") returns "". RFC 9112 also lets a server answer an empty or invalid Host with a 400; either way, this Location is wrong.

## auto-7 (low): The test CA's /acme/validity hook kills the test server for a value that isn't a number

Status: fixed in 43bc4150 (the test server's /acme/validity reads before and after with std::from_chars, a century at most either way, and answers a 400 otherwise; the CA checks mbedtls_x509write_crt_set_validity's result, and answers a 500 serverInternal for a validity mbedTLS can't write), with http_acme_auto (before=soon is a NetError of HTTP_STATUS, and the test server goes on) and acme_ca_tests (a validity 9000 years long fails the finalize with "the validity isn't X.509's", and the order is finalized after)

Where: tests/native/http_server/http_server.cpp:540

acme_answer parses before and after with std::stoll on a detached accept thread. A non-numeric value throws std::invalid_argument, which terminates the whole fb_http_test_server, and the program loses every port. An out-of-range value throws std::out_of_range. A huge value that does parse overflows now ± seconds in acme_ca.cpp, or formats a year mbedTLS rejects; that error is discarded with (void). The neighbouring /status/ and /redirect/ paths use std::stoi in the same way.

```
// The test CA's validity hook given a before that isn't a number.
Import std.core
Import std.net
Import std.net.http

Global base = Input() ?? ""

Try
    Print(Try Http.GetText("{base}/acme/validity?before=soon&after=40"))
Catch e As Error
    Print("failed: {e.Message}")
End Try

Try
    Print(Try Http.GetText("{base}/acme/validity"))
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: failed: <a 400 for the bad value>
2020 to 2120

Actual: terminate called after throwing an instance of 'std::invalid_argument'
  what():  stoll
failed: Recv failure: Connection was reset
failed: Failed to connect to 127.0.0.1:64648 after 0 ms: Could not connect to server
(exit 3)

Suggested fix: Parse with std::from_chars and answer a 400 for a value that isn't a number or is out of a sane range, such as ±100 years. Check mbedtls_x509write_crt_set_validity's result instead of discarding it.

Skeptic: I ran refute-auto/a7.fb. It printed "terminate called after throwing an instance of 'std::invalid_argument' what(): stoll", then 'failed: Recv failure: Connection was reset', then 'failed: Failed to connect ...', and exited 3. The whole fb_http_test_server died. http_server.cpp lines 540-541 call std::stoll on the raw query (inside the acme_lock scope, on a detached thread), and acme_ca.cpp:1074 now passes computed times to mbedtls_x509write_crt_set_validity and discards the result with (void). This is test infrastructure only, so it's low.

## tls-1 (low): ListenTls's handshake is limited per read, not to IdleTimeout overall, so a client that trickles bytes keeps it open indefinitely

Status: fixed in 17c36556 (wire.fb's Paced, a TcpStream that TLS goes through, sets each read's ReadTimeout to what's left of a Deadline while one's set; TlsDuplex.HandshakeAsync sets one of IdleTimeout over the whole handshake, and clears it after; wss:// goes through a Paced without one), with http_server_tls: a client that sends a handshake record a byte every 300 ms, inside the IdleTimeout of a second, is closed within 3 seconds

Where: std/net/http/server.fb:711

ListenTls's doc comment (server.fb:553-555), stdlib.md ("a handshake that fails or takes longer than `IdleTimeout` closes its connection") and decisions.md ("each connection's handshake runs in its own Answer, within IdleTimeout") all say the handshake is bounded by IdleTimeout. The code doesn't do that. Answer only sets `stream.ReadTimeout = Idle` and then awaits `secure.HandshakeAsync(tls)`, and TlsStream.HandshakeAsync reads the TCP stream many times. ReadTimeout is a timeout for each read, so the handshake is only closed when a single gap between bytes is longer than IdleTimeout. A client that sends one byte of its ClientHello just inside every IdleTimeout keeps a handshake, and its connection slot, open with no limit. That is a slowloris against HTTPS, and the docs say it can't happen. The test's "silent client" case only checks a client that sends nothing at all, so this was never tested. Confirmed in debug and in release (--release prints the same).

```
// Put it in tests/programs, next to http_server_tls/. A client that trickles its ClientHello
// one byte every 600 ms: each read comes within the server's IdleTimeout of 1 second, but the
// handshake takes far longer than IdleTimeout.
Import std.core
Import std.crypto
Import std.ffi
Import std.net
Import std.net.http
Import std.time
Import "./http_server_tls/certificates"

Local server = HttpServer()
server.IdleTimeout = 1.Seconds
server.Get("/", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("hi"))
End Function)
Try
    Local key = (Try? Key.FromPem(localhost_key)).Expect("the key reads")
    Local chain = (Try? Certificate.AllFromPem(localhost_pem)).Expect("the chain reads")
    server.ListenTls(SocketAddress(IpAddress.Loopback, 0), TlsServer(chain, key))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Local slow = Try Await TcpStream.ConnectAsync("127.0.0.1", port)
    Local started = Instant.Now()
    // A handshake record's header and a ClientHello's start, a byte at a time.
    Local bytes: UInt8[] = [0x16, 0x03, 0x01, 0x02, 0x00, 0x01, 0x00, 0x01, 0xFC, 0x03]
    Local sent = 0
    For b In bytes
        Try
            Try Await slow.WriteAsync(Buffer<UInt8>.From([b]))
        Catch e As IOError
            Break
        End Try
        sent += 1
        Await Timer.After(600.Milliseconds)
    End For

    slow.ReadTimeout = 300.Milliseconds
    Local open = False
    Try
        Local count = Try Await slow.ReadAsync(Buffer<UInt8>(16))
        open = count > 0
    Catch e As IOError
        open = e.Cause Is TimeoutError
    End Try

    Local seconds = started.Elapsed.TotalSeconds
    Print("sent {sent} of {bytes.Count} bytes; open after {seconds >= 5.0 ? "5 s or more" : "less"}: {open}")
    slow.Close()
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: The server closes the connection about 1 s after it was accepted, as the docs say. The last field is then False, for example "sent 3 of 10 bytes; open after 5 s or more: False". The count before a write fails can vary, but the connection must not still be open.

Actual: sent 10 of 10 bytes; open after 5 s or more: True
(the handshake is still waiting after more than 6 s with IdleTimeout set to 1 s)

Suggested fix: Put a deadline on the handshake as a whole. One way: in Answer, or in TlsDuplex.HandshakeAsync, start a watchdog coroutine, `Await Timer.After(Idle)`, that closes Tcp if `Tls` is still Null. The handshake's pending read then fails and HandshakeAsync returns False. Another way: give HandshakeAsync a Deadline and set Tcp.ReadTimeout to deadline.Remaining before each read. Add a trickling-client case to http_server_tls. If a per-read limit is what's intended instead, change ListenTls's comment, the stdlib.md sentence and the decisions.md row to say that each read of the handshake waits up to IdleTimeout.

Skeptic: I ran the reviewer's program, with a silent-client control added, at scratchpad/rv-https/tlsref/trickle.fb. Debug printed "sent 10 of 10 bytes; elapsed 6.4056234; open: True" and "silent open after 1.5 s: False". Release printed the same (elapsed 6.3996862, open: True). So a handshake that runs over 6 s with IdleTimeout set to 1 s stays open, while a silent client is closed. Answer (server.fb ~703) only sets stream.ReadTimeout = Idle and awaits TlsDuplex.HandshakeAsync. HandshakeAsync (wire.fb:51-63) has no overall deadline, and its own comment says "takes longer than the ReadTimeout", which is a per-read limit. That contradicts stdlib.md:1519 ("a handshake that fails or takes longer than `IdleTimeout` closes its connection"), ListenTls's doc comment (server.fb:553-555) and decisions.md:911 ("within IdleTimeout"). I rate it low, not medium. IdleTimeout is defined (server.fb:529) as how long a connection waits "for its next request, or for more of one", which is per gap. Plain HTTP head reading already lets a trickling client hold a slot the same way, so HTTPS adds no new slowloris exposure. The defect is that the code does not deliver what the docs promise. The fix is either a whole-handshake deadline or rewording the three docs.

## tls-3 (low): HttpServer's class comment was badly rewrapped when ListenTls was added

Status: fixed in 17c36556 (HttpServer's class comment rewrapped to 100 columns, its text as it was); comment formatting, which no program shows

Where: std/net/http/server.fb:448

Adding "or ListenTls over TLS" to HttpServer's doc comment broke its wrapping. Line 448 is now 119 characters, past the 100-column limit the rest of std/ keeps. The awk check `length > 101` over the diff's added lines flags only this line, apart from a panic .out line. Lines 451-452 are left as short fragments ("// A response has its" / "// Content-Length and Date set, ..."). It's comment formatting only, and no program can show it.

```
// Not a runtime defect: run this in the checkout to see it.
//   awk 'NR>=447 && NR<=452 {print NR": "length": "$0}' std/net/http/server.fb
Import std.core
Print("see server.fb lines 447 to 452")
```

Expected: The comment for HttpServer (server.fb:447-452) is wrapped at 100 columns like the rest of std.

Actual: Line 448 is 119 columns ("// where Listen has it listen, or ListenTls over TLS, as Serve runs. A request that breaks HTTP/1.1's rules is answered"), and lines 451-452 are broken short.

Suggested fix: Rewrap lines 447-452 into lines of 100 columns or less without changing the text.

Skeptic: Running awk on server.fb 447-452 at 4d8a2a7b shows line 448 is 119 columns ("// where Listen has it listen, or ListenTls over TLS, as Serve runs. A request that breaks HTTP/1.1's rules is answered"). At 75910bde the same comment line was within 100 columns, so the change broke the wrap. One correction to the finding: the short fragment "// A response has its" followed by "// Content-Length ..." was already there at 75910bde (lines 441-442), so the change didn't cause it, though a rewrap should fix it too. This is comment formatting only, the kind of minor fix CLAUDE.md says to make directly.

## Refuted by the skeptics (no action)

- tls-2: No test calls ListenTls(host, port, tls). I ran the reviewer's program (scratchpad/rv-https/tlsref/hostport.fb) and it printed "hello", so the overload works. It is true that no test calls ListenTls(host, port, tls): grep finds only the SocketAddress form in http_server_tls.fb:81 and panic_http_listen_tls_twice.fb:35. But no wrong behavior shows, the overload is three lines that mirror the tested one, and project practice does not require testing every overload: HttpServer.Listen(host, port) has no test calling it either (grep for erver.Listen( without SocketAddress finds nothing). Neither the milestones row nor stdlib.md requires per-overload tests. This is a coverage suggestion, not a defect.
- auto-8: No test covers the folder's fallbacks: an expired kept certificate, other names, or a key that isn't the chain's. Coverage gap only, not wrong behavior. The finding says the expired-certificate and other-names paths of Stored() work today, and the code agrees: Stored() returns Null on a names.txt mismatch and when NotAfter <= now. I checked that http_acme_auto.fb/.out cover only reuse ('made again'), so the gap is real. Nothing contradicts the spec, stdlib.md, or the milestones row, and the mismatched-key case is auto-2's, whose fix will need its own test.
