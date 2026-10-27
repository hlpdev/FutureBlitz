# The review of M6 step 30's ACME part (std.net.acme: AcmeClient, AcmeTokens, CertificateRequest, and the tests' Pebble-like CA)

12 confirmed findings: 0 high, 2 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## client-1 (medium): A second RegisterAsync on a client (or RegisterAsync after ObtainAsync) signs newAccount with kid, which the CA rejects

Status: fixed in 97f61f4e (RegisterAsync's newAccount POST is signed with the key's JWK whether the client has a kid or not, through PostAsync's and Signed's byKey), with http_acme: a client registers again after its order and gets the same account ("registered again: True"), and a client that orders first, registering then, registers after and finds that account ("ordered first: True, True"); both were 400 malformed before

Where: std/net/acme_client.fb:245

Signed() uses the kid whenever Kid is set, for every URL. RFC 8555 section 6.2 requires newAccount requests to carry the jwk, and the test CA enforces this, as Pebble and Let's Encrypt do. RegisterAsync is documented to make the account "or find the one the key has". Once a client has registered, or once ObtainAsync has auto-registered, any later RegisterAsync on the same client, for example to look the account up again or to send contacts after an Obtain, fails with a 400 malformed error. No test covers this: http_acme calls RegisterAsync once per client.

```
// Run as an http_ program, under fb_http_test_server, whose first input line is its HTTP URL.
Import std.core
Import std.collections
Import std.crypto
Import std.net
Import std.text

Global base = Input() ?? ""

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For

    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Async Function Run()
    Try
        Local acme = AcmeClient("{base}/acme/dir", KeyOf(1))
        Local first = Try Await acme.RegisterAsync(["mailto:a@example.com"])
        Print("first {first.EndsWith("/acme/account/1")}")
        // RegisterAsync "finds the one the key has": asking again gives the same account.
        Local again = Try Await acme.RegisterAsync(["mailto:a@example.com"])
        Print("again {again == first}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: first True
again True

Actual: first True
failed: the ACME CA answered 400: a new account's JWS has its jwk (malformed)
(the same in debug and release, under a fb_http_test_server built from 0ed29228)

Suggested fix: Have Signed take whether to use the JWK, and have RegisterAsync's newAccount POST always send the jwk, for example a PostAsync parameter or a check that url is the directory's newAccount. Add the case above to http_acme: RegisterAsync twice on one client, and again after ObtainAsync.

Skeptic: I ran rv-acme/refute-client/http_c1.fb, which calls RegisterAsync twice on one client, under the fb_http_test_server built from 0ed29228. Debug and release both printed "first True" and then "failed: the ACME CA answered 400: a new account's JWS has its jwk (malformed)". In std/net/acme_client.fb:245-250, Signed() picks the kid whenever Kid is set, whatever the URL. RegisterAsync (line 97) posts to newAccount through it. RFC 8555 section 6.2 requires the jwk for newAccount, acme_ca.cpp:733 enforces that, and RegisterAsync is documented (in its comment and in stdlib.md) to 'find the one the key has'. That also breaks RegisterAsync after ObtainAsync's own registration. The fix is for newAccount to always sign with the jwk, plus a test.

## csr-1 (medium): CheckName accepts names that aren't DNS host names: a label that starts or ends with a hyphen, and a dotted-decimal IP address

Status: fixed in 97f61f4e (CheckName panics for a label that starts or ends with a hyphen, and for a name whose last label is all digits, as an IPv4 address's is), with panic_acme_hyphen_start ("-example.com"), panic_acme_hyphen_end ("example-.com"), and panic_acme_address ("127.0.0.1"), each after a request for names with inner hyphens and digits; stdlib.md and CertificateRequest's comment give the rule

Where: std/net/acme.fb:135

Acme.CertificateRequest's comment and docs/stdlib.md both say it panics for "a name that isn't a DNS name". CheckName only checks that each byte is a letter, a digit, or '-', and that labels are 1 to 63 bytes. It never checks the LDH rule (RFC 1123 / RFC 5890) that a label can't start or end with '-'. It also lets a dotted-decimal address such as "127.0.0.1" through, which RFC 1123 2.1 rules out as a host name and RFC 8738 says goes as an "ip" identifier. These names end up in the CSR's subjectAltName as dNSName entries, and ObtainAsync sends them to the CA as "dns" identifiers. Let's Encrypt (Boulder) refuses all three, so the user gets a NetError from the CA after registering and ordering, not the early panic the docs promise. Nothing in the tests covers these cases, and tests/native/acme_ca doesn't check for them either. Debug and release behave the same.

```
Import std.core
Import std.collections
Import std.crypto
Import std.net.acme

Local bytes: UInt8[] = []
For i = 1 To 32
    bytes.Add(i As UInt8)
End For

Local key = Key(KeyKind.ECDSA_P256, True, bytes)
For name In ["-example.com", "example-.com", "127.0.0.1"]
    Print(name, Acme.CertificateRequest(key, [name]).Count)
