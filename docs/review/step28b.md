# The review of M6 step 28's second half: std.net's TcpListener, TcpStream, UdpSocket, IpcListener, IpcStream (as step 13's Readers and Writers, blocking and Async), and LAN discovery (mDNS and DNS-SD), on step 13's streams

23 confirmed findings: 3 high, 7 medium, 13 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## lan-1 (high): Every answer and announcement carries the A records of every interface, so a found service's Address can be one the finder can't reach

Status: fixed in 1de61f1d and 0a77e3cf

Where: std/net/lan.fb:311

Offered.AddressRecords puts all of V4Addresses() into every response. It does this for the unicast reply to a query that came in on one interface, and for the multicast copy SendEverywhere sends through each interface. RFC 6762 6.2 says a responder MUST NOT include addresses that aren't valid on the interface it sends on. Found.Services then keeps those addresses in the order the A records came, and LanService.Address takes the first one. On this PC (Hyper-V Default Switch, Ethernet, WSL) the first is the Default Switch's 172.30.144.1. A player on the 10.19.128.x LAN who joins at service.Address would try 172.30.144.1:4242, which is internal to this PC. Machines with Hyper-V, WSL, VirtualBox, Docker, or a VPN are the common case. Other responders can also see a peer's addresses from another interface, under their own host name, as a conflict.

```
Import std.core
Import std.collections
Import std.net
Import std.time

// A one-shot PTR query for _fblan._udp.local.
Function Query(): UInt8[]
    Local bytes: UInt8[] = [0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]
    For label In ["_fblan", "_udp", "local"]
        bytes.Add(label.Bytes.Count As UInt8)
        bytes.AddAll(label.Bytes)
    End For
    Local tail: UInt8[] = [0, 0, 12, 0, 1]
    bytes.AddAll(tail)
    Return bytes
End Function

// Whether the packet holds an A record's data of `address`: a length of 4, then its bytes.
Function Holds(packet: UInt8[], address: IpAddress): Bool
    Local wanted: UInt8[] = [0, 4]
    wanted.AddAll(address.Bytes)
    For at = 0 To packet.Count - wanted.Count
        If packet[at..at + wanted.Count] == wanted Then Return True
    End For
    Return False
End Function

Const port: UInt16 = 55380
Try
    Local announcer = LanAnnouncer.Start("_fblan._udp", "Game", 4242, Null, port)
    Local v4: IpAddress[] = []
    For address In Lan.LocalAddresses()
        If address.IsV4 Then v4.Add(address)
    End For
    Print("this machine's IPv4 addresses: {v4}")

    // What a finder gets: every interface's addresses, and Address is the first.
    For service In Await Lan.FindAsync("_fblan._udp", 1500.Milliseconds, port)
        Print("found: {service.Addresses}, Address {service.Address ?? SocketAddress(IpAddress.Any, 0)}")
    End For

    // A query through the last interface only, as a machine on that interface's network asks.
    Local through = v4[v4.Count - 1]
    Local socket = UdpSocket.Bind(SocketAddress(through, 0))
    socket.SetMulticastInterface(through)
    socket.Send(Query(), SocketAddress(IpAddress.V4(224, 0, 0, 251), port))
    Local reply = Try Await socket.ReceiveAsync(2.Seconds)
    For address In v4
        Print("answer to a query from {through} has {address}: {Holds(reply.Bytes, address)}")
    End For
    announcer.Stop()
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: The answer to a query from 172.28.128.1 holds only that interface's address (True for 172.28.128.1 and False for the other two), and a finder's Address is one on the network it asked from.

Actual: this machine's IPv4 addresses: [172.30.144.1, 10.19.128.21, 172.28.128.1]
found: [172.30.144.1, 10.19.128.21, 172.28.128.1], Address 172.30.144.1:4242
answer to a query from 172.28.128.1 has 172.30.144.1: True
answer to a query from 172.28.128.1 has 10.19.128.21: True
answer to a query from 172.28.128.1 has 172.28.128.1: True

Suggested fix: Give each response only the addresses of the interface it goes through. In SendEverywhere, build the records per address it sends through. For a unicast reply, use the local addresses on the querier's subnet; fb_io_net_interfaces would need to return each address's netmask, which uv_interface_addresses has. On the Find side, Found.Services should put the address the answer came from (Senders) first when it is among the A records, so Address can always be reached.

Skeptic: Ran the reviewer's program (scratchpad/rv-net/lanrefute/lan1.fb), debug and --release, same output: 'this machine's IPv4 addresses: [172.30.144.1, 10.19.128.21, 172.28.128.1]', 'found: [172.30.144.1, 10.19.128.21, 172.28.128.1], Address 172.30.144.1:4242', and the unicast answer to a query sent only through 172.28.128.1 holds all three A records (True, True, True). Code: Offered.AddressRecords (lan.fb:311) always uses V4Addresses(), for Answer's one-shot unicast reply and for each interface's copy in SendEverywhere (which sends the same bytes through every interface). RFC 6762 6.2 says a responder MUST NOT include addresses that aren't valid on the interface it sends on, and stdlib.md says this is mDNS as RFC 6762 has it. Found.Services keeps A records in arrival order and uses Senders only when there are none, so LanService.Address (Addresses[0]) is the Default Switch's 172.30.144.1. A peer on 10.19.128.x can't route to that. Lane decision 11 (IPv4, every interface but loopback's) doesn't cover this. The headline use, joining a found LAN game at Address, fails on common machines (WSL, Hyper-V, VPN, Docker).

## runtime-1 (high): On Linux, a write to a connection the other side closed kills the program with SIGPIPE instead of throwing RESET

Status: fixed in 0498f8af

Where: runtime/src/net.cpp:739

Nothing in the runtime ignores or blocks SIGPIPE. `fb run` and the test runner start programs with SIGPIPE at its default (process.cpp's POSIX_SPAWN_SETSIGDEF). On Linux, libuv writes a stream with plain writev, with no MSG_NOSIGNAL; only macOS gets SO_NOSIGPIPE, in uv__socket. So the first uv_write to a TCP or Unix-socket peer that has reset raises SIGPIPE on the I/O thread, and that ends the whole process (exit 141). The program never sees the IOError of RESET that tcp.fb and stdlib.md promise. Any server whose client disconnects mid-conversation hits this. net_tcp.fb's last section does exactly this, so that test would die on Linux CI. I showed it by compiling rev-5's net.cpp and the bundled libuv 1.52.1 on WSL Ubuntu, with a small stand-in for the I/O thread, and calling the fb_io_net_ functions in the order tcp.fb and ipc.fb call them (harness: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-net/runtime/posix/main.cpp, scenarios sigpipe-tcp and sigpipe-ipc).

```
// What's written to a connection the other side closed is reset: an IOError whose Cause is a
// NetError of RESET, which the program catches, and then it goes on.
Import std.core
Import std.ffi
Import std.io
Import std.net
Import std.time

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    server.Close()
    Print(client.Read(Buffer<UInt8>(1)))
    Local kind = "none"
    For i = 1 To 500
        Try
            client.WriteText("after")
        Catch e As IOError
            kind = "{(e.Cause As NetError).Kind}"
            Break
        End Try

        Sleep(10.Milliseconds)
    End For
    Print(kind)
Catch e As Error
    Print("failed: {e.Message}")
