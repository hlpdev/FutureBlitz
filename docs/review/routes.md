# The review of M6 step 30's Routes<T> std part (std/net/http/routes.fb: routes from a Class's Async Methods with @Get and the rest, each taking one Struct that Load makes from the path, the query, and a JSON body)

4 confirmed findings: 0 high, 2 medium, 2 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## routes-1 (medium): Any one-argument Async Method giving an HttpResponse, attributed or not, must take a type Load can make, or the program won't compile, with the error inside routes.fb

Status: fixed in ee7afaf5 and 7dc7c380: a Method of the HttpRequest alone is a route of its own, whose loop doesn't Load, and HttpRequest implements Loadable, always a ParseError, so the loop of one argument, which a Method of the request is among, compiles for it and leaves it to that one; and Routes' loops pick the Methods with a route's attribute (spec 13's Where m Has), so a helper without one takes what it likes. http_routes has @Get Raw(request), and unattributed Helper(request) and Wrapped(make: Func(): HttpResponse), a 404

Where: std/net/http/routes.fb:115

Register's first typed loop (`Where M: Func(A): Task<HttpResponse>`) builds a handler, and so instantiates Bound<A> and Load<A>, for every Async Method of one argument that gives an HttpResponse. It checks the @Get/@Post attributes only at run time, in AddRoutes. So a plain helper such as `Async Method Logged(request: HttpRequest): HttpResponse`, with no attribute, stops the whole program compiling. The error is `std/net/http/routes.fb:115:16: error: Load can't make Duplex here`, and it names no line of the user's program. The same happens for an attributed `@Get Async Method Raw(request: HttpRequest)`, the most natural handler signature. stdlib.md and routes.fb's comment say a route Method "takes one Struct or Class", and HttpRequest is a Class. Neither says that unattributed Methods are constrained too.

```
Import std.core
Import std.net
Import std.net.http

Struct Id
    Field Id: Int
End Struct

Class Api
    @Get("/users/:id")
    Async Method Show(args: Id, request: HttpRequest): HttpResponse => Await Logged(request)

    // A helper, no attribute: not a route.
    Async Method Logged(request: HttpRequest): HttpResponse
        Print("got", request.Path)
        Return HttpResponse.Of(204)
    End Method
End Class

Local server = HttpServer()
server.Routes(Api())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local r = Await HttpClient().GetAsync("http://127.0.0.1:{server.LocalAddress.Port}/users/7")
    Print(r.Status)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: got /users/7
