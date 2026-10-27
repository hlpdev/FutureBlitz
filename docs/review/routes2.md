# The review of M6 step 30's Routes<T> over lane 2's picks (routes from a Class's attributed Async Methods, each taking nothing, the request, or one Struct that Load makes from the path, the query, and a JSON body)

4 confirmed findings: 0 high, 1 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## routes-1 (medium): An attributed Method taking a scalar or HttpRequest? becomes a route that always answers 400, instead of panicking at Routes

Status: fixed in 2ec3e31e: the one-argument and (A, HttpRequest) loops call Misshapen when Load<A> of an empty Object is a WRONG_TYPE ParseError, as it is for a number, a String, an array, or an HttpRequest?, while a Struct or Class gives a value or a missing Field; panic_http_routes_number (n: Int), panic_http_routes_text_request (text: String, request), and panic_http_routes_maybe_request (HttpRequest?)

Where: std/net/http/routes.fb:75

The Func(A): Task<HttpResponse> and Func(A, HttpRequest): Task<HttpResponse> loops take any A. The only A they skip is one whose TypeName is exactly "HttpRequest". So `n: Int`, `s: String`, `request: HttpRequest?`, or `(a: Int, request: HttpRequest)` all register a route. Every request to it then fails in Load<A>(Object), so it answers 400 "expected Int32, found an Object". For HttpRequest?, the 400 carries the internal stand-in's message, "an HttpRequest is a server's, which Load doesn't make". The docs (routes.fb's comment, stdlib.md) say a route takes nothing, the request, one Struct or Class, or that and the request, and that a misshapen attributed Method panics at Routes rather than leave a broken path. This one doesn't panic, so the user only finds out at request time.

```
Import std.core
Import std.net
Import std.net.http

Class P
    @Get("/int")
    Async Method TakesInt(n: Int): HttpResponse => HttpResponse.Of(Body.Text("int {n}"))

    @Get("/maybe")
    Async Method TakesMaybe(request: HttpRequest?): HttpResponse => HttpResponse.Of(Body.Text("maybe"))
End Class

Local server = HttpServer()
server.Routes(P())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local client = HttpClient()
    For path In ["/int?n=3", "/maybe"]
        Local r = Await client.GetAsync(base + path)
        Print(path, "->", r.Status, r.Text)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: panic: P.TakesInt has a route's attribute, but isn't an Async Method that throws nothing, gives an HttpResponse, and takes nothing, the request, one Struct or Class, or that and the request at ...:14:1

Actual: /int?n=3 -> 400 expected Int32, found an Object
/maybe -> 400 an HttpRequest is a server's, which Load doesn't make

Suggested fix: In the one-argument and (A, HttpRequest) loops, check at Routes time that A is a Struct or Class, and call Misshapen otherwise. One way: try Load<A>(Value.Object()) once at registration. A scalar, a String, or HttpRequest? fails with WRONG_TYPE ("found an Object"), while a Struct or Class gives a value or a missing-Field error. Add a panic_http_routes_* test for an Int argument and one for HttpRequest?.

Skeptic: I ran rv-routes2/refute/r1.fb, the finding's program, in debug and release. Both builds printed `/int?n=3 -> 400 expected Int32, found an Object` and `/maybe -> 400 an HttpRequest is a server's, which Load doesn't make`, and Routes didn't panic. In routes.fb, the Func(A): Task<HttpResponse> loop (lines 75-88) skips only an A whose `TypeName<A>() == "HttpRequest"`, and the (A, HttpRequest) loop (lines 90-102) skips nothing. So an Int, a String, or an HttpRequest? argument registers a route that always answers 400. That contradicts Routes' comment and stdlib.md, which say a route takes nothing, the request, one Struct or Class, or that and the request, and that a Method of the wrong shape panics at Routes. Misshapen's own message makes the same promise. For HttpRequest?, the 400 also exposes the internal Loadable stand-in's message.

## routes-2 (low): Matching the request and the result by TypeName drops a user's own type named HttpRequest or HttpResponse without a route or a panic

Status: fixed in 2ec3e31e: each route loop, whose Func type names std's types, adds its Method's name and TypeName<M>() to a Set, and the one-argument loop and the misshapen ones skip what's in it, so no TypeName is compared with std's names; http_routes_own_request routes a program's own Struct HttpRequest as any Struct (200 own hi), and panic_http_routes_own_response and panic_http_routes_own_request panic for a program's own HttpResponse result and its own HttpRequest beside a Struct

Where: std/net/http/routes.fb:78

TypeName gives a type's name without its module (spec 13). So `TypeName<A>() == "HttpRequest"` (line 78), `TypeName<B>() == "HttpRequest"` (line 123), and `TypeName<R>() == "Task<HttpResponse>"` (lines 109, 116, 123) also match a program's own Struct called HttpRequest or HttpResponse. Take an attributed `Async Method A(request: HttpRequest)` where HttpRequest is the program's own Struct, which Load can make. The one-argument loop skips it as if it were std's request, and the request-alone loop doesn't match it, so the path is a silent 404. The same holds for `(args: Id, request: <own HttpRequest>)`: the misshapen loop skips it by name and nothing registers it, so there is no route and no panic.

```
Import std.core
Import std.net
Import std.net.http

Struct HttpRequest
    Field Text: String = ""
End Struct

Class P
    @Get("/a")
    Async Method A(request: HttpRequest): HttpResponse => HttpResponse.Of(Body.Text("a {request.Text}"))