End Try
Print("end")
```

Expected: 0
RESET
end
(this is what Windows prints)

Actual: On Linux the program prints 0 and is then killed by SIGPIPE (exit 141). It never prints RESET or end. Harness output on Linux: sigpipe-tcp prints "read: 0", then exits with 141, the same for sigpipe-ipc over a Unix socket. With SIGPIPE ignored, the same calls give "write 2: EPIPE, kind 1" (RESET) and carry on.

Suggested fix: Keep SIGPIPE from ending the program on POSIX. One way: block it on the I/O thread with pthread_sigmask when io.cpp starts that thread. Every socket write, and left_behind's probe, runs on that thread, so this leaves the program's own SIGPIPE handling alone. The other way is std::signal(SIGPIPE, SIG_IGN) the first time the sockets are used, as Node does. Run net_tcp.fb on Linux to confirm.

Skeptic: I built rev-5's net.cpp and the bundled libuv 1.52.1 on WSL Ubuntu, with an I/O-thread stand-in that matches io.cpp's, and called the fb_io_net_ functions in the order tcp.fb uses them (rt-refute/posix/main.cpp, scenario sigpipe-tcp). With SIGPIPE at its default it printed 'listen: ok / connect: ok / accept: ok / read: 0 / write 1: ok' and then exited 141. sigpipe-ipc did the same over a Unix socket and died on the first write. With SIGPIPE ignored, the same calls gave 'write 2: EPIPE, kind 1' (RESET) and carried on to 'end'. Grepping rev-5 outside _deps for SIGPIPE, SIG_IGN, and sigmask finds only compiler/process.cpp, which resets SIGPIPE to its default for the programs it starts. The runtime itself never ignores or blocks it. libuv's unix/stream.c writes with writev or sendmsg with flags 0, and SO_NOSIGPIPE exists only on macOS. So on Linux any write to a peer that has reset kills the program, and net_tcp.fb's last section, which does exactly that, would die on Linux CI.

## streams-1 (high): On Linux, writing to a TCP or IPC connection the other side closed kills the program with SIGPIPE instead of throwing RESET

Status: fixed in 0498f8af

Where: runtime/src/net.cpp:739

The runtime never ignores or blocks SIGPIPE. libuv writes stream sockets with writev() (src/unix/stream.c:812) and doesn't pass MSG_NOSIGNAL. It sets SO_NOSIGPIPE only where that option exists (macOS and the BSDs, core.c:532), not on Linux. So on Linux, a uv_write to a connection the peer reset raises SIGPIPE on the I/O thread, and that ends the whole process. The fb CLI also starts programs with SIGPIPE at its default (compiler/src/process.cpp:255-275), so the step's own tests/programs/net_tcp.fb (its last part: server.Close(), then writes until RESET) would die on Linux CI. IPC over Unix sockets has the same problem. I showed this with a libuv-only C mirror of what net.cpp does (libuv 1.52.1 from _deps, built by hand in WSL Ubuntu): .../scratchpad/rv-net/streams/posix/sigpipe.c ends with exit status 141 (128+SIGPIPE) and never reports the write error. Windows doesn't have the problem: the program below prints the expected lines there.

```
Import std.core
Import std.ffi
Import std.io
Import std.net
Import std.time

// The other side closes, and this side writes until its OS says the connection's reset, as
// net_tcp.fb's last part does.
Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    server.Close()
    Print(client.Read(Buffer<UInt8>(1)))
    Local kind = "none"
    For i = 1 To 500
        Try
            client.WriteText("after")
        Catch e As IOError
            kind = "{(e.Cause As NetError).Kind}"
            Break
        End Try

        Sleep(10.Milliseconds)
    End For
    Print(kind)
Catch e As Error
    Print("failed: {e.Message}")
End Try
Print("ended")
```

Expected: 0
RESET
ended   (what Windows prints)

Actual: On Linux: 0, then the process is killed by SIGPIPE (exit 141) with nothing else printed. The libuv mirror posix/sigpipe.c prints "connected: 0" then "exit status 141".

Suggested fix: Keep SIGPIPE from ending the process on POSIX. One way: block it with pthread_sigmask on the I/O thread when it starts, since every socket write happens there, so console output keeps the default behaviour. The other way, Node's, is signal(SIGPIPE, SIG_IGN) once the first socket is made. With either, EPIPE reaches on_written as UV_EPIPE, which maps to RESET. net_tcp.fb already covers this once CI runs on Linux.

Skeptic: I read the code first. Nothing in runtime/ ignores or blocks SIGPIPE (grep found no SIGPIPE, sigaction or sigmask there). compiler/src/process.cpp:255-275 starts programs with SIGPIPE at its default. libuv's unix/stream.c writes with uv__writev and no MSG_NOSIGNAL, and sets SO_NOSIGPIPE only where it exists (core.c:532), which Linux doesn't have. I built my own mirror in WSL Ubuntu against the libuv 1.52.1 from _deps (rv-net/streams-v/posix/sigpipe.c). It does what net.cpp does: listen, connect, accept, close the server side, read EOF, then uv_write every 10ms. It printed "connected: 0 / read: EOF / write 1: ok", then "exit status 141" (killed by SIGPIPE). With signal(SIGPIPE, SIG_IGN) first, it printed "write 2: EPIPE / ended" and exit status 0, which is the path that would give RESET. On Windows, the finding's program (my s1.fb) prints "0 / RESET / ended" in both debug and release. net_tcp.fb's last part does exactly this, so it would kill the test program on Linux.

## runtime-2 (medium): TcpListener.Listen with a backlog of 0 or less crashes on Windows with libuv's assertion

Status: fixed in 19950238 and 0a77e3cf

Where: runtime/src/net.cpp:1118

fb_io_net_listen passes the backlog straight to uv_listen. libuv's Windows uv__tcp_listen does assert(backlog > 0), and that assertion is live in the bundled build, debug and release alike. So TcpListener.Listen(address, 0), or any backlog of 0 or less, aborts the program with "Assertion failed" and 0xC0000409. There's no panic at the caller and no NetError. On Linux and macOS, listen() takes 0, so the same program works there. IpcListener ignores the backlog on Windows, so only TCP crashes. std's convention for a bad argument is a panic at the caller, as CheckName does for an IPC name.

```
// A listener's backlog of 0 is the OS's to raise to what it allows, or a panic where it's given,
// not a crash.
Import std.core
Import std.net

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0), 0)
    Print("backlog 0 listens")
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: backlog 0 listens (or a panic at the caller saying a backlog is at least 1)

Actual: Assertion failed: backlog > 0, file .../_deps/libuv-src/src/win/tcp.c, line 624
error: backlog0.exe exited with code 0xC0000409 (in debug and in --release; -5 does the same)

Suggested fix: Panic at the caller for a backlog under 1 in TcpListener.Listen and IpcListener.Listen (@PanicsAtCaller, as CheckName does), or clamp it to at least 1 in fb_io_net_listen and fb_io_net_listen_pipe. Add a panic_ test for it.

Skeptic: I ran rt-refute/backlog0.fb, TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0), 0). In debug and in --release it printed 'Assertion failed: backlog > 0, file .../libuv-src/src/win/tcp.c, line 624' and exited 0xC0000409. A backlog of -5 (backlogneg.fb) did the same. IpcListener.Listen(name, 0) printed 'ipc backlog 0 listens'. fb_io_net_listen passes the backlog to uv_listen unchecked (net.cpp:1118). So a bad argument aborts the process where std's convention is a panic at the caller.

## runtime-3 (medium): A ReadTimeout of zero never reads: a blocking Read never starts libuv reading, so the bytes that came are never taken

Status: fixed in 134219b2 and e2b6732f

Where: runtime/src/net.cpp:1173

fb_io_net_read only runs read_more when `timeout != 0`. Nothing else starts a stream reading: not connect, not accept, and a zero-timeout read doesn't either. So a TcpStream or IpcStream whose ReadTimeout is Duration.Zero(), or negative, which NanosecondsOf makes 0, throws a TimeoutError on every Read, even when the other side's bytes have long been in the OS's buffer. A program that polls a stream without waiting, as a game's loop does, never gets anything. The same happens once reading pauses at the 64 KiB read-ahead: a zero-timeout reader drains the buffer and reading never starts again. runtime.hpp says 0 gives UV_ETIMEDOUT "when there's nothing", but here there always is something. UDP's Receive with a zero timeout has a related race: receive_more is queued and the zero wait returns before it runs. On Linux it timed out in 1 of 3 runs with a datagram already waiting 200 ms (harness scenario zero-receive). On Linux every zero-timeout read timed out (harness scenario zero-read).