204
(The unattributed helper isn't a route and doesn't constrain anything. At the least, an error should point at the user's Method.)

Actual: std/net/http/routes.fb:115:16: error: Load can't make Duplex here; it makes numbers, Bool, String, Enums, Value, T?, T[], Map<String, V>, and Structs and Classes of them, and what implements Loadable
error: couldn't build helper2.fb because of 1 error

Suggested fix: Keep the typed loops from binding Methods that aren't routes. That needs a way to filter a typed For Method by attribute at compile time, or a Load<A> inside a For Method copy that can't make A should become a run-time refusal instead of a compile error. Support `Func(HttpRequest): Task<HttpResponse>` Methods as routes of their own, in a loop that doesn't Load. Until then, stdlib.md and routes.fb's comment should say that every one-argument Async Method giving an HttpResponse must take a type Load makes, and that HttpRequest alone isn't one. Add a test.

Skeptic: I ran rv-routes/refute/r1.fb (the finding's program) and it doesn't compile: `std/net/http/routes.fb:115:16: error: Load can't make Duplex here ...`. The error points at `args = Load<A>(fields)` and names no line of the user's file. Register's first typed loop (routes.fb:44-55) builds the handler, and so instantiates Bound<A> and Load<A>, for every `Func(A): Task<HttpResponse>` Method. The @Get/@Post check happens only later, at run time, in AddRoutes. So an unattributed helper `Async Method Logged(request: HttpRequest): HttpResponse` stops the program compiling. stdlib.md says routes are the attributed Methods taking 'one Struct or Class', and HttpRequest is a Class. It never says that unattributed Methods are constrained, or that a Method taking just HttpRequest is refused. This is std's own instantiation, not lane 2's typed-For-Method bug.

## routes-3 (medium): A @Flatten Field's own Fields are never bound from the path or the query

Status: fixed in ee7afaf5: Bound looks up a @Flatten Field's own Fields, recursively, by their keys; http_routes' /list?page=2&SIZE=5 binds a flattened Paging, and a @Skip Field is left out

Where: std/net/http/routes.fb:88

Bound goes over A's top-level Fields only and keys each value by the Field's name. For `@Flatten Field Paging: Page`, it looks for a query key "Paging" and never for Page's Limit and Offset. Load<List> reads a flattened Field's Fields from the top-level Object, as load.fb shows: Load<List> of {Name:"a", Limit:"5", Offset:"2"} gives "a 5 2". But Bound never puts Limit and Offset in that Object, so `?limit=5&offset=2` is silently ignored and the defaults stay. A shared paging or filter Struct flattened into each route's argument is exactly what @Flatten is for.

```
Import std.core
Import std.net
Import std.net.http

Struct Page
    Field Limit: Int = 10
    Field Offset: Int = 0
End Struct

Struct List
    Field Name: String = ""
    @Flatten Field Paging: Page
End Struct

Class Api
    @Get("/list")
    Async Method List(args: List): HttpResponse
        Return HttpResponse.Of(Body.Text("{args.Name} {args.Paging.Limit} {args.Paging.Offset}"))
    End Method
End Class

Local server = HttpServer()
server.Routes(Api())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local r = Await HttpClient().GetAsync("http://127.0.0.1:{server.LocalAddress.Port}/list?name=a&limit=5&offset=2")
    Print(r.Status, r.Text)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 200 a 5 2

Actual: 200 a 10 0

Suggested fix: In Bound, recurse into a @Flatten Field's type's Fields with For Field, which Where lines can name, and look each one up by its own key. Alternatively, fill the Object with every path and query parameter by name and let Load pick its keys. Then add a case to http_routes.

Skeptic: r3.fb prints `200 a 10 0` in both debug and release for `/list?name=a&limit=5&offset=2`; it should print `200 a 5 2`. Bound (routes.fb:88-112) looks only at A's top-level Fields, keyed by `f.Attribute(Rename)?.Name ?? f.Name`. For `@Flatten Field Paging: Page` it looks up the key "Paging" and never Limit or Offset. stdlib.md says the argument 'is made by Load<T>', and Load honors @Flatten. Bound copies Load's @Rename mapping itself but not @Flatten, so a Struct using a documented Load attribute is bound wrong, and the parameters are dropped without an error or a 400.

## routes-2 (low): An attributed Method that isn't one of the two shapes is silently not a route (404), with no panic

Status: fixed in ee7afaf5 and 7dc7c380: a Method of no argument is a route (http_routes' /ping), and one of up to four arguments with a route's attribute that isn't a route's shape panics at Routes, naming it: panic_http_routes_shape (not Async), panic_http_routes_request_first, panic_http_routes_result (gives a String), panic_http_routes_arguments (three), and, as a typed For Method takes throwing Methods now, panic_http_routes_throws and panic_http_routes_async_throws

Where: std/net/http/routes.fb:44

Routes only goes over `Func(A): Task<HttpResponse>` and `Func(A, HttpRequest): Task<HttpResponse>`. A Method with @Get and the rest in any other shape is dropped without a word, and its path is a 404 at run time. The dropped shapes include no argument (`Async Method Health(): HttpResponse`, the usual health check), a non-Async Method, an Async Method with Throws, and the request first. These are easy mistakes that std could catch. A typed loop over `Func(): Task<HttpResponse>` compiles and works, so zero-argument routes could be supported, and a loop over `Func(A): HttpResponse` with m.Has(Get) works too, so std could panic for 'isn't Async'. I checked both in a small program. The test only shows an unattributed non-Async Method being skipped.

```
Import std.core
Import std.net
Import std.net.http

Struct Id
    Field Id: Int
End Struct

Class Missing Extends Error
End Class

Class Api
    @Get("/health")
    Async Method Health(): HttpResponse => HttpResponse.Of(Body.Text("ok"))

    @Get("/plain/:id")
    Method Plain(args: Id): HttpResponse => HttpResponse.Of(Body.Text("plain {args.Id}"))

    @Get("/find/:id")
    Async Method Find(args: Id): HttpResponse Throws Missing
        If args.Id == 0 Then Throw Missing("none")
        Return HttpResponse.Of(Body.Text("found {args.Id}"))
    End Method

    @Get("/first/:id")
    Async Method First(request: HttpRequest, args: Id): HttpResponse => HttpResponse.Of(Body.Text("first"))
End Class

Local server = HttpServer()
server.Routes(Api())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local base = "http://127.0.0.1:{server.LocalAddress.Port}"
    Local client = HttpClient()
    For path In ["/health", "/plain/1", "/find/1", "/first/1"]
        Local r = Await client.GetAsync(base + path)
        Print(path, "->", r.Status, r.Text)
    End For
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: /health -> 200 ok (a Method of no argument is a route), and a panic at server.Routes for the others, such as 'Plain has @Get but isn't an Async Method of one Struct or Class, or that and the HttpRequest'

Actual: /health -> 404 
/plain/1 -> 404 
/find/1 -> 404 
/first/1 -> 404 
(same in debug and release; no panic)

Suggested fix: Add a typed loop over `Func(): Task<HttpResponse>` for Methods of no argument. Add loops over the shapes that can be named, such as `Func(A): HttpResponse` and `Func(A, HttpRequest): HttpResponse`, that panic at the caller when the Method has a route attribute. Say in stdlib.md which attributed Methods are refused and which are ignored, and test both.

Skeptic: r2.fb gives `/health -> 404`, `/plain/1 -> 404`, `/find/1 -> 404` and `/first/1 -> 404`, the same in debug and release, with no panic. Most of these shapes (no argument, not Async, the request first) are outside what stdlib.md calls a route, so ignoring them matches the doc as written; supporting zero-argument routes would be a feature, not a fix. The Throws case is different. `@Get("/find/:id") Async Method Find(args: Id): HttpResponse Throws Missing` is an Async Method that takes one Struct and gives an HttpResponse, which is stdlib.md's description of a route, yet it isn't registered and gives a 404 silently. So either the doc must exclude Throws Methods or Routes must refuse them (a panic at the caller, like its other misuse panics). Low, because it's a silent mismatch for an edge shape, not a wrong binding.

## routes-4 (low): @Renamed(From) keys are never looked up, so an old query name doesn't bind

Status: fixed in ee7afaf5: a Field with no value by its own key is looked up by its @Renamed old one, whatever the case, and stored under it for Load; http_routes' /hello?HANDLE=Bob binds it, and the new name wins when both come

Where: std/net/http/routes.fb:89

Bound fills the Object only under `f.Attribute(Rename)?.Name ?? f.Name`. Load falls back to a Field's @Renamed(From) key when the Object doesn't have its own key, as Load<Id> of {Id:"1", old:"4"} giving Count 4 shows. Bound never looks for a path or query parameter under the From name, so a route Struct can't keep an old query name working, as @Renamed is meant to.

```
Import std.core
Import std.net
Import std.net.http

Struct Id
    Field Id: Int
    @Renamed("old") Field Count: Int = 0
End Struct

Class Api
    @Get("/id/:id")
    Async Method Ids(args: Id): HttpResponse => HttpResponse.Of(Body.Text("{args.Id} count={args.Count}"))
End Class

Local server = HttpServer()
server.Routes(Api())
Try
    server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local r = Await HttpClient().GetAsync("http://127.0.0.1:{server.LocalAddress.Port}/id/1?old=4")
    Print(r.Status, r.Text)
    server.Stop()
    Await serving
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 200 1 count=4

Actual: 200 1 count=0

Suggested fix: When neither the path nor the query has the key, look for f.Attribute(Renamed)?.From the same case-insensitive way, and store it under that key so Load's fallback finds it. Otherwise, say in stdlib.md that only the name and @Rename are read.

Skeptic: r4.fb prints `200 1 count=0` in both debug and release for `/id/1?old=4`; it should print `200 1 count=4`. Bound keys each lookup only by @Rename or the name (routes.fb:89), so the `@Renamed("old")` From key is never looked up in the path or the query. Load would fall back to it if Bound put it in the Object. It's the same class of defect as routes-3: a Load attribute that stdlib.md documents is silently ignored, with no doc caveat. Low, since @Renamed is aimed at old saves more than at query names.

## Refuted by the skeptics (no action)

- routes-5: A T[] Field gets only the first of a repeated query key. r5.fb does print `200 tags=1` for `?tags=a&tags=b`, in both debug and release. But stdlib.md defines the binding as 'a path parameter or, failing that, a query parameter ... as text', one parameter's text per Field, and lists only numbers, Bool and Enum names as what Load reads that text as. Nothing promises T[] Fields or the merging of repeated keys. Taking the first match (routes.fb:103-107) is consistent with that description, so this is a feature request or doc clarification, not a bug against the spec or stdlib.md.
