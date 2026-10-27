# The review of M6 step 29's TLS half (std.net.tls over mbedTLS)

18 confirmed findings: 1 high, 4 medium, 13 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## runtime-1 (high): On Windows, the OS's roots miss every root Windows hasn't downloaded yet, so a client refuses servers Windows trusts, depending on the machine

Status: fixed in 5779a795 (tls_roots.cpp's os_verify, mbedTLS's verify callback for the OS's roots: on Windows, when a chain's one flaw is BADCERT_NOT_TRUSTED, it builds the chain with CertGetCertificateChain, the other side's certificates in a store in memory, and checks CertVerifyCertificateChainPolicy's CERT_CHAIN_POLICY_SSL, which fetches a root Windows trusts that the ROOT store hasn't got; any other flaw, like a name or a date, stands. tls.cpp's connections and libcurl's, through CURLOPT_SSL_CTX_FUNCTION ahead of libcurl's own check, which still says why a chain failed, both use it), with tls_roots_tests' "a chain none of the OS's roots signed is trusted as the OS trusts it": one of the OS's roots, which mbedTLS alone flags NOT_TRUSTED without roots, is trusted on Windows through os_verify, and isn't elsewhere, a name it isn't for stays flagged, and the test CA stays untrusted; stdlib.md says how Windows' roots are read and checked

Where: runtime/src/tls_roots.cpp:70

os_chain() (tls.cpp:54) trusts what fb::runtime::os_roots() reads, which on Windows is whatever is in the ROOT store right now (CertOpenSystemStoreW(0, L"ROOT")). Windows ships only part of its trusted list there. It adds the rest from Microsoft's list (authroot.stl) only when CryptoAPI or Schannel first builds a chain that needs one. On this machine the store held 59 certificates. TlsClient() refused www.actalis.it, www.d-trust.net, www.harica.gr, www.swisssign.com, www.certainly.com, and at first example.com (SSL.com TLS ECC Root CA 2022), all of which Windows trusts. After PowerShell's Invoke-WebRequest or an X509Chain.Build fetched each root, the same unchanged program connected. So whether a program reaches a public HTTPS server depends on what else has run on the machine before: on a fresh Windows install many servers fail. std.net.http shares the same roots and has the same problem.

```
Import std.core
Import std.ffi
Import std.io
Import std.net

// TLS to `host` on port 443, trusting the OS's roots: the first line of its answer.
Function Get(host: String)
    Try
        Local tcp = Try TcpStream.Connect(host, 443)
        Local tls = Try TlsClient().Connect(tcp, host)
        Try tls.Write(Buffer<UInt8>.From("HEAD / HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n\r\n".Bytes))
        Local reader = BufferedReader(tls)
        Print("{host}: {tls.Version}, {Try reader.ReadLine() ?? "-"}")
    Catch e As Error
        Print("{host}: {e.Message}")
    End Try
End Function

Get("www.actalis.it")
Get("www.d-trust.net")
Get("www.harica.gr")
// Then run in PowerShell: Invoke-WebRequest -UseBasicParsing -Method Head https://www.actalis.it (and the other two), which succeeds and makes Windows fetch the roots, and run this program again.
```