```
// A ReadTimeout of zero takes what's come without waiting: here two bytes came long before.
Import std.core
Import std.ffi
Import std.io
Import std.net
Import std.time

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    server.WriteText("hi")
    Sleep(200.Milliseconds)
    client.ReadTimeout = Duration.Zero()
    Local into = Buffer<UInt8>(2)
    Local result = "nothing"
    For i = 1 To 5
        Try
            result = "read {client.Read(into)}"
            Break
        Catch e As IOError
            result = "timed out {i} times"
            Sleep(100.Milliseconds)
        End Try
    End For
    Print(result)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: read 2

Actual: timed out 5 times (debug and --release, on Windows; the Linux harness's zero-timeout reads give ETIMEDOUT every time)

Suggested fix: Have a stream that isn't readable start reading even for a zero timeout: queue read_more, then return UV_ETIMEDOUT without waiting. Better, start reading when a stream connects or is accepted, bounded by read_ahead as it already is, so a zero-timeout Read sees what the OS has. Do the same for a UDP socket from its bind, so Receive with a zero timeout doesn't race. Add a test that polls with a zero ReadTimeout.

Skeptic: rt-refute/zeroread.fb, in debug and --release on Windows: the server writes 'hi', the program sleeps 200 ms, then sets ReadTimeout = Duration.Zero(). It printed 'timed out 5 times: reading from 127.0.0.1:51899 took longer than it was given'. Once a 1 ms read had started libuv reading, a zero-timeout read did work ('zero after read 2'). The IPC version printed 'timed out 5 times'. The Linux harness's zero-read gave ETIMEDOUT 3 of 3 times, and zero-receive (UDP) timed out 4 of 5 times with a datagram already waiting 200 ms (Windows' UDP case got it 3 of 3). The cause: fb_io_net_read runs read_more only when timeout != 0 (net.cpp:1173), and nothing else starts reading. That breaks socket.fb's 'and 0 ... doesn't wait' and runtime.hpp's 'giving UV_ETIMEDOUT when there's nothing', because here the bytes are there and are never taken.

## runtime-5 (medium): On macOS, IPC names longer than 50 bytes fail with NetError OTHER, though 1 to 64 bytes are allowed

Status: fixed in 72f15bd2 and 0a77e3cf

Where: runtime/src/net.cpp:372

pipe_path puts a Unix socket at <uv_os_tmpdir()>/<name>.sock. On macOS uv_os_tmpdir is $TMPDIR, which is /var/folders/xx/<30 chars>/T/, 48 bytes once libuv drops the trailing slash. sun_path holds 104 bytes there, and UV_PIPE_NO_TRUNCATE makes uv_pipe_bind2 and uv_pipe_connect2 give UV_EINVAL for a longer path. The path is 54 + N bytes, so any name over 50 bytes fails to Listen or Connect, with "invalid argument". ipc.fb's CheckName, stdlib.md, and decision 7 all promise names up to 64 bytes. Linux's /tmp leaves room, but Linux fails the same way under a long TMPDIR. Shown with rev-5's net.cpp on Linux with TMPDIR set to a macOS-shaped 48-byte folder (harness scenario long-name): the 64-byte name gives EINVAL, error kind 6 (OTHER). The same program prints True 64 on Windows.

```
// An IPC name of 64 bytes, the most IpcListener takes, listens and connects on every OS.
Import std.core
Import std.net

Const name = "a123456789b123456789c123456789d123456789e123456789f123456789wxyz"
Try
    Local listener = IpcListener.Listen(name)
    Local client = IpcStream.Connect(name)
    Print(listener.Accept().Name == name, name.Bytes.Count)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: True 64

Actual: On macOS (TMPDIR /var/folders/xx/<30 chars>/T/): failed: listening at a123...wxyz failed: invalid argument. Linux harness with TMPDIR=/tmp/folders/3l/l8sbp0d93j54r2fqq2xc0rf40000gn/T (48 bytes): "listen at a 64-byte name: EINVAL, kind 6".

Suggested fix: Put the sockets where any 64-byte name fits: /tmp on POSIX (5 + 64 + 5 = 74 bytes, under 104), or fall back to /tmp when TMPDIR's path wouldn't fit. Add a test with a 64-byte name, which CI's macOS runner would then check.

Skeptic: The Linux harness's long-name scenario, with TMPDIR=/tmp/folders/3l/l8sbp0d93j54r2fqq2xc0rf40000gn/T/ (macOS's shape: libuv drops the trailing slash, leaving 48 bytes), gave 'listen at a 64-byte name: EINVAL, kind 6' (OTHER). 50- and 51-byte names listened and connected. With TMPDIR unset (/tmp), all three worked. pipe_path puts the socket at uv_os_tmpdir() + '/' + name + '.sock' (net.cpp:372). uv_os_tmpdir reads TMPDIR first, and uv_pipe_bind2 and uv_pipe_connect2 with UV_PIPE_NO_TRUNCATE return EINVAL when namelen > sizeof(sun_path): 108 on Linux, 104 on macOS. Under macOS's default $TMPDIR the path is 54 + N bytes, so any name over 50 bytes fails. stdlib.md and ipc.fb's CheckName promise 1 to 64.

## streams-2 (medium): A blocking Read with a zero (or negative) ReadTimeout never starts reading, and stalls for good after 64 KiB

Status: fixed in 134219b2, 72f15bd2, and 0a77e3cf

Where: runtime/src/net.cpp:1173

fb_io_net_read posts read_more to the I/O thread only when `timeout != 0`. A zero ReadTimeout should poll: NanosecondsOf says 0 "doesn't wait". But if nothing has started the libuv read, a zero-timeout Read never starts one, so it throws a TimeoutError on every call, even with bytes waiting in the OS. Once some earlier Read has started reading, on_read stops at read_ahead (64 KiB), and only a Read that finds the buffer empty and posts read_more starts it again. A zero-timeout Read never does that, so polling gets the first 64 KiB and then nothing more. TcpStream and IpcStream both have this. ReadAsync doesn't, because its Await posts read_more, and UdpSocket.Receive(Zero) doesn't either, because wait_blocking always posts receive_more. Shown on Windows: the program below prints -1 after 100 polls over a second. A second program (rv-net/streams/poll2.fb) does one blocking Read, then has 200000 more bytes written and polls with a zero timeout: it ends at 65537 of 200001 bytes.

```
Import std.core
Import std.ffi
Import std.io
Import std.net
Import std.time

// A game polls its connection each frame with a zero ReadTimeout.
Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    server.WriteText("hi")
    client.ReadTimeout = Duration.Zero()
    Local got: Int64 = -1
    For frame = 1 To 100
        Try
            got = client.Read(Buffer<UInt8>(16))
            Break
        Catch e As IOError
            Sleep(10.Milliseconds)
        End Try
    End For
    Print(got)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 2

Actual: -1 (every Read throws an IOError whose Cause is a TimeoutError; the same with IpcStream)

Suggested fix: When the stream isn't readable, post read_more even when timeout is 0, and don't wait for it: run(read_more), then take(). Add a zero-ReadTimeout poll to net_tcp.fb, including more than 64 KiB, and to net_ipc.fb.

Skeptic: On Windows, s2.fb (the finding's program) prints -1 in debug and release: all 100 zero-timeout Reads threw, with "hi" waiting in the OS. poll2.fb (one blocking Read, then 200000 bytes, then zero-timeout polls) ends at 65537. The code agrees. fb_io_net_read posts read_more only when `!readable(socket) && timeout != 0`. Nothing else starts a stream's read (finish_connect and on_connection don't). on_read stops once 64 KiB are buffered, and only a Read with a non-zero timeout restarts it. So a zero ReadTimeout, which NanosecondsOf documents as 'doesn't wait', never gets bytes, or stalls after 64 KiB. IpcStream uses the same StreamSocket.Read path.

## streams-3 (medium): TcpListener.Listen with a backlog of 0 or less aborts the process on Windows (libuv assert)

Status: fixed in 19950238 and 0a77e3cf

Where: std/net/tcp.fb:160

Listen passes `backlog` straight to uv_listen. On Windows, libuv's uv__tcp_listen runs `assert(backlog > 0)` (src/win/tcp.c:624), so Listen(address, 0) or Listen(address, -1) prints "Assertion failed: backlog > 0" and exits with 0xC0000409, in debug and release builds alike. Linux and macOS give the value to listen(), which clamps it, and IpcListener's libuv code clamps it on both OSes. So the same call works there and crashes on Windows. std's conventions make a bad size a panic at the caller.

```
Import std.core
Import std.net

// A backlog of 0: a bug, which std's conventions panic for at the caller, or a value the OS
// takes; not a crash inside libuv.
Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0), 0)
    Print("listening")
Catch e As NetError
    Print(e.Kind, e.Message)
End Try
```

Expected: A panic at the caller's line for a backlog below 1, or "listening" with the value clamped, as on Linux and macOS.

Actual: Assertion failed: backlog > 0, file .../_deps/libuv-src/src/win/tcp.c, line 624
error: backlog.exe exited with code 0xC0000409   (the same for -1, and with --release)

Suggested fix: Make TcpListener.Listen (both overloads) @PanicsAtCaller and panic for a backlog below 1, or clamp it as libuv's Unix pipe code does (0 becomes 1, a negative value becomes SOMAXCONN). Do the same for IpcListener so the two agree, and add a panic_ test.

Skeptic: On Windows, TcpListener.Listen(address, 0) (s3.fb) prints "Assertion failed: backlog > 0, file .../libuv-src/src/win/tcp.c, line 624" and exits with 0xC0000409, in debug and in --release. Listen(address, -1) (s3b.fb) does the same. tcp.fb passes backlog straight to fb_io_net_listen, then to uv_listen, and libuv's win/tcp.c:624 has assert(backlog > 0). The Unix side hands the value to listen(), and libuv's unix/pipe.c:163-166 clamps 0 and negatives. So one call crashes inside libuv on one OS and works on the others, where std's conventions call for a panic at the caller for a bad size.

## streams-4 (medium): On Linux and macOS, a failed IpcListener.Listen at a live name gives that listener a phantom connection

Status: fixed in aad3d823, 72f15bd2, and 0a77e3cf

Where: runtime/src/net.cpp:890

bind_pipe handles UV_EADDRINUSE by calling left_behind(path), which connect()s to the socket file to see whether anything listens there. When a live listener has the name, that probe connects: an AF_UNIX connect completes once it's in the backlog. The probe is then closed. The live listener's on_connection accepts it and queues it, so its next Accept returns an IpcStream that reads its end at once, ahead of the real client. This affects the usual single-instance check, a second program trying Listen and connecting when that fails, and it affects net_ipc.fb's own "Something listens at the name already" step. That test doesn't notice because it never accepts on that listener again. Windows doesn't have this. I showed it with a libuv mirror of bind_pipe and left_behind (rv-net/streams/posix/phantom.c, in WSL): "second bind: EADDRINUSE / left behind: 0 / the first listener accepted a connection / connections the first listener has: 1". On Windows, the program below prints the expected lines.

```
Import std.core
Import std.ffi
Import std.io
Import std.net

