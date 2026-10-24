# The review of M6 step 29's HTTP client: HttpClient, Http.Get and the one-call helpers, and Download over libcurl 8.23.0 with the bundled mbedTLS and the OS's certificate roots, blocking and Async, tested against a local test server in tests/native (TlsStream and the TLS listener wait for step 13's streams)

24 confirmed findings: 1 high, 8 medium, 15 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S29-client-1 (high): Decompress does nothing: libcurl is built with no decoders, so a compressed response throws, and no test checks the done-when's decompression

Status: fixed in b8d13693: libcurl is built over the bundled zlib-ng and zstd, so Decompress asks for deflate, gzip, and zstd and decodes them; the test server's /compressed/<coding> compresses when it's asked to, and http_decompress checks each coding, blocking and Async, and that with Decompress off a body comes as it is, compressed or not

Where: third_party/CMakeLists.txt:309

third_party/CMakeLists.txt forces CURL_ZLIB, CURL_ZSTD, and CURL_BROTLI OFF, so libcurl can decode no content encoding. With Decompress = True (the default), CURLOPT_ACCEPT_ENCODING "" just sends "Accept-Encoding: identity". A server that sends gzip anyway, such as a static host serving a stored .gz object with Content-Encoding: gzip, makes every request fail with NetError OTHER "Unrecognized content encoding type". With Decompress = False the same response returns its 30 raw gzip bytes, so the default setting is the worse one. I checked this with a local Python server on 127.0.0.1 that answers /gzip with gzip.compress(b"hello gzip") and Content-Encoding: gzip: client.Get printed "NetError OTHER: Unrecognized content encoding type". The milestones row's done-when says the client "decompresses from a local test server", but the test server (tests/native/http_server/http_server.cpp) has no compressed path, and no test sets or checks Decompress. stdlib.md's Bundled C libraries table also says zlib-ng backs "curl's gzip". The program below runs under fb_http_test_server.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""
Global secure = Input() ?? ""
Global refused = Input() ?? ""

Local client = HttpClient()
Try
    Print("[" + client.Get("{base}/header/accept-encoding").Text + "]")
    client.Decompress = False
    Print("[" + client.Get("{base}/header/accept-encoding").Text + "]")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: [deflate, gzip] (the encodings libcurl decodes), then [(none)]. A gzip response decodes to its text.

Actual: [identity], then [(none)]. A gzip response from a local server throws NetError OTHER: Unrecognized content encoding type.

Suggested fix: Build libcurl with zlib (CURL_ZLIB ON against the bundled zlib-ng, which step 22 brings, or bring it here), and zstd/brotli if wanted. Add a /gzip path (and /deflate) to the test server that compresses only when Accept-Encoding asks, plus a test for Decompress True (decoded text) and False (raw bytes plus the Content-Encoding header). Until then, the docs should say Decompress decodes nothing.