Expected: www.actalis.it: TLS 1.2, HTTP/1.1 301 Moved Permanently
www.d-trust.net: TLS 1.3, HTTP/1.1 301 Moved Permanently
www.harica.gr: TLS 1.3, HTTP/1.1 200 OK
(the same on every run, as Schannel's 200 for each shows Windows trusts them)

Actual: First run:
www.actalis.it: the server's certificate isn't signed by a root this side trusts
www.d-trust.net: the server's certificate isn't signed by a root this side trusts
www.harica.gr: the server's certificate isn't signed by a root this side trusts
After Invoke-WebRequest returned 200 for each, the same program prints the expected lines.

Suggested fix: On Windows, when the OS's roots are in use, don't treat the ROOT store's snapshot as the whole trust list. One option: set mbedtls_ssl_set_verify (or the config's f_vrfy) to a callback that, when mbedTLS's only flag is BADCERT_NOT_TRUSTED, builds the peer's chain with CertGetCertificateChain (with the peer's intermediates in an in-memory store) and CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL). Either one triggers Windows' root auto-update and decides trust as Windows does. Another option: reread the store after a chain build. Add a test of the fallback, and document in stdlib.md how the OS's roots are read on Windows.

Skeptic: Ran r1.fb (TlsClient().Connect over TcpStream to port 443, OS roots, debug build). The machine's LocalMachine ROOT store has 63 certificates (Get-ChildItem Cert:\LocalMachine\Root). Hosts whose roots are already there connected (www.buypass.no, www.google.com, aws.amazon.com, www.certigna.com, www.globalsign.com, www.trustcor.com). Four servers whose roots are in Microsoft's root program but not yet downloaded here were refused with "the server's certificate isn't signed by a root this side trusts": www.telia.com, valid-isrgrootx2.letsencrypt.org, www.e-szigno.hu, and www.twca.com.tw. I did not run Invoke-WebRequest, so as not to change the store. Code: tls_roots.cpp read_roots() only enumerates CertOpenSystemStoreW(0, L"ROOT"), and tls.cpp os_chain() reads that once and trusts nothing else, so roots Windows fetches on demand are never consulted. stdlib.md does say Windows' ROOT store. But decisions.md ("TLS trusts the OS's certificate store ... as in a browser") and the milestone row (the OS's roots, the client the package manager needs) mean trusting what the OS trusts. Whether a public server is reachable then depends on what the machine has run before, and a fresh install is the worst case.

## runtime-2 (medium): A certificate refused for its key usage or extended key usage gets mbedTLS's raw text, and reasons()' usage branch never runs

Status: fixed in a86db803 (describe() reads MBEDTLS_ERR_SSL_BAD_CERTIFICATE's flags as it does CERT_VERIFY_FAILED's), with tls_tests' "a client whose certificate isn't for a TLS client is refused for its usage", over the fb_tls_ functions, since Identify now panics for such a certificate (runtime-4): the server says "the client's certificate isn't for a TLS client"

Where: runtime/src/tls.cpp:251

mbedTLS's mbedtls_ssl_verify_certificate (ssl_tls.c:8842) checks keyUsage and extendedKeyUsage after the chain check. When they don't fit, it sets BADCERT_KEY_USAGE or BADCERT_EXT_KEY_USAGE in verify_result and returns MBEDTLS_ERR_SSL_BAD_CERTIFICATE, not MBEDTLS_ERR_X509_CERT_VERIFY_FAILED. describe() runs reasons() only for X509_CERT_VERIFY_FAILED, so BAD_CERTIFICATE goes to the mbedtls_strerror fallback. reasons()' "isn't for a TLS client/server" branch therefore never runs. A server that requires client certificates and gets one made for servers (EKU serverAuth only, a common mistake) throws a NetError whose message is mbedTLS's internal text. No test covers a certificate refused for its usage.

```
// Beside a copy of tests/programs/net_tls/certificates.fb.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./certificates"

// Two ends' bytes in memory, and whether each end's reading has ended.
Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method
End Class

Function Certificates(pem: String): Certificate[] => (Try? Certificate.AllFromPem(pem)).Expect("reads")
Function KeyOf(pem: String): Key => (Try? Key.FromPem(pem)).Expect("reads")

Async Function Accept(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "server: {tls.Version}"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Connect(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "client: {tls.Version}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

// A handshake through a pipe in memory: what the server says, then the client.
Function Run(client: TlsClient, server: TlsServer)
    Local pipe = Pipe()
    Local accepted = Accept(server, PipeEnd(pipe, 1))
    Local connected = Connect(client, PipeEnd(pipe, 0))
    Local said = connected.Wait()
    pipe.Ended[1] = True
    Print(accepted.Wait())
    Print(said)
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

// A client whose certificate is a server's (extendedKeyUsage=serverAuth), to a server that asks.
Local server = TlsServer(Certificates(localhost_pem), KeyOf(localhost_key))
server.RequireClients(Certificates(ca_pem))
Local client = Client()
client.Identify(Certificates(localhost_pem), KeyOf(localhost_key))
Run(client, server)
```

Expected: server: the client's certificate isn't for a TLS client
client: TLS 1.3

Actual: server: mbedTLS failed: SSL - Processing of the Certificate handshake message failed
client: TLS 1.3
(the same with --release)

Suggested fix: In describe(), handle MBEDTLS_ERR_SSL_BAD_CERTIFICATE the way X509_CERT_VERIFY_FAILED is handled: `if (result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || result == MBEDTLS_ERR_SSL_BAD_CERTIFICATE)`, then build the message from reasons(flags). Add this case to net_tls.fb (a client identified with localhost_pem to a server that requires clients).

Skeptic: Ran r2.fb, a client identified with localhost_pem (EKU serverAuth) to a server with RequireClients, in debug and release. Both print "server: mbedTLS failed: SSL - Processing of the Certificate handshake message failed" / "client: TLS 1.3". mbedTLS's ssl_tls.c:8835/8849 returns MBEDTLS_ERR_SSL_BAD_CERTIFICATE after setting BADCERT_EXT_KEY_USAGE (8703). tls.cpp describe() only calls reasons() for MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, so reasons()' 'isn't for a TLS client/server' branch can never run, and the code falls back to mbedtls_strerror. That breaks stdlib.md's "a NetError of TLS that says why".

## std-1 (medium): TlsListener.Accept(timeout) doesn't bound the handshake, so a client that connects and stays silent holds Accept, and every later client, forever

Status: fixed in a86db803 (TlsListener's Accept and AcceptAsync set the TcpStream's ReadTimeout to what's left of the timeout for the handshake, and Null after it), with net_tls_tcp's silent clients: Accept(300 ms) and AcceptAsync(300 ms) throw a TimeoutError within a second, and the listener answers the next client

Where: std/net/tls.fb:566

Accept passes `timeout` only to TcpListener.Accept, then runs Server.Accept(tcp) on a TcpStream that has no ReadTimeout. The handshake's Receive blocks until the client sends something or closes. A client that only opens a TCP connection, whether a slow client, a port scanner or a slowloris, blocks Accept(500 ms) indefinitely. A blocking TLS server loop serves nobody else in that time. The caller can't set a ReadTimeout on the accepted stream before the handshake, because TlsListener hides it. The doc comment says the timeout covers the TCP wait only. Even so, the parameter gives no bound on the call, which the HTTP server planned over std's TCP and TLS will need.

```
// Put next to tests/programs/net_tls/ (it imports its certificates).
Import std.core
Import std.crypto
Import std.net
Import std.time
Import "./net_tls/certificates"

Const ports = Channel<UInt16>(1)
Local accepting = Thread.Start(Function()
    Try
        Local chain = (Try? Certificate.AllFromPem(localhost_pem)).Expect("reads")
        Local key = (Try? Key.FromPem(localhost_key)).Expect("reads")
        Local listener = TlsListener.Listen("127.0.0.1", 0, TlsServer(chain, key))
        ports.Send(listener.LocalAddress.Port)
        Local tls = listener.Accept(500.Milliseconds)
        Print("accepted {tls.Version}")
    Catch e As NetError
        Print("Accept(500 ms) gave a NetError {e.Kind}")
    Catch e As TimeoutError
        Print("Accept(500 ms) gave a TimeoutError")
    End Try
End Function)

Try
    Local quiet = TcpStream.Connect("127.0.0.1", ports.Receive() ?? 0)
    Sleep(3.Seconds)
    Print("3 s later, the silent client closes")
    quiet.Close()
Catch e As Error
    Print("failed: {e.Message}")
End Try

accepting.Join()
```

Expected: Accept(500 ms) gave a TimeoutError
3 s later, the silent client closes
(or, at least, Accept returns within roughly its timeout)

Actual: 3 s later, the silent client closes
Accept(500 ms) gave a NetError CLOSED
(Accept(500 ms) blocked for the full 3 s, until the client closed)

Suggested fix: Bound the handshake too. Before Server.Accept, set tcp.ReadTimeout to what is left of `timeout` (or to a handshake timeout the listener holds, such as a HandshakeTimeout property), and set it back to Null after the handshake. Do the same in AcceptAsync. Then document that a client too slow to finish its handshake throws a TimeoutError here.

Skeptic: Ran the program as given, with net_tls/certificates.fb copied next to it under rv-tls/v1/s1.fb, in debug. It printed "3 s later, the silent client closes" and then "Accept(500 ms) gave a NetError CLOSED": Accept(500 ms) returned only when the silent client closed, so a single idle TCP connection holds Accept for as long as it stays open. In tls.fb:566-570, `timeout` goes only to Listener.Accept. Server.Accept(tcp) then runs the handshake on a TcpStream with no ReadTimeout, and TlsListener gives the caller no way to set one before the handshake. The doc comment does say the timeout is for the TCP wait, but nothing at all bounds the handshake, so any blocking TlsListener server can be stalled indefinitely. The workaround is TcpListener plus TlsServer.Accept with a ReadTimeout set.

## std-3 (medium): Write after Close succeeds silently, and the bytes are lost after close_notify

Status: fixed in a86db803 (TlsStream's Closed: Close and CloseAsync send close_notify the first time only, and Write and WriteAsync throw an IOError whose Cause is a NetError of CLOSED after it), with net_tls's "past the handshake": a second Close sends 0 bytes, and a write after it throws "the TlsStream is closed" with a NetError of CLOSED

Where: std/net/tls.fb:524

Close sends close_notify but leaves no mark on the TlsStream, and mbedTLS's ssl_write keeps sealing records after close_notify. So a later Write or WriteAsync encrypts and sends application data that the peer discards: its read has already ended at close_notify, and RFC 8446 forbids data after it. The caller gets no error. TcpStream throws an IOError whose Cause is a NetError of CLOSED for a Write after Close, and TlsStream's doc says close_notify 'tells the other side nothing more comes'. A second Close also sends a second close_notify.

```
// Put next to tests/programs/net_tls/ (it imports its certificates).
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test certificates read")
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Local all = Try Await ReadAllAsync(tls)
        Return "server read {all.Count} bytes"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Try Await tls.WriteAsync(Buffer<UInt8>.From("hello".Bytes))
        Try Await tls.CloseAsync()
        Try Await tls.WriteAsync(Buffer<UInt8>.From(" after Close".Bytes))
        Try Await tls.FlushAsync()
        Return "client wrote after Close without an error"
    Catch e As NetError
        Return "client: NetError {e.Kind}: {e.Message}"
    Catch e As TimeoutError
        Return "client: TimeoutError"
    Catch e As IOError
        Return "client: IOError {e.Kind}: {e.Message}"
    End Try
End Function

Local client = TlsClient()
client.TrustOnly(Certificates(ca_pem))
Local server = TlsServer(Certificates(localhost_pem), (Try? Key.FromPem(localhost_key)).Expect("reads"))
Local near = PipeEnd(Pipe(), 0)
Local served = Serve(server, near.Other())
Print(Talk(client, near).Wait())
Print(served.Wait())
near.Hang()
```

Expected: client: IOError OTHER: ... (an IOError whose Cause is a NetError of CLOSED, as TcpStream gives for a Write after Close)
server read 5 bytes

Actual: client wrote after Close without an error
server read 5 bytes

Suggested fix: Keep a Closed flag on TlsStream. Close and CloseAsync set it and do nothing the second time. Write, WriteAsync and Seal throw IOError(IOErrorKind.OTHER, "the TLS stream is closed", Null, NetError(NetErrorKind.CLOSED, ...)) once it is set. Document this on Close, and add a case to net_tls.fb.

Skeptic: Ran an extended version of the program (rv-tls/v1/s3.fb) in debug. It printed "client wrote after Close without an error; 74 more bytes went out" and "server read 5 bytes": a WriteAsync and a second Close after CloseAsync both succeed silently, and the data sent after close_notify is lost. For comparison, a TcpStream Write after Close in the same program gave "IOError OTHER: the TcpStream is closed; cause CLOSED". In tls.fb:524-532, Close records no state, and Write/Seal (489-511) never check for it.

## std-4 (medium): A host over 255 bytes isn't checked, and its handshake fails with a garbled 'HMAC_DRBG - Read/write error in file' message

Status: fixed in a86db803 (CheckHost panics for a host of more than 255 bytes, and describe() gives PSA's statuses their own text, runtime-3), with panic_tls_host_long (255 bytes fit, 256 panic at the Connect line) and tls_tests' "mbedTLS's PSA statuses say what they are" for a 300-byte host through the fb_tls_ functions

Where: std/net/tls.fb:251

CheckHost panics only for an empty host or one with a NUL. mbedtls_ssl_set_hostname refuses a name longer than 255 bytes (MBEDTLS_SSL_MAX_HOST_NAME_LEN) with MBEDTLS_ERR_SSL_BAD_INPUT_DATA. In mbedTLS 4 that code is PSA_ERROR_INVALID_ARGUMENT (-0x87). describe() falls through to mbedtls_strerror, which splits the code into a high part, 0x80, and a low part, 0x07, and prints 'UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file'. So Connect throws a NetError of TLS whose message is false and doesn't say the host is the problem. The same fallback garbles every other PSA-coded error that reaches describe's default branch.

```
Import std.core
Import std.io
Import std.net

Local host = ""
For i = 1 To 300
    host = host + "a"
End For

Try
    TlsClient().Connect(MemoryStream(), host)
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
End Try
```

Expected: panic: a host's name is at most 255 bytes ... at the Connect line (as for an empty host or a NUL), or at least a NetError that says the host's name is too long

Actual: NetError TLS: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file

Suggested fix: In CheckHost, panic for a host over 255 bytes, as for an empty host or a NUL, and say so in Connect's doc comment. Separately, the runtime's describe() should map PSA_ERROR_* codes (psa_status_t, or mbedTLS 4's PSA-aliased SSL codes such as BAD_INPUT_DATA) to its own text rather than pass them to mbedtls_strerror. Add a panic_tls_host_long test.

Skeptic: Ran the program (rv-tls/v1/s4.fb) in debug. With a 300-byte host, Connect threw "NetError TLS: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file". CheckHost (tls.fb:251-254) checks only for an empty host and a NUL. runtime/src/tls.cpp:379 passes the host to mbedtls_ssl_set_hostname, and describe() at line 289 falls back to mbedtls_strerror, which gives the garbled text. The message is false and doesn't say the host is the problem.

## runtime-3 (low): describe()'s mbedtls_strerror fallback turns mbedTLS 4's PSA error codes into nonsense, such as a "BIGNUM" error for a ClientHello that's too big

Status: fixed in a86db803 (describe() names BUFFER_TOO_SMALL and ALLOC_FAILED, and gives any other code from -128 to -255 as PSA's status, not mbedtls_strerror's misreading; CheckProtocols panics past 8192 bytes, std-5), with tls_tests' "mbedTLS's PSA statuses say what they are": 70 protocols of 250 bytes given straight to the runtime say "a handshake message this side made is bigger than mbedTLS's buffer", and panic_tls_protocols_long

Where: runtime/src/tls.cpp:288

In mbedTLS 4, many SSL errors are PSA statuses: MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL is PSA_ERROR_BUFFER_TOO_SMALL (-138), BAD_INPUT_DATA is -135, and ALLOC_FAILED is -141. mbedtls_strerror splits a code into a high part (& 0xFF80) and a low part (& 0x7F), which can't decode these. -138 comes out as "UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output". One way to hit it: an Offer list that passes CheckProtocols (every name 1 to 255 bytes) but makes a ClientHello bigger than mbedTLS's 16 KB output buffer. Every handshake then fails with that text, and Offer gives no panic for a list that can never be sent.

```
// Beside a copy of tests/programs/net_tls/certificates.fb.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method
End Class

Function Certificates(pem: String): Certificate[] => (Try? Certificate.AllFromPem(pem)).Expect("reads")
Function KeyOf(pem: String): Key => (Try? Key.FromPem(pem)).Expect("reads")

Async Function Accept(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "server: {tls.Version}"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Connect(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "client: {tls.Version}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Function Run(client: TlsClient, server: TlsServer)
    Local pipe = Pipe()
    Local accepted = Accept(server, PipeEnd(pipe, 1))
    Local connected = Connect(client, PipeEnd(pipe, 0))
    Local said = connected.Wait()
    pipe.Ended[1] = True
    Print(accepted.Wait())
    Print(said)
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

// 70 protocols of 250 bytes each, which Offer takes, but no ClientHello holds.
Local names: String[] = []
For i = 0 Until 70
    Local name = "p{i}"
    While name.Bytes.Count < 250
        name += "x"
    End While

    names.Add(name)
End For

Local client = Client()
client.Offer(names)
Run(client, TlsServer(Certificates(localhost_pem), KeyOf(localhost_key)))
```

Expected: Offer panics for a list too long for a ClientHello. Failing that, a message that names the problem, for example "client: the protocols offered don't fit in a TLS handshake" or at least text that isn't about BIGNUM.

Actual: server: the stream ended before the TLS handshake did
client: mbedTLS failed: UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output
(the same with --release)

Suggested fix: In describe(), check for the PSA-range codes before calling mbedtls_strerror: MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL, BAD_INPUT_DATA, ALLOC_FAILED, and any code from -128 to -255. Give each its own text, or a plain "mbedTLS failed with PSA status N". Also have CheckProtocols panic when the protocols' total (each name's length plus 1) won't fit a ClientHello (well under 16384 bytes, for example 8192 in all), so a list that can never be sent fails at Offer. Test both.

Skeptic: Ran r3.fb, which offers 70 ALPN names of 250 bytes, in debug and release. Both print "client: mbedTLS failed: UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output" (server: "the stream ended before the TLS handshake did"). describe()'s default branch passes mbedTLS 4's PSA-status codes, here BUFFER_TOO_SMALL -138, to mbedtls_strerror, which misdecodes them. The message doesn't say why, against stdlib.md. Offer accepts a list that can never be sent. Low, because the trigger here is contrived, but the fallback is wrong for any PSA-range code.

## runtime-4 (low): A server whose certificate isn't for TLS servers says the two sides have "no version of TLS or cipher suite in common"

Status: fixed in a86db803 (fb_tls_config_usable checks the first certificate's key usage for digitalSignature and its extended key usage for serverAuth or clientAuth, and CheckIdentity panics when it fails, for TlsServer and for Identify), with panic_tls_server_usage (the test client's certificate as a server's) and panic_tls_client_usage (the test server's as a client's), and stdlib.md's identity panics

Where: runtime/src/tls.cpp:277

TlsServer(chain, key) accepts a chain whose first certificate has extendedKeyUsage=clientAuth (or lacks digitalSignature). mbedTLS's server skips such a certificate when it picks one (ssl_tls13_server.c:1143, "This avoids sending the client a cert it'll reject"). It then has no certificate to use and fails with MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE, which describe() reports as a version or cipher-suite mismatch. The real cause, a misconfigured server certificate, is never named, and the client only sees "the server refused the handshake". TlsServer's constructor already panics for keys and certificates TLS can't use, but doesn't check this case.

```
// Beside a copy of tests/programs/net_tls/certificates.fb.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method
End Class

Function Certificates(pem: String): Certificate[] => (Try? Certificate.AllFromPem(pem)).Expect("reads")
Function KeyOf(pem: String): Key => (Try? Key.FromPem(pem)).Expect("reads")

Async Function Accept(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "server: {tls.Version}"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Connect(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "client: {tls.Version}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Function Run(client: TlsClient, server: TlsServer)
    Local pipe = Pipe()
    Local accepted = Accept(server, PipeEnd(pipe, 1))
    Local connected = Connect(client, PipeEnd(pipe, 0))
    Local said = connected.Wait()
    pipe.Ended[1] = True
    Print(accepted.Wait())
    Print(said)
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

// A server whose certificate is a client's (extendedKeyUsage=clientAuth).
Run(Client(), TlsServer(Certificates(client_pem), KeyOf(client_key)))
```

Expected: TlsServer(...) panics with something like "the chain's first certificate, CN=FutureBlitz Test Client, isn't for a TLS server". Failing that, the server's NetError says its own certificate isn't for a TLS server.

Actual: server: the client and the server have no version of TLS or cipher suite in common
client: the server refused the handshake

Suggested fix: In CheckIdentity, through a new runtime check, test the first certificate with mbedtls_x509_crt_check_key_usage(MBEDTLS_X509_KU_DIGITAL_SIGNATURE) and mbedtls_x509_crt_check_extended_key_usage(MBEDTLS_OID_SERVER_AUTH for TlsServer, CLIENT_AUTH for TlsClient.Identify). Panic when either fails, as for other unusable identities, and document the panic. Add a panic_tls_ test for it.

Skeptic: Ran r4.fb, TlsServer(client_pem, client_key) where client_pem has EKU clientAuth, in debug and release. Prints "server: the client and the server have no version of TLS or cipher suite in common" / "client: the server refused the handshake". The constructor accepts the unusable identity, and the server's NetError names a version/cipher mismatch instead of its own certificate's usage (MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE maps to that text in describe()). The message is misleading for a misconfiguration TlsServer could catch.

## runtime-5 (low): A client to an IP address sends that address as SNI, which RFC 6066 forbids

Status: fixed in a86db803 (std.net.tls passes an IP host's bytes to fb_tls_new, which sets no hostname, so no server_name goes, and its verify callback checks the leaf's subjectAltName iPAddress entries, flagging CN_MISMATCH for none), with tls_tests' "a client of an IP address sends no server_name, and checks the address": the ClientHello holds "localhost" for a name and neither 127.0.0.1 nor ::1 for those, both of which connect, and 127.0.0.2 fails as "the server's certificate isn't for 127.0.0.2"; net_tls connects to ::1 too

Where: runtime/src/tls.cpp:379

fb_tls_new calls mbedtls_ssl_set_hostname(host) for every client host, IP literals included. mbedTLS 4.1's ssl_write_hostname_ext writes ssl->hostname into server_name without checking for an IP. So Connect(stream, "127.0.0.1") or "::1" puts the literal in the ClientHello's server_name, against RFC 6066 section 3 ("Literal IPv4 and IPv6 addresses are not permitted in HostName"). OpenSSL's, Go's, and Schannel's clients leave SNI out for IPs. A strict server can refuse the handshake. The test of an IP host only checks that the handshake succeeds against mbedTLS's own server, which ignores SNI.

```
// Beside a copy of tests/programs/net_tls/certificates.fb.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method
End Class

Function Certificates(pem: String): Certificate[] => (Try? Certificate.AllFromPem(pem)).Expect("reads")

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

// Whether `bytes` hold `text`'s bytes.
Function Holds(bytes: UInt8[], text: String): Bool
    Local wanted = text.Bytes
    For i = 0 Until bytes.Count - wanted.Count + 1
        Local all = True
        For j = 0 Until wanted.Count
            If bytes[i + j] != wanted[j] Then all = False
        End For

        If all Then Return True
    End For

    Return False
End Function

Async Function Connect2(client: TlsClient, pipe: PipeEnd, host: String): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, host)
        Return tls.Version
    Catch e As Error
        Return e.Message
    End Try
End Function

// The ClientHello a client to `host` sends.
Async Function Hello(host: String): UInt8[]
    Local pipe = Pipe()
    Local connected = Connect2(Client(), PipeEnd(pipe, 0), host)
    For i = 0 Until 5
        Await NextFrame()
    End For

    pipe.Ended[0] = True
    Await connected
    Return pipe.Waiting[1]
End Function

// RFC 6066 section 3: "Literal IPv4 and IPv6 addresses are not permitted in HostName."
For host In ["localhost", "127.0.0.1", "::1"]
    Local hello = Hello(host).Wait()
    Print("{host}: a {hello.Count}-byte ClientHello, whose server_name is the host: {Holds(hello, host)}")
End For
```

Expected: localhost: a 312-byte ClientHello, whose server_name is the host: True
127.0.0.1: a 300-byte ClientHello, whose server_name is the host: False
::1: a 300-byte ClientHello, whose server_name is the host: False
(no server_name extension for an IP: 300 bytes, 12 fewer than a 9-byte name's)

Actual: localhost: a 312-byte ClientHello, whose server_name is the host: True
127.0.0.1: a 312-byte ClientHello, whose server_name is the host: True
::1: a 306-byte ClientHello, whose server_name is the host: True

Suggested fix: When the host parses as an IPv4 or IPv6 address, keep the address for verification and leave SNI out. mbedTLS sends SNI from ssl->hostname, so one option is a verify callback (mbedtls_ssl_set_verify) that checks the peer's IP SAN itself (mbedtls_x509_crt_verify's cn check, or a comparison against subject_alt_names' iPAddress entries) with a hostname that isn't set. Another option is to check for an IP-skipping option in mbedTLS 4.1. Test that a client to 127.0.0.1 sends no server_name.

Skeptic: Ran r5.fb (captures the ClientHello), in debug and release. localhost: 312 bytes, holds the host. 127.0.0.1: 312 bytes, holds the literal. ::1: 306 bytes, holds the literal. The 9-byte IP literal gives the same size as the 9-byte name, which shows it is sent as server_name. tls.cpp fb_tls_new calls mbedtls_ssl_set_hostname for every client host, and mbedTLS 4.1.1's ssl_write_hostname_ext (ssl_client.c:25) writes ssl->hostname with no IP check. Neither mbedtls_ssl_set_hostname nor the extension writer skips IPs. That goes against RFC 6066 section 3. Low: most servers ignore it.

## runtime-6 (low): runtime.hpp still says fb_io_net_error_kind gives 7 for a timeout, which is now OTHER's ordinal

Status: fixed in a86db803 (runtime.hpp says 8 for a timeout, past NetErrorKind's ordinals, as net.cpp's meaning_t and socket.fb's TIMED_OUT have it); a comment, which the net programs' timeouts, TIMED_OUT's, test

Where: runtime/include/fb/runtime.hpp:778

Commit 97756322 inserted MEANS_HTTP_STATUS into net.cpp's meaning_t and moved socket.fb's TIMED_OUT to 8. The declaration's comment still says "a NetErrorKind's ordinal, or 7 for a timeout". 7 is now NetErrorKind.OTHER, so anyone following the header would read every OTHER as a timeout.

```
Import std.core

// runtime.hpp says fb_io_net_error_kind gives "a NetErrorKind's ordinal, or 7 for a timeout".
Print("OTHER is {NetErrorKind.OTHER As Int32}, HTTP_STATUS is {NetErrorKind.HTTP_STATUS As Int32}")
```

Expected: OTHER's ordinal differs from the timeout value the header documents (the header should say 8).

Actual: OTHER is 7, HTTP_STATUS is 6 (while runtime.hpp:778 says 7 means a timeout; net.cpp gives 8)

Suggested fix: Change runtime.hpp:778 to "or 8 for a timeout, past NetErrorKind's ordinals", or better, name no number and point to meaning_t in net.cpp and TIMED_OUT in socket.fb.

Skeptic: Read the code. runtime/include/fb/runtime.hpp:778 still says fb_io_net_error_kind gives "a NetErrorKind's ordinal, or 7 for a timeout". After 97756322, net.cpp's meaning_t has MEANS_HTTP_STATUS (6), MEANS_OTHER (7) and MEANS_TIMED_OUT (8), and socket.fb has Const TIMED_OUT = 8. r6.fb prints "OTHER is 7, HTTP_STATUS is 6". The header comment is stale and now names OTHER's ordinal as the timeout.

## std-2 (low): A failed handshake writes its alert but never flushes it, so over a buffered stream the other side sees CLOSED, not TLS

Status: fixed in a86db803 (a failed handshake's step flushes after the alert it sends, ignoring a failure as the send does, in Handshake and HandshakeAsync), with net_tls's "through a stream that holds what's written until Flush": the server says the client refused the handshake, not CLOSED

Where: std/net/tls.fb:363

In Handshake and HandshakeAsync, the HANDSHAKE_FAILED branch runs `Try? Send()` (`Try? Await SendAsync()`) and throws. It never calls Inner.Flush. Every other path that sends (the handshake's steps, Read's answers, Close) flushes after Send, and the comment says the alert 'goes to the other side, whose handshake fails at it'. Over a stream whose Write holds bytes until Flush, the 7-byte alert stays in the buffer. The peer then waits until the stream ends and reports NetError CLOSED instead of the TLS refusal.

```
// Put next to tests/programs/net_tls/ (it imports its certificates).
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

// A pipe in memory, as tests/programs/net_tls.fb has; what an end writes reaches the other end at
// once, or, when the end is Held, only at Flush.
Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int
    Field Held: Bool
    Field Pending: UInt8[]

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
        Pending = []
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        If Held
            Pending.AddAll(from.ToArray())
        Else
            Joined.Waiting[1 - Side].AddAll(from.ToArray())
        End If
    End Method

    Method Flush() Throws IOError
        Joined.Waiting[1 - Side].AddAll(Pending)
        Pending = []
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test certificates read")
End Function

Function Server(pem: String, key: String): TlsServer
    Return TlsServer(Certificates(pem), (Try? Key.FromPem(key)).Expect("the test keys read"))
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "server ok"
    Catch e As TimeoutError
        Return "TimeoutError"
    Catch e As NetError
        Return "server: NetError {e.Kind}: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "client ok"
    Catch e As TimeoutError
        Return "TimeoutError"
    Catch e As NetError
        Return "client: NetError {e.Kind}: {e.Message}"
    End Try
End Function

// The client refuses a server whose certificate a root it doesn't trust signed; its end of the
// pipe holds what it writes until Flush, as a buffered stream does.
For held In [False, True]
    Local near = PipeEnd(Pipe(), 0)
    near.Held = held
    Local served = Serve(Server(rogue_pem, rogue_key), near.Other())
    Local said = Talk(Client(), near).Wait()
    Print("held {held}: {near.Pending.Count} bytes left unflushed")
    near.Hang()
    Print("  {said}")
    Print("  {served.Wait()}")
End For
```

Expected: held False: 0 bytes left unflushed
  client: NetError TLS: the server's certificate isn't signed by a root this side trusts
  server: NetError TLS: the client refused the handshake, or sent a record that doesn't authenticate
held True: 0 bytes left unflushed
  client: NetError TLS: the server's certificate isn't signed by a root this side trusts
  server: NetError TLS: the client refused the handshake, or sent a record that doesn't authenticate

Actual: held False: 0 bytes left unflushed
  client: NetError TLS: the server's certificate isn't signed by a root this side trusts
  server: NetError TLS: the client refused the handshake, or sent a record that doesn't authenticate
held True: 7 bytes left unflushed
  client: NetError TLS: the server's certificate isn't signed by a root this side trusts
  server: NetError CLOSED: the stream ended before the TLS handshake did

Suggested fix: In both failure branches, flush after sending the alert and ignore its failure, as the send does. For example: `If (Try? Send()) == True Then Try? Inner.Flush()`, and the Async twin with SendAsync and FlushAsync. Add a held-until-Flush stream case to net_tls.fb.

Skeptic: Ran the program (rv-tls/v1/s2.fb) in debug. With held False, the server gets "NetError TLS: the client refused the handshake...". With held True it printed "held True: 7 bytes left unflushed", and the server got "NetError CLOSED: the stream ended before the TLS handshake did". In tls.fb:360-365 and 384-387, the HANDSHAKE_FAILED branch runs `Try? Send()` / `Try? Await SendAsync()` with no Inner.Flush. Every other sending path flushes (lines 368, 390, 473, 526). This contradicts the comment there that the alert reaches the other side. It only matters over a stream that buffers until Flush, and the only effect is the peer's error message, so I rate it low.

## std-5 (low): Offer doesn't check ALPN's 65535-byte list limit; a longer list fails every handshake with the same garbled message

Status: fixed in a86db803 (CheckProtocols panics when the names, each with a byte for its length, come to more than 8192 bytes, what a ClientHello has room for, under ALPN's 65535), with panic_tls_protocols_long: 32 names of 255 bytes, 8192 in all, are offered, and one more panics at the Offer line; Offer's comments and stdlib.md say so

Where: std/net/tls.fb:154

CheckProtocols checks each name for 1 to 255 bytes and no NUL, but not the total. mbedtls_ssl_conf_alpn_protocols refuses a list longer than MBEDTLS_SSL_MAX_ALPN_LIST_LEN (65535 bytes) with BAD_INPUT_DATA. set_up then fails, Offer itself returns normally, and every later Connect throws 'NetError TLS: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file'. Offer's doc promises a panic for names ALPN can't carry.

```
Import std.core
Import std.io
Import std.net

Local name = ""
For i = 1 To 255
    name = name + "a"
End For

Local names: String[] = []
For i = 1 To 300
    names.Add(name)
End For

Local client = TlsClient()
client.Offer(names)
Print("offered {names.Count} names")
Try
    client.Connect(MemoryStream(), "localhost")
Catch e As NetError
    Print("NetError {e.Kind}: {e.Message}")
End Try
```

Expected: panic at the Offer line: the application protocols' names come to at most 65535 bytes (each name's bytes plus 1)

Actual: offered 300 names
NetError TLS: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file

Suggested fix: In CheckProtocols, add up each name's byte count plus 1 and panic past 65535, and say so in both Offer doc comments.

Skeptic: Ran the program (rv-tls/v1/s5.fb) in debug. It printed "offered 300 names" and then "NetError TLS: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file". CheckProtocols (tls.fb:154-163) checks each name's length but not the list's total. The refusal happens later, at runtime/src/tls.cpp:145 (mbedtls_ssl_conf_alpn_protocols), and surfaces at Connect as a garbled NetError. The Offer docs promise a panic for names ALPN can't carry.

## std-6 (low): TlsListener.Listen isn't @PanicsAtCaller, so a backlog below 1 panics at std/net/tls.fb:549, not at the caller's line

Status: fixed in a86db803 (both TlsListener.Listen overloads are @PanicsAtCaller, and say the backlog's panic), with panic_tls_listener_backlog: a backlog of 1 listens, and 0 panics at the program's line

Where: std/net/tls.fb:547

Both TlsListener.Listen overloads forward `backlog` to TcpListener.Listen, which panics for a backlog below 1 through CheckBacklog (@PanicsAtCaller). TlsListener.Listen isn't @PanicsAtCaller itself, so the panic's place is the line inside std/net/tls.fb. std's convention is that a bug panics at the caller's line, and TcpListener.Listen does that for the same input.

```
// Put next to tests/programs/net_tls/ (it imports its certificates).
Import std.core
Import std.crypto
Import std.net
Import "./net_tls/certificates"

Local chain = (Try? Certificate.AllFromPem(localhost_pem)).Expect("reads")
Local key = (Try? Key.FromPem(localhost_key)).Expect("reads")
Local listener = Try TlsListener.Listen(SocketAddress(IpAddress.Loopback, 0), TlsServer(chain, key), 0)
Print("unreachable {listener.LocalAddress.Port}")
```

Expected: panic: a listener keeps a backlog of 1 connection at least, not 0 at <program>.fb:9:22

Actual: panic: a listener keeps a backlog of 1 connection at least, not 0 at std/net/tls.fb:549:32

Suggested fix: Mark both TlsListener.Listen overloads @PanicsAtCaller, mention the backlog panic in their doc comments as TcpListener's do, and add a panic_tls_listener_backlog test.

Skeptic: Ran it (rv-tls/v1/s6.fb) in debug and release. Both printed "panic: a listener keeps a backlog of 1 connection at least, not 0 at std/net/tls.fb:549:32". The same misuse through TcpListener.Listen (s6b.fb) panics at s6b.fb:5:22. Neither TlsListener.Listen overload (tls.fb:547, 553) is @PanicsAtCaller. docs/stdlib.md:264 says misuse panics at the caller's line (@PanicsAtCaller).

## tests-1 (low): A failed handshake's alert is written but never flushed, so over a buffered stream the other side never learns why

Status: fixed in a86db803, as std-2, the same bug

Where: std/net/tls.fb:363

When a handshake fails, Handshake and HandshakeAsync call `Try? Send()` / `Try? Await SendAsync()` (lines 363 and 385) to send the alert that says why, but they never call Inner.Flush/FlushAsync. Every other Send in the handshake is followed by a Flush. Over a stream that holds writes until Flush, the alert stays in that stream's buffer. The peer then waits until the stream ends and reports NetError CLOSED instead of TLS, or waits until its timeout if the stream stays open. net_tls.fb's PipeEnd delivers every Write at once, so no test catches this. In a control run, with Write flushing straight away, the server reports TLS as expected.

```
// Put in tests/programs/ beside net_tls/ and run it there.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Held: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Held = [[], []]
        Ended = [False, False]
    End New
End Class

// A pipe end that holds what's written until Flush, as a buffered stream does.
Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read waits forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        Joined.Held[1 - Side].AddAll(from.ToArray())
    End Method

    Method Flush() Throws IOError
        Joined.Waiting[1 - Side].AddAll(Joined.Held[1 - Side])
        Joined.Held[1 - Side].Clear()
    End Method

    Async Method FlushAsync() Throws IOError
        Try Flush()
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("read")
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "served"
    Catch e As NetError
        Return "server: NetError {e.Kind}: {e.Message}"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "connected"
    Catch e As NetError
        Return "client: NetError {e.Kind}: {e.Message}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Local near = PipeEnd(Pipe(), 0)
Local server = TlsServer(Certificates(rogue_pem), (Try? Key.FromPem(rogue_key)).Expect("key"))
Local served = Serve(server, near.Other())
Local client = TlsClient()
client.TrustOnly(Certificates(ca_pem))
Print(Talk(client, near).Wait())
For i = 1 To 20
    NextFrame().Wait()
End For
near.Hang()
Print(served.Wait())
```

Expected: client: NetError TLS: the server's certificate isn't signed by a root this side trusts
server: NetError TLS: the client refused the handshake, or sent a record that doesn't authenticate

Actual: client: NetError TLS: the server's certificate isn't signed by a root this side trusts
server: NetError CLOSED: the stream ended before the TLS handshake did

Suggested fix: In Handshake, after a failed step, write `If Try? Send() == True Then Try? Inner.Flush()`, and do the same with SendAsync/FlushAsync in HandshakeAsync. Add a test to net_tls.fb over a pipe end that holds writes until Flush.

Skeptic: I ran the program as t1.fb (it uses a pipe end that holds writes until Flush). Debug and release both print "server: NetError CLOSED: the stream ended before the TLS handshake did". In a control run, t1c.fb, Write delivers at once, and the server prints "NetError TLS: the client refused the handshake, ...". In std/net/tls.fb, lines 363 and 385 call `Try? Send()` / `Try? Await SendAsync()` without Inner.Flush/FlushAsync, while every other Send in Handshake, Close and Read is followed by a flush (lines 368, 390, 473, 526). So the alert is held in the buffer. Only the peer's diagnosis suffers (CLOSED instead of TLS, or waiting until a timeout), so I rate it low.

## tests-2 (low): A TlsStream write after Close succeeds silently, and the other side drops the bytes

Status: fixed in a86db803, as std-3, the same bug

Where: std/net/tls.fb:475

After Close or CloseAsync has sent close_notify, a later Write or WriteAsync still seals records and sends them, and throws nothing. The peer's Read returns 0 at close_notify, so it never sees those bytes and the data is lost without any error. A TcpStream throws an IOError, with a NetError Cause, when written to after Close ("the TcpStream is closed"). TlsStream has no such check, and no test writes after Close.

```
// Put in tests/programs/ beside net_tls/ and run it there.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        If Joined.Ended[1 - Side]
            Throw IOError(IOErrorKind.OTHER, "the pipe's other end is closed")
        End If

        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function NewPipe(): PipeEnd
    Return PipeEnd(Pipe(), 0)
End Function

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test's certificates read")
End Function

Function KeyOf(pem: String): Key
    Return (Try? Key.FromPem(pem)).Expect("the test's keys read")
End Function

Function Server(pem: String, key: String): TlsServer
    Return TlsServer(Certificates(pem), KeyOf(key))
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Local all = Try Await ReadAllAsync(tls)
        Local again = Try Await tls.ReadAsync(Buffer<UInt8>(10))
        Return "server read {all.Count} bytes, then {again}"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Try Await tls.WriteAsync(Buffer<UInt8>.From("abc".Bytes))
        Try Await tls.CloseAsync()
        Try Await tls.WriteAsync(Buffer<UInt8>.From("lost".Bytes))
        Return "client wrote after Close"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Local near = NewPipe()
Local served = Serve(Server(localhost_pem, localhost_key), near.Other())
Print(Talk(Client(), near).Wait())
Print(served.Wait())
```

Expected: client: <an IOError saying the TLS stream is closed, as a TcpStream's write after Close throws>
server read 3 bytes, then 0

Actual: client wrote after Close
server read 3 bytes, then 0

Suggested fix: Have TlsStream remember that Close or CloseAsync ran, and make Write and WriteAsync throw an IOError then, with a NetError of CLOSED as its Cause, as a closed TcpStream's does. Document this, and add the case to net_tls.fb.

Skeptic: t2.fb prints "client wrote after Close" and "server read 3 bytes, then 0". After the run, 37 bytes are left unread on the server's side of the pipe: a sealed record sent after close_notify, which the peer never reads, so the data is lost without any error. TlsStream.Close (tls.fb lines 524-532) says close_notify "tells the other side nothing more comes", yet Write and WriteAsync (lines 489-501) keep no closed state and still seal and send. This needs a misuse to happen, and the docs set no rule for writes after Close, so I rate it low.

## tests-3 (low): Offer accepts an ALPN list too large to send, and the handshake then fails with a garbled mbedTLS message

Status: fixed in a86db803, as std-5 and runtime-3, the same bug: panic_tls_protocols_long has a list at the limit and one past it, and panic_tls_protocol_long (5cb40a90) a 255-byte name that's offered and a 256-byte one that panics

Where: std/net/tls.fb:156

CheckProtocols checks only each name: 1 to 255 bytes, no NUL. It doesn't check the whole list, whose ClientHello has to fit in mbedTLS's record buffer. With 70 names of 253 bytes (about 17.7 KB), the client's handshake fails with NetError TLS "mbedTLS failed: UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output". With 300 names, both sides give a similar message, ending in "HMAC_DRBG - Read/write error in file". docs/stdlib.md and Offer's comment say Offer panics only for a bad name. A single 255-byte name works, and 60 such names work. No test reaches the size limit, either per name (256 bytes) or for the whole list.

```
// Put in tests/programs/ beside net_tls/ and run it there.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        If Joined.Ended[1 - Side]
            Throw IOError(IOErrorKind.OTHER, "the pipe's other end is closed")
        End If

        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function NewPipe(): PipeEnd
    Return PipeEnd(Pipe(), 0)
End Function

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test's certificates read")
End Function

Function KeyOf(pem: String): Key
    Return (Try? Key.FromPem(pem)).Expect("the test's keys read")
End Function

Function Server(pem: String, key: String): TlsServer
    Return TlsServer(Certificates(pem), KeyOf(key))
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "served, {(tls.Protocol ?? "none").Bytes.Count} chars"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        Return "connected, {(tls.Protocol ?? "none").Bytes.Count} chars"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Function Attempt(names: String[])
    Local near = NewPipe()
    Local server = Server(localhost_pem, localhost_key)
    server.Offer(names)
    Local served = Serve(server, near.Other())
    Local client = Client()
    client.Offer(names)
    Print(Talk(client, near).Wait())
    near.Hang()
    Print(served.Wait())
End Function

Local long = ""
For i = 1 To 255
    long = long + "a"
End For
Attempt([long])
Local short = ""
For i = 1 To 250
    short = short + "b"
End For
Local many: String[] = []
For i = 1 To 70
    many.Add("{i:000}" + short)
End For
Attempt(many)
```

Expected: connected, 255 chars
served, 255 chars
then either a panic at the second client's Offer for a list ALPN can't carry, or a handshake that works or fails with a message that says what's wrong

Actual: connected, 255 chars
served, 255 chars
client: mbedTLS failed: UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output
server: mbedTLS failed: UNKNOWN ERROR CODE (0080) : HMAC_DRBG - Read/write error in file

Suggested fix: Have CheckProtocols also panic when the encoded list (the sum of 1 + the name's byte count for each name) goes over a limit that fits a ClientHello, for example a few KB, and document that limit in Offer and docs/stdlib.md. Add panic tests for a 256-byte name and for a list that is too long, and a passing test for a 255-byte name.

Skeptic: t3.fb: a single 255-byte name works on both sides ("connected, 255 chars"), and so do 60 names of 253 bytes. With 64, 65 or 70 names the client fails with "mbedTLS failed: UNKNOWN ERROR CODE (0080) : BIGNUM - The input arguments are negative or result in illegal output" and the server gets CLOSED. With 300 names both sides fail with "... HMAC_DRBG - Read/write error in file". CheckProtocols (tls.fb lines 154-163) checks each name, not the size of the whole list. The message is garbled because runtime/src/tls.cpp's describe() falls through to mbedtls_strerror. Offer's docs promise a panic only for a bad name. Real ALPN lists are tiny, so I rate it low.

## tests-4 (low): No test covers two documented behaviors: an ending without close_notify after the handshake, and a non-socket stream failing during the handshake

Status: fixed in a86db803: net_tls's "a stream that fails" gives a NetError of OTHER from the pipe's failure in the handshake, "past the handshake" gives the server an IOError of UNEXPECTED_END when the client's stream ends without close_notify, and the client connects to ::1

Where: tests/programs/net_tls.fb:224

The milestones row's done-when holds, but docs/stdlib.md and tls.fb promise two more behaviors that no test checks. (1) Past the handshake, a stream that ends without close_notify throws an IOError of UNEXPECTED_END. net_tls.fb's only stream that ends ("a stream that ends") ends before the handshake, so READ_CUT_SHORT is never reached. (2) During the handshake, a failure of a stream that isn't a socket becomes a NetError of OTHER through Failed(). net_tls.fb's pipe never throws during a handshake. Both work today, as the program shows, but a regression in either would go unnoticed. The program also connects to "::1", which the certificate names but no test uses.

```
// Put in tests/programs/ beside net_tls/ and run it there.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        If Joined.Ended[1 - Side]
            Throw IOError(IOErrorKind.OTHER, "the pipe's other end is closed")
        End If

        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function NewPipe(): PipeEnd
    Return PipeEnd(Pipe(), 0)
End Function

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test's certificates read")
End Function

Function KeyOf(pem: String): Key
    Return (Try? Key.FromPem(pem)).Expect("the test's keys read")
End Function

Function Server(pem: String, key: String): TlsServer
    Return TlsServer(Certificates(pem), KeyOf(key))
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

// The server writes a line and hangs up without close_notify.
Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Try Await tls.WriteAsync(Buffer<UInt8>.From("hi\n".Bytes))
        pipe.Hang()
        Return "served"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "::1")
        Local reader = BufferedReader(tls)
        Local first = Try Await reader.ReadLineAsync() ?? "-"
        Local after = Try Await reader.ReadLineAsync() ?? "the end"
        Return "client got {first}, then {after}"
    Catch e As IOError
        Return "client: IOError {e.Kind}: {e.Message}"
    Catch e As NetError
        Return "client: NetError {e.Kind}: {e.Message}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Local near = NewPipe()
Local served = Serve(Server(localhost_pem, localhost_key), near.Other())
Print(Talk(Client(), near).Wait())
Print(served.Wait())

// A pipe whose other end is gone before the handshake: its write fails.
Local gone = NewPipe()
gone.Hang()
Print(Talk(Client(), gone).Wait())
```

Expected: client: IOError UNEXPECTED_END: the stream ended without TLS's close_notify, so what came may be cut short
served
client: NetError OTHER: the stream under TLS failed: the pipe's other end is closed
(and net_tls.out should have lines like these)

Actual: Same output as expected, but no test in tests/programs/net_tls*.fb produces either line: neither net_tls.out nor net_tls_tcp.out has UNEXPECTED_END or a handshake NetError of OTHER.

Suggested fix: Add these two cases to net_tls.fb and net_tls.out: a server that writes and then hangs up without CloseAsync, and a client over a pipe whose write fails during the handshake. Optionally, also connect to "::1".

Skeptic: t4.fb works as the docs say: "client: IOError UNEXPECTED_END: the stream ended without TLS's close_notify, ...", "served", and "client: NetError OTHER: the stream under TLS failed: the pipe's other end is closed" (and connecting to "::1" works). But neither net_tls.out nor net_tls_tcp.out has an UNEXPECTED_END line or a handshake NetError OTHER line. Grepping tests/ for UNEXPECTED_END, "cut short" and "under TLS failed" finds no TLS test, and tests/runtime has only tls_roots_tests.cpp. net_tls's "a stream that ends" case ends before the handshake, so it gives CLOSED. This is a real coverage gap.

## tests-5 (low): docs/stdlib.md says every failure past the handshake has a NetError of TLS as its Cause, but the stream's own IOError passes through unchanged

Status: fixed in a86db803 (stdlib.md, tls.fb's header, and TlsStream's comment say TLS's failure past the handshake has a NetError of TLS as its Cause, and the stream's own failure is thrown as it was), with net_tls's "past the handshake": a write over a pipe whose other end closed throws the pipe's IOError, with no Cause

Where: docs/stdlib.md:1122

docs/stdlib.md says: "Past the handshake, a failure is an IOError whose Cause is a NetError of TLS, or of UNEXPECTED_END...". tls.fb's header says the same. When the stream under the TlsStream fails after the handshake, Read, Write, Flush and Close rethrow that stream's own IOError unchanged. For a pipe, that IOError has no Cause at all. For a TcpStream, its Cause is a NetError of RESET, CLOSED, and so on, not TLS. Wrapping only TLS's own failures is a reasonable design, but the docs don't describe it.

```
// Put in tests/programs/ beside net_tls/ and run it there.
Import std.core
Import std.collections
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls/certificates"

Class Pipe
    Field Waiting: UInt8[][]
    Field Ended: Bool[]

    New()
        Waiting = [[], []]
        Ended = [False, False]
    End New
End Class

Class PipeEnd Implements Reader, Writer
    Field Joined: Pipe
    Field Side: Int

    New(joined: Pipe, side: Int)
        Joined = joined
        Side = side
    End New

    Method Other(): PipeEnd => PipeEnd(Joined, 1 - Side)

    Method Read(into: Buffer<UInt8>): Int64 Throws IOError
        Local waiting = Joined.Waiting[Side]
        If into.Count == 0 Then Return 0
        If waiting.Count == 0
            If Joined.Ended[Side] Then Return 0
            Panic("a blocking read of a pipe in memory waits for the other end forever")
        End If

        Local count = Min(into.Count, waiting.Count As Int64)
        For i = 0 Until count
            into[i] = waiting[i As Int]
        End For

        waiting.RemoveRange(0, count As Int)
        Return count
    End Method

    Async Method ReadAsync(into: Buffer<UInt8>): Int64 Throws IOError
        While Joined.Waiting[Side].Count == 0 && !Joined.Ended[Side]
            Await NextFrame()
        End While

        Return Try Read(into)
    End Method

    Method Write(from: Buffer<UInt8>) Throws IOError
        If Joined.Ended[1 - Side]
            Throw IOError(IOErrorKind.OTHER, "the pipe's other end is closed")
        End If

        Joined.Waiting[1 - Side].AddAll(from.ToArray())
    End Method

    Method Hang()
        Joined.Ended[1 - Side] = True
    End Method
End Class

Function NewPipe(): PipeEnd
    Return PipeEnd(Pipe(), 0)
End Function

Function Certificates(pem: String): Certificate[]
    Return (Try? Certificate.AllFromPem(pem)).Expect("the test's certificates read")
End Function

Function KeyOf(pem: String): Key
    Return (Try? Key.FromPem(pem)).Expect("the test's keys read")
End Function

Function Server(pem: String, key: String): TlsServer
    Return TlsServer(Certificates(pem), KeyOf(key))
End Function

Function Client(): TlsClient
    Local client = TlsClient()
    client.TrustOnly(Certificates(ca_pem))
    Return client
End Function

Async Function Serve(server: TlsServer, pipe: PipeEnd): String
    Try
        Local tls = Try Await server.AcceptAsync(pipe)
        Return "served"
    Catch e As Error
        Return "server: {e.Message}"
    End Try
End Function

// After the handshake the stream under it fails: what a write throws.
Async Function Talk(client: TlsClient, pipe: PipeEnd): String
    Try
        Local tls = Try Await client.ConnectAsync(pipe, "localhost")
        pipe.Hang()
        Try Await tls.WriteAsync(Buffer<UInt8>.From("hello".Bytes))
        Return "wrote"
    Catch e As IOError
        Local net = e.Cause As? NetError
        Return "IOError {e.Kind}: {e.Message}; Cause a NetError: {net != Null}"
    Catch e As Error
        Return "client: {e.Message}"
    End Try
End Function

Local near = NewPipe()
Local served = Serve(Server(localhost_pem, localhost_key), near.Other())
Print(Talk(Client(), near).Wait())
Print(served.Wait())
```

Expected: Per docs/stdlib.md: an IOError whose Cause is a NetError (of TLS)

Actual: IOError OTHER: the pipe's other end is closed; Cause a NetError: False
served

Suggested fix: In docs/stdlib.md's std.net.tls bullet and tls.fb's header, say that past the handshake, a failure of TLS itself is an IOError whose Cause is a NetError of TLS, and that the stream's own failure is thrown as that stream threw it. Optionally, add this case to net_tls.fb.

Skeptic: t5.fb prints "IOError OTHER: the pipe's other end is closed; Cause a NetError: False". docs/stdlib.md (around line 1122) says "Past the handshake, a failure is an IOError whose Cause is a NetError of TLS, or of UNEXPECTED_END", and the headers of tls.fb (lines 13-16) and TlsStream (lines 308-311) say the same. But Send, Receive and Flush rethrow Inner's IOError unchanged (tls.fb lines 403-442, 514-520); only Broken() (line 460) wraps a NetError of TLS. The docs don't match what the code does.

## tests-6 (low): net_tls_tcp.fb's header comment says the listener refuses the first Async client, but the client refuses the listener

Status: fixed in a86db803 (the header says the first Async client refuses the listener's certificate)

Where: tests/programs/net_tls_tcp.fb:3

The header comment reads: "then one on the main thread answers Async clients, the first of which it refuses". In the test, the first client is TlsClient() trusting the OS's roots. That client refuses the listener's certificate, and the listener only sees the refusal, as net_tls_tcp.out shows. The comment at the call site ("A client that trusts only the OS's roots refuses the test certificate") has it right.

```
// Put in tests/programs/ beside net_tls_tcp/ and run it there: the test's own refused case.
Import std.core
Import std.crypto
Import std.ffi
Import std.io
Import std.net
Import "./net_tls_tcp/certificates"

Async Function Answer(listener: TlsListener): String
    Try
        Local tls = Try Await listener.AcceptAsync()
        Return "answered"
    Catch e As NetError
        Return "the listener: NetError {e.Kind}: {e.Message}"
    End Try
End Function

Async Function Ask(client: TlsClient, port: UInt16): String
    Try
        Local tcp = Try Await TcpStream.ConnectAsync("localhost", port)
        Local tls = Try Await client.ConnectAsync(tcp, "localhost")
        Return "connected"
    Catch e As NetError
        Return "the client: NetError {e.Kind}: {e.Message}"
    End Try
End Function

Try
    Local server = TlsServer(Try Certificate.AllFromPem(localhost_pem), Try Key.FromPem(localhost_key))
    Local listener = TlsListener.Listen(SocketAddress(IpAddress.Loopback, 0), server)
    Local refused = Answer(listener)
    Print(Ask(TlsClient(), listener.LocalAddress.Port).Wait())
    Print(refused.Wait())
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: The comment should describe what happens: the client refuses the listener's certificate.

Actual: the client: NetError TLS: the server's certificate isn't signed by a root this side trusts
the listener: NetError TLS: the client refused the handshake, or sent a record that doesn't authenticate

Suggested fix: Reword the comment, for example: "then one on the main thread answers Async clients, the first of which refuses its certificate, and goes on to the next".

Skeptic: tests/programs/net_tls_tcp.fb line 3 says the listener answers Async clients, "the first of which it refuses". The call site (lines 91-94) says "A client that trusts only the OS's roots refuses the test certificate", and net_tls_tcp.out shows "the client: NetError TLS: the server's certificate isn't signed by a root this side trusts" and then "the listener: NetError TLS". So the client refuses the listener, not the other way round, and the header comment is wrong.

## Refuted by the skeptics (no action)

- runtime-7: No test runs TLS 1.2, though the docs promise TLS 1.2 and 1.3. Ran r7.fb (TlsClient to tls-v1-2.badssl.com:1012), which prints "TLS 1.2", so the 1.2 path works. It's true that net_tls.out and net_tls_tcp.out only show TLS 1.3 and no grep hit caps a version. But this is a coverage gap, not wrong behavior. Nothing in the code contradicts stdlib.md's "TLS 1.2 and 1.3", and the milestone row's done-when doesn't require a 1.2 test. Refuted as a bug; worth adding a test at the user's discretion.
- std-7: docs/stdlib.md's error areas list NetError's kinds without HTTP_STATUS, which sits between TLS and OTHER. Not a contradiction. The error-areas paragraph in docs/stdlib.md (lines 293-297) says each Kind is an Enum and "a later step can add to its area's". HTTP_STATUS is documented where it was added, in std.net.http's bullet (stdlib.md:1155: "throw a NetError of `HTTP_STATUS` for a status that isn't success"), so the finding's claim that the docs don't say it exists is wrong. git log -S shows HTTP_STATUS came from lane 9's HTTP commits (a37feab1, 73a8601c), not from this TLS step. The docs promise no ordinals.