// A second Listen at a name a live listener has, as a program checking whether another instance
// runs does, then a real connection.
Try
    Local listener = IpcListener.Listen("fb-review-phantom")
    Try
        IpcListener.Listen("fb-review-phantom")
    Catch e As NetError
        Print("taken")
    End Try
    Local client = IpcStream.Connect("fb-review-phantom")
    client.WriteText("real")
    Local server = listener.Accept()
    Print(server.Read(Buffer<UInt8>(4)))
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: taken
4   (what Windows prints)

Actual: On Linux and macOS: taken, then 0. The first Accept gives the probe's connection, already closed, not the real client.

Suggested fix: Find out whether a listener is alive without connecting to it. For example, the listener holds an flock on a <name>.lock file next to the socket, and Listen takes the socket file over only when it can get that lock. Add the program above to net_ipc.fb, with an Accept after the failed Listen.

Skeptic: My WSL mirror of bind_pipe and left_behind (rv-net/streams-v/posix/phantom.c) uses the _deps libuv. Listener A binds and listens at a path. A second uv_pipe_bind2 there gets EADDRINUSE, and left_behind's connect() probe connects and then closes. A real client then connects and writes "real". It printed: "second bind: EADDRINUSE / left behind: 0 / A accepted connection 1 / real client connected: 0 / A accepted connection 2 / connection 1 read EOF / connection 2 read 4 bytes: real". In net.cpp, the probe runs on the same I/O loop as the live listener, so its on_connection queues the probe first, and the next Accept returns an IpcStream that reads 0 at once. The finding's program would print taken, then 0, on Linux, and the same on macOS by the same code.

## streams-5 (medium): IPC names of up to 64 bytes are promised, but on macOS (and on Linux with a long TMPDIR) names over about 50 bytes fail

Status: fixed in 72f15bd2 and 0a77e3cf

Where: std/net/ipc.fb:18

CheckName and docs/stdlib.md allow 1 to 64 bytes. pipe_path builds uv_os_tmpdir() + "/" + name + ".sock", and uv_pipe_bind2 and uv_pipe_connect2 with UV_PIPE_NO_TRUNCATE return UV_EINVAL when that's longer than sun_path. macOS's sun_path is 104 bytes, and its default TMPDIR (/var/folders/xx/<30 chars>/T) is 48, so the path is 54 + n bytes: any name over 50 bytes throws a NetError of OTHER ("invalid argument") from Listen and from Connect. Linux's sun_path is 108, so a TMPDIR of 39 bytes or more breaks 64-byte names there. Shown with a libuv mirror of pipe_path and fb_io_net_listen_pipe in WSL (rv-net/streams/posix/longname.c): with TMPDIR unset, "path 74 bytes ... bind: ok"; with a 51-byte TMPDIR, "path 121 bytes, sun_path 108 bytes, bind: EINVAL". On Windows the program below prints 64.

```
Import std.core
Import std.net

// A 64-byte name, the longest CheckName allows.
Try
    Local listener = IpcListener.Listen("fb-review-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    Local client = IpcStream.Connect(listener.Name)
    Print(listener.Accept().Name.Bytes.Count)
Catch e As NetError
    Print(e.Kind, e.Message)
Catch e As TimeoutError
    Print("timeout")
End Try
```

Expected: 64   (what Windows and Linux with /tmp print)

Actual: On macOS with its default TMPDIR: OTHER listening at fb-review-aaaa... failed: invalid argument

Suggested fix: Make every allowed name fit: on POSIX, use a short folder when TMPDIR would make the path longer than sun_path (for example /tmp, which is what macOS's own tools use), or lower LONGEST to what fits under every folder. Either way, check the whole path's length and report it clearly. Add a 64-byte name to net_ipc.fb.

Skeptic: pipe_path is uv_os_tmpdir() + '/' + name + '.sock'. uv_pipe_bind2 and uv_pipe_connect2 with UV_PIPE_NO_TRUNCATE return UV_EINVAL when namelen > sizeof(sun_path) (unix/pipe.c). My WSL mirror (rv-net/streams-v/posix/longname.c) binds names of 40 to 64 bytes. With TMPDIR unset, all of them bind (64 gives a 74-byte path). With a TMPDIR shaped like macOS's (/tmp/rv/folders/2x/<30 chars>/T/, 51 bytes), 48 binds, and 52 through 64 give EINVAL ("path 109 bytes: EINVAL" ... "121 bytes: EINVAL"). macOS has a 104-byte sun_path and a 48-byte default TMPDIR, so names over 50 bytes fail there. That contradicts stdlib.md's and CheckName's 1-to-64-byte names, and the failure surfaces as NetError OTHER "invalid argument".

## lan-2 (low): Stop says goodbye to the host's A records and the type's enumeration PTR, which other announcers still have

Status: fixed in 1de61f1d and 0a77e3cf

Where: std/net/lan.fb:257

Records(True) sends TTL-0 copies of every record: the instance's PTR, SRV, and TXT, but also _services._dns-sd._udp.local PTR <type> and <host>.local's A records. Those last two are shared. Another LanAnnouncer of the same type, in this program or another one on the machine, still offers them, and the OS's own responder (Windows' DNS Client, Avahi, mDNSResponder) answers for <hostname>.local too. So stopping one service makes every mDNS cache on the LAN forget the machine's addresses and the type's enumeration entry while other services still need them. RFC 6762 10.1 sends a goodbye only for records that are about to stop being valid. Bonjour skips the goodbye for a record that is still registered elsewhere.

