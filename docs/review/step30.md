# The review of M6 step 30's server half (std.net.http's HttpServer over HTTP/1.1, and std.net.ws's WebSockets)

19 confirmed findings: 1 high, 5 medium, 13 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## server-1 (high): A *name route that matches nothing beats the exact route: "/" loses to "/*all", and "/files" loses to "/files/*path"

Status: fixed in 749ee5c8: Better takes the shorter of two matches when one starts the other, since the longer one's last segment is a *name that matched none of the path; http_server_routes.fb has / beside /*all, /files beside /files/*path, /a/:b beside /a/:b/*rest and /a/fixed, and /x/:y/z beside /x/*rest, each added in the order that lost before

Where: std/net/http/server.fb:361

MatchOf gives an exact route's kinds, like [0] for "/files", and a rest route that matched an empty rest gives one more kind, like [0,2] for "/files/*path". Better finds them equal over the shorter length and then returns `a.Count > b.Count`, so the longer one, the *name route, always wins. That makes a root route unreachable next to a catch-all, and an exact route unreachable next to its own *rest. The order the routes were added doesn't matter. This contradicts HttpRouter's doc and stdlib.md ("the one whose segments are text first, segment by segment, takes it", "a parameter before the rest"). The same thing happens with "/a/:b" next to "/a/:b/*rest". Debug and release behave the same.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text

Function Text(text: String): HttpResponse => HttpResponse.Of(Body.Text(text))

Local server = HttpServer()
server.Get("/", Async Function(request: HttpRequest) => Text("root"))
server.Get("/*all", Async Function(request: HttpRequest) => Text("all {request.Param("all") ?? "?"}"))
server.Get("/files", Async Function(request: HttpRequest) => Text("files"))
server.Get("/files/*path", Async Function(request: HttpRequest) => Text("path {request.Param("path") ?? "?"}"))
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local client = HttpClient()
    Print((Await client.GetAsync("{base}/")).Text)
    Print((Await client.GetAsync("{base}/files")).Text)
    Print((Await client.GetAsync("{base}/files/a")).Text)
    Print((Await client.GetAsync("{base}/x/y")).Text)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: root
files
path a
all x/y

Actual: all 
path 
path a
all x/y

Suggested fix: When one match is a prefix of the other, the longer one can only end in a *name that matched an empty rest, so the shorter one should win. In Better, return `a.Count < b.Count` instead of `a.Count > b.Count`, and update its comment to match. Add these two cases to http_server.fb.

Skeptic: I ran rv-step30/server-verify/s1.fb, the finding's program plus /a/:b next to /a/:b/*rest, in debug and in release. Both printed `all ` / `path ` / `path a` / `all x/y` / `abrest ` / `abrest r`. So GET / reaches /*all instead of /, GET /files reaches /files/*path, and GET /a/q reaches /a/:b/*rest. The cause is Better in server.fb, lines 361-366: it ends with `Return a.Count > b.Count`, and MatchOf (lines 341-357) adds a trailing kind 2 when the rest is empty, so the *name route always wins the tie. That makes the exact routes unreachable, which contradicts HttpRouter's doc (text first, segment by segment). http_server.fb's test has /files/*path but no /files route, so nothing covers this.

## server-2 (medium): HttpResponse.Of takes any Int as a status and the server sends an invalid or interim status line as the final response

Status: fixed in 5d36c899: Of panics at the caller unless the status is from 200 to 599; panic_http_status.fb makes a 204 and a 599, then a 103

Where: std/net/http/server.fb:105

Of(status, body) doesn't check status, and ResponseBytes writes `HTTP/1.1 {status} ...` whatever it is. A status of 1000 or -5 gives a status line that isn't three digits, which no client can parse. A -5 also drops the body, because `status < 200` counts as bodiless. A 1xx such as 103 goes out as the only response to the request, so a keep-alive client waits forever for the final one. std's convention is to panic at the caller for a bad argument, as Redirect already does for a bad URL and MaxBodySize does for a negative size.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    stream.ReadTimeout = 2.Seconds
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function
Local server = HttpServer()
server.Get("/big", Async Function(request: HttpRequest) => HttpResponse.Of(1000, Body.Text("x")))
server.Get("/neg", Async Function(request: HttpRequest) => HttpResponse.Of(-5, Body.Text("x")))
server.Get("/info", Async Function(request: HttpRequest) => HttpResponse.Of(103, Body.Text("x")))
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    For path In ["/big", "/neg", "/info"]
        Print(Await Raw(port, "GET {path} HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"))
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: A panic at HttpResponse.Of(1000, ...) in the handler, along the lines of "a response's status is from 200 to 599, not 1000", and the same for -5 and 103.

Actual: HTTP/1.1 1000  | Content-Type: text/plain; charset=utf-8 | Content-Length: 1 | Connection: close |  | x
HTTP/1.1 -5  | Content-Type: text/plain; charset=utf-8 | Connection: close |  | 
HTTP/1.1 103 Early Hints | Content-Type: text/plain; charset=utf-8 | Connection: close |  | 

Suggested fix: Mark Of(status, body) @PanicsAtCaller and panic unless 200 <= status <= 599. A 101 comes only from WebSocket.AcceptAsync, which writes its own. Document this in Of's comment and in stdlib.md, and add a panic_http_status program.

Skeptic: I ran s2.fb in debug. It printed `HTTP/1.1 1000  | ... Content-Length: 1 ... | x`, then `HTTP/1.1 -5  | ... Connection: close |  |` with no Content-Length and no body, then `HTTP/1.1 103 Early Hints | ...` as the final response. In the code, Of (lines 105-115) never checks the status, and ResponseBytes (lines 673 and 689) writes `HTTP/1.1 {status}` as given and treats `status < 200` as bodiless. The server sends a status line no client can parse, or an interim 1xx as the only answer, which leaves a keep-alive client waiting. A bad argument should panic at the caller, as Redirect's and MaxBodySize's already do.

## server-3 (medium): An HTTP/1.0 request with two Host fields is answered, and the docs say a request without a Host is a 400 when an HTTP/1.0 one isn't