End For
```

Expected: panic: "-example.com" ... (a label can't start or end with a hyphen) at <file>:13:... (and the same panic for "example-.com"; "127.0.0.1" is refused as an IP address, not a DNS name)

Actual: -example.com 254
example-.com 255
127.0.0.1 249
(openssl req -text reads the CSRs as DNS:-example.com, DNS:example-.com, DNS:127.0.0.1)

Suggested fix: In CheckName, panic when a label's first or last byte is '-'. Also panic when every label is digits, or at least the last one (a TLD is never all digits), with a message that an IP address isn't a DNS name. Add panic_acme_ tests for a leading hyphen, a trailing hyphen, and "127.0.0.1".

Skeptic: Ran the finding's program as C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-acme/csr-names/names.fb. Debug and --release both printed "-example.com 254", "example-.com 255", "127.0.0.1 249" and exited 0, so no panic. CheckName in std/net/acme.fb (lines ~131-161) only checks byte classes, the 1-63 label length and the 253 total. It never rejects a label that starts or ends with '-', and it never rejects an all-numeric (dotted-decimal) name. These names are not valid DNS host names under LDH / RFC 1123 2.1. The Acme.CertificateRequest comment and the stdlib.md ACME bullet both promise a panic for "a name that isn't a DNS name" (stdlib.md: "each panics for ... a name that isn't one"). Instead, the names go into the CSR as dNSName entries and to the CA as dns identifiers. None of the panic_acme_ tests covers these cases.

## ca-1 (low): A POST-as-GET to a challenge URL starts the CA's check; real CAs only do that for a POST of {}

Status: fixed in f7879c12 (a challenge's check starts only for a POST whose payload is an object, and a POST-as-GET only gives the challenge), with acme_ca_tests: a POST-as-GET of each challenge leaves it pending, and its authorization pending with no Retry-After, before the POST of {}

Where: tests/native/acme_ca/acme_ca.cpp:866

RFC 8555 7.5.1 has the client answer a challenge with a POST whose payload is {}. A POST-as-GET (empty payload) to the challenge URL only reads it. Pebble and Boulder both return the challenge unchanged for a POST-as-GET. The test CA's /challenge/ branch moves any POST from pending to processing, whatever the payload, so a client that 'answered' with an empty payload would pass every test here and then hang at Let's Encrypt. Today AcmeClient sends "{}", but nothing checks that. How I ran it: build-rev's tests/native/fb_http_test_server.exe is stale (Oct 23) and answers /acme/... with 404, so I built fb_http_test_server from 0ed29228 by hand, at C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-acme/ca/server.exe. Each http_ program here is built with fb build and run as `server.exe prog.exe`. Debug and release print the same.

```
// A POST-as-GET (empty payload) to a challenge's URL only reads it at a real CA (RFC 8555 7.5.1:
// the client responds with a POST of {}); here it starts the check.
Import std.core
Import std.collections
Import std.crypto
Import std.data.json
Import std.ffi
Import std.io
Import std.net
Import std.net.acme
Import std.net.http
Import std.text

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

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Global key = KeyOf(1)
Global client = HttpClient()
Global nonce = ""
Global kid: String? = Null

Function B64(bytes: UInt8[]): String => Base64.Encode(bytes, True, False)

Async Function Post(url: String, payload: String): HttpResponse Throws NetError, TimeoutError
    Local header = Value.Object()
    header["alg"] = Value("ES256")
    If kid != Null
        header["kid"] = Value(kid ?? "")
    Else
        header["jwk"] = Acme.Jwk(key)
    End If
    header["nonce"] = Value(nonce)
    header["url"] = Value(url)
    Local protected = B64(Json.Write(header).Bytes)
    Local content = B64(payload.Bytes)
    Local jws = Value.Object()
    jws["protected"] = Value(protected)
    jws["payload"] = Value(content)
    jws["signature"] = Value(B64(EcdsaP256.Sign(key.Bytes, "{protected}.{content}")))
    Local response = Try Await client.SendAsync(HttpMethod.POST, url,
                                              Body(Json.Write(jws).Bytes, "application/jose+json"))
    nonce = response.Headers.Get("Replay-Nonce") ?? ""
    Return response
End Function

Function StatusOf(response: HttpResponse): String
    Local read = Try? Json.Read(response.Text)
    If read == Null Then Return "?"
    Return read["status"]?.AsString() ?? "?"
End Function

Function At(object: Value?, key: String): Value? => object == Null ? Null : object[key]

Function Text(object: Value?, key: String): String => At(object, key)?.AsString() ?? ""

Function First(object: Value?, key: String): Value?
    Local items = At(object, key)?.AsArray() ?? []
    If items.Count == 0 Then Return Null
    Return items[0]
End Function

Async Function Respond(listener: TcpListener, tokens: AcmeTokens)
    Loop
        Try
            Local connection = Try Await listener.AcceptAsync()
            Local reader = BufferedReader(connection)
            Local request = Try Await reader.ReadLineAsync() ?? ""
            Loop
                Local header = Try Await reader.ReadLineAsync() ?? ""
                If header == "" Then Break
            End Loop
            Local parts = request.Split(" ")
            Local answer = parts.Count > 1 ? tokens.Answer(parts[1]) : Null
            Local response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
            If answer != Null
                response = "HTTP/1.1 200 OK\r\nContent-Length: {answer.Bytes.Count}\r\n\r\n{answer}"
            End If
            Try Await connection.WriteAsync(Buffer<UInt8>.From(response.Bytes))
            connection.Close()
        Catch e As Error
            Return
        End Try
    End Loop
End Function