```
Import std.core
Import std.collections
Import std.net
Import std.time

// A dotted name's bytes as DNS writes them, uncompressed.
Function Wire(dotted: String): UInt8[]
    Local bytes: UInt8[] = []
    Local label: UInt8[] = []
    For byte In dotted.Bytes
        If byte == 46
            bytes.Add(label.Count As UInt8)
            bytes.AddAll(label)
            label = []
        Else
            label.Add(byte)
        End If
    End For
    bytes.Add(label.Count As UInt8)
    bytes.AddAll(label)
    bytes.Add(0)
    Return bytes
End Function

Function Short(packet: UInt8[], at: Int): Int => ((packet[at] As Int) << 8) | (packet[at + 1] As Int)

// Where the uncompressed name at `at` ends.
Function NameEnd(packet: UInt8[], at: Int): Int
    Local here = at
    While packet[here] != 0
        here += 1 + (packet[here] As Int)
    End While
    Return here + 1
End Function

// Which of `names` the name at `at` is.
Function Which(packet: UInt8[], at: Int, names: String[]): String
    Local finish = NameEnd(packet, at)
    For name In names
        If packet[at..finish] == Wire(name) Then Return name
    End For
    Return "?"
End Function

Const port: UInt16 = 55363
Try
    Local first = LanAnnouncer.Start("_fbbye._udp", "First", 4242, Null, port)
    Local second = LanAnnouncer.Start("_fbbye._udp", "Second", 4343, Null, port)
    Local names = ["_services._dns-sd._udp.local", "_fbbye._udp.local", "Second._fbbye._udp.local",
                   first.Service.Host]
    // Listens on the discovery port, as another machine's mDNS cache would.
    Local cache = UdpSocket.Bind(SocketAddress(IpAddress.Any, port), True)
    cache.JoinMulticast(IpAddress.V4(224, 0, 0, 251), first.Service.Addresses[0])
    Await Timer.After(1500.Milliseconds)
    second.Stop()
    Local done = False
    While !done
        Local packet = (Try Await cache.ReceiveAsync(1.Seconds)).Bytes
        Local count = Short(packet, 6) + Short(packet, 8) + Short(packet, 10)
        Local at = 12
        For i = 1 To count
            Local finish = NameEnd(packet, at)
            Local ttl = (Short(packet, finish + 4) << 16) | Short(packet, finish + 6)
            If ttl != 0 Then Break
            done = True
            Local type = Short(packet, finish)
            Local target = type == 12 ? " -> " + Which(packet, finish + 10, names) : ""
            Print("{Which(packet, at, names)} type {type} ttl 0{target}")
            at = finish + 10 + Short(packet, finish + 8)
        End For
    End While
    Print("first still offered:", !first.IsStopped)
    first.Stop()
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: _fbbye._udp.local type 12 ttl 0 -> Second._fbbye._udp.local
Second._fbbye._udp.local type 33 ttl 0
Second._fbbye._udp.local type 16 ttl 0
first still offered: True
(no goodbye for the enumeration PTR or for hlpdev-pc.local's A records, which First still answers for)

Actual: _fbbye._udp.local type 12 ttl 0 -> Second._fbbye._udp.local
_services._dns-sd._udp.local type 12 ttl 0 -> _fbbye._udp.local
Second._fbbye._udp.local type 33 ttl 0
Second._fbbye._udp.local type 16 ttl 0
hlpdev-pc.local type 1 ttl 0
hlpdev-pc.local type 1 ttl 0
hlpdev-pc.local type 1 ttl 0
first still offered: True

Suggested fix: Make a goodbye cover only the instance's own records: the type's PTR to it, its SRV, and its TXT. Leave out the enumeration PTR and the host's A records, or send those only when no other announcer in the process still offers them, for example through a process-wide count of live announcers per type and per host.

Skeptic: Ran lan2.fb: after second.Stop() a socket on the discovery port, joined to the group, got TTL-0 records for '_fbbye._udp.local -> Second', '_services._dns-sd._udp.local type 12 ttl 0 -> _fbbye._udp.local', Second's SRV and TXT, and three 'hlpdev-pc.local type 1 ttl 0' records, while 'first still offered: True'. Code: Records(True) (lan.fb:257) puts the enumeration PTR and Extra's AddressRecords into the goodbye, and the A goodbyes carry the cache-flush bit (Record(..., unique=True)). RFC 6762 10.1 sends goodbyes only for records that are about to become invalid, and both of these are still offered by First (and the host name by the OS's responder). Severity is low: std's own Find ignores goodbyes, and other caches re-query the A records on demand (First answers them). The type stays gone from a DNS-SD type enumeration until that browser asks again.

## lan-3 (low): A text too big for one datagram gets past Start's checks, and the service is then silently never announced or found

Status: fixed in 5e2d7d08 and 0a77e3cf

Where: std/net/lan.fb:711

TextData checks each pair against 255 bytes but never checks the total. Record writes the TXT rdlength as data.Count As UInt16, which wraps above 65535, and the response is bigger than a UDP datagram can be. Every Send in Announce, in Answer, and in Stop's goodbye then fails, and the NetError is swallowed. Start returns normally and nothing can find the service. RFC 6762 17 also caps an mDNS packet at 9000 bytes, which Bonjour holds to.

```
Import std.core
Import std.collections
Import std.net
Import std.time

Const port: UInt16 = 55373
Try
    // 300 pairs of 254 bytes each, every one within the 255 that Start checks.
    Local value = ""
    For i = 1 To 250
        value = value + "v"
    End For
    Local text = Map<String, String>()
    For i = 100 To 399
        text["{i}"] = value
    End For
    Local announcer = LanAnnouncer.Start("_fbbig._udp", "Game", 4242, text, port)
    Print("started")
    Print((Await Lan.FindAsync("_fbbig._udp", 1500.Milliseconds, port)).Count)
    announcer.Stop()
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: A panic at Start's caller saying the text is too big for an mDNS packet (as for a pair over 255 bytes). Otherwise "started" then 1.

Actual: started
0

Suggested fix: In TextData, panic when the TXT data's total goes past what fits in one mDNS packet with the service's other records (about 8900 bytes, RFC 6763 6.2 / RFC 6762 17). Document the limit beside the 255-byte one in Start's comment and in stdlib.md.

Skeptic: Ran lan3.fb (300 pairs of 254 bytes, about 76 KB of TXT data), debug and --release: 'started' then '0'. No panic, and the service is never found. TextData (lan.fb:711) checks only each pair's 255 bytes. Record writes data.Count As UInt16, which wraps, and the datagram is over UDP's limit, so every Send fails, and Announce's and Answer's catches swallow it. Start's comment says a text that isn't one panics, but this one starts and is silently never announced. A control, lan3b.fb at about 10 KB, is found (1), so only the 9000-byte cap the reviewer proposes is a judgment call. The wrap and the silent failure are not.

## lan-4 (low): The announcer answers queries that already list its answer (no known-answer suppression)

Status: fixed in 1de61f1d and 0a77e3cf

Where: std/net/lan.fb:268

ReadMessage reads a query's answer section, but Offered.Answer never looks at it. RFC 6762 7.1 says a responder MUST NOT answer when the query's Answer Section already holds the record with at least half its TTL. Bonjour and Avahi browsers keep re-querying with their known answers, and each time this announcer multicasts its PTR, SRV, TXT, and A records through every interface.

```
Import std.core
Import std.collections
Import std.net
Import std.time

Function Wire(labels: String[]): UInt8[]
    Local bytes: UInt8[] = []
    For label In labels
        bytes.Add(label.Bytes.Count As UInt8)
        bytes.AddAll(label.Bytes)
    End For
    bytes.Add(0)
    Return bytes
End Function

Const port: UInt16 = 55371
Try
    Local announcer = LanAnnouncer.Start("_fbka._udp", "Game", 4242, Null, port)
    // A querier on the discovery port, as Bonjour's browsers are on 5353.
    Local querier = UdpSocket.Bind(SocketAddress(IpAddress.Any, port), True)
    Local through = announcer.Service.Addresses[0]
    querier.JoinMulticast(IpAddress.V4(224, 0, 0, 251), through)
    querier.SetMulticastInterface(through)
    Await Timer.After(1500.Milliseconds)
    // Lets the announcements go by.
    Loop
        If Try? Await querier.ReceiveAsync(300.Milliseconds) == Null Then Break
    End Loop

    // A query for the type's PTRs that already knows the service's, with its whole TTL.
    Local query: UInt8[] = [0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0]
    query.AddAll(Wire(["_fbka", "_udp", "local"]))
    Local question: UInt8[] = [0, 12, 0, 1]
    query.AddAll(question)
    query.AddAll(Wire(["_fbka", "_udp", "local"]))
    Local target = Wire(["Game", "_fbka", "_udp", "local"])
    Local known: UInt8[] = [0, 12, 0, 1, 0, 0, 17, 148, 0, target.Count As UInt8]
    query.AddAll(known)
    query.AddAll(target)
    querier.Send(query, SocketAddress(IpAddress.V4(224, 0, 0, 251), port))

    Local answered = False
    Loop
        Local datagram = Try? Await querier.ReceiveAsync(1.Seconds)
        If datagram == Null Then Break
        If (datagram.Bytes[2] & 128) != 0 Then answered = True
    End Loop
    Print("answered a query that knew the answer:", answered)
    announcer.Stop()
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: answered a query that knew the answer: False

Actual: answered a query that knew the answer: True (as a control, the same program asking for _fbzz._udp prints False, so the True is this query's answer)

Suggested fix: In Offered.Answer, drop each answer that the query's records already hold, with the same name, type, and data and a TTL of at least half of ours. Return Null when nothing is left to answer, and include the additional records only for answers that remain.

Skeptic: Ran lan4.fb: a PTR query for _fbka._udp.local whose answer section already holds Game._fbka._udp.local's PTR with TTL 4500 still gets a response ('answered a query that knew the answer: True'). Control lan4c.fb, the same query for _fbzz._udp, prints False, so the True is this query's answer. Code: Offered.Answer (lan.fb:268) never looks at query.Records. RFC 6762 7.1 says a responder MUST NOT answer when the answer is already listed with at least half its TTL. Only extra multicast traffic results, so low.

## lan-5 (low): TXT keys are case-sensitive, so keys that differ only by case both survive and lookups miss

Status: fixed in 1de61f1d and 0a77e3cf

Where: std/net/lan.fb:692

RFC 6763 6.4 says case is ignored when reading a key: 'papersize', 'PAPERSIZE', and 'Papersize' are the same key, and a client MUST ignore every occurrence after the first. TextOf keeps keys exactly as they come and drops only exact duplicates. TextData accepts two keys that differ only by case. So a Find gives both, and service.Text lookups depend on the case another responder used (for example a Bonjour printer's 'PaperSize').

```
Import std.core
Import std.collections
Import std.net
Import std.time