End Class

Local server = HttpServer()
server.Routes(P())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local r = Await HttpClient().GetAsync(base + "/a?text=hi")
    Print(r.Status, r.Text)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 200 a hi

Actual: 404

Suggested fix: Tell std's types apart by identity, not by name. One way: put a Package attribute on std's HttpRequest and HttpResponse declarations (e.g. @ServerRequest) and test `A.Has(ServerRequest)` (spec 13's T.Has reads a type's own attributes) instead of comparing TypeName strings. Add a test with a program-local Struct named HttpRequest.

Skeptic: I ran rv-routes2/refute/r2.fb, where the program declares its own Struct HttpRequest, in debug and release. Both printed `404`, and there was no route and no panic. Spec 13 (line 238) says TypeName gives the declared name without its module. So line 78's `TypeName<A>() == "HttpRequest"` skips the program's own Struct as if it were std's request. The request-alone loop doesn't match it either, because its type isn't std's HttpRequest. Line 123's name check also lets an (args, own HttpRequest) Method through the misshapen loop. The result is a silent missing route, which breaks the doc's 'adds a route for each Async Method ... one Struct'. It's a rare case because it needs a type that shares std's name, so the severity is low.

## routes-3 (low): Attributed Static Methods and ByRef Methods are silently left out; the docs say a misshapen route panics

Status: fixed in 2ec3e31e: Routes' comment and stdlib.md say Static Methods and those with a ByRef parameter, which no For Method loop goes over, are left alone; http_routes' /static and /byref, an attributed Static Async Method and a Method of a ByRef Struct, are 404s

Where: std/net/http/routes.fb:36

For Method doesn't go over Static Methods, and a typed loop never includes a ByRef Method (spec 13). So `@Get("/s") Static Async Method S(): HttpResponse` gets no route, and so does a one-argument `@Get("/r") Method R(ByRef a: Id): HttpResponse`, and neither panics. Both paths are silent 404s. Routes' comment and stdlib.md say a Method of up to four arguments that isn't a route's shape panics at Routes "rather than leave the path a 404", as the panic tests put it. Neither text mentions Static or ByRef Methods.

```
Import std.core
Import std.net
Import std.net.http

Struct Id
    Field Id: Int = 4
End Struct

Class P
    @Get("/s")
    Static Async Method S(): HttpResponse => HttpResponse.Of(Body.Text("static"))

    @Get("/r")
    Method R(ByRef a: Id): HttpResponse => HttpResponse.Of(Body.Text("byref"))
End Class

Local server = HttpServer()
server.Routes(P())
Print("Routes didn't panic")
```

Expected: a panic at Routes for P.S or P.R (or docs that say Static and ByRef Methods are left out)

Actual: Routes didn't panic (and GET /s is a 404)

Suggested fix: The loops can't see these Methods, so say so in Routes' comment and stdlib.md: Static Methods and ByRef Methods are never routes, and their attributes do nothing. If the language later gains a Static Method loop, panic for them there.

Skeptic: I ran rv-routes2/refute/r3.fb. It printed `Routes didn't panic`. Spec 13 (lines 186-188) says a typed loop never includes a ByRef Method, and the loops can't see Static ones either. So the attributed sync `Method R(ByRef a: Id): HttpResponse` gets no route and no panic, even though Routes' comment and stdlib.md say an attributed Method of up to four arguments that 'isn't Async' panics at Routes. The attributed Static Async Method S is also silently left out, and neither text mentions it. The behavior follows from the language, so the defect is in the docs: they should say Static and ByRef Methods are never routes. Low.

## routes-4 (low): The 400 for a non-JSON body repeats itself: "the body isn't JSON: the text isn't JSON: ..."

Status: fixed in 2ec3e31e: Bound trims Json's "the text isn't JSON: " from the message it puts after its own "the body isn't JSON: "; http_routes.out's not-json line says it once

Where: std/net/http/routes.fb:212

Bound wraps Json.Read's ParseError message, which already starts with "the text isn't JSON:", inside its own "the body isn't JSON: " prefix. So the 400 says the same thing twice. The doubled text is also baked into tests/programs/http_routes.out.

```
Import std.core
Import std.net
Import std.net.http

Class NewUser
    Field Name: String
End Class

Struct Create
    @JsonBody Field User: NewUser
End Struct

Class P
    @Post("/users")
    Async Method Make(a: Create): HttpResponse => HttpResponse.Of(Body.Text(a.User.Name))
End Class

Local server = HttpServer()
server.Routes(P())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local r = Await HttpClient().PostAsync("http://127.0.0.1:{server.LocalAddress.Port}/users", Body.Json("not json"))
    Print(r.Status, r.Text)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 400 the body isn't JSON: there's a word JSON doesn't have

Actual: 400 the body isn't JSON: the text isn't JSON: there's a word JSON doesn't have

Suggested fix: Drop one of the two prefixes, either by returning Refused(e.Message) with Json's own wording or by writing "the body " in place of Json's "the text " prefix. Then update http_routes.out.

Skeptic: I ran rv-routes2/refute/r4.fb. It printed `400 the body isn't JSON: the text isn't JSON: there's a word JSON doesn't have`. Line 212 of routes.fb wraps e.Message in "the body isn't JSON: ", and std/data/json.fb:72 already throws "the text isn't JSON: ...". The doubled text is baked into tests/programs/http_routes.out line 12. The fix is to change the wording and update the .out. Low.