Async Function Run()
    Try
        Local listener = TcpListener.Listen("127.0.0.1", challengePort)
        Defer listener.Close()
        Local tokens = AcmeTokens()
        Local responding = Respond(listener, tokens)
        nonce = (Try Await client.HeadAsync("{base}/acme/nonce")).Headers.Get("Replay-Nonce") ?? ""
        Local made = Try Await Post("{base}/acme/account", "{{\"termsOfServiceAgreed\":true}}")
        kid = made.Headers.Get("Location")
        Local order = Try Await Post("{base}/acme/order",
            "{{\"identifiers\":[{{\"type\":\"dns\",\"value\":\"localhost\"}}]}}")
        Local authz = First(Try? Json.Read(order.Text), "authorizations")?.AsString() ?? ""
        Local authorization = Try? Json.Read((Try Await Post(authz, "")).Text)
        Local challenge = First(authorization, "challenges")
        Local token = Text(challenge, "token")
        Local url = Text(challenge, "url")
        tokens.Publish(token, Acme.KeyAuthorization(token, key))
        // A POST-as-GET of the challenge: it only reads it.
        Print("challenge after POST-as-GET: {StatusOf(Try Await Post(url, ""))}")
        Print("authorization: {StatusOf(Try Await Post(authz, ""))}")
        Print("authorization: {StatusOf(Try Await Post(authz, ""))}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: challenge after POST-as-GET: pending
authorization: pending
authorization: pending

Actual: challenge after POST-as-GET: processing
authorization: pending
authorization: valid

Suggested fix: In the /challenge/ branch, start the check (pending to processing) only when the payload is a JSON object ({}). For a POST-as-GET (empty request_signed.payload), return the challenge unchanged. Add a case to acme_ca_tests.cpp: a POST-as-GET of /challenge/N leaves it pending, and the authorization never turns valid.

Skeptic: Built http_ca1_pag.fb (same as the finding's program) in debug and release under C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-acme/ca-verify. I ran it with the fb_http_test_server built by hand from 0ed29228; build-rev's copy is from Oct 23 and stale. Both builds printed 'challenge after POST-as-GET: processing / authorization: pending / authorization: valid'. In acme_ca.cpp, the /challenge/ branch moves the challenge from pending to processing on any POST and never checks the payload. A POST-as-GET's empty payload is turned into an empty json_t earlier in the code. RFC 8555 7.5.1 has the client answer with a POST of {}, and stdlib.md says the CA works 'as Pebble is', but Pebble only returns the challenge for a POST-as-GET. AcmeClient does send "{}" today (acme_client.fb:163), so the client is correct. The only gap is that no test would catch it going wrong, so this is low, not medium.

## ca-2 (low): fetch_challenge isn't an HTTP client: a chunked body or an HTTP/1.0 status line fails a correct answer

Status: fixed in f7879c12 (acme_ca's body_of_ok reads an HTTP/1.0 or 1.1 200 by its Content-Length, in chunks for Transfer-Encoding: chunked, with chunk extensions, or to the end, and refuses another status or a response cut short; fetch_challenge uses it), with acme_ca_tests' "the CA's check reads an HTTP-01 answer as an HTTP client does" and http_acme, whose responder now answers in chunks (97f61f4e), which issued only after the fix. Redirects aren't followed: the fetch is to the program's own port

Where: tests/native/http_server/http_server.cpp:510

The CA's HTTP-01 check reads the raw bytes the challenge port sends. It needs them to start with exactly "HTTP/1.1 200", and it takes everything after the first blank line as the body. It never decodes Transfer-Encoding: chunked, and it refuses an HTTP/1.0 status line. Pebble's and Boulder's checks use Go's net/http client, which accepts both. A correct responder that streams its answer chunked (as an HttpServer may, and step 30's automatic HTTPS will answer HTTP-01 from a server) is reported as 'isn't the key authorization'. An HTTP/1.0 responder is reported as 'didn't answer'.

```
// The CA's HTTP-01 check against answers a real CA (Pebble, Boulder) takes: the key
// authorization with a trailing newline (RFC 8555 8.3: the server SHOULD ignore whitespace at
// the end of the body), a chunked body, and an HTTP/1.0 status line.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import std.net.http
Import std.text

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

Global mode = "plain"

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Function Hex(n: Int): String
    Local digits = "0123456789abcdef"
    Local text = ""
    Local left = n
    Loop
        text = digits.Slice(left % 16, left % 16 + 1) + text
        left = left / 16
        If left == 0 Then Break
    End Loop
    Return text
End Function

Function Answered(answer: String): String
    If mode == "newline"
        Local body = answer + "\n"
        Return "HTTP/1.1 200 OK\r\nContent-Length: {body.Bytes.Count}\r\n\r\n{body}"
    End If
    If mode == "chunked"
        Return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" +
               "{Hex(answer.Bytes.Count)}\r\n{answer}\r\n0\r\n\r\n"
    End If
    If mode == "http10"
        Return "HTTP/1.0 200 OK\r\nContent-Length: {answer.Bytes.Count}\r\n\r\n{answer}"
    End If
    Return "HTTP/1.1 200 OK\r\nContent-Length: {answer.Bytes.Count}\r\n\r\n{answer}"
End Function

Async Function Respond(listener: TcpListener, tokens: AcmeTokens)
    Loop
        Try
            Local connection = Try Await listener.AcceptAsync()
            Local reader = BufferedReader(connection)
            Local request = Try Await reader.ReadLineAsync() ?? ""
            Loop
                Local header = Try Await reader.ReadLineAsync() ?? ""
                If header == "" Then Break
            End Loop
            Local parts = request.Split(" ")
            Local answer = parts.Count > 1 ? tokens.Answer(parts[1]) : Null
            Local response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
            If answer != Null Then response = Answered(answer)
            Try Await connection.WriteAsync(Buffer<UInt8>.From(response.Bytes))
            connection.Close()
        Catch e As Error
            Return
        End Try
    End Loop
End Function

Async Function Run()
    Try
        Local listener = TcpListener.Listen("127.0.0.1", challengePort)
        Defer listener.Close()
        Local tokens = AcmeTokens()
        Local responding = Respond(listener, tokens)
        Local acme = AcmeClient("{base}/acme/dir", KeyOf(1))
        For each In ["plain", "newline", "chunked", "http10"]
            mode = each
            Try
                Local pem = Try Await acme.ObtainAsync(["localhost"], KeyOf(40), tokens)
                Print("{each}: issued")
            Catch e As NetError
                Print("{each}: {e.Message}")
            End Try
        End For
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: plain: issued
newline: issued
chunked: issued
http10: issued

Actual: plain: issued
newline: the CA found the authorization of localhost invalid: the response at localhost isn't the key authorization
chunked: the CA found the authorization of localhost invalid: the response at localhost isn't the key authorization
http10: the CA found the authorization of localhost invalid: localhost didn't answer the challenge

Suggested fix: Parse the response in fetch_challenge as an HTTP client would. Take any "HTTP/1.x 200" status line. Read the body by Content-Length, or decode it when Transfer-Encoding is chunked, or read to close when there's neither. Ideally also follow a redirect, as Pebble and Boulder do for HTTP-01. Add http_ program cases (or acme_ca_tests cases through a fetch) for a chunked answer and an HTTP/1.0 answer.

Skeptic: I ran http_ca2_respond in debug and release. Both printed 'plain: issued', then 'isn't the key authorization' for the newline and chunked answers, then 'localhost didn't answer the challenge' for the HTTP/1.0 answer. fetch_challenge in http_server.cpp requires response.starts_with("HTTP/1.1 200") and takes the raw bytes after the first CRLFCRLF as the body. It never decodes chunked transfer encoding and refuses an HTTP/1.0 status line, so it rejects correct answers that Pebble's Go net/http check accepts. Today's responders all send Content-Length over HTTP/1.1, so nothing fails yet. Step 30's HttpServer that answers HTTP-01 is still to come, so this is low.

## ca-3 (low): The CA's key-authorization check is byte-exact; RFC 8555 8.3 has a CA ignore trailing whitespace

Status: fixed in f7879c12 (the CA trims spaces, tabs, CRs, and LFs after the fetched key authorization before comparing it), with acme_ca_tests: the second name's responder answers with " \r\n" after its key authorization, and its authorization turns valid

Where: tests/native/acme_ca/acme_ca.cpp:831

The check is `*fetched == expected`. RFC 8555 8.3 says the server SHOULD ignore whitespace characters at the end of the body. Pebble and Boulder trim the end of the body before comparing. A responder that sends the key authorization with a trailing newline, as many static-file setups and echo-style handlers do, is valid at Let's Encrypt and invalid here. The 'newline' line of the ca-2 program shows it.

```
(the ca-2 program: its 'newline' mode sends the key authorization followed by "\n" with a correct Content-Length)
```

Expected: newline: issued

Actual: newline: the CA found the authorization of localhost invalid: the response at localhost isn't the key authorization

Suggested fix: Trim trailing ' ', '\t', '\r', '\n' from *fetched before comparing it with the expected key authorization in the /authz/ branch. Add an acme_ca_tests case in which responses()[token] has a trailing "\n" and the authorization still turns valid.

Skeptic: The 'newline' line of the ca-2 run, debug and release: a key authorization followed by "\n", with a correct Content-Length, gave 'the response at localhost isn't the key authorization'. In the /authz/ branch of acme_ca.cpp the check is `fetched && *fetched == expected`, a byte-exact comparison. RFC 8555 8.3 says the server SHOULD ignore whitespace at the end of the body, and Pebble trims trailing whitespace before comparing. So the CA rejects an answer that a real CA accepts.

## ca-4 (low): The CA never checks a POST's Content-Type (application/jose+json); acme_request_t carries no headers

Status: fixed in f7879c12 (acme_request_t has the request's content_type, which the test server passes from its Content-Type header, and a POST that isn't application/jose+json is a 415 malformed problem, with a Replay-Nonce), with acme_ca_tests' text/plain newAccount: 415, malformed, with a nonce; every other test's POST says application/jose+json, as AcmeClient's do in http_acme

Where: tests/native/acme_ca/acme_ca.hpp:21

RFC 8555 6.2 requires POSTs to the CA to have Content-Type application/jose+json, and Pebble and Boulder refuse anything else as malformed. acme_request_t has only method, path, and body, and acme_answer in http_server.cpp passes no headers, so the CA accepts a JWS sent as text/plain (or with no Content-Type). The tests therefore don't check that AcmeClient sends the right type. It does today: Body(..., "application/jose+json").

```
// RFC 8555 6.2: a POST's Content-Type is application/jose+json, and a CA refuses another with
// malformed (Pebble and Boulder do). Here a JWS sent as text/plain makes an account.
Import std.core
Import std.collections
Import std.crypto
Import std.data.json
Import std.ffi
Import std.io
Import std.net
Import std.net.acme
Import std.net.http
Import std.text

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

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Global key = KeyOf(1)
Global client = HttpClient()
Global nonce = ""
Global kid: String? = Null

Function B64(bytes: UInt8[]): String => Base64.Encode(bytes, True, False)

Async Function Post(url: String, payload: String, kind: String = "application/jose+json"): HttpResponse Throws NetError, TimeoutError
    Local header = Value.Object()
    header["alg"] = Value("ES256")
    If kid != Null
        header["kid"] = Value(kid ?? "")
    Else
        header["jwk"] = Acme.Jwk(key)
    End If
    header["nonce"] = Value(nonce)
    header["url"] = Value(url)
    Local protected = B64(Json.Write(header).Bytes)
    Local content = B64(payload.Bytes)
    Local jws = Value.Object()
    jws["protected"] = Value(protected)
    jws["payload"] = Value(content)
    jws["signature"] = Value(B64(EcdsaP256.Sign(key.Bytes, "{protected}.{content}")))
    Local response = Try Await client.SendAsync(HttpMethod.POST, url,
                                              Body(Json.Write(jws).Bytes, kind))
    nonce = response.Headers.Get("Replay-Nonce") ?? ""
    Return response
End Function

Function StatusOf(response: HttpResponse): String
    Local read = Try? Json.Read(response.Text)
    If read == Null Then Return "?"
    Return read["status"]?.AsString() ?? "?"
End Function

Async Function Run()
    Try
        nonce = (Try Await client.HeadAsync("{base}/acme/nonce")).Headers.Get("Replay-Nonce") ?? ""
        Local made = Try Await Post("{base}/acme/account", "{{\"termsOfServiceAgreed\":true}}",
                                    "text/plain")
        Print("text/plain new account: {made.Status} {StatusOf(made)}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: text/plain new account: 415 ?   (or 400: a malformed problem, as Pebble and Boulder answer)

Actual: text/plain new account: 201 valid

Suggested fix: Add a content_type (or headers) member to acme_request_t, and have acme_answer pass the request's Content-Type. In handle, refuse a POST whose type isn't application/jose+json with problem(415, "malformed", ...), with a Replay-Nonce. Add an acme_ca_tests case for it.

Skeptic: I ran http_ca4_ctype in debug. It printed 'text/plain new account: 201 valid'. acme_request_t (acme_ca.hpp:21-25) holds only method, path, and body, and acme_answer in http_server.cpp passes no headers, so the CA can't see a Content-Type at all. RFC 8555 6.2 says a request without application/jose+json MUST get a 415, and Pebble enforces it. AcmeClient does send application/jose+json (acme_client.fb:196). The gap is that no test checks it.

## ca-6 (low): Certificate serials are 0x10 then one byte of the order number, so they repeat after 256 orders

Status: fixed in f7879c12 (a serial is 0x10 and then the order's number in 8 bytes, big-endian), with acme_ca_tests: the first certificate's serial is 10 00 00 00 00 00 00 00 01

Where: tests/native/acme_ca/acme_ca.cpp:941

`serial{ 0x10, static_cast<std::uint8_t>(*index + 1) }` truncates the order number to a byte. Order 256 gets 10:00, and order 257 gets 10:01, the same serial as order 1. RFC 5280 4.1.2.2 requires a CA's serials to be unique, and a client that caches or compares certificates by issuer and serial would see two different certificates as one.

```
// RFC 5280 4.1.2.2: a CA's serial numbers are unique. The test CA's are 0x10 then the order's
// number as one byte, so the 257th certificate has the 1st's serial.
Import std.core
Import std.collections
Import std.crypto
Import std.data.json
Import std.ffi
Import std.io
Import std.net
Import std.net.acme
Import std.net.http
Import std.text

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

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Global key = KeyOf(1)
Global client = HttpClient()
Global nonce = ""
Global kid: String? = Null

Function B64(bytes: UInt8[]): String => Base64.Encode(bytes, True, False)

Async Function Post(url: String, payload: String): HttpResponse Throws NetError, TimeoutError
    Local header = Value.Object()
    header["alg"] = Value("ES256")
    If kid != Null
        header["kid"] = Value(kid ?? "")
    Else
        header["jwk"] = Acme.Jwk(key)
    End If
    header["nonce"] = Value(nonce)
    header["url"] = Value(url)
    Local protected = B64(Json.Write(header).Bytes)
    Local content = B64(payload.Bytes)
    Local jws = Value.Object()
    jws["protected"] = Value(protected)
    jws["payload"] = Value(content)
    jws["signature"] = Value(B64(EcdsaP256.Sign(key.Bytes, "{protected}.{content}")))
    Local response = Try Await client.SendAsync(HttpMethod.POST, url,
                                              Body(Json.Write(jws).Bytes, "application/jose+json"))
    nonce = response.Headers.Get("Replay-Nonce") ?? ""
    Return response
End Function

Function At(object: Value?, key: String): Value? => object == Null ? Null : object[key]

Function Text(object: Value?, key: String): String => At(object, key)?.AsString() ?? ""

Function First(object: Value?, key: String): Value?
    Local items = At(object, key)?.AsArray() ?? []
    If items.Count == 0 Then Return Null
    Return items[0]
End Function

Async Function Respond(listener: TcpListener, tokens: AcmeTokens)
    Loop
        Try
            Local connection = Try Await listener.AcceptAsync()
            Local reader = BufferedReader(connection)
            Local request = Try Await reader.ReadLineAsync() ?? ""
            Loop
                Local header = Try Await reader.ReadLineAsync() ?? ""
                If header == "" Then Break
            End Loop
            Local parts = request.Split(" ")
            Local answer = parts.Count > 1 ? tokens.Answer(parts[1]) : Null
            Local response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
            If answer != Null
                response = "HTTP/1.1 200 OK\r\nContent-Length: {answer.Bytes.Count}\r\n\r\n{answer}"
            End If
            Try Await connection.WriteAsync(Buffer<UInt8>.From(response.Bytes))
            connection.Close()
        Catch e As Error
            Return
        End Try
    End Loop
End Function

Async Function Run()
    Try
        Local listener = TcpListener.Listen("127.0.0.1", challengePort)
        Defer listener.Close()
        Local tokens = AcmeTokens()
        Local responding = Respond(listener, tokens)
        nonce = (Try Await client.HeadAsync("{base}/acme/nonce")).Headers.Get("Replay-Nonce") ?? ""
        Local made = Try Await Post("{base}/acme/account", "{{\"termsOfServiceAgreed\":true}}")
        kid = made.Headers.Get("Location")
        Local serials: String[] = []
        For n = 1 To 257
            Local order = Try Await Post("{base}/acme/order",
                "{{\"identifiers\":[{{\"type\":\"dns\",\"value\":\"localhost\"}}]}}")
            Local ordered = Try? Json.Read(order.Text)
            Local authz = First(ordered, "authorizations")?.AsString() ?? ""
            Local authorization = Try? Json.Read((Try Await Post(authz, "")).Text)
            Local challenge = First(authorization, "challenges")
            Local token = Text(challenge, "token")
            tokens.Publish(token, Acme.KeyAuthorization(token, key))
            Try Await Post(Text(challenge, "url"), "{{}}")
            Try Await Post(authz, "")
            Try Await Post(authz, "")
            Local csr = Base64.Encode(Acme.CertificateRequest(KeyOf(40), ["localhost"]), True, False)
            Local finalized = Try? Json.Read((Try Await Post(Text(ordered, "finalize"),
                                                             "{{\"csr\":\"{csr}\"}}")).Text)
            Local pem = (Try Await Post(Text(finalized, "certificate"), "")).Text
            serials.Add((Try Certificate.AllFromPem(pem))[0].SerialNumber)
        End For
        Print("1st {serials[0]}, 256th {serials[255]}, 257th {serials[256]}")
        Print("257th repeats the 1st: {serials[256] == serials[0]}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: 1st 10:01, 256th 01:10:00, 257th 01:10:01   (some unique serials)
257th repeats the 1st: False

Actual: 1st 10:01, 256th 10:00, 257th 10:01
257th repeats the 1st: True

Suggested fix: Write the order number as a big-endian 8-byte value after the 0x10 prefix, so serials stay unique and deterministic. Or use 16 random bytes with the top bit clear, as real CAs do.

Skeptic: I ran http_ca6_serial in debug. It printed '1st 10:01, 256th 10:00, 257th 10:01' and '257th repeats the 1st: True'. At acme_ca.cpp:941 the serial is built as `std::array<std::uint8_t, 2> serial{ 0x10, static_cast<std::uint8_t>(*index + 1) }`, which truncates the order index to one byte. That gives duplicate serials from the same issuer, which RFC 5280 4.1.2.2 forbids. No current test makes 256 orders, so this is low, but it is plainly a truncation bug.

## ca-7 (low): The CA writes the certificate into a fixed 4096-byte buffer, so an order with many names can't be issued

Status: fixed in f7879c12 (the certificate's buffer doubles, up to 64 KiB, while mbedTLS says it's too small, and an order of more than 100 names is refused as rejectedIdentifier, as Let's Encrypt's are), with acme_ca_tests' "an order of 100 names is issued, and one of more is refused": 101 names of 59 bytes are refused, and 100 are issued, a certificate of more than 4096 bytes that verifies for its first and last names

Where: tests/native/acme_ca/acme_ca.cpp:953

`std::vector<std::uint8_t> der(4096)` caps the issued certificate's DER at 4 KB. Let's Encrypt issues for up to 100 names. An order for 100 names of 59 bytes passes every authorization and has a well-formed CSR, but finalize answers 500 serverInternal 'the certificate couldn't be made'.

```
// An order for 100 names of 59 bytes, which Let's Encrypt takes (100 names at most): the test CA
// writes the certificate into 4096 bytes, so it can't issue it.
Import std.core
Import std.collections
Import std.crypto
Import std.data.json
Import std.ffi
Import std.io
Import std.net
Import std.net.acme
Import std.net.http
Import std.text

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

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Global key = KeyOf(1)
Global client = HttpClient()
Global nonce = ""
Global kid: String? = Null

Function B64(bytes: UInt8[]): String => Base64.Encode(bytes, True, False)

Async Function Post(url: String, payload: String): HttpResponse Throws NetError, TimeoutError
    Local header = Value.Object()
    header["alg"] = Value("ES256")
    If kid != Null
        header["kid"] = Value(kid ?? "")
    Else
        header["jwk"] = Acme.Jwk(key)
    End If
    header["nonce"] = Value(nonce)
    header["url"] = Value(url)
    Local protected = B64(Json.Write(header).Bytes)
    Local content = B64(payload.Bytes)
    Local jws = Value.Object()
    jws["protected"] = Value(protected)
    jws["payload"] = Value(content)
    jws["signature"] = Value(B64(EcdsaP256.Sign(key.Bytes, "{protected}.{content}")))
    Local response = Try Await client.SendAsync(HttpMethod.POST, url,
                                              Body(Json.Write(jws).Bytes, "application/jose+json"))
    nonce = response.Headers.Get("Replay-Nonce") ?? ""
    Return response
End Function

Function StatusOf(response: HttpResponse): String
    Local read = Try? Json.Read(response.Text)
    If read == Null Then Return "?"
    Return read["status"]?.AsString() ?? "?"
End Function

Function At(object: Value?, key: String): Value? => object == Null ? Null : object[key]

Function Text(object: Value?, key: String): String => At(object, key)?.AsString() ?? ""

Function First(object: Value?, key: String): Value?
    Local items = At(object, key)?.AsArray() ?? []
    If items.Count == 0 Then Return Null
    Return items[0]
End Function

Async Function Respond(listener: TcpListener, tokens: AcmeTokens)
    Loop
        Try
            Local connection = Try Await listener.AcceptAsync()
            Local reader = BufferedReader(connection)
            Local request = Try Await reader.ReadLineAsync() ?? ""
            Loop
                Local header = Try Await reader.ReadLineAsync() ?? ""
                If header == "" Then Break
            End Loop
            Local parts = request.Split(" ")
            Local answer = parts.Count > 1 ? tokens.Answer(parts[1]) : Null
            Local response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"
            If answer != Null
                response = "HTTP/1.1 200 OK\r\nContent-Length: {answer.Bytes.Count}\r\n\r\n{answer}"
            End If
            Try Await connection.WriteAsync(Buffer<UInt8>.From(response.Bytes))
            connection.Close()
        Catch e As Error
            Return
        End Try
    End Loop
End Function

Async Function Run()
    Try
        Local listener = TcpListener.Listen("127.0.0.1", challengePort)
        Defer listener.Close()
        Local tokens = AcmeTokens()
        Local responding = Respond(listener, tokens)
        nonce = (Try Await client.HeadAsync("{base}/acme/nonce")).Headers.Get("Replay-Nonce") ?? ""
        Local made = Try Await Post("{base}/acme/account", "{{\"termsOfServiceAgreed\":true}}")
        kid = made.Headers.Get("Location")
        Local names: String[] = []
        Local identifiers = ""
        For n = 100 To 199
            Local name = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa{n}.localhost"
            names.Add(name)
            identifiers += "{identifiers == "" ? "" : ","}{{\"type\":\"dns\",\"value\":\"{name}\"}}"
        End For
        Print("{names.Count} names of {names[0].Bytes.Count} bytes")
        Local order = Try Await Post("{base}/acme/order", "{{\"identifiers\":[{identifiers}]}}")
        Local ordered = Try? Json.Read(order.Text)
        For each In At(ordered, "authorizations")?.AsArray() ?? []
            Local authz = each.AsString() ?? ""
            Local authorization = Try? Json.Read((Try Await Post(authz, "")).Text)
            Local challenge = First(authorization, "challenges")
            Local token = Text(challenge, "token")
            tokens.Publish(token, Acme.KeyAuthorization(token, key))
            Try Await Post(Text(challenge, "url"), "{{}}")
            Try Await Post(authz, "")
            Try Await Post(authz, "")
        End For
        Local csr = Base64.Encode(Acme.CertificateRequest(KeyOf(40), names), True, False)
        Local finalized = Try Await Post(Text(ordered, "finalize"), "{{\"csr\":\"{csr}\"}}")
        Print("finalize: {finalized.Status} {StatusOf(finalized)} {Text(Try? Json.Read(finalized.Text), "detail")}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: 100 names of 59 bytes
finalize: 200 valid 

Actual: 100 names of 59 bytes
finalize: 500 ? the certificate couldn't be made

Suggested fix: Size the buffer from the request, for example 1024 plus 16 bytes per name plus the names' total length. Or retry with a doubled buffer while mbedtls_x509write_crt_der returns MBEDTLS_ERR_ASN1_BUF_TOO_SMALL. The CA could also refuse more than 100 identifiers at newOrder as rejectedIdentifier, as Let's Encrypt does.

Skeptic: I ran http_ca7_names in debug. It printed '100 names of 59 bytes' and then 'finalize: 500 ? the certificate couldn't be made'. That order passed every authorization and had a well-formed CSR. At acme_ca.cpp:953, `std::vector<std::uint8_t> der(4096)` is a fixed buffer, and mbedtls_x509write_crt_der fails when the certificate doesn't fit. A real CA (Let's Encrypt allows up to 100 names) would issue it, and the CA has no newOrder limit that would refuse it earlier. No current test hits it.

## ca-9 (low): acme_ca_tests says 'The same key finds its account again' but uses a new key; the CA's existing-account path (200) is never tested

Status: fixed in f7879c12: acme_ca_tests' first client asks for its account again with onlyReturnExisting, signed by its JWK, and gets 200 and its own Location; the new key's accountDoesNotExist has its own comment

Where: tests/native/acme_ca/acme_ca_tests.cpp:311

Under the comment 'The same key finds its account again', the test builds `client_t again(ca)`, whose constructor generates a new P-256 key, and checks that onlyReturnExisting gives accountDoesNotExist. That is the opposite case. RFC 8555 7.3.1's lookup of a known key (200 plus the existing account's Location, the `made ? 201 : 200` branch) is never exercised by fb_acme_ca_tests or by http_acme, where ObtainAsync skips registering once Kid is set. The CA does behave correctly here (the program below shows it), so the defect is the test and its comment.

```
// RFC 8555 7.3.1: a new-account request with a key the CA knows gives that account (200, its
// Location). acme_ca_tests.cpp says it checks this but makes a new key instead.
Import std.core
Import std.crypto
Import std.net

Global base = Input() ?? ""

Function KeyOf(first: Int): Key
    Local bytes: UInt8[] = []
    For i = 0 Until 32
        bytes.Add((first + i) As UInt8)
    End For
    Return Key(KeyKind.ECDSA_P256, True, bytes)
End Function

Async Function Run()
    Try
        Local first = Try Await AcmeClient("{base}/acme/dir", KeyOf(1)).RegisterAsync()
        Local again = Try Await AcmeClient("{base}/acme/dir", KeyOf(1)).RegisterAsync()
        Local other = Try Await AcmeClient("{base}/acme/dir", KeyOf(2)).RegisterAsync()
        Print("same key, same account: {first == again}; another key, another: {other != first}")
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Await Run()
```

Expected: same key, same account: True; another key, another: True   (and a fb_acme_ca_tests case checking the first half)

Actual: same key, same account: True; another key, another: True   (the CA is right, but acme_ca_tests.cpp:311-316 checks only a new key's accountDoesNotExist, under a comment that says it checks the same key)

Suggested fix: In acme_ca_tests.cpp, have the first client post to /account again (onlyReturnExisting true, or terms agreed). Check status 200 and that the Location equals client.account(). Keep the new-key accountDoesNotExist check under its own comment.

Skeptic: In acme_ca_tests.cpp:311-316, under the comment '// The same key finds its account again.', the code builds `client_t again(ca)`. Its constructor (line 86-93) calls psa_generate_key, which makes a new P-256 key. It then checks for 400 accountDoesNotExist, which is the opposite case. Grepping for every '/account' post in acme_ca_tests.cpp, none re-posts with an existing key and checks 200 and the Location. http_acme never registers the same key twice, since ObtainAsync skips registering once Kid is set (acme_client.fb:118). My run of http_ca9_again printed 'same key, same account: True; another key, another: True', so the CA itself is right. The defect is the test's comment and the CA's 200 path going untested.

## client-2 (low): AcmeClient's New doesn't panic for a malformed P-256 private key; the panic comes later, inside std

Status: fixed in 97f61f4e (AcmeClient's New takes the key's public key, which panics at the caller for bytes that aren't a P-256 private key), with panic_acme_client_key_bytes: a client of a 16-byte key panics where it's made

Where: std/net/acme_client.fb:67

New is documented, in its comment and in stdlib.md, to panic unless accountKey is an ECDSA P-256 private key. It checks only Kind and IsPrivate. Key's constructor checks nothing, so a key of 16 bytes, or 32 bytes of zeros, gets through. The first request then panics inside EcdsaP256.Sign, and the panic is reported at std/net/acme_client.fb:249 rather than where the user made the client. So @PanicsAtCaller doesn't help, and the user's own line never appears.

```
// Run as an http_ program, under fb_http_test_server, whose first input line is its HTTP URL.
Import std.core
Import std.collections
Import std.crypto
Import std.net
Import std.text

Global base = Input() ?? ""

Async Function Run(acme: AcmeClient)
    Try
        Print(Try Await acme.RegisterAsync())
    Catch e As Error
        Print("failed: {e.Message}")
    End Try
End Function

Local short: UInt8[] = []
For i = 1 To 16
    short.Add(i As UInt8)
End For

// 16 bytes isn't a P-256 private key: AcmeClient's New says it panics, here, at line 23.
Local acme = AcmeClient("{base}/acme/dir", Key(KeyKind.ECDSA_P256, True, short))
Print("made")
Await Run(acme)
```

Expected: panic: ... at http_c2.fb:23:... (raised by AcmeClient's New, before "made")

Actual: made
panic: an EcdsaP256 private key is 32 bytes, a number from 1 up to the curve's order at std/net/acme_client.fb:249:29
(the same in debug and release)

Suggested fix: In New, also check the key's bytes, for example by calling EcdsaP256.PublicKeyOf(accountKey.Bytes) (it is @PanicsAtCaller) or by checking for 32 bytes and a valid scalar. Then the panic happens at the caller. Extend panic_acme_client_key to cover it.

Skeptic: I ran rv-acme/refute-client/http_c2.fb, which makes an AcmeClient from Key(ECDSA_P256, True, 16 bytes). Debug and release both printed "made", then "panic: an EcdsaP256 private key is 32 bytes, a number from 1 up to the curve's order at std/net/acme_client.fb:249:29" (exit 101). New (lines 63-69) is @PanicsAtCaller and says it 'Panics unless accountKey is an ECDSA P-256 private key', but it checks only Kind and IsPrivate. pem.fb's Key doc defines a P-256 private key as 32 bytes. stdlib.md says misuse like a wrong-size key 'panics at the caller'. Here the panic comes later, inside a std method, and the user's line never appears. The panic does still happen, so I rate it low rather than medium.

## client-4 (low): AcmeClient's panic for a public P-256 key says "not ECDSA_P256"

Status: fixed in 97f61f4e (AcmeClient's New says "an ACME account's key is a private key, not a public one" for a public P-256 key, and the kind only for another kind), with panic_acme_client_key's new message

Where: std/net/acme_client.fb:68

The message interpolates only accountKey.Kind. For a P-256 key that is public rather than private, it reads "an ECDSA P-256 private key, not ECDSA_P256", which seems to contradict itself. tests/programs/panic_acme_client_key.out expects that confusing text. Acme.CertificateRequest's message, "... private key, not {key.Kind}", has the same flaw for a public P-256 key.

```
Import std.core
Import std.collections
Import std.crypto
Import std.net

Local bytes: UInt8[] = []
For i = 1 To 32
    bytes.Add(i As UInt8)
End For

AcmeClient("http://127.0.0.1:1/dir", Key(KeyKind.ECDSA_P256, False, EcdsaP256.PublicKeyOf(bytes)))
Print("unreachable")
```

Expected: panic: an ACME account's key is an ECDSA P-256 private key, not a public one at ...:11:1

Actual: panic: an ACME account's key is an ECDSA P-256 private key, not ECDSA_P256 at p.fb:11:1

Suggested fix: Use separate messages for the wrong kind and for a public key, for example "..., not a public key", in AcmeClient's New and in Acme.CertificateRequest, and update panic_acme_client_key.out.

Skeptic: I ran rv-acme/refute-client/p4.fb, which makes an AcmeClient with a public P-256 key. It printed "panic: an ACME account's key is an ECDSA P-256 private key, not ECDSA_P256 at p4.fb:11:1". The message contradicts itself for a key that is ECDSA_P256 but public, and tests/programs/panic_acme_client_key.out expects that exact text. acme.fb:76 (CertificateRequest) interpolates only key.Kind under the same || !IsPrivate condition, so it has the same flaw for a public P-256 key.

## csr-2 (low): A public P-256 key's panic reads "... a P-256 private key, not ECDSA_P256", and no test covers it

Status: fixed in 97f61f4e (CertificateRequest says "an ACME certificate's key is a private key, not a public one" for a public P-256 key), with panic_acme_public_key

Where: std/net/acme.fb:76

CertificateRequest panics for a public ECDSA P-256 key, which is correct. Its message, though, interpolates key.Kind, so it reads "is an ECDSA P-256 private key, not ECDSA_P256": it names the very kind it wants as the wrong one, and the user isn't told the real problem, that the key is public. AcmeClient's New has the same message shape, and panic_acme_client_key.out shows it too. panic_acme_key.fb only tests an ED25519 key, so the `!key.IsPrivate` branch has no test.

```
Import std.core
Import std.collections
Import std.crypto
Import std.net.acme

Local bytes: UInt8[] = []
For i = 1 To 32
    bytes.Add(i As UInt8)
End For

Acme.CertificateRequest(Key(KeyKind.ECDSA_P256, False, EcdsaP256.PublicKeyOf(bytes)), ["example.com"])
Print("unreachable")
```

Expected: panic: an ACME certificate's key is an ECDSA P-256 private key, not a public one at pubkey.fb:11:1

Actual: panic: an ACME certificate's key is an ECDSA P-256 private key, not ECDSA_P256 at pubkey.fb:11:1

Suggested fix: Use a separate message when the kind is right but the key is public, for example "..., not a public one", or say "a public {key.Kind} key". Do the same in AcmeClient's New. Add a panic_acme_ test for a public P-256 key passed to CertificateRequest.

Skeptic: Ran the finding's program as C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-acme/csr-names/pubkey.fb. Debug and release both printed "panic: an ACME certificate's key is an ECDSA P-256 private key, not ECDSA_P256 at pubkey.fb:11:1" (exit 101). That message names the required kind as the wrong one and never says the key is public. The cause is that std/net/acme.fb line 76 uses the same message, interpolating {key.Kind}, for both the wrong-kind case and the !key.IsPrivate case. AcmeClient's New in std/net/acme_client.fb line 68 has the same shape, and tests/programs/panic_acme_client_key.out records "...private key, not ECDSA_P256". tests/programs/panic_acme_key.fb only tests an ED25519 key, so nothing tests CertificateRequest's public-key branch.

## Refuted by the skeptics (no action)

- client-3: Acme.Jwk of a malformed P-256 public key panics with a slice error or gives a JWK of garbage. I reproduced it with rv-acme/refute-client/p3.fb: 65 zero bytes give a JWK of zeros, and 33 bytes panic with 'slice 33..65 is out of range for 33 elements at p3.fb:17:7'. That is a panic at the caller's own line, because Jwk/Thumbprint are @PanicsAtCaller. Neither Jwk's doc nor stdlib.md promises to validate a public key's bytes. They say only that it 'panics for another kind of key'. std.crypto doesn't validate malformed public keys elsewhere either: p3b.fb shows EcdsaP256.Verify returns False for both the 65 zero bytes and the 33-byte key, with no panic. So this is garbage in, garbage out, consistent with std.crypto, with no documented contract broken. It would be a nicer message, but it isn't a bug.
- client-5: Several documented client paths have no test: finding an existing account, ObtainAsync's own registration, multi-name orders. The coverage claim is accurate: http_acme registers each client once, registers explicitly before Obtain, and orders only ["localhost"]. But it isn't wrong behavior and contradicts no doc. I ran rv-acme/refute-client/http_c5.fb under the 0ed29228 test server: ObtainAsync on three names with auto-registration printed 'issued 2: CN=localhost http://127.0.0.1:52219/acme/account/1', and a fresh client on the same key printed 'found True'. The one real defect it would have caught is client-1, whose fix brings its own test (repeat RegisterAsync, and RegisterAsync after ObtainAsync). That is the natural place to add these cases.
- ca-5: The issued certificate's CN is the order's first name even past 64 bytes, and ignores the CSR's CN. It reproduces: a 65-byte first name gave subject CN=<the 65-byte name>, because the CA sets 'CN=' + order.names.front(). But Pebble does the same: newCertificate takes the CN from the first name in its domains list, not from the CSR. stdlib.md only asks for a CA 'as Pebble is', and nothing in the spec, stdlib.md, or the milestones row says the test CA must copy the CSR's subject or cap the CN at 64. No client behavior depends on it. ObtainAsync parsed the chain fine, and the client's own CN choice (the first name of at most 64 bytes) is tested directly by net_acme_request. It's a gap between a test fixture and Boulder that changes no test's result. Refuted.
- ca-8: The CA never reuses a valid authorization, so the client's 'already valid' path is untested and http_acme's second order fails where Let's Encrypt would issue. It reproduces: the second order for localhost fails with 'localhost didn't answer the challenge', because newOrder always makes new pending authorizations. But RFC 8555 doesn't require a CA to reuse authorizations; reuse is a server policy. Pebble can be configured not to reuse (PEBBLE_AUTHZREUSE=0), and its default reuses only about half the time, so a client can't rely on it. The http_acme case 'a second order, whose challenge nothing answers' is valid against an RFC-conforming CA that doesn't reuse, and AcmeClient handles both cases (acme_client.fb:149). Untested coverage of the 'already valid' branch is a test suggestion, not a defect that breaks the spec, stdlib.md, or the milestones row. Refuted.