Skeptic: I ran f1.fb under fb_http_test_server. It printed [identity] with the default Decompress = True, then [(none)] with it off. I then served /gzip from my own Python server on 127.0.0.1:48321, which sends gzip.compress(b"hello gzip") with Content-Encoding: gzip. client.Get threw "NetError OTHER: Unrecognized content encoding type", and with Decompress = False the same request returned 200 with the 30 raw bytes and Content-Encoding gzip. So the default setting is the one that fails. third_party/CMakeLists.txt:309-311 forces CURL_ZLIB, CURL_ZSTD, and CURL_BROTLI OFF, and no zlib-ng is fetched (step 22 hasn't landed, though m6-plan.md lists 22 as something 29 needs). Nothing in tests/native/http_server or the http_ programs sends or checks a compressed response. That leaves the milestones row's done-when ("decompresses from a local test server") unmet. It also contradicts client.fb's header comment ("decompresses what comes compressed") and stdlib.md's table, which says zlib-ng backs curl's gzip. The lane's listed decisions leave out brotli but say nothing about zlib.

## S29-async-server-1 (medium): Decompress decodes nothing: libcurl is built without zlib, so the done-when's "decompresses" is unmet and untested

Status: fixed in b8d13693: the fix of S29-client-1

Where: third_party/CMakeLists.txt:309

third_party/CMakeLists.txt forces CURL_ZLIB (and ZSTD, BROTLI) OFF, so libcurl has no content decoder at all. With Decompress = True, fb_http_options sets CURLOPT_ACCEPT_ENCODING to "" and libcurl, knowing no encodings, sends `Accept-Encoding: identity`. No response is ever compressed, so nothing is ever decompressed. milestones.md's step 29 done-when says the client "decompresses from a local test server". stdlib.md:644 says Decompress "asks for what libcurl decodes and decodes it", the zlib-ng row (stdlib.md:141) says it backs "curl's gzip", and client.fb's HttpClient comment says it "decompresses what comes so". The test server has no compressed path (answer() lists none), no test touches Decompress or Accept-Encoding, and milestones.md's Deferred work has no row for this. The step depends on step 22 (zlib-ng), which hasn't landed (m6-plan.md: "29 net II | 21, 22, 28").

```
// Decompress asks for compressed responses: Accept-Encoding goes with each request.
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
Print(client.Decompress)
Try
    Print(client.Get("{base}/header/accept-encoding").Text)
    Print((Await client.GetAsync("{base}/header/accept-encoding")).Text)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: True
an Accept-Encoding that names gzip (e.g. "deflate, gzip")
the same, for the Async request

Actual: True
identity
identity

Suggested fix: Build libcurl with CURL_ZLIB against the bundled zlib-ng (land step 22's zlib-ng first), add a gzip-encoded path to tests/native's server, and add a test that checks the decoded body and that Decompress = False gets the raw bytes. If that has to wait, add a Deferred work row and make stdlib.md and client.fb's comments say Decompress decodes nothing yet.

Skeptic: I ran f1.fb under fb_http_test_server.exe from build-rev (debug). It printed True / identity / identity, so a client with Decompress = True asks for no encoding at all. third_party/CMakeLists.txt:309-311 forces CURL_ZLIB, CURL_ZSTD and CURL_BROTLI OFF. http.cpp:893 sets CURLOPT_ACCEPT_ENCODING to "", and with no decoders built libcurl offers only identity. The milestones row 29 done-when says the client "decompresses from a local test server". stdlib.md's zlib-ng row says zlib-ng backs "curl's gzip". client.fb:2 and :128-129 say the client decompresses what comes compressed. answer() in tests/native/http_server has no compressed path, no test touches Decompress, and Deferred work has no row for it. Step 22 (std.compress and zlib-ng) hasn't landed: there is no std/compress, and m6-plan.md lists 22 as a dependency of 29. The lane's decision lists drop nghttp2, brotli and others, but not zlib. The done-when is unmet.

## S29-async-server-2 (medium): DownloadAsync's progress is told about a followed redirect's own body, as if it were the download

Status: fixed in 925fa1f4: on_progress tells nothing while a redirect that's followed comes (goes_on reads its status and Location header, which libcurl has before its body), so progress never counts a redirect's own body; the test server's /redirect-with-body sends 5000 bytes in two halves 300 ms apart, and http_progress checks that Download and DownloadAsync are told 2/2 alone

Where: runtime/src/http.cpp:200

on_progress answers the waiting progress Task whenever curl's dlnow changes, including while the body of a 3xx response that redirect() will follow is arriving. DownloadAsync then calls progress(n, n) with the redirect body's size and its Content-Length. After the redirect, the counts reset to 0, so the next calls go backwards (got and all both drop). Real servers often send a body with a redirect (nginx's 301 HTML, for example), so a progress bar shows 100% of the wrong response before the real download starts. stdlib.md says progress is told "how many came and how many the response said" of the body Download gives. The blocking path happens to skip the redirect hop here, because settle_blocking resets the counts in the same perform. The test server can't show this: every redirect it sends has an empty body. This repro used a 20-line local Python http.server on 127.0.0.1 that runs the program with its URL as the first input line. /bigmoved answers `302 Found`, `Location: /final`, and a 100000-byte body of 'x'; /final answers 200 with the 2-byte body "ok".

```
// Download's progress is told of the response it gives: a redirect's own body isn't that.
Import std.core
Import std.collections
Import std.net.http

Global base = Input() ?? ""
Global client = HttpClient()
Global told: String[] = []

Function Tell(got: Int64, all: Int64)
    If told.Count < 3 Then told.Add("{got}/{all}")
End Function

Try
    Local bytes = client.Download("{base}/bigmoved", Tell)
    Print("blocking {bytes.Count}: {told}")
    told.Clear()
    bytes = Await client.DownloadAsync("{base}/bigmoved", Tell)
    Print("async {bytes.Count}: {told}")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: blocking 2: [2/2]
async 2: [2/2, 2/2]  (only the final response's counts)

Actual: blocking 2: [2/2]
async 2: [100000/100000, 2/2, 2/2]  (the same in every run)

Suggested fix: Don't report progress for a hop that will be followed. For example, in on_progress, read CURLINFO_RESPONSE_CODE and skip answering when transfer.redirects >= 0 and the code is 301/302/303/307/308 with a Location. Or count body bytes in on_data only once the response isn't a followed redirect. Add a redirect with a body to tests/native's server (for example, give /redirect/<n> a body) and a DownloadAsync test that checks the counts never go down.

Skeptic: I wrote a local Python server (redirect_server.py in my scratch dir): /bigmoved answers 302 to /final with a 100000-byte body, and /final answers "ok". Debug and release, three runs each, printed `blocking 2: [2/2]` and `async 2: [100000/100000, 2/2, 2/2]`. When the redirect's 300000-byte body came in 100000-byte pieces 0.2 s apart, the blocking path showed it too: `blocking 2: [100000/300000, 200000/300000, 2/2]` and `async 2: [100000/300000, 200000/300000, 300000/300000]`. Both paths therefore tell progress about the redirect's own body, and the counts then go down. on_progress (http.cpp:200-224) answers on every change of dlnow. run_blocking (http.cpp:550-552, 561) returns 0 for each change too, before settle_blocking/redirect() has reset the counts. redirect() resets them only at http.cpp:459-464, after the hop's body has already been reported. stdlib.md says progress is told how many came of the body Download gives, and how many that response said.

## S29-async-server-4 (medium): A URL with a NUL is cut at the NUL, so the request goes to a different URL than the one given

Status: fixed in bac48c94: a URL with a NUL is refused before the transfer starts, as the outcome BAD_URL (set_text and refuse in http.cpp), so it's a NetError of OTHER, blocking or awaited, and so is a Proxy with one; http_refused checks both

Where: runtime/src/http.cpp:842

fb_http_new copies the URL into a std::string and hands its c_str() to CURLOPT_URL, and FutureBlitz Strings may hold NULs. libcurl therefore sees only the part before the first NUL. The request succeeds against that prefix, and response.Url reports the prefix. Other malformed URLs throw a NetError ("isn't an HTTP or HTTPS URL"). Truncation also lets "http://a.example\0.b.example/" go to a.example, which defeats a check of the URL string a program makes before fetching. fb_http_proxy has the same truncation. This is the shared path of blocking and Async requests.

```
// A URL with a NUL in it isn't one to send: it's no URL at all, not the part before the NUL.
Import std.core
Import std.net.http

Global base = Input() ?? ""
Global client = HttpClient()

Local url = "{base}/hello\0/../status/500"
Try
    Local response = client.Get(url)
    Print("blocking {response.Status} {response.Text}")
    response = Await client.GetAsync(url)
    Print("async {response.Status} {response.Text}")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: NetError OTHER

Actual: blocking 200 hello
async 200 hello  (debug and release)

Suggested fix: Reject a URL (and a Proxy) that contains '\0'. Either check in client.fb's Transfer.New and throw a NetError of OTHER, or have fb_http_new mark the transfer so that perform and wait_async end it with OUTCOME_BAD_URL. Add the case to http_client.fb's failures.

Skeptic: I ran f4.fb (url = "{base}/hello\0/../status/500") under the test server. Debug and release both printed `blocking 200 hello http://127.0.0.1:<port>/hello` and `async 200 hello http://127.0.0.1:<port>/hello`. The request went to the part of the URL before the NUL, and response.Url reports that prefix. http.cpp:842-844 builds a std::string from the whole view and passes c_str() to CURLOPT_URL. fb_http_proxy (http.cpp:896-899) truncates the same way. Elsewhere std rejects a NUL that C would cut off: database.fb:310 for a path, passwords.fb:85 for bcrypt. stdlib.md says a URL that isn't HTTP's or HTTPS's is a NetError of OTHER, but this one is silently sent somewhere else, so a check of the URL string can be bypassed.

## S29-client-2 (medium): A URL with a NUL is cut at the NUL and the shorter URL is fetched

Status: fixed in bac48c94: the fix of S29-async-server-4

Where: runtime/src/http.cpp:844

fb_http_new copies the URL into a std::string and hands libcurl address.c_str(), so anything after a \0 in the String is silently dropped. libcurl rejects CR, LF, tab, and space in a URL (each gives NetError OTHER, "URL rejected: Bad path"), but a NUL gets through: the request goes to a different URL than the one given, and the response's Url hides that. Proxy has the same problem, since CURLOPT_PROXY gets given.proxy.c_str(). std checks header names and values for NUL, but not URLs.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
Local url = base + "/hello\0/../nowhere"
Print(url.Bytes.Count)
Try
    Local response = client.Get(url)
    Print("{response.Status} {response.Text} {response.Url}")
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: 40, then NetError OTHER: ... isn't an HTTP or HTTPS URL ... (as for a URL with \r\n or a space)

Actual: 40, then 200 hello http://127.0.0.1:<port>/hello

Suggested fix: In Transfer.New (or fb_http_new), treat a URL that contains a NUL as BAD_URL, for example by setting an outcome the transfer reports without performing, so it throws NetError OTHER. Do the same for Proxy (a NUL makes it unusable). Add a test next to the ftp:// and http://[::1/ cases.

Skeptic: f2.fb printed 40, then "200 hello http://127.0.0.1:52687/hello", in both debug and release. The URL is cut at the \0 and the shorter one is fetched. fb_http_new (runtime/src/http.cpp:842-844) builds a std::string from the full view, then passes address.c_str() to CURLOPT_URL. fb_http_proxy at line 899 does the same with given.proxy.c_str(). std checks header fields for NUL but not URLs. This lets a URL cut at a NUL bypass a suffix check, e.g. "http://evil\0.trusted.com" fetches http://evil.

## S29-client-3 (medium): A URL over 8,000,000 bytes panics in the runtime instead of throwing a NetError

Status: fixed in bac48c94: a URL, Proxy, User-Agent, or Cookie longer than libcurl's 8,000,000 bytes is refused the same way, as is a redirect's URL, instead of panicking; http_refused checks each, blocking and awaited

Where: runtime/src/http.cpp:844

libcurl refuses any string option longer than CURL_MAX_INPUT_LENGTH (8,000,000 bytes) with CURLE_BAD_FUNCTION_ARGUMENT. fb_http_new wraps the CURLOPT_URL setopt in check(), which panics, and the panic has no location in the program. std.net.http doesn't check a URL's length first, so http.hpp's claim that std checks what it passes and a bad value here is a bug doesn't hold. The same panic happens for a UserAgent (line 905) or Proxy over 8 MB. Debug and release both panic.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

// A query of 2^23 bytes, past libcurl's 8,000,000-byte limit for a string option.
Local query = "a"
For i = 0 Until 23
    query = query + query
End For

Local client = HttpClient()
Try
    Print(client.Get("{base}/hello?q={query}").Status)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: NetError OTHER

Actual: panic: std.net.http's URL failed: A libcurl function was given a bad argument (exit 101)

Suggested fix: Turn a refused CURLOPT_URL into the BAD_URL outcome instead of panicking (do the same for the redirect's setopt), so Get throws NetError OTHER. For UserAgent and Proxy, either check their length in std where they're set (@PanicsAtCaller, as other misuse is) or turn them into a failed transfer too. Add a test.

Skeptic: f3.fb (a 2^23-byte query) printed "panic: std.net.http's URL failed: A libcurl function was given a bad argument" and exited 101, in both debug and release. fb_http_new wraps the CURLOPT_URL setopt in check() (http.cpp:844), which calls fb_panic with no place. http.hpp:11-12 says std checks what it passes first, but client.fb's Transfer.New doesn't check the URL's length. stdlib.md says a bad URL is a NetError OTHER and a real failure throws, so a long URL that came from input crashes the program where it should throw.

## S29-tls-build-1 (medium): An https:// proxy's certificate is checked against no roots at all, so every request through one fails with NetError TLS

Status: fixed in b64bf7df: fb_http_new gives libcurl the roots for an HTTPS proxy's TLS too (CURLOPT_PROXY_CAINFO_BLOB), and roots past libcurl's 8,000,000 bytes end the transfer as a TLS failure instead of panicking; http_tls goes through the test server's HTTPS ports as proxies with the test CA trusted, which works, and with the OS's roots, which fails

Where: runtime/src/http.cpp:869

fb_http_new passes the roots (the OS's, or the client's own from TrustOnly) only as CURLOPT_CAINFO_BLOB, which covers the origin's TLS. libcurl checks an HTTPS proxy's TLS against a separate setting, CURLOPT_PROXY_CAINFO_BLOB, which nothing sets. The build also has CURL_CA_BUNDLE and CURL_CA_PATH set to none, so the proxy connection trusts nothing. Any Proxy (or http_proxy/https_proxy variable) of the form https://... therefore always fails certificate verification. That holds whatever the OS's roots or TrustOnly say, and it contradicts the docs: 'trusting the OS's roots' and 'Trusts only roots for HTTPS'. I checked that the test server's HTTPS port works as an HTTPS proxy: the system curl.exe with --proxy-insecure through it gets 'proxied /hello'. So the only thing failing here is trust. No test covers an HTTPS proxy. Run it as: fb build proxy.fb -o proxy.exe, then fb_http_test_server proxy.exe.

```
// An HTTPS proxy's certificate: the client trusts the test CA, which signed the proxy's.
Import std.core
Import std.crypto
Import std.net.http

Local base = Input() ?? ""
Local secure = Input() ?? ""
Local refused = Input() ?? ""
Local pem = ""
Loop
    Local line = Input() ?? ""
    pem = pem + line + "\n"
    If line == "-----END CERTIFICATE-----" Then Break
End Loop

Local client = HttpClient()
Local roots: Certificate[] = (Try? Certificate.AllFromPem(pem)) ?? []
client.TrustOnly(roots)
Try
    Print("direct: " + client.Get("{secure}/hello").Text)
Catch e As NetError
    Print("direct: NetError {e.Kind}")
End Try

// The test server answers an absolute URL as a proxy would: "proxied <path>".
client.Proxy = base
Try
    Print("http proxy: " + client.Get("{base}/hello").Text)
Catch e As NetError
    Print("http proxy: NetError {e.Kind}")
End Try

client.Proxy = secure
Try
    Print("https proxy: " + client.Get("{base}/hello").Text)
Catch e As NetError
    Print("https proxy: NetError {e.Kind}: {e.Message}")
End Try
```

Expected: direct: hello
http proxy: proxied /hello
https proxy: proxied /hello

Actual: direct: hello
http proxy: proxied /hello
https proxy: NetError TLS: mbedTLS: The certificate is not correctly signed by the trusted CA
(the same in debug and --release)

Suggested fix: In fb_http_new, set CURLOPT_PROXY_CAINFO_BLOB to the same roots and flags as CURLOPT_CAINFO_BLOB. Add an http_tls case that uses the secure URL as Proxy with TrustOnly(test CA), plus one with TrustOsRoots() that must fail.

Skeptic: I ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s29/tlsbuild/proxy.fb under fb_http_test_server (stdin closed). Debug and --release both print the same thing: '1', 'direct: hello', 'http proxy: proxied /hello', then 'https proxy: NetError TLS: mbedTLS: The certificate is not correctly signed by the trusted CA'. The direct request to the same secure URL verifies under TrustOnly(test CA), so the only difference on the proxy leg is which roots it checks against. runtime/src/http.cpp has one CA option, CURLOPT_CAINFO_BLOB at line 869, and no CURLOPT_PROXY_CAINFO_BLOB. In curl 8.23's lib/vtls/vtls_config.c, line 320 sets ssl_origin->ca_info_blob from BLOB_CAINFO and line 403 sets ssl_proxy->ca_info_blob only from BLOB_CAINFO_PROXY. With CURL_CA_BUNDLE and CURL_CA_PATH set to none, an https:// proxy trusts nothing, whatever the OS roots or TrustOnly say. That contradicts stdlib.md ('trusting the OS's roots') and client.fb's TrustOnly docs.

## S29-tls-build-2 (medium): libcurl is built with no decoders, so Decompress decodes nothing, and a gzipped response throws NetError OTHER

Status: fixed in b8d13693: the fix of S29-client-1

Where: third_party/CMakeLists.txt:309

third_party builds curl with CURL_ZLIB, CURL_BROTLI, and CURL_ZSTD OFF. curl_config.h has no HAVE_LIBZ, and no compression library is bundled. With Decompress True (the default), CURLOPT_ACCEPT_ENCODING "" asks for every encoding libcurl decodes, and that list is only 'identity'. The done-when in milestone row 29 says the client 'decompresses from a local test server', but the test server has no compressed endpoint and no test checks decoding. client.fb says it 'decompresses what comes compressed', and stdlib.md's bundled table says zlib-ng backs 'curl's gzip'. Neither is true of this build. Worse, a server that sends Content-Encoding: gzip anyway makes every default request throw. I checked this with a local Python server (127.0.0.1) that answers with gzip: Decompress on gives 'NetError OTHER: Unrecognized content encoding type', and Decompress off gives the raw 25 compressed bytes. Run the program below as: fb build decompress.fb -o decompress.exe, then fb_http_test_server decompress.exe.

```
// What a client with Decompress on asks for: a gzip or deflate it can decode.
Import std.core
Import std.net.http

Local base = Input() ?? ""

Local client = HttpClient()
Print(client.Decompress)
Try
    Print(client.Get("{base}/header/accept-encoding").Text)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: True
an Accept-Encoding that lists gzip and deflate (for example "deflate, gzip"); and a gzipped response decoded to its text

Actual: True
identity
(and against a server that sends gzip: "NetError OTHER: Unrecognized content encoding type")

Suggested fix: Build libcurl over the bundled zlib (zlib-ng in its zlib-compatible mode, as stdlib.md plans): set CURL_ZLIB ON with ZLIB_INCLUDE_DIR/ZLIB_LIBRARY pointing at that target, add its archive to libcurl's archives, and add zlib-ng to libcurl's needs. Add a /gzip (and /deflate) endpoint to the test server, with a test that Decompress decodes it and that Decompress False gives the raw bytes. If zlib has to wait for step 22, record that in Deferred work, and in the meantime make the docs and the done-when say so.

Skeptic: decompress.fb under the test server prints 'True' then 'identity': with Decompress on, curl advertises no encodings. _deps/curl-build/lib/curl_config.h has '/* #undef HAVE_LIBZ */', and third_party sets CURL_ZLIB, CURL_ZSTD and CURL_BROTLI OFF, with no zlib bundled at this commit (step 22 hasn't landed). The test server and http_*.fb/.out have no gzip or deflate endpoint or check. Against my own local Python server (127.0.0.1, which always sends Content-Encoding: gzip), gz.fb printed 'on: NetError OTHER: Unrecognized content encoding type' and 'off: 40 bytes'. Milestone row 29's done-when says the client 'decompresses from a local test server'. client.fb says it 'decompresses what comes compressed', and stdlib.md's table says zlib-ng backs curl's gzip. None of that holds, and Deferred work has no entry for it.

## S29-tls-build-3 (medium): libcurl's notice leaves out the ISC license of its inet_pton.c and inet_ntop.c, which every HTTP program links

Status: fixed in 69729d6b: curl's notice has the comments before the code of lib/curlx/inet_ntop.c and inet_pton.c after its COPYING, and its licenses are "the curl and ISC licenses"; driver_tests checks the notice's texts

Where: third_party/CMakeLists.txt:332

The notice for curl is only curl's COPYING (notices/curl.txt, 22 lines), and fb reports 'under the curl license'. But curl 8.23's lib/curlx/inet_ntop.c and lib/curlx/inet_pton.c are 'Copyright (C) 1996-2022 Internet Software Consortium', SPDX ISC. Their license requires that 'the above copyright notice and this permission notice appear in all copies'. Both are compiled unconditionally, and both end up in every program that makes an HttpClient: nm of a --release build shows T curlx_inet_ntop and T curlx_inet_pton. libuv's entry already handles the same case for its ISC inet.c, so programs that ship the notice fb names still ship without the ISC notice.

```
// Any program that makes an HttpClient links libcurl, whose curlx/inet_pton.c and inet_ntop.c are
// ISC-licensed.
Import std.core
Import std.net.http

Local client = HttpClient()
Print(client.Decompress)
```

Expected: fb build --release prints libcurl under the curl and ISC licenses, and notices/curl.txt has the ISC copyright and permission text from lib/curlx/inet_ntop.c and inet_pton.c

Actual: 'Bundled libcurl 8.23.0, under the curl license; ship its notices with the program: .../third_party/notices/curl.txt'. curl.txt has no ISC text (grep -c ISC gives 0), yet nm notice.exe shows 'T curlx_inet_ntop' and 'T curlx_inet_pton'

Suggested fix: As for libuv: append the leading license comment of lib/curlx/inet_ntop.c and lib/curlx/inet_pton.c to the curl notice, and set FB_CURL_LICENSES to "the curl and ISC licenses". A driver test could check that the notice names both.

Skeptic: curl-src/lib/curlx/inet_ntop.c and inet_pton.c are both 'SPDX-License-Identifier: ISC' (Internet Software Consortium), and their notice must 'appear in all copies'. Neither has a HAVE_INET_* guard. Across curl's lib and include, the SPDX counts are 409 curl, 2 ISC, and 1 'curl AND ISC' (vssh.c, which isn't built). fb build --release notice.fb prints 'Bundled libcurl 8.23.0, under the curl license'. nm notice.exe shows 'T curlx_inet_ntop' and 'T curlx_inet_pton'. grep -c for ISC and for 'Internet Software' in build-rev/third_party/notices/curl.txt both give 0. libuv's entry in third_party/CMakeLists.txt (lines 29-43) already appends ISC inet.c's header and names ISC, so curl's entry falls short of that precedent.

## S29-async-server-3 (low): Download's progress says -1 ("didn't say") for a response whose Content-Length is 0

Status: fixed in 925fa1f4: on_progress takes how many the response said from CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, which is -1 only when it didn't say; http_progress checks that /bytes/0 tells 0/0 and /chunked 50/-1

Where: runtime/src/http.cpp:208

on_progress stores `expected > 0 ? expected : -1`. curl's xferinfo dltotal is 0 both when the size is unknown and when the response says Content-Length: 0, so an empty body that states its size is reported as having no stated size. fb/http.hpp and stdlib.md say fb_http_expected gives "how many it said it has, or -1 when it didn't say". The blocking and Async paths share this.

```
// An empty body's response says it has 0 bytes, which Download's progress is told.
Import std.core
Import std.net.http

Global base = Input() ?? ""
Global client = HttpClient()

Try
    Local head = client.Head("{base}/bytes/0")
    Print("Content-Length: {head.Headers.Get("Content-Length") ?? "none"}")
    client.Download("{base}/bytes/0", Function(got: Int64, all: Int64)
        Print("blocking {got}/{all}")
    End Function)
    Await client.DownloadAsync("{base}/bytes/0", Function(got: Int64, all: Int64)
        Print("async {got}/{all}")
    End Function)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: Content-Length: 0
blocking 0/0
async 0/0

Actual: Content-Length: 0
blocking 0/-1
async 0/-1  (debug and release)

Suggested fix: Take the expected size from curl_easy_getinfo(CURLINFO_CONTENT_LENGTH_DOWNLOAD_T), which is -1 only when the response didn't say, rather than from xferinfo's dltotal. Add /bytes/0 to a progress test.

Skeptic: I ran f3.fb under the test server. Debug and release both printed `Content-Length: 0`, `blocking 0/-1`, `async 0/-1`. http.cpp:208 stores `expected > 0 ? expected : -1`, so a response that says Content-Length: 0 is reported as one that didn't say. client.fb:228-229 and stdlib.md say progress gets "how many the response said it has, or -1 when it didn't say". It's wrong only for an empty body that states its length, so I rate it low.

## S29-async-server-5 (low): The test server runs http_ programs with the environment's proxy, so the tests send their requests to it

Status: fixed in 9af835f5: the test server runs its program with its own environment but http_proxy, https_proxy, and all_proxy, of either case (names_proxy in run()); a program test sets all three and checks that a GET of the server's URL still works

Where: tests/native/http_server/http_server.cpp:658

run() starts the program with the server's own environment: environ for posix_spawn, and the inherited block for CreateProcessA. The tests' clients leave Proxy as Null, which uses http_proxy/https_proxy. On a machine or CI runner behind a proxy, every request to the test server's 127.0.0.1 URLs goes to that proxy instead. The http_ tests then fail (or hit the network, which the step's tests must never do).

```
// The test server's URLs with a proxy in the environment, as a machine behind one has.
// Run as: http_proxy=http://127.0.0.1:9 fb_http_test_server proxy_env.exe
Import std.core
Import std.net.http

Global base = Input() ?? ""

Try
    Print(HttpClient().Get("{base}/hello").Text)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: hello

Actual: NetError REFUSED  (prints hello only when http_proxy is unset)

Suggested fix: In the test server's run(), give the child an environment without http_proxy, https_proxy, all_proxy and their uppercase forms, or with no_proxy=127.0.0.1 added. On Windows that means passing an environment block to CreateProcessA; on POSIX, a filtered envp to posix_spawn.

Skeptic: f5.fb run under fb_http_test_server.exe printed `hello`. The same exe run as `http_proxy=http://127.0.0.1:9 fb_http_test_server.exe f5.exe` printed `NetError REFUSED`: the client sent its 127.0.0.1 request to the environment's proxy. run() in http_server.cpp passes the server's own environment to the child: lpEnvironment is nullptr in CreateProcessA at line 684, and `environ` goes to posix_spawn at line 715. The tests' clients leave Proxy as Null, which means the environment's proxy (client.fb:141-143). On a machine or runner with http_proxy set, the http_ tests fail or reach a real proxy, against the rule that tests never use the network.

## S29-async-server-6 (low): http_async's progress check passes on the call Downloaded makes at the end, alone

Status: fixed in 925fa1f4: http_async and http_progress download /bytes/200000?pause=100000, whose first half comes 300 ms before the rest, and check that progress was told of less than all, blocking and awaited; http_progress covers Http.Download, GetBytes, DownloadAsync, and GetBytesAsync too

Where: tests/programs/http_async.fb:98

Transfer.Downloaded (client.fb:387-389) always calls progress once after the transfer ends. http_async's `{calls > 0}` is therefore true even if no progress Task were ever answered while the download came, so it doesn't check what the test's header claims ("downloads with progress on the main thread"). For 200000 bytes, the async download told progress 3 times in each of 20 release runs, so a stronger check holds.

```
// http_async's download check passes on the call DownloadAsync makes once it's done, alone.
Import std.core
Import std.net.http

Global base = Input() ?? ""
Global calls = 0

Local client = HttpClient()
Try
    Local bytes = Await client.DownloadAsync("{base}/bytes/0", Function(got: Int64, all: Int64)
        calls += 1
    End Function)
    Print("{bytes.Count} calls {calls}: calls > 0 is {calls > 0}")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: a check that only progress told during the download satisfies (no call came here before the end, so it should be False)

Actual: 0 calls 1: calls > 0 is True

Suggested fix: In http_async.fb, check that progress was told before the end: count the calls with got < all, or check calls > 1 for the 200000-byte download. Do the same for http_client's blocking Download.

Skeptic: f6.fb (DownloadAsync of /bytes/0) printed `0 calls 1: calls > 0 is True`. Transfer.Downloaded (client.fb:387-389) always calls progress once at the end, so http_async.fb:98's `{calls > 0}` passes even if no progress were told while the download came. If on_progress never answered, the loop would end on the final answer and print the same line (1 call, ordered, last = expected = 200000). The test therefore doesn't check what its header claims. Also, grepping tests/ finds no test of the blocking Download with progress (the fb_http_advance path), or of Http.Download and Http.GetBytes. The only blocking Download, in http_tls.fb:47, passes no progress.

## S29-async-server-8 (low): stdlib.md drops or postpones parts of the design with no Deferred work row

Status: fixed in 2b6344ff: stdlib.md says HTTP/2, decompression, and the timeouts work now, and that a Download to a file waits for step 14's files; milestones.md is the coordinator's, so the Deferred work rows, a Download to a path (step 14) and TlsStream and the TLS listener with the done-when of a client and server in one program (step 13), are in the lane's report

Where: docs/stdlib.md:654

The step rewrote stdlib.md's std.net.http entry. Before, it read "(libcurl: redirects, proxies, cookies, gzip, HTTP/2), `Download(url, path, progress)`". Now it reads "HTTP/1.1 only, for now", "a request may take as long as it takes but its connecting 30 seconds until std.time's Durations set them", and `Download(url, progress)`, which only returns bytes in memory: the file-path form the package manager would use is gone. The step's own TlsStream and TLS listener also wait for step 13. None of these has a row in milestones.md's Deferred work, which CLAUDE.md says lists everything left for later and where it's planned. The step's done-when ("a TLS client and server in one program talk through a test certificate") is likewise not recorded as moved.

```
// The design's Download(url, path, progress) writes the body to a file.
Import std.core
Import std.net.http

Global base = Input() ?? ""

Try
    HttpClient().Download("{base}/bytes/1000", "bytes.bin", Null)
    Print("saved")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: saved (per the earlier design), or a Deferred work row saying when the file form, HTTP/2, settable timeouts, and TlsStream/the TLS listener come

Actual: error: too many arguments to 'Download'; milestones.md's Deferred work has no row for any of these

Suggested fix: Add Deferred work rows: Download to a path (step 14's files), TlsStream and the TLS listener with their done-when (step 13), settable timeouts (std.time's Durations), HTTP/2 if it's still planned, and decompression (see S29-async-server-1). Each row names the step it moves to.

Skeptic: f8.fb fails to compile with `error: too many arguments to 'Download'`. The diff rewrites stdlib.md's entry: `Download(url, path, progress)` and "(libcurl: redirects, proxies, cookies, gzip, HTTP/2)" become `Download(url, progress)`, "HTTP/1.1 only, for now", and timeouts "until std.time's Durations set them". stdlib.md:625 still lists TlsStream and a TLS listener, and milestones row 29 has them in scope, with the done-when "a TLS client and server in one program talk through a test certificate". None of these exist, and the change doesn't touch docs/milestones.md: Deferred work gets no row for TlsStream or the listener, decompression, Download to a path, HTTP/2, or settable timeouts. milestones.md says "Each step adds what it leaves out", and CLAUDE.md says Deferred work lists everything left for later. The deferrals themselves are lane decisions; not recording them breaks that rule.

## S29-client-10 (low): A redirect to a URL that isn't HTTP or HTTPS is reported as the request's own URL not being HTTP

Status: fixed in c1429637: a URL a redirect gave that libcurl refuses as not HTTP's or HTTPS's ends the transfer as the outcome BAD_REDIRECT, whose NetError says "<url> was sent on to <target>, which isn't an HTTP or HTTPS URL"; http_refused checks one redirect and two

Where: std/net/http/client.fb:401

Fail's BAD_URL message is "{Url} isn't an HTTP or HTTPS URL", where Url is the URL the request was given. When the request is fine but a redirect points at ftp:// (CURLOPT_PROTOCOLS_STR refuses it as UNSUPPORTED_PROTOCOL), the message calls the http:// URL not HTTP's.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
Try
    Print(client.Get("{base}/redirect-to?url=ftp://127.0.0.1/file").Status)
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: NetError OTHER: a message naming the redirect's target, like "http://127.0.0.1:<port>/redirect-to?url=... was sent on to ftp://127.0.0.1/file, which isn't an HTTP or HTTPS URL"

Actual: NetError OTHER: http://127.0.0.1:<port>/redirect-to?url=ftp://127.0.0.1/file isn't an HTTP or HTTPS URL: Protocol "ftp" is disabled

Suggested fix: When a transfer has followed redirects, have the message name the effective or target URL (fb_http_url, or the target kept in redirect()), or reject a non-HTTP Location in redirect() with its own message. Add the case to http_client.fb.

Skeptic: f10.fb printed "NetError OTHER: http://127.0.0.1:52708/redirect-to?url=ftp://127.0.0.1/file isn't an HTTP or HTTPS URL: Protocol \"ftp\" is disabled". The request's own URL is http://, but the message says it isn't HTTP. redirect() sets the ftp target, curl refuses it with UNSUPPORTED_PROTOCOL, outcome_of maps that to BAD_URL, and Fail (client.fb:400-402) always names the original Url.

## S29-client-11 (low): UserAgent = "" sends no User-Agent header at all

Status: fixed in 5b1fcee4: an empty UserAgent goes as an empty User-Agent header, unless the request's headers have one; the test server's /header/<name> tells an empty value from none, and http_client checks it

Where: std/net/http/client.fb:158

UserAgent is documented as the User-Agent header a request sends unless its headers have one, checked as a header's value is, and an empty value is a valid header value (Headers.Add("X", "") sends "X:"). But libcurl skips CURLOPT_USERAGENT when it's empty, so UserAgent = "" sends no User-Agent, and the docs don't mention it. I checked this with a local Python server on 127.0.0.1:48219 whose /headers path echoes the request's headers. An empty DefaultHeaders value arrives as "x-empty:", but there is no user-agent line.

```
Import std.core
Import std.net.http

Global py = "http://127.0.0.1:48219"

Local client = HttpClient()
client.UserAgent = ""
client.DefaultHeaders.Add("X-Empty", "")
Try
    Print(client.Get("{py}/headers").Text)
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: host: 127.0.0.1:48219
user-agent:
accept: */*
accept-encoding: identity
x-empty:   (or docs saying an empty UserAgent sends none)

Actual: host: 127.0.0.1:48219
accept: */*
accept-encoding: identity
x-empty:

Suggested fix: Either document that an empty UserAgent sends no User-Agent, or send it as an empty header (add "User-Agent;" to the request's list when the agent is empty and the headers have none).

Skeptic: Against my Python /headers echo on 127.0.0.1:48321, f11.fb with UserAgent = "" printed host, accept, accept-encoding, and "x-empty: ", but no user-agent line. A default client sent "user-agent: FutureBlitz". client.fb:155 documents UserAgent as "The User-Agent header a request sends unless its headers have one", and "" passes CheckField. fb_http_user_agent passes "" to CURLOPT_USERAGENT, and libcurl skips an empty agent. Headers.Add("X", "") sends an empty header, so the two behave differently. Nothing documents that an empty UserAgent sends none.

## S29-client-4 (low): Body doesn't check its ContentType, so a bad one panics inside std/net/http/client.fb, not at the program's line

Status: fixed in 3d7a3787: Body's New and Text are @PanicsAtCaller and check a Content-Type as Headers.Add checks a value; panic_http_body_content_type

Where: std/net/http/client.fb:63

Body.New, Body.Text, and the other Body constructors accept any contentType String. The check happens only when Transfer.New calls sent.Add("Content-Type", contentType) (line 345), and because Transfer.New isn't @PanicsAtCaller, the panic reports std's own line. std's convention (decisions.md, @PanicsAtCaller) is that a misuse panic points at the program's line, as Headers.Add and UserAgent already do. The panic also comes at send time, possibly far from where the Body was made.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local body = Body.Text("hi", "text/plain\r\nX-Injected: yes")
Print("made the body")
Local client = HttpClient()
Try
    Print(client.Post("{base}/echo", body).Text)
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: panic: the value for Content-Type has a control character, which a header can't have at <program>.fb:6:1

Actual: made the body
panic: the value for Content-Type has a control character, which a header can't have at std/net/http/client.fb:345:17

Suggested fix: Make Body's New and Text @PanicsAtCaller and have them run CheckField("Content-Type", contentType) when contentType isn't Null (Json and Form use fixed types). Add a panic_http_body_content_type test.

Skeptic: f4.fb printed "made the body", then "panic: the value for Content-Type has a control character, which a header can't have at std/net/http/client.fb:345:17" and exited 101, in both debug and release. Body.New and Body.Text (client.fb:63-72) store contentType unchecked. The check runs in Transfer.New's sent.Add (line 345), which isn't @PanicsAtCaller. decisions.md:583 and stdlib.md:179 say a misuse panic in std points at the program's line. The panic still fires with a clear message, so only its place is wrong.

## S29-client-5 (low): Download's progress says -1 ("didn't say") for a response whose Content-Length is 0

Status: fixed in 925fa1f4: the fix of S29-async-server-3

Where: runtime/src/http.cpp:208

on_progress stores expected = expected > 0 ? expected : -1, so a response that says Content-Length: 0 is reported the same way as one with no length. HttpClient.Download's docs say progress gets how many bytes the response said it has, or -1 when it didn't say. The test server's /bytes/0 sends Content-Length: 0, and progress's final call is (0, -1). libcurl's xferinfo gives dltotal 0 both for an unknown length and for 0, so the runtime has to read CURLINFO_CONTENT_LENGTH_DOWNLOAD_T (-1 when unknown) to tell them apart. DownloadAsync uses the same code.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
Try
    Local said: Int64 = 99
    Local got: Int64 = 99
    Local bytes = client.Download("{base}/bytes/0", Function(g: Int64, all: Int64)
        got = g
        said = all
    End Function)
    Print("empty: {bytes.Count} got {got} said {said}")
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: empty: 0 got 0 said 0

Actual: empty: 0 got 0 said -1

Suggested fix: Set expected from curl_easy_getinfo(CURLINFO_CONTENT_LENGTH_DOWNLOAD_T), which is -1 only when there's no length, once the headers have come (for example in a header callback or on_progress), instead of from dltotal > 0. Add the /bytes/0 case to the download tests.

Skeptic: f5.fb printed "empty: 0 got 0 said -1" in both debug and release. f5b.fb showed that /bytes/0's response has Content-Length: 0, that /bytes/5 reports said 5, and that /chunked reports -1. on_progress (http.cpp:208) sets expected = expected > 0 ? expected : -1, so a stated length of 0 comes out as "didn't say". That contradicts Download's doc (client.fb:227-229): "how many the response said it has, or -1 when it didn't say".

## S29-client-6 (low): Download's progress counts a redirect response's body as the download's, so progress can go backwards

Status: fixed in 925fa1f4: the fix of S29-async-server-2

Where: runtime/src/http.cpp:461

on_progress counts every response's body, including the 3xx responses the runtime follows. redirect() resets downloaded and expected afterwards, but a redirect body that arrives in pieces has already been reported. The test server's redirects all have empty bodies, so the tests can't see this. I reproduced it with a local Python server on 127.0.0.1:48219. Its /slow-redirect answers "302 Found" with Location: /small and Content-Length: 5000, sends the first 2500 bytes, pauses 1.5 s, then sends the rest. /small answers 100 bytes. progress was first told 2500 of 5000, which is the redirect page's size, then 100 of 100.

```
Import std.core
Import std.collections
Import std.net.http

Global py = "http://127.0.0.1:48219"
Global seen: String[] = []
Global last: Int64 = 0
Global backwards = False

Local client = HttpClient()
Try
    Local bytes = client.Download("{py}/slow-redirect", Function(got: Int64, all: Int64)
        If got < last Then backwards = True
        last = got
        Local entry = "{got}/{all}"
        If seen.Count == 0 || seen[seen.Count - 1] != entry Then seen.Add(entry)
    End Function)
    Print(bytes.Count)
    Print("went backwards: {backwards}")
    Print(seen)
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: 100
went backwards: False
[100/100]

Actual: 100
went backwards: False... no: 100
went backwards: True
[2500/5000, 100/100]

Suggested fix: Don't count or report a response that will be followed: in on_progress or on_data, ignore bytes while the response code is a 3xx the transfer will follow (for example, check CURLINFO_RESPONSE_CODE and the redirect settings, or set a flag from a header callback). Add a test-server path that sends a redirect with a body in pieces.

Skeptic: My Python server on 127.0.0.1:48321 answers /slow-redirect with a 302 to /small and Content-Length 5000, sends 2500 bytes, waits 1.5 s, then sends the rest. /small answers 100 bytes. f6.fb printed 100, "went backwards: True", and [2500/5000, 100/100]. on_progress counts every response's body, and redirect() (http.cpp:458-464) resets downloaded and expected only after the redirect's body was already reported. The returned bytes are right, since received is cleared.

## S29-client-7 (low): Send(PUT or PATCH) without a body sends no Content-Length, while POST sends Content-Length: 0

Status: fixed in 3b15fa8e: set_request gives a PUT or PATCH without a body an empty one, as it gives a POST, so each says Content-Length: 0; http_client checks POST, PUT, PATCH, and DELETE

Where: runtime/src/http.cpp:315

set_request gives a POST with no body an empty body, so it sends Content-Length: 0. PUT and PATCH sent through Send with no body instead go out as a bodiless custom request with no Content-Length. RFC 9110 section 8.6 says a user agent SHOULD send Content-Length when the method gives content a meaning, and servers that require it answer 411 Length Required.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
For method In [HttpMethod.POST, HttpMethod.PUT, HttpMethod.PATCH]
    Try
        Local response = client.Send(method, "{base}/header/content-length")
        Print("{method} without a body: Content-Length {response.Text}")
    Catch e As NetError
        Print("NetError {e.Kind}: {e.Message}")
    Catch e As TimeoutError
        Print("TimeoutError")
    End Try
End For
```

Expected: POST without a body: Content-Length 0
PUT without a body: Content-Length 0
PATCH without a body: Content-Length 0

Actual: POST without a body: Content-Length 0
PUT without a body: Content-Length (none)
PATCH without a body: Content-Length (none)

Suggested fix: In set_request, treat PUT and PATCH like POST and send an empty body when none was given (the condition becomes transfer.body || method is POST, PUT, or PATCH). Add these cases to http_client.fb.

Skeptic: f7.fb printed "POST without a body: Content-Length 0", then "PUT without a body: Content-Length (none)" and the same for PATCH, in both debug and release. set_request (http.cpp:314) gives an empty body only to `transfer.body || method == "POST"`. RFC 9110 section 8.6 says a user agent SHOULD send Content-Length when the method gives content a meaning. The fetch standard, which the lane follows for redirects, sends Content-Length: 0 for a bodiless POST or PUT. Servers that require a length answer 411. It's reachable only through Send(PUT/PATCH, url) with no body.

## S29-client-8 (low): EnsureSuccess's message ends in a space for a status with no name

Status: fixed in 17ed2655: EnsureSuccess adds the status's reason only when it has one; http_client checks 404's message and 418's

Where: std/net/http/client.fb:119

The message is "{Url} answered {Status} {reason}", and StatusCode.Reason gives "" for an unnamed status such as 418 or 599, so the message ends with a stray space.

```
Import std.core
Import std.net.http

Global base = Input() ?? ""

Local client = HttpClient()
Try
    client.Get("{base}/status/418").EnsureSuccess()
Catch e As NetError
    Print("[{e.Message}]")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: [http://127.0.0.1:<port>/status/418 answered 418]

Actual: [http://127.0.0.1:<port>/status/418 answered 418 ]

Suggested fix: Add the reason only when it isn't empty, for example reason == "" ? "{Url} answered {Status}" : "{Url} answered {Status} {reason}".

Skeptic: f8.fb printed "[http://127.0.0.1:52703/status/418 answered 418 ]", ending in a stray space. EnsureSuccess (client.fb:118-119) formats "{Url} answered {Status} {reason}", and StatusCode.Reason gives "" for 418.

## S29-client-9 (low): Body.Form's doc says it encodes "as a browser sends them", but browsers encode differently

Status: fixed in 37445826: Form's comment says it writes application/x-www-form-urlencoded, which servers read as they read a browser's, and no longer that it's what a browser sends

Where: std/net/http/client.fb:77

The comment says the fields are encoded as a browser sends them, then lists the actual rule: every byte except letters, digits, '-', '.', '_', and '~' becomes %XX. Browsers use the URL standard's application/x-www-form-urlencoded serializer, which turns a space into '+', leaves '*' as is, and encodes '~' as %7E. Servers decode both forms, so only the claim is wrong.

```
Import std.core
Import std.collections
Import std.net.http

Local form = Body.Form({"a b": "~*"})
Print(HttpResponse(200, Headers(), form.Bytes, "").Text)
```

Expected: a+b=%7E* (what a browser sends, as the comment claims)

Actual: a%20b=~%2A

Suggested fix: Drop "as a browser sends them" from the comment (keep the exact rule, which servers decode the same way), or switch to the URL standard's serializer and update net_http_headers.out.

Skeptic: f9.fb printed "a%20b=~%2A". The WHATWG application/x-www-form-urlencoded serializer that browsers use turns a space into '+', leaves '*' as is, and encodes '~' as %7E, so a browser sends "a+b=%7E*". client.fb:77's "as a browser sends them" is therefore false. The rule listed after it matches what the code does, and servers decode both forms, so only the comment is wrong.

## S29-tls-build-4 (low): A certificate error's NetError message ends in a newline, and puts each further problem on a line of its own

Status: fixed in 611e19e7: fb_http_message gives libcurl's words as one line, each line's end trimmed and the lines joined by "; "; http_tls prints a TLS failure's Message

Where: runtime/src/http.cpp:1000

fb_http_message returns libcurl's error buffer as it is. For a verification failure, curl's mbedTLS backend fills it with mbedtls_x509_crt_verify_info, which ends every flag's text with '\n'. So every certificate error's Message ends in a newline. With two problems (I checked one expired with a wrong name, served from a local Python TLS server), the message is 'mbedTLS: The certificate validity has expired\nThe certificate Common Name (CN) does not match with the expected CN\n'. No other NetError message has a trailing newline, and no test prints a TLS error's Message. Run it as: fb build message2.fb -o message2.exe, then fb_http_test_server message2.exe.

```
// What a certificate error's NetError says, between brackets.
Import std.core
Import std.net.http

Local base = Input() ?? ""
Local secure = Input() ?? ""

Function Tried(client: HttpClient, url: String): String
    Try
        Return client.Get(url).Text
    Catch e As NetError
        Return "NetError {e.Kind} [{e.Message}]"
    Catch e As TimeoutError
        Return "TimeoutError"
    End Try
End Function

// The OS's roots don't trust the test server's certificate, and none trust nothing.
Local client = HttpClient()
Print(Tried(client, "{secure}/hello"))
client.TrustOnly([])
Print(Tried(client, "{secure}/hello"))
```

Expected: NetError TLS [mbedTLS: The certificate is not correctly signed by the trusted CA]
NetError TLS [mbedTLS: The certificate is not correctly signed by the trusted CA]

Actual: NetError TLS [mbedTLS: The certificate is not correctly signed by the trusted CA
]
NetError TLS [mbedTLS: The certificate is not correctly signed by the trusted CA
]
(the same in --release)

Suggested fix: In fb_http_message, trim trailing whitespace from the error buffer and join its inner lines with "; ". Add a test that prints a TLS error's Message.

Skeptic: message.fb under the test server, piped through od -c, shows 'NetError TLS [mbedTLS: The certificate is not correctly signed by the trusted CA\n]' for both the default client and TrustOnly([]), the same in --release. curl's vtls/mbedtls.c:465 calls mbedtls_x509_crt_verify_info(buf, 128, "", flags), which ends each flag's text with '\n', then failf("mbedTLS: %s"). fb_http_message (http.cpp ~line 1000) returns the error buffer untrimmed. The proxy run shows the same stray newline. No test prints a TLS error's Message.

## S29-tls-build-5 (low): The docs say macOS's anchors are filtered to those valid now and for servers, but the macOS code takes every anchor

Status: fixed in 73aa3171: tls_roots.hpp and stdlib.md say only Windows' roots are filtered, and macOS's system anchors taken whole, whose dates mbedTLS checks as it verifies; a filter on Apple's trust settings can't be built or run here, on Windows, so the docs are what changed

Where: runtime/src/tls_roots.cpp:96

tls_roots.hpp says: 'Windows' ROOT store and macOS's system anchors, each that's for servers and valid now'. stdlib.md's std.net section says the same: 'each that's valid now and for servers'. The __APPLE__ read_roots() copies every certificate SecTrustCopyAnchorCertificates gives. It doesn't check validity dates or usage, and it doesn't read trust settings, so an anchor Apple trusts only for something else, or one that has expired, is still trusted for TLS. I checked this from the source only: this machine is Windows, so the macOS branch can't run here, and no .fb program can list the roots. The program below is the one that would run on macOS. On Windows I did check that the filter the docs describe really happens: on this machine, the six roots restricted to code signing by their EKU property are left out, and so are the expired ones.

```
// On macOS: the default client's roots are every system anchor, filtered or not; nothing a
// program sees tells them apart, so this only shows that the OS's roots are used.
Import std.core
Import std.net.http

Local base = Input() ?? ""
Local secure = Input() ?? ""
Try
    Print(HttpClient().Get("{secure}/hello").Text)
Catch e As NetError
    Print("NetError {e.Kind}")
Catch e As TimeoutError
    Print("TimeoutError")
End Try
```

Expected: The macOS branch keeps only anchors that are valid now and trusted for TLS servers, as tls_roots.hpp and stdlib.md say; or else the docs say macOS's anchors are taken whole

Actual: From the source: the macOS read_roots() appends every anchor's DER and has no date, usage, or trust-settings check (tls_roots.cpp lines 96-120)

Suggested fix: Either filter on macOS: skip anchors whose notAfter or notBefore is outside now, and consult SecTrustSettingsCopyTrustSettings for kSecPolicyAppleSSL as Go once did. Or narrow both docs to say the filtering is Windows' only.

Skeptic: I could only check this from the source, since the macOS branch can't run on Windows. In tls_roots.cpp lines 96-119, the __APPLE__ read_roots() appends every SecTrustCopyAnchorCertificates entry. It checks no dates, no usage, and no trust settings. tls_roots.hpp ('each that's for servers and valid now') and stdlib.md's std.net section ('each that's valid now and for servers') both say the filter covers macOS too, so the code plainly contradicts both. The dates part has no practical effect: mbedTLS's x509_crt_verify_chain (x509_crt.c ~2541) checks validity time on every chain certificate, trusted roots included, before stopping. The usage part does: on macOS an anchor Apple restricts to other purposes would still be trusted for TLS. Either the code or the docs need changing.

## S29-tls-build-6 (low): third_party's mbedTLS comment still says only its crypto library is built, and TLS comes 'later'

Status: fixed in 8f086a86: the comment says mbedTLS's mbedtls and mbedx509 libraries are built too, for the TLS of std.net.http's client

Where: third_party/CMakeLists.txt:93

The mbedTLS block says it serves std.crypto 'and std.net.tls later. Only its crypto library (tfpsacrypto) is built.' Since this step, its mbedtls and mbedx509 libraries are built too, for libcurl: FB_CURL_ARCHIVES lists them, fb_http_test_server and fb_runtime_tests link them, and curl's MBEDTLS_LIBRARY and MBEDX509_LIBRARY name them. Every HTTPS program links them, and std.net.http's TLS already depends on them.

```
// Building this links libmbedtls.a and libmbedx509.a, libcurl's archives beside its own.
Import std.core
Import std.net.http

Local client = HttpClient()
Print(client.Decompress)
```

Expected: The comment names mbedTLS's TLS and X.509 libraries (mbedtls, mbedx509) as built for std.net.http's client, beside tfpsacrypto

Actual: The comment says 'Only its crypto library (tfpsacrypto) is built', while fb build lists mbedTLS and libcurl and links _deps/mbedtls-build/library/libmbedtls.a and libmbedx509.a

Suggested fix: Reword it, for example: 'std.crypto's ..., through TF-PSA-Crypto's PSA API, and the TLS of std.net.http's client (its mbedtls and mbedx509 libraries) and std.net.tls.' Drop the 'Only its crypto library' sentence.

Skeptic: third_party/CMakeLists.txt lines 92-95 still say mbedTLS serves 'std.net.tls later. Only its crypto library (tfpsacrypto) is built.' Since this change, the curl block names MBEDTLS_LIBRARY mbedtls and MBEDX509_LIBRARY mbedx509, and its own comment says curl is 'over mbedTLS's TLS (its mbedtls and mbedx509 libraries)'. Every HTTP build lists mbedTLS beside libcurl. The comment is stale, which is a minor comment fix.

## Refuted by the skeptics (no action)

- S29-async-server-7: No test covers an Async transfer still running when its coroutine goes or the program ends. I ran the program as given. Debug printed idle False / scheduler gone / hello / hello / end, exit 0, in 0.88 s with no leak report. f7d.fb, where a Global's Destroy starts GetAsync of /slow as the Globals go, printed end / late started, exit 0, in about 1 s with no leak report. f7c.fb, with a pending top-level Slow(), waited for it as spec 02 says ("once its coroutines and jobs are done"). The behavior is right, as the finding says itself. A missing test doesn't contradict the spec, stdlib.md or the milestones row, and the done-when doesn't name these paths, so I don't confirm it as a bug.