Status: fixed in 5d36c899: two Hosts are a 400 whatever the version, and only an HTTP/1.1 request needs one; the class comment and stdlib.md say so; http_server.fb sends HTTP/1.0 with two Hosts (a 400) and with none (a 200)

Where: std/net/http/server.fb:543

Read refuses only when `hosts != 1 && head.Parts[2] == "HTTP/1.1"`. RFC 9112 section 3.2 says a server MUST answer 400 to any request with more than one Host field, whatever its version. Two conflicting Hosts are a request-smuggling and cache-poisoning vector. The class comment ("one without a Host, or with two, is a 400") and stdlib.md ("a request without one Host is a 400") also say that an HTTP/1.0 request without a Host is refused. It isn't, and the RFC agrees with the code there.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    stream.ReadTimeout = 2.Seconds
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function
Local server = HttpServer()
server.Get("/", Async Function(request: HttpRequest) => HttpResponse.Of(Body.Text("root")))
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print(Await Raw(port, "GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n"))
    Print(Await Raw(port, "GET / HTTP/1.0\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 
HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 4 | Connection: close |  | root
(the second line is right, but the docs say it should be a 400)

Actual: HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 4 | Connection: close |  | root
HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 4 | Connection: close |  | root

Suggested fix: Use `If hosts > 1 || (hosts == 0 && head.Parts[2] == "HTTP/1.1")`. Reword the class comment and stdlib.md to say that an HTTP/1.1 request without a Host, or any request with two, is a 400. Add the HTTP/1.0 two-Host case to http_server.fb's refusals.

Skeptic: I ran s3.fb in debug and in release. `GET / HTTP/1.0` with `Host: a` and `Host: b` got `HTTP/1.1 200 OK ... root`. Line 543's check, `If hosts != 1 && head.Parts[2] == "HTTP/1.1"`, skips HTTP/1.0 entirely. RFC 9112 section 3.2 requires a 400 for any request with more than one Host. The class comment (line 387, "one without a Host, or with two, is a 400") and stdlib.md ("a request without one Host is a 400") also don't match the code. For HTTP/1.0 with no Host, the code agrees with the RFC (a 200), but the docs say it's a 400 too, so they need rewording.

## wire-1 (medium): An empty Transfer-Encoding or Content-Length value gets past the rule against both framings

Status: fixed in 909a29ee: BodyFraming refuses any Transfer-Encoding field beside a Content-Length, and one with no codings, and reads each comma-separated item of each Content-Length, refusing an empty one; http_server.fb sends 'Transfer-Encoding: ,', an empty Transfer-Encoding, an empty second Content-Length, and 'Content-Length: 3,' (each a 400), and 'Content-Length: 3, 3' (a 200)

Where: std/net/http/wire.fb:87

BodyFraming decides from Items(), and Items() drops empty items. So `Transfer-Encoding: ,` (or an empty value) next to a Content-Length counts as no Transfer-Encoding, and the request is framed by Content-Length. The comment says both at once is refused because that is how requests get smuggled past a proxy, and docs/stdlib.md says bodies use a Content-Length or chunks 'and never both'. The same gap lets an empty second Content-Length through beside `Content-Length: 3`. A proxy in front may read these fields differently, which is the smuggling case the refusal exists to stop.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Post("/echo", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("{request.Verb} {request.Version} [{request.Text}]"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") && !line.StartsWith("Content-Type") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Local host = "Host: localhost\r\n"
    Print("empty TE + CL:", Await Raw(port, "POST /echo HTTP/1.1\r\n{host}" +
                                            "Transfer-Encoding: ,\r\nContent-Length: 3\r\n\r\nabc"))
    Print("CL + empty CL:", Await Raw(port, "POST /echo HTTP/1.1\r\n{host}Content-Length: 3\r\n" +
                                            "Content-Length: \r\n\r\nabc"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: empty TE + CL: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 
CL + empty CL: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 

Actual: empty TE + CL: HTTP/1.1 200 OK | Content-Length: 19 |  | POST HTTP/1.1 [abc]
CL + empty CL: HTTP/1.1 200 OK | Content-Length: 19 |  | POST HTTP/1.1 [abc]
(the same in debug and release)

Suggested fix: In BodyFraming, test Contains("Transfer-Encoding") rather than codings.Count > 0. A Transfer-Encoding with no codings, or one next to a Content-Length, is a 400. Also refuse a Content-Length whose raw value, or any comma-separated item of it, is empty, instead of relying on Items(), which drops empty items. Add both cases to http_server.fb's refusals.

Skeptic: Ran rv-step30/wire/w1.fb in debug and release. Every case got 200 OK with 'POST HTTP/1.1 [abc]': 'Transfer-Encoding: ,' with CL 3, an empty 'Transfer-Encoding:' with CL 3, CL 3 plus an empty second CL, and 'Content-Length: 3,'. In wire.fb:87-88, BodyFraming tests Items("Transfer-Encoding").Count > 0, and Items (lines 66-76) drops empty items, so a Transfer-Encoding field that is present but empty never triggers the both-framings refusal at line 90. Line 106 refuses an empty CL only when no CL item is left at all. This contradicts the comment at lines 84-85 and stdlib.md's 'and never both'. RFC 9112 6.3 also says an invalid Content-Length (1*DIGIT, and empty isn't one) is an unrecoverable 400.

## wire-2 (medium): A chunked body's lines accept a bare LF and invalid chunk extensions

Status: fixed in 909a29ee: a chunk's size line and its data's end must end with CR LF, and ChunkSize reads RFC 9112's extensions, `;name` or `;name=value` with a token or quoted string and spaces around, refusing anything else; http_server.fb sends a body of bare LFs, a chunk ended with a bare LF, '3 junk', '3;a\rb', and an unclosed quoted value (each a 400), and '3 ; a ; b = c ; d="x \" y"' (a 200)

Where: std/net/http/wire.fb:455

ReadChunksAsync reads each chunk-size line and each chunk's terminator with ReadLineAsync. That function accepts a bare LF and never checks `ending`, so `3\nabc\n0\n\n` parses as a valid body. RFC 9112 section 2.2 lets a recipient accept a bare LF only in the start-line and fields; section 7.1 requires CRLF after the chunk size and after the chunk data. ChunkSize also stops at the first ';', space, or tab after the hex digits and ignores everything after it. So `3 junk` is read as size 3, and `3;a\rb`, with a CR inside the extension, is accepted too. These are the chunk-parsing differences behind the 'funky chunks' request-smuggling attacks when a proxy sits in front.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Post("/echo", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("{request.Verb} {request.Version} [{request.Text}]"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") && !line.StartsWith("Content-Type") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Local start = "POST /echo HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: chunked\r\n\r\n"
    Print("chunk bare LF:", Await Raw(port, start + "3\nabc\n0\n\n"))
    Print("chunk junk:", Await Raw(port, start + "3 junk\r\nabc\r\n0\r\n\r\n"))
    Print("chunk ext CR:", Await Raw(port, start + "3;a\rb\r\nabc\r\n0\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: chunk bare LF: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 
chunk junk: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 
chunk ext CR: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 

Actual: chunk bare LF: HTTP/1.1 200 OK | Content-Length: 19 |  | POST HTTP/1.1 [abc]
chunk junk: HTTP/1.1 200 OK | Content-Length: 19 |  | POST HTTP/1.1 [abc]
chunk ext CR: HTTP/1.1 200 OK | Content-Length: 19 |  | POST HTTP/1.1 [abc]
(the same in debug and release)

Suggested fix: In ReadChunksAsync, require ending == 2 for each chunk-size line and for each chunk's terminating line, and refuse a bare LF with a 400. In ChunkSize, after the hex digits allow only BWS then ';' extensions of the form chunk-ext-name [BWS '=' BWS (token / quoted-string)]. Refuse anything else, control characters included, with a 400. Add these cases to http_server.fb.

Skeptic: Ran rv-step30/wire/w2.fb in debug and release. '3\nabc\n0\n\n', '3 junk\r\n...' and '3;a\rb\r\n...' all got 200 OK with [abc]. ReadChunksAsync (wire.fb:455, 472) never looks at the line ending ReadLineAsync returns, so a bare LF is accepted. ChunkSize (line 327) breaks at the first ';', space or tab after the digits and ignores the rest, so 'junk' and a CR inside an extension pass. RFC 9112 7.1 requires CRLF after chunk-size [chunk-ext] and after chunk-data, and section 2.2's bare-LF leniency covers only the start-line and fields. This leniency is the chunk-parsing gap that smuggling attacks use.

## wire-3 (medium): An HTTP/1.0 request with Transfer-Encoding is read as chunked and its connection kept open

Status: fixed in 909a29ee: an HTTP/1.0 request with a Transfer-Encoding is a 400, and its connection closes; http_server.fb's case pipelines a GET after it, which isn't answered

Where: std/net/http/wire.fb:86

RFC 9112 section 6.1 says a server that receives an HTTP/1.0 message with a Transfer-Encoding field MUST treat its framing as faulty, even when a Content-Length is present, and close the connection after it. BodyFraming never sees the version, so an HTTP/1.0 request with `Connection: keep-alive` and `Transfer-Encoding: chunked` is read as chunked. Its connection then stays open, and the next pipelined request on it is answered. This is another framing difference a proxy in front could be made to disagree with.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Post("/echo", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("{request.Verb} {request.Version} [{request.Text}]"))
End Function)
server.Get("/v", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("{request.Version}"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") && !line.StartsWith("Content-Type") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print("1.0 chunked:", Await Raw(port, "POST /echo HTTP/1.0\r\nConnection: keep-alive\r\n" +
                                          "Transfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n" +
                                          "GET /v HTTP/1.0\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 1.0 chunked: HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 
(or at least one response with Connection: close and no answer to the second request)

Actual: 1.0 chunked: HTTP/1.1 200 OK | Content-Length: 19 | Connection: keep-alive |  | POST HTTP/1.0 [abc]HTTP/1.1 200 OK | Content-Length: 8 | Connection: close |  | HTTP/1.0
(the same in debug and release)

Suggested fix: Have BodyFraming check the head's version (Parts[2]). For HTTP/1.0 with any Transfer-Encoding, throw WireError(400, ...) so the connection is refused and closed. Add the case to http_server.fb's refusals.

Skeptic: Ran rv-step30/wire/w3.fb in debug and release. Output: 'HTTP/1.1 200 OK | Content-Length: 19 | Connection: keep-alive | | POST HTTP/1.0 [abc]HTTP/1.1 200 OK | Content-Length: 8 | Connection: close | | HTTP/1.0'. The HTTP/1.0 request with Transfer-Encoding was read as chunked, its connection was kept alive, and the pipelined second request was answered. BodyFraming (wire.fb:86) never sees the version. RFC 9112 6.1 says a server receiving an HTTP/1.0 message with Transfer-Encoding MUST treat its framing as faulty and close the connection.

## server-4 (low): HttpResponse.Redirect takes a status that isn't a redirect

Status: fixed in 5d36c899: Redirect panics at the caller unless the status is from 300 to 399; panic_http_redirect_status.fb makes a 301 and a 308, then a 200

Where: std/net/http/server.fb:123

Redirect(url, status) says it gives "a 302 Found unless status says another", but it passes any status to Of. Redirect("/x", 200) gives a 200 OK with a Location header, which no client follows. The argument is wrong, so std's convention is to panic at the caller, as Redirect already does for a URL with a control character.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    stream.ReadTimeout = 2.Seconds
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function
Local server = HttpServer()
server.Get("/moved", Async Function(request: HttpRequest) => HttpResponse.Redirect("/x", 200))
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print(Await Raw(port, "GET /moved HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: A panic at Redirect("/x", 200), along the lines of "a redirect's status is from 300 to 399, not 200".

Actual: HTTP/1.1 200 OK | Location: /x | Content-Length: 0 | Connection: close |  | 

Suggested fix: In Redirect, panic unless 300 <= status <= 399, say so in its comment, and extend panic_http_redirect (or add a second panic program).

Skeptic: I ran s4.fb in debug. `Redirect("/x", 200)` sent `HTTP/1.1 200 OK | Location: /x | Content-Length: 0`. Redirect (lines 120-127) passes status straight to Of without checking it, so the "redirect" it documents is a 200 with a Location that no client follows. It's already @PanicsAtCaller (for the URL), so a status outside 300-399 should panic there too. It only happens when the caller passes a wrong argument, so low.

## server-5 (low): A route path that names two parameters the same is accepted, and the first one's value is lost

Status: fixed in 5d36c899: Add panics when the joined path, its group's prefix's included, names a parameter twice; panic_http_route_param_twice.fb adds a route with two names, then Group("/teams/:id").Get("/members/:id")

Where: std/net/http/server.fb:293

SegmentsOf checks each :name and *name on its own and never checks that the names are distinct, so "/users/:id/posts/:id" is accepted. The same goes for a group prefix and a route that both use :id, as in Group("/teams/:id").Get("/members/:id"). In Routed, Bind overwrites, so Param("id") gives only the last segment and the first can't be read. The docs say a bad path panics where it's added, and this is a path whose parameters can't all be read.

```
Import std.core
Import std.net
Import std.net.http

Local server = HttpServer()
server.Get("/users/:id/posts/:id", Async Function(request: HttpRequest) => HttpResponse.Of(204))
Print("added")
Local group = server.Group("/teams/:id")
group.Get("/members/:id", Async Function(request: HttpRequest) => HttpResponse.Of(204))
Print("added again")
```

Expected: A panic at the first Get, along the lines of "\"/users/:id/posts/:id\" has two parameters named id".

Actual: added
added again

Suggested fix: In Add, once the prefix and the path are joined, collect the parameter names from the segments and panic when one repeats. Add a panic_http_route_twice_name program.

Skeptic: I ran s5.fb in debug and it printed `added` and `added again`, so neither `/users/:id/posts/:id` nor Group("/teams/:id").Get("/members/:id") panics. SegmentsOf (lines 293-309) and Add (lines 244-256) never check that parameter names are distinct. Routed binds each segment in turn (line 628) and Bind (line 98) overwrites, so Param("id") gives only the last one and the first can't be read. That's a bad path the docs say panics where it's added.

## server-6 (low): An absolute-form target whose scheme is in uppercase is refused with a 400

Status: fixed in 5d36c899: an absolute-form target's scheme is compared in lowercase; http_server.fb sends GET HTTP://localhost/users/caps

Where: std/net/http/server.fb:569

Read only accepts an absolute-form target that starts with the exact strings "http://" or "https://". RFC 9110 section 4.2.3 says the scheme is case-insensitive, so "GET HTTP://localhost/users/1 HTTP/1.1" is a valid request, and the server answers it with a 400.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    stream.ReadTimeout = 2.Seconds
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Local lines: String[] = []
    For line In answer.Split("\r\n")
        If !line.StartsWith("Date: ") Then lines.Add(line)
    End For
    Return lines.Join(" | ")
End Function
Local server = HttpServer()
server.Get("/users/:id", Async Function(request: HttpRequest) => HttpResponse.Of(Body.Text("user {request.Param("id") ?? "?"}")))
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print(Await Raw(port, "GET http://localhost/users/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"))
    Print(Await Raw(port, "GET HTTP://localhost/users/1 HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 6 | Connection: close |  | user 1
HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 6 | Connection: close |  | user 1

Actual: HTTP/1.1 200 OK | Content-Type: text/plain; charset=utf-8 | Content-Length: 6 | Connection: close |  | user 1
HTTP/1.1 400 Bad Request | Content-Length: 0 | Connection: close |  | 

Suggested fix: Compare the start case-insensitively, for example `target.ToLower().StartsWith("http://") || ...StartsWith("https://")`. Url.Parse already lowercases the scheme.

Skeptic: I ran s6.fb in debug. The `http://localhost/users/1` target got `200 OK ... user 1`, and `HTTP://localhost/users/1` got `HTTP/1.1 400 Bad Request`. Line 569 compares the exact strings with `target.StartsWith("http://") || target.StartsWith("https://")`, but RFC 9110 section 4.2.3 says the scheme is case-insensitive, so a valid absolute-form request is refused.

## server-7 (low): The METHODS Const is never used, and its comment says Route takes only those methods when it takes any token

Status: fixed in 5d36c899: METHODS is gone, and HttpRequest's comment is whole lines; comment and dead code only, so no test

Where: std/net/http/server.fb:33

`Const METHODS = ["GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"]` is commented "The methods a route can have, as Route takes them", but nothing in std refers to it (grep finds only its declaration). Route accepts any token, such as PROPFIND or MKCOL. Separately, HttpRequest's comment on lines 35-38 breaks off mid-sentence ("its target, and the" / "target's path"), left over from an edit.

```
Import std.core
Import std.net
Import std.net.http

Local server = HttpServer()
server.Route("PROPFIND", "/", Async Function(request: HttpRequest) => HttpResponse.Of(204))
server.Route("MKCOL", "/", Async Function(request: HttpRequest) => HttpResponse.Of(204))
Print("added")
```

Expected: Code that matches its comment: either Route accepts only METHODS, or, as the docs intend, there is no METHODS Const claiming it does.

Actual: added (Route accepts methods outside METHODS, and METHODS is dead code)

Suggested fix: Delete the METHODS Const and its comment, and rewrap HttpRequest's comment into whole lines.

Skeptic: Grepping rev-5/std for METHODS finds only its declaration, at server.fb line 33. I ran s7.fb in debug and it printed `added`: Route takes PROPFIND and MKCOL, as its own comment at line 216 says it should ("any token"), and stdlib.md agrees. That makes METHODS's comment, "The methods a route can have, as Route takes them", false, and the Const is dead code. HttpRequest's comment at lines 35-38 also breaks off mid-line ("its target, and the" / "target's path, ..."), left over from an edit.

## wire-4 (low): Linger isn't 'a second at most': a client that trickles bytes keeps it going

Status: fixed in 909a29ee: Linger and LingerAsync read until a Deadline a second off, setting each read's timeout to what's left; net_websocket_linger.fb's client trickles a byte every 300 ms for 3.6 s, and the refused Accept gives back in under 2.5 s

Where: std/net/http/wire.fb:358

Linger and LingerAsync set ReadTimeout to 1 second, then keep reading until 1 MiB or a read times out. The timeout applies to each read, not to the whole linger, so a client that sends a byte every 300 ms keeps the refused connection lingering as long as it keeps sending, up to 1 MiB (hours at that rate). The comment says 'for a second at most'. On the blocking WebSocket.Accept path, the accepting thread is held the whole time. On HttpServer and AcceptAsync, each such connection stays open well past its refusal.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text
Import std.time

Const ports = Channel<UInt16>(1)
Const took = Channel<String>(1)
Local server = Thread.Start(Function()
    Try
        Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
        ports.Send(listener.LocalAddress.Port)
        Local stream = listener.Accept()
        Local watch = Stopwatch.StartNew()
        Try
            WebSocket.Accept(stream)
        Catch e As NetError
        End Try
        Local seconds = watch.Elapsed.TotalSeconds
        took.Send("Accept lingered over 2 seconds: {seconds > 2.0}")
    Catch e As Error
        took.Send("server failed: {e.Message}")
    End Try
End Function)

Try
    Local stream = TcpStream.Connect(SocketAddress(IpAddress.Loopback, ports.Receive() ?? 0))
    stream.WriteBytes("POST / HTTP/1.1\r\nHost: a\r\n\r\n".Bytes)
    For i = 1 To 12
        Sleep(300.Milliseconds)
        Try
            stream.WriteBytes("x".Bytes)
        Catch e As IOError
            Break
        End Try
    End For
    stream.Close()
Catch e As Error
    Print("client failed: {e.Message}")
End Try

server.Join()
Print(took.Receive() ?? "")
```

Expected: Accept lingered over 2 seconds: False

Actual: Accept lingered over 2 seconds: True

Suggested fix: Give Linger and LingerAsync a deadline: Instant.Now() plus LINGER_SECONDS before the loop. Before each read, set ReadTimeout to the time left, and break once it's gone or a read times out. Add a test whose client trickles bytes and checks that the refusal closes within about a second.

Skeptic: Ran rv-step30/wire/w4.fb in debug and release. Output: 'Accept lingered over 2 seconds: True (3.75...)' both times. The client sent a byte every 300 ms for 3.6 s, and the refused Accept lingered until the client stopped. Linger and LingerAsync (wire.fb:358-390) set a 1 s ReadTimeout per read with no overall deadline, which contradicts the comment's 'for a second at most'. I rated it low: a slow client can already hold a connection before it is refused, so this adds little new exposure.

## wire-5 (low): A Transfer-Encoding whose last coding isn't chunked gets a 501, not the 400 RFC 9112 requires

Status: fixed in 909a29ee: a last coding that isn't chunked is a 400, chunked twice a 400, and another coding before chunked a 501; http_server.fb sends 'chunked, gzip' and 'chunked, chunked' (400s), and 'gzip, chunked' stays a 501

Where: std/net/http/wire.fb:93

RFC 9112 section 6.3 says that when a request's Transfer-Encoding is present and chunked isn't the final coding, the server MUST respond with 400 and close the connection. BodyFraming throws 501 for the first coding that isn't chunked before it checks where chunked comes. So `chunked, gzip` gets a 501. A 501 is right only for an unknown coding when chunked is last, as in `gzip, chunked`.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Post("/echo", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("[{request.Text}]"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Return answer.Split("\r\n")[0]
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print("chunked, gzip:", Await Raw(port, "POST /echo HTTP/1.1\r\nHost: localhost\r\n" +
                                            "Transfer-Encoding: chunked, gzip\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: chunked, gzip: HTTP/1.1 400 Bad Request

Actual: chunked, gzip: HTTP/1.1 501 Not Implemented

Suggested fix: In BodyFraming, first check that the last coding is "chunked" and throw a 400 if it isn't. Then throw a 501 for any other coding before it, and a 400 for chunked more than once. Add the case to http_server.fb.

Skeptic: Ran rv-step30/wire/w567.fb in debug and release. 'Transfer-Encoding: chunked, gzip' got 'HTTP/1.1 501 Not Implemented'. The loop at wire.fb:93-98 throws 501 for the first non-chunked coding before anything checks that chunked is last. RFC 9112 6.3 says that when chunked isn't the final coding in a request, the server MUST respond 400 and close. 'gzip, chunked' getting 501 is fine.

## wire-6 (low): HTTP/1.2 (a higher 1.x minor version) is refused with a 505 instead of being handled as 1.1

Status: fixed in 909a29ee: RequestLine reads HTTP/1.x past 1.1 as HTTP/1.1, and keeps the 505 for other major versions; http_server.fb's GET /version with HTTP/1.2 answers HTTP/1.1, and HTTP/2.0 stays a 505

Where: std/net/http/wire.fb:203

RFC 9110 section 2.5 says a recipient that receives a message with a major version it implements and a higher minor version SHOULD process it as the highest minor version it has. So an HTTP/1.2 request should be handled as HTTP/1.1. RequestLine accepts only exactly HTTP/1.0 or HTTP/1.1 and answers every other well-formed version, HTTP/1.2 included, with a 505.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Get("/v", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("{request.Version}"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Return answer.Split("\r\n")[0]
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Print("1.2:", Await Raw(port, "GET /v HTTP/1.2\r\nHost: localhost\r\n\r\n"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 1.2: HTTP/1.1 200 OK

Actual: 1.2: HTTP/1.1 505 HTTP Version Not Supported

Suggested fix: In RequestLine, accept HTTP/1.<digit> and record a minor version above 1 as "HTTP/1.1" in Parts[2], so the server and the handshake treat it as 1.1. Keep the 505 for other major versions. Add the case to http_server.fb.

Skeptic: Ran rv-step30/wire/w567.fb in debug and release. 'GET /v HTTP/1.2' got 'HTTP/1.1 505 HTTP Version Not Supported'. RequestLine (wire.fb:203-207) accepts only exactly 1.0 or 1.1. RFC 9110 2.5 says a recipient SHOULD process a higher minor version of a major version it implements as its highest minor version, so HTTP/1.2 should be handled as 1.1. The lanes' listed decisions don't cover this; only the code comment states the 505 policy.

## wire-7 (low): Leading zeros push valid chunk sizes and Content-Lengths past the digit limits

Status: fixed in 909a29ee: ChunkSize and DigitsOf skip leading zeros before counting digits against their 15 and 18, and DigitsOf's comment says so; http_server.fb sends a 16-digit chunk size and a 19-character Content-Length, both of 3 (200s)

Where: std/net/http/wire.fb:332

ChunkSize counts every hex digit against its limit of 15, leading zeros included, so a 16-digit size such as `0000000000000003` is refused with a 413 'a chunk is too big', though it means 3. DigitsOf refuses any Content-Length over 18 characters, so `Content-Length: 0000000000000000003` (19 characters, meaning 3) is a 400. Both are valid under RFC 9112's grammars (1*HEXDIG, 1*DIGIT). DigitsOf's comment, 'one past what 18 digits hold', also doesn't describe what the code does.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text
Import std.time

Local server = HttpServer()
server.Post("/echo", Async Function(request: HttpRequest): HttpResponse
    Return HttpResponse.Of(Body.Text("[{request.Text}]"))
End Function)

Async Function Raw(port: UInt16, request: String): String Throws Error
    Local stream = Try Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, port))
    Try Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Try Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Try Await ReadAllAsync(stream)) ?? "?"
    Return answer.Split("\r\n")[0]
End Function

Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local port = server.LocalAddress.Port
    Local host = "Host: localhost\r\n"
    Print("chunk 16 digits:", Await Raw(port, "POST /echo HTTP/1.1\r\n{host}" +
                                              "Transfer-Encoding: chunked\r\n\r\n" +
                                              "0000000000000003\r\nabc\r\n0\r\n\r\n"))
    Print("CL 19 chars:", Await Raw(port, "POST /echo HTTP/1.1\r\n{host}" +
                                          "Content-Length: 0000000000000000003\r\n\r\nabc"))
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: chunk 16 digits: HTTP/1.1 200 OK
CL 19 chars: HTTP/1.1 200 OK

Actual: chunk 16 digits: HTTP/1.1 413 Content Too Large
CL 19 chars: HTTP/1.1 400 Bad Request

Suggested fix: In ChunkSize and DigitsOf, skip leading zeros before counting digits, and count only the significant ones against the 15 and 18 limits. Reword DigitsOf's comment to say what it refuses. Add the cases to http_server.fb.

Skeptic: Ran rv-step30/wire/w567.fb in debug and release. A chunk size of '0000000000000003' got 'HTTP/1.1 413 Content Too Large', and 'Content-Length: 0000000000000000003' got 'HTTP/1.1 400 Bad Request'. ChunkSize (wire.fb:331-332) counts leading zeros against its 15-digit limit, and DigitsOf (line 303) refuses any text longer than 18 characters. Both values are valid under 1*HEXDIG and 1*DIGIT and mean 3, and a 413 for a 3-byte chunk is the wrong status. DigitsOf's comment, 'one past what 18 digits hold', doesn't match its length check.

## wire-8 (low): The WebSocket handshake's 405 has no Allow field

Status: fixed in bef0a604: the handshake's 405 has Allow: GET; net_websocket_frames.out's POST case shows it

Where: std/net/ws/handshake.fb:108

RFC 9110 section 15.5.6 says an origin server MUST generate an Allow field in a 405 response. When a handshake's method isn't GET, AnswerOf refuses it with 405 Method Not Allowed, but adds only Content-Length and Connection, never `Allow: GET`. net_websocket_frames.out records the 405 without Allow, so the test locks in the omission. HttpServer's own 405 does set Allow.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text

Async Function Serve(listener: TcpListener) Throws Error
    Try
        Try Await WebSocket.AcceptAsync(Try Await listener.AcceptAsync())
    Catch e As NetError
    End Try
End Function

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local served = Serve(listener)
    Local stream = Await TcpStream.ConnectAsync(listener.LocalAddress)
    Local request = "POST /chat HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n" +
                    "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                    "Sec-WebSocket-Version: 13\r\n\r\n"
    Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Await stream.ShutdownWriteAsync()
    Local answer = String.FromUtf8(Await ReadAllAsync(stream)) ?? "?"
    Print(answer.Split("\r\n")[0])
    Print("Allow:", answer.Contains("\r\nAllow: GET\r\n"))
    Try? Await served
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: HTTP/1.1 405 Method Not Allowed
Allow: True

Actual: HTTP/1.1 405 Method Not Allowed
Allow: False

Suggested fix: In AnswerOf's method check, add Names "Allow" and Values "GET" before Refuse(405, ...). Update net_websocket_frames.out's POST case to show `Allow: GET`.

Skeptic: Ran rv-step30/wire/w8.fb in debug and release. Output: 'HTTP/1.1 405 Method Not Allowed' then 'Allow: False'. AnswerOf (handshake.fb:107-109) refuses with 405, and Answer.Bytes adds only Content-Length and Connection. RFC 9110 15.5.6 says an origin server MUST generate Allow in a 405. tests/programs/net_websocket_frames.out:133 records '405 Method Not Allowed | Content-Length: 0 | Connection: close' with no Allow.

## wire-9 (low): HeadParser's IsEmpty comment sits above LongLine

Status: fixed in 909a29ee: IsEmpty's comment is above IsEmpty; comment only, so no test

Where: std/net/http/wire.fb:134

In HeadParser, the comment 'Whether it has nothing yet but empty lines.' (line 134) describes IsEmpty, but it sits above LongLine's own comment and Property. IsEmpty (line 141) has no comment. This is a comment-only problem. The program below shows the behavior that comment describes: a connection that sends only empty lines and then ends counts as 'nothing yet', so it closes before it asked.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text

Async Function Serve(listener: TcpListener): String Throws Error
    Try
        Try Await WebSocket.AcceptAsync(Try Await listener.AcceptAsync())
    Catch e As NetError
        Return "{e.Kind}"
    End Try
    Return "opened"
End Function

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local served = Serve(listener)
    Local stream = Await TcpStream.ConnectAsync(listener.LocalAddress)
    Await stream.WriteAsync(Buffer<UInt8>.From("\r\n\r\n".Bytes))
    Await stream.ShutdownWriteAsync()
    Await ReadAllAsync(stream)
    Print(Await served)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: CLOSED, and the comment 'Whether it has nothing yet but empty lines.' directly above `Package Property IsEmpty: Bool`

Actual: CLOSED, with that comment above LongLine's comment and `Package Property LongLine: Int` (lines 134-139), and IsEmpty uncommented

Suggested fix: Move line 134's comment down so it sits directly above `Package Property IsEmpty: Bool`.

Skeptic: wire.fb:134 'Whether it has nothing yet but empty lines.' sits directly above LongLine's own comment (135-136) and its Property (137), and IsEmpty (141) has no comment. The behavior is as described: rv-step30/wire/w9.fb printed CLOSED in debug and release. This is comment-only.

## ws-1 (low): A valid handshake carrying Content-Length: 0 is refused with 400 as "asked to upgrade with a body"

Status: fixed in bef0a604: a handshake is refused for a body only when it has a Transfer-Encoding or a Content-Length that isn't 0; net_websocket_frames.fb's 'a body' sends 5 bytes and 'a chunked body' a chunked one (400s), and 'an empty body' a Content-Length of 0, which opens

Where: std/net/ws/handshake.fb:138

AnswerOf refuses any upgrade request that has a Content-Length or Transfer-Encoding field, so `Content-Length: 0` gets a 400 too. That header means there is no body, so the problem text ("asked to upgrade with a body") is false. RFC 6455 section 4.2.1 doesn't forbid the field, so this request is a valid WebSocket handshake. docs/stdlib.md lists 400, 405, and 426 only for requests that aren't a WebSocket's. Accept, AcceptAsync(stream), and AcceptAsync(request) all go through AnswerOf. net_websocket_frames.fb's "a body" case uses Content-Length: 0, so the test locks the refusal in instead of catching it.

```
Import std.core
Import std.collections
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text

Async Function Served(listener: TcpListener): String Throws Error
    Try
        Local ws = Try Await WebSocket.AcceptAsync(Try Await listener.AcceptAsync())
        Return "server: opened"
    Catch e As NetError
        Return "server: {e.Kind}"
    End Try
End Function

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local served = Served(listener)
    Local stream = Await TcpStream.ConnectAsync(SocketAddress(IpAddress.Loopback, listener.LocalAddress.Port))
    Local request = "GET /chat HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n" +
                    "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                    "Sec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n"
    Await stream.WriteAsync(Buffer<UInt8>.From(request.Bytes))
    Local line: UInt8[] = []
    Local one = Buffer<UInt8>(1)
    While line.Count < 2 || line[line.Count - 2..] != [13 As UInt8, 10]
        If Await stream.ReadAsync(one) == 0 Then Break
        line.Add(one[0])
    End While
    Print("client:", (String.FromUtf8(line) ?? "").TrimEnd())
    Print(Await served)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: client: HTTP/1.1 101 Switching Protocols
server: opened

Actual: client: HTTP/1.1 400 Bad Request
server: OTHER

Suggested fix: Refuse only a request that actually has a body: a Transfer-Encoding, or a Content-Length that isn't 0 (BodyFraming gives NONE or a length of 0). Change net_websocket_frames.fb's "a body" case to Content-Length: 5 and add a Content-Length: 0 case that opens.

Skeptic: Ran the finding's program, unchanged, as rv-step30/wsv/ws1.fb in a debug build. It printed "client: HTTP/1.1 400 Bad Request" and "server: OTHER". The cause is std/net/ws/handshake.fb:138, `If head.Contains("Content-Length") || head.Contains("Transfer-Encoding")`, which refuses with "asked to upgrade with a body" without checking the value. So Content-Length: 0, which means no body, is refused too. RFC 6455 4.2.1 doesn't forbid the field, and docs/stdlib.md gives 400/405/426 only for "a request that isn't a WebSocket's". This request is one, so the refusal contradicts the doc and the problem text is false. tests/programs/net_websocket_frames.fb:279's "a body" case uses Content-Length: 0, so the test locks the refusal in. I rate it low rather than medium because real clients rarely send Content-Length: 0 on an upgrade GET.

## ws-2 (low): After Close(), a connection that ends without the other side's Close gives Null instead of throwing NetError CLOSED

Status: fixed in bef0a604: a connection that ends without the other side's Close throws a NetError of CLOSED, CLOSING or OPEN; net_websocket.fb's server drops the connection after the client's Close, and ReceiveAsync throws CLOSED with 1006

Where: std/net/ws/websocket.fb:532

Took sets Failure (a NetError of CLOSED) only when Status is OPEN. Once Close/CloseAsync has made the WebSocket CLOSING, a peer that drops TCP without answering makes Receive/ReceiveAsync return Null, the same as a clean close handshake. Only CloseCode (1006) tells the two apart. This contradicts three docs: Receive's own comment ("Null once the other side's Close has come"), the class comment ("every error is a NetError: of CLOSED for a connection that ended without a Close frame"), and docs/stdlib.md ("a connection that ends without a Close throws one of CLOSED"). No test covers the CLOSING case. Debug and release give the same output.

```
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text
Import std.text.encoding

// A raw server: answers the handshake, then sends `frames`, then does `ending`:
// "close" closes TCP at once, "wait" reads until the client ends.
Async Function Raw(listener: TcpListener, frames: UInt8[][], ending: String) Throws Error
    Local stream = Try Await listener.AcceptAsync()
    Local head: UInt8[] = []
    Local one = Buffer<UInt8>(1)
    While head.Count < 4 || head[head.Count - 4..] != [13 As UInt8, 10, 13, 10]
        If Try Await stream.ReadAsync(one) == 0 Then Return
        head.Add(one[0])
    End While
    Local key = ""
    For line In (String.FromUtf8(head) ?? "").Split("\r\n")
        If line.StartsWith("Sec-WebSocket-Key: ") Then key = line.After(": ") ?? ""
    End For
    Local answer = Base64.Encode(Sha1.Hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
    Local text = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
                 "Connection: Upgrade\r\nSec-WebSocket-Accept: {answer}\r\n\r\n"
    Try Await stream.WriteAsync(Buffer<UInt8>.From(text.Bytes))
    For f In frames
        Try Await stream.WriteAsync(Buffer<UInt8>.From(f))
    End For
    If ending == "close"
        stream.Close()
        Return
    End If
    Try Await ReadAllAsync(stream)
End Function

Async Function Run(name: String, frames: UInt8[][], ending: String, closeFirst: Bool) Throws Error
    Local listener = Try TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local served = Raw(listener, frames, ending)
    Local ws = Try Await WebSocket.ConnectAsync("ws://{listener.LocalAddress}/")
    If closeFirst Then Await ws.CloseAsync()
    Loop
        Try
            Local m = Try Await ws.ReceiveAsync()
            If m == Null
                Print(name, "-> Null, code", ws.CloseCode ?? 0, ws.State)
                Break
            End If
            Print(name, "-> message", m)
        Catch e As NetError
            Print(name, "-> threw", e.Kind, "code", ws.CloseCode ?? 0, ws.State)
            Break
        End Try
    End Loop
    Try? Await served
End Function

Try
    // Client closes first, the server ends TCP without a Close frame.
    Await Run("closing, no reply", [], "close", True)
    // Server ends TCP while open, for comparison.
    Await Run("open, no close", [], "close", False)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: closing, no reply -> threw CLOSED code 1006 CLOSED
open, no close -> threw CLOSED code 1006 CLOSED

Actual: closing, no reply -> Null, code 1006 CLOSED
open, no close -> threw CLOSED code 1006 CLOSED

Suggested fix: In Took, set Failure for a Null frame whenever Status isn't already CLOSED (OPEN or CLOSING), not only when it's OPEN. Or, if Null is the intended result after our own Close, say so in Receive's comment, the class comment, and stdlib.md. Either way, add a CLOSING case to net_websocket.fb.

Skeptic: Ran the finding's program as rv-step30/wsv/ws2.fb in debug and in release. Both printed "closing, no reply -> Null, code 1006 CLOSED" and "open, no close -> threw CLOSED code 1006 CLOSED". In websocket.fb, Took (lines 528-537) sets Failure to NetError CLOSED only when `Status == WebSocketState.OPEN`. After CloseAsync has made it CLOSING, a peer that drops TCP makes ReceiveAsync return Null. Three docs say otherwise: Receive's comment ("Null once the other side's Close has come"), the class comment ("of CLOSED for a connection that ended without a Close frame"), and stdlib.md ("a connection that ends without a Close throws one of CLOSED"). No other side's Close came here, so the code contradicts all three.

## ws-3 (low): CloseCode reports the code this side failed with (1002/1007/1009) although no Close came

Status: fixed in bef0a604: Fail leaves CloseCode at 1006, as RFC 6455's 7.1.5 has it when no Close came, and its comment and stdlib.md say so; net_websocket.fb's server sends a masked frame, and the client's CloseCode after is 1006

Where: std/net/ws/websocket.fb:618

Fail sets Code to the code this side sends. So after a frame that breaks the RFC, CloseCode gives 1002, 1007, or 1009 even though no Close frame came from the other side. CloseCode's comment says it is "the code of the Close that came, 1005 for one without, or 1006 when the connection ended without one, or Null while none has", and docs/stdlib.md describes it the same way. In the program, the client receives a masked frame and fails it, and CloseCode then reads 1002. That is the code it sent, not one it received. Browsers' CloseEvent reports 1006 in this case. Debug and release give the same output. No test reads CloseCode after a failure.

```
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.ws
Import std.text
Import std.text.encoding

// A raw server: answers the handshake, sends `frames`, then reads until the client ends.
Async Function Raw(listener: TcpListener, frames: UInt8[][]) Throws Error
    Local stream = Try Await listener.AcceptAsync()
    Local head: UInt8[] = []
    Local one = Buffer<UInt8>(1)
    While head.Count < 4 || head[head.Count - 4..] != [13 As UInt8, 10, 13, 10]
        If Try Await stream.ReadAsync(one) == 0 Then Return
        head.Add(one[0])
    End While
    Local key = ""
    For line In (String.FromUtf8(head) ?? "").Split("\r\n")
        If line.StartsWith("Sec-WebSocket-Key: ") Then key = line.After(": ") ?? ""
    End For
    Local answer = Base64.Encode(Sha1.Hash(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
    Local text = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n" +
                 "Connection: Upgrade\r\nSec-WebSocket-Accept: {answer}\r\n\r\n"
    Try Await stream.WriteAsync(Buffer<UInt8>.From(text.Bytes))
    For f In frames
        Try Await stream.WriteAsync(Buffer<UInt8>.From(f))
    End For
    Try Await ReadAllAsync(stream)
End Function

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    // A masked text frame "hi", which a client must fail with 1002.
    Local served = Raw(listener, [[0x81 As UInt8, 0x82, 1, 2, 3, 4, 0x69, 0x6B]])
    Local ws = Await WebSocket.ConnectAsync("ws://{listener.LocalAddress}/")
    Try
        Await ws.ReceiveAsync()
    Catch e As NetError
        Print("threw", e.Kind, "CloseCode", ws.CloseCode?.ToString() ?? "Null", ws.State)
    End Try
    Try? Await served
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: threw OTHER CloseCode Null CLOSED (no Close came; or 1006, as the connection ended without one), or the docs say CloseCode is the code this side failed with

Actual: threw OTHER CloseCode 1002 CLOSED

Suggested fix: Either leave Code alone in Fail (so it stays Null, or becomes 1006 once the connection is closed), or document CloseCode as "the code of the Close that came, or the one it failed the connection with" in websocket.fb's comment and docs/stdlib.md. Add a test that reads CloseCode after a failure.

Skeptic: Ran the finding's program as rv-step30/wsv/ws3b.fb. I changed it only to reuse ws2's Raw with ending "wait"; the masked text frame to the client is the same. Debug and release both printed "threw OTHER CloseCode 1002 CLOSED". websocket.fb:618 in Fail sets `Code = code`, which is the code this side sends. CloseCode's comment says "The code of the Close that came, 1005 for one without, or 1006 when the connection ended without one, or Null while none has", and stdlib.md describes it the same way (1005/1006). RFC 6455 7.1.5 makes the close code 1006 when no Close frame was received. No Close came here, yet CloseCode reports 1002. The existing tests only read CloseCode after a received Close or an Abort (net_websocket.out: 1006), never after a failure.