Const port: UInt16 = 55375
Try
    Local text = Map<String, String>()
    text["Map"] = "dust"
    text["map"] = "fields"
    Local announcer = LanAnnouncer.Start("_fbkey._udp", "Game", 4242, text, port)
    For service In Await Lan.FindAsync("_fbkey._udp", 1500.Milliseconds, port)
        Print(service.Text.Count, service.Text.Get("map") ?? "none", service.Text.Get("MAP") ?? "none")
    End For
    announcer.Stop()
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: A panic at Start for two keys that are the same key under DNS-SD's rule. Otherwise one key kept (the first), found under either case, e.g. "1 dust dust".

Actual: 2 fields none

Suggested fix: In TextData, panic on a key equal to an earlier one ignoring ASCII case. In TextOf, skip a key whose lowercase form was already seen. Either store keys lowercased and document that, or document that Text's keys keep the sender's case.

Skeptic: Ran lan5.fb (text 'Map'='dust' then 'map'='fields') in debug and release: '2 fields none dust'. Both keys survive the round trip, Get("map") gives the second pair's value, and Get("MAP") gives nothing. Code: TextOf (lan.fb:692) dedupes with case-sensitive text.Contains(key), and TextData accepts both keys. RFC 6763 6.4 ignores case in keys and says a client MUST ignore every occurrence of a key after the first. stdlib.md says this is DNS-SD's per RFC 6763, so a Find should keep one key, the first ('dust').

## lan-6 (low): The goodbye is never tested: Find can't hear one, though net_lan.fb's header and Stop's comment say it has the service forgotten

Status: fixed in 1de61f1d

Where: tests/programs/net_lan.fb:79

Find binds an ephemeral port and never joins the group, so the multicast goodbye Stop sends can't reach it. The test's 'A goodbye' section stops Second and only then starts a new Find. That passes whether or not Stop sends a goodbye, and whatever records or TTLs the goodbye holds. The file header says 'a goodbye has them forgotten', and Stop's comment says 'so what found the service forgets it'. No std code forgets anything, and a Find that is listening while the goodbye goes out still returns the service.

```
Import std.core
Import std.net
Import std.time

Const port: UInt16 = 55372
Try
    Local announcer = LanAnnouncer.Start("_fbgb._udp", "Game", 4242, Null, port)
    Local finding = Lan.FindAsync("_fbgb._udp", 1500.Milliseconds, port)
    Await Timer.After(700.Milliseconds)
    // Says goodbye while the Find still listens.
    announcer.Stop()
    Local found = Await finding
    Print(found.Count, announcer.IsStopped)
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: The test checks a goodbye. For example, a socket on the discovery port, joined to the group, sees Second's PTR, SRV, and TXT with TTL 0 after Stop. Otherwise the comments don't claim that something forgets the service.

Actual: 1 True (the goodbye isn't heard by the Find that is running; the test's later Find passes with or without a goodbye)

Suggested fix: In net_lan.fb, listen on the discovery port (UdpSocket.Bind(..., True) plus JoinMulticast) and check that Stop's packet holds the instance's records with TTL 0. Reword Stop's comment and the test header to say other mDNS caches drop the service.

Skeptic: Ran lan6.fb: a FindAsync that is listening while Stop's goodbye goes out still returns the service ('1 True'). Find binds an ephemeral port and never joins 224.0.0.251, so no goodbye can reach it. net_lan.fb's goodbye section stops Second and only then starts a new Find, which passes whether or not Stop sends anything. lan-2 shows the goodbye's contents are wrong, and no test caught it. The test header's 'a goodbye has them forgotten' claims coverage the test doesn't have. Stop's own comment is fair for other mDNS caches, so the fix is a goodbye check on a group-joined discovery-port socket plus rewording the header.

## lan-7 (low): Start's name and text panics have no tests

Status: fixed in ae4ad80c

Where: std/net/lan.fb:759

InstanceLabel panics for a name of 0 or more than 63 bytes. TextData panics for an empty key, a key with '=', and a pair over 255 bytes. These are @PanicsAtCaller messages that reach users, but tests/programs has only panic_net_lan_type.fb, which covers the type. The project's rule is that each panic has a test.

```
Import std.core
Import std.collections
Import std.net

// A key, '=', and a value of 256 bytes, one past what a TXT string holds.
Local value = ""
For i = 1 To 254
    value = value + "v"
End For
Local text = Map<String, String>()
text["k"] = value
Try
    Local announcer = LanAnnouncer.Start("_fbpanic._udp", "Game", 4242, text, 55381)
    Print("started")
Catch e As NetError
    Print("failed: {e.Message}")
End Try
```

Expected: A panic_net_lan_*.fb test with its .out for each of these panics.

Actual: panic: a LanService's text has a key and its value of 255 bytes at most, not 256 at lan7.fb:13:23 (correct, but no test covers it; likewise "a LanService's name has 1 to 63 bytes, not 0" and "...keys of a byte at least, without '=', not \"a=b\"")

Suggested fix: Add tests/programs/panic_net_lan_name.fb, panic_net_lan_key.fb, and panic_net_lan_text.fb, each with its .out, for the name length, the key ('=' or empty), and the 256-byte pair.

Skeptic: Ran lan7.fb, lan7b.fb, and lan7c.fb: each exits 101 with the right @PanicsAtCaller message ('...255 bytes at most, not 256 at lan7.fb:13:23', 'a LanService's name has 1 to 63 bytes, not 0 at lan7b.fb:6:23', '...without '=', not "a=b" at lan7c.fb:8:23'). tests/programs has only panic_net_lan_type for LAN discovery. The same step's IPC name panics have panic_net_ipc_name and panic_net_ipc_empty, and docs/history.md shows earlier steps adding 'a panic program for each panic' (including as a review fix, Buffer's panics). The behavior is correct; the gap is a missing test for each of the three panics.

## runtime-7 (low): A Write after the stream's own ShutdownWrite throws RESET ("broken pipe") rather than CLOSED

Status: fixed in cdef6d6a, 134219b2, and 0a77e3cf

Where: runtime/src/net.cpp:1420

After uv_shutdown, libuv clears the stream's WRITABLE flag, and uv_write then returns UV_EPIPE synchronously, on Windows and Unix alike. fb_io_net_error_kind maps UV_EPIPE to RESET. tcp.fb documents RESET as "a connection the other side ended badly", and stdlib.md gives CLOSED for a socket that's closed. So writing after your own ShutdownWrite, a mistake on this side, reads as the peer resetting. A second ShutdownWrite gives CLOSED (UV_ENOTCONN), so the two calls disagree.

```
// A Write after this side's own ShutdownWrite: what's ended here is CLOSED, not RESET, which is
// for a connection the other side ended badly.
Import std.core
Import std.io
Import std.net

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    client.ShutdownWrite()
    Try
        client.WriteText("more")
    Catch e As IOError
        Print((e.Cause As NetError).Kind, e.Message)
    End Try
    Try
        client.ShutdownWrite()
    Catch e As IOError
        Print((e.Cause As NetError).Kind, e.Message)
    End Try
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: CLOSED writing to 127.0.0.1:<port> failed: ...
CLOSED ending what's written to 127.0.0.1:<port> failed: socket is not connected

Actual: RESET writing to 127.0.0.1:57815 failed: broken pipe
CLOSED ending what's written to 127.0.0.1:57815 failed: socket is not connected

Suggested fix: Remember on the socket that this side shut down writing (set in start_shutdown), and have writable() give UV_ENOTCONN, which is CLOSED, for a write after it. Leave EPIPE as RESET for a real reset by the other side, and test both.

Skeptic: rt-refute/shutwrite.fb in debug and --release on Windows printed 'RESET writing to 127.0.0.1:51595 failed: broken pipe', then 'CLOSED ending what's written to 127.0.0.1:51595 failed: socket is not connected'. The Linux harness's shutdown-write scenario gave the same: 'write after: EPIPE, kind 1 / shutdown again: ENOTCONN, kind 4'. After uv_shutdown, uv_write returns UV_EPIPE synchronously, and fb_io_net_error_kind maps EPIPE to RESET (net.cpp:1420). tcp.fb defines RESET as 'a connection the other side ended badly', but here the other side did nothing, and the two calls disagree about the same state.

## runtime-8 (low): The Unix-socket takeover runs in a test only if panic_net_ipc_name.fb happened to run earlier on that machine

Status: fixed in 72f15bd2

Where: tests/programs/net_ipc.fb:26

Decision 7 and stdlib.md say a Unix socket file that nothing listens at is taken over. The only test of that is net_ipc.fb's first section, which relies on panic_net_ipc_name.fb having left a stale fb-test-net-left-behind.sock behind. program_tests.cpp runs the programs in directory_iterator order, which promises no order. If net_ipc.fb runs first, as on a fresh CI runner whose directory lists it first, there is no stale file. The section then prints the same line through an ordinary Listen, so a broken takeover passes. On Windows, which has no takeover, it always passes. The code itself works: the Linux harness's stale scenario, which binds and closes a Unix socket without unlinking it before fb_io_net_listen_pipe, gives ok.

```
// net_ipc.fb's first section, on a machine where no program left fb-test-net-left-behind.sock
// behind: it prints the same as when it takes a left-behind file over.
Import std.core
Import std.net

Try
    Local taken = IpcListener.Listen("fb-test-net-left-behind")
    Local connected = IpcStream.Connect(taken.Name)
    Print(taken.Accept().Name)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: A test that fails when a stale Unix socket file isn't taken over, whatever order the tests run in

Actual: fb-test-net-left-behind, whether or not a stale file existed, so the check passes without any takeover (always on Windows, and on POSIX whenever net_ipc.fb runs before panic_net_ipc_name.fb)

Suggested fix: Make the stale file in the test itself before listening. For example, add a POSIX runtime test in tests/runtime that binds a Unix socket at <tmp>/<name>.sock, closes it without unlinking, and expects fb_io_net_listen_pipe to give 0 and a connect to work. Or start the panicking program from net_ipc.fb first, if std can run a process.

Skeptic: By reading: net_ipc.fb's first section passes only if Listen('fb-test-net-left-behind') works. That holds through an ordinary bind when no stale file exists, and its own comment says the takeover happens 'when that program ran first'. A panic leaves the file because end_program() calls run_end and _Exit, not run_finish, so close_all never runs. program_tests.cpp runs the programs in std::filesystem::directory_iterator order, which is unspecified. On NTFS, ls -U puts net_ipc.fb at 359 and panic_net_ipc_name.fb at 1171, so net_ipc runs first, though Windows has no takeover anyway. On WSL's tmpfs the order was reversed (9 vs 52), and ext4 and APFS use hash order. So on a fresh POSIX runner the takeover code may never run in a test, and a broken takeover would still pass. The code itself works: the harness's stale scenario printed 'stale file there: yes / listen over it: ok / connect: ok'.

## streams-10 (low): net_udp.fb never sends a broadcast or tests the datagram size limit, and udp.fb's LARGEST is unused

Status: fixed in e2b6732f and 0a77e3cf

Where: tests/programs/net_udp.fb:55

The review covers broadcast and datagram sizes, but net_udp.fb only sets SetBroadcast, SetMulticastLoopback, and SetMulticastTtl ("Broadcast and multicast are options it sets"). It never sends to a broadcast address, and never checks that a Send without SetBroadcast fails. Nothing tests the 65507-byte limit Send's comment gives, or what larger sizes do. udp.fb declares `Const LARGEST = 65507` and never uses it, while the runtime's own check is 65536 and the OS decides the rest. The program below shows the behaviour such a test would pin down on Windows, where all of it works.

```
Import std.core
Import std.net
Import std.time

Function Bytes(count: Int): UInt8[]
    Local bytes: UInt8[] = []
    For i = 1 To count
        bytes.Add((i % 256) As UInt8)
    End For
    Return bytes
End Function

Try
    Local receiver = UdpSocket.Bind(SocketAddress(IpAddress.V4(0, 0, 0, 0), 0), True)
    Local port = receiver.LocalAddress.Port
    Local sender = UdpSocket.Bind(SocketAddress(IpAddress.V4(0, 0, 0, 0), 0))
    Try
        sender.Send([1], SocketAddress(IpAddress.V4(255, 255, 255, 255), port))
    Catch e As NetError
        Print("without SetBroadcast:", e.Kind)
    End Try
    sender.SetBroadcast(True)
    sender.Send([2], SocketAddress(IpAddress.V4(255, 255, 255, 255), port))
    Print("broadcast received:", receiver.Receive(1.Seconds).Bytes)

    Local a = UdpSocket.Bind(SocketAddress(IpAddress.Loopback, 0))
    Local b = UdpSocket.Bind(SocketAddress(IpAddress.Loopback, 0))
    a.Send(Bytes(65507), b.LocalAddress)
    Print(b.Receive(1.Seconds).Bytes.Count)
    Try
        a.Send(Bytes(65508), b.LocalAddress)
    Catch e As NetError
        Print("65508:", e.Kind)
    End Try
Catch e As NetError
    Print(e.Kind, e.Message)
Catch e As TimeoutError
    Print("timeout")
End Try
```

Expected: without SetBroadcast: OTHER
broadcast received: [2]
65507
65508: OTHER

Actual: Windows prints exactly that, but net_udp.fb checks none of it, and LARGEST is dead code.

Suggested fix: Add an actual broadcast round trip, and the 65507 and 65508 sizes, to net_udp.fb. Either use LARGEST, for example to give a clear error for an oversized IPv4 Send, or delete it.

Skeptic: udp.fb:17 declares `Const LARGEST = 65507`, and grep finds no other use in std/net, so it's dead code. net_udp.fb only calls SetBroadcast(True), SetMulticastLoopback, and SetMulticastTtl(1). It never sends to a broadcast address, never checks that a Send fails without SetBroadcast, and never checks the 65507-byte limit Send's comment promises. On Windows the finding's program (s10.fb) prints "without SetBroadcast: OTHER / broadcast received: [2] / 65507 / 65508: OTHER", so the behaviour works there but nothing tests it. net_udp.fb's own comment says macOS sends only 9216 bytes at once unless told otherwise. So Send's 'up to 65507 bytes' is probably not true on macOS either, which a size test would have caught.

## streams-6 (low): IpcListener.Listen on POSIX deletes any non-socket file at $TMPDIR/<name>.sock

Status: fixed in a3d16880, 72f15bd2, and 0a77e3cf

Where: runtime/src/net.cpp:891

left_behind takes ECONNREFUSED from connect() to mean a Unix socket nothing listens at, and bind_pipe then unlinks the path. Linux's connect() also returns ECONNREFUSED for a regular file, so a user's file named <name>.sock in the temporary folder is deleted, and the listener takes its place. Shown in WSL (rv-net/streams/posix/regular.py: with a regular file at the path, bind gives EADDRINUSE and connect gives ECONNREFUSED, exactly the condition left_behind tests). The program below assumes the file exists first; it can't make the file itself, since std has no file writing yet.

```
Import std.core
Import std.net

// Run on Linux, TMPDIR unset, after: echo notes > /tmp/fb-review-notes.sock
// That file is the user's, not a socket.
Try
    Local listener = IpcListener.Listen("fb-review-notes")
    Print("listening")
Catch e As NetError
    Print(e.Kind, e.Message)
End Try
```

Expected: OTHER listening at fb-review-notes failed: address already in use   (and /tmp/fb-review-notes.sock still holds "notes")

Actual: listening   (and /tmp/fb-review-notes.sock has been unlinked and replaced by the listener's socket)

Suggested fix: In left_behind, lstat the path first and treat it as left behind only when S_ISSOCK holds.

Skeptic: My WSL mirror (rv-net/streams-v/posix/regular.c) writes a regular file, "notes", at /tmp/fb-review-notes-v.sock, then runs bind_pipe's logic. It printed "bind: EADDRINUSE / probe connect: Connection refused / unlinked the file; bind again: ok / path is now a socket", and afterwards the file's contents are gone. left_behind takes any ECONNREFUSED as a stale socket, and Linux's connect() gives ECONNREFUSED for a non-socket file. So a user's regular file is deleted. stdlib.md only promises to take over a Unix socket's file that a program left behind. It needs a non-socket <name>.sock in the temporary folder, so the severity is low.

## streams-7 (low): SetMulticastTtl doesn't check the 1-to-255 range its comment gives, and out-of-range values act differently on each OS

Status: fixed in b9fdf617 and 0a77e3cf

Where: std/net/udp.fb:137

The comment says hops go "from 1 ... to 255", but the value goes straight to libuv. On Windows, libuv accepts -1 to 255 (VALIDATE_MULTICAST_TTL, src/win/udp.c:1005). On Linux and macOS, it accepts 0 to 255 (uv__setsockopt_maybe_char, src/unix/udp.c:1129). So 0 is set everywhere, -1 is set on Windows but throws a NetError of OTHER elsewhere, and 256 throws a NetError of OTHER. An out-of-range count is a bug, which std's conventions panic for at the caller, and which should behave the same on every OS.

```
Import std.core
Import std.net

Try
    Local socket = UdpSocket.Bind(SocketAddress(IpAddress.Loopback, 0))
    For hops In [1, 255, 0, 256, -1]
        Try
            socket.SetMulticastTtl(hops)
            Print(hops, "set")
        Catch e As NetError
            Print(hops, e.Kind, e.Message)
        End Try
    End For
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: 1 set
255 set
then a panic at the caller for 0 (outside the documented 1 to 255), the same on every OS

Actual: Windows: 1 set / 255 set / 0 set / 256 OTHER setting the multicast TTL to 256 failed: invalid argument / -1 set. Linux and macOS: -1 throws OTHER instead.

Suggested fix: Make SetMulticastTtl @PanicsAtCaller and panic for hops outside 1 to 255, or say 0 to 255 and check that, and add a panic_ test.

Skeptic: On Windows, s7.fb prints "1 set / 255 set / 0 set / 256 OTHER setting the multicast TTL to 256 failed: invalid argument / -1 set". udp.fb's SetMulticastTtl passes hops straight to uv_udp_set_multicast_ttl, though its comment gives the range as 1 to 255. libuv's win/udp.c:1005 accepts -1 to 255, and unix/udp.c:1129 accepts only 0 to 255. So 0, outside the documented range, is set everywhere, and -1 is set on Windows but throws NetError OTHER on Linux and macOS. An out-of-range argument should behave the same on every OS, and std's conventions panic for one at the caller.

## streams-8 (low): IPC names that differ only in case are one name on Windows (and macOS) but two on Linux, and the docs don't say so

Status: fixed in 72f15bd2 and 0a77e3cf

Where: std/net/ipc.fb:155

CheckName allows upper- and lowercase letters, and docs/stdlib.md describes a name as "1 to 64 ASCII letters, digits, '.', '-', and '_'". Windows named pipes compare names without case, and macOS's default APFS file names do too, but Linux's don't. So two listeners at "fb-review-Game" and "fb-review-game" work on Linux and collide on Windows. Shown on Windows: the program below prints the collision.

```
Import std.core
Import std.net

Try
    Local upper = IpcListener.Listen("fb-review-Game")
    Local lower = IpcListener.Listen("fb-review-game")
    Print("two listeners:", upper.Name, lower.Name)
Catch e As NetError
    Print(e.Kind, e.Message)
End Try
```

Expected: The same result on every OS: either two listeners (on Linux today: two listeners: fb-review-Game fb-review-game), or a collision the docs describe.

Actual: Windows: OTHER listening at fb-review-game failed: address already in use. Linux: two listeners.

Suggested fix: Make every OS agree, for example by folding names to lowercase in CheckName's callers or allowing only lowercase letters. At least, say in ipc.fb and stdlib.md that names differing only in case may be the same name.

Skeptic: On Windows, s8.fb prints "OTHER listening at fb-review-game failed: address already in use", after a listener at fb-review-Game. Named pipes ignore case, and so does macOS's default APFS. Linux's ext4 Unix socket paths are case-sensitive, so the same program gets two listeners there. CheckName allows both cases, and neither stdlib.md nor ipc.fb says that names differing only in case may be one name. The same program behaves differently by OS, and nothing documents it.

## streams-9 (low): TcpStream.Close's and IpcStream.Close's comments say a waiting read throws a NetError, but it throws an IOError whose Cause is the NetError

Status: fixed in 87934987

Where: std/net/tcp.fb:138

Both Close comments (tcp.fb:138, ipc.fb:92) say "what's waiting for it throws a NetError of CLOSED, and so does what uses it after". A ReadAsync or WriteAsync that's waiting, or a Read or Write afterwards, throws an IOError, as the class comments and decision 1 say, with the NetError of CLOSED as its Cause. Only Connect, Accept, and the other socket calls throw the NetError itself. Shown on Windows with the program below.

```
Import std.core
Import std.ffi
Import std.io
Import std.net

Async Function ReadOne(stream: TcpStream): String
    Try
        Try Await stream.ReadAsync(Buffer<UInt8>(4))
        Return "read"
    Catch e As IOError
        Return "IOError, Cause {(e.Cause As NetError).Kind}"
    End Try
End Function

Try
    Local listener = TcpListener.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local client = TcpStream.Connect(listener.LocalAddress)
    Local server = listener.Accept()
    Local reading = ReadOne(client)
    Await NextFrame()
    client.Close()
    Print(Await reading)
Catch e As Error
    Print("failed: {e.Message}")
End Try
```

Expected: What the comment describes: a NetError of CLOSED

Actual: IOError, Cause CLOSED

Suggested fix: Change both comments to say a waiting or later Read or Write throws an IOError whose Cause is a NetError of CLOSED.

Skeptic: s9.fb prints "IOError, Cause CLOSED" for a ReadAsync waiting when Close runs, and "after: IOError, Cause CLOSED" for a Read afterwards, in debug and release. StreamSocket.Open and StreamError wrap the NetError in an IOError. TcpStream.Close's comment (tcp.fb:138) and IpcStream.Close's (ipc.fb) say what's waiting, and what uses it after, throws a NetError of CLOSED. That contradicts the code and the class comments. Only the listeners' Close comments are accurate.

## landing-1 (medium): net_connections fails when another run of it is going at the same time on the machine

Status: fixed in 6965c04f

Where: tests/programs/net_connections.fb:75

Found by the landing's verify.py, in the concurrent repeats, not by the review. An IPC name is the machine's: a named pipe on Windows, a Unix socket's file in the temporary folder elsewhere. net_connections listened at the fixed "fb-test-net-connections", so while another verify on the machine ran it too (lanes whose branches stack on this one), one run printed "failed: listening at fb-test-net-connections failed: address already in use". Two copies started together reproduce it; 30 runs one after another don't. net_ipc and net_tests.cpp had fixed names the same way, and net_ipc's first section relied on panic_net_ipc_name having left its socket file behind. Each run's IPC names now have a random Uuid in them (a process ID in net_tests.cpp, which keeps its folder per process too), net_ipc prints whether a name is the one it gave rather than the name, and the takeover is net_tests.cpp's alone. Checked: net_connections and net_ipc, 50 runs in debug and 50 in release, the two loops at once.

## landing-2 (medium): net_lan's Finds see the services of another run of it at the same time on the machine

Status: fixed in 1e1b37a9

Where: tests/programs/net_lan.fb:14

Found by the landing's verify.py, in the concurrent repeats. net_lan kept to the fixed discovery port 55353, and net_lan_packets to 55360 to 55362, so another run's announcers of the same type answered its Finds: after Stop, a run printed "True [First game, Second game]" and a count of 1 or 2 where the .out has "True [First game]" and 0. A second copy started one to four seconds after the first reproduced it 4 times in 4. Each part now takes a discovery port the OS picks (a UDP socket bound to port 0, then closed), so runs at once keep to their own. Checked: net_lan and net_lan_packets, 50 runs in debug and 50 in release, the two loops at once.

## Refuted by the skeptics (no action)

- runtime-4: On POSIX, listening at a name that's taken leaves the live listener with a connection nobody made. The behavior is real on POSIX. The Linux harness's phantom scenario printed 'first listen: ok / second listen: EADDRINUSE / first accept with nothing connecting: ok / its read: 0', and on Windows phantom.fb printed 'OTHER / nothing to accept'. But this follows from lane decision 7, which prescribes detecting a stale file as 'connect refused, then unlink': a connect probe must connect, so a live listener sees a connection that ends at once. Nothing in the spec, stdlib.md ('A Unix socket's file that a program left behind is taken over'), or the milestones row says a failed Listen leaves no trace. Listen still throws as ipc.fb documents, and the probe is an ordinary connection that ends at once, which any server meets anyway. The proposed fix, an flock file, would replace the decided method. So it is a decision's consequence, not a bug by the rules given.
- runtime-6: A listener's backlog bounds nothing: the I/O thread accepts every connection at once and keeps them without limit. rt-refute/backlog1.fb on Windows printed 'connected 50, refused 0 / waiting to be accepted: 50' with a backlog of 1, so on_connection does accept eagerly. But the lane documented that design. tcp.fb's and ipc.fb's BACKLOG say it bounds connections 'the OS has and the I/O thread hasn't taken yet', and net.cpp's on_connection says 'it accepts it at once, as libuv asks, and keeps it until it's taken'. stdlib.md and decision 5 only say the backlog is 511 unless given. This matches Node's libuv model. No doc promises that a program which doesn't Accept gets refusals, so this is a design choice, not a contradiction. At most, TcpListener.Listen's wording could say that connections are taken at once.
