# The review of M6 step 31 (std.net.game: reliable UDP for games)

15 confirmed findings: 4 high, 8 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## peer-1 (high): Flush panics when a host has two unreliable channels and one Flush overflows 16 packets

Status: fixed in 1536f5a2 (net_game's "two unreliable channels": [20, 20]): Flush keeps how many unreliable parts each channel has back, so each goes before that channel's newer ones.

Where: std/net/game/peer.fb:282

When a Flush passes MOST_PACKETS, leftover unreliable parts go back with Outbound[part.Channel].Fresh.Insert(unsent, part). But `unsent` is one counter shared by every channel, while each channel's Fresh was emptied in Due. The first unreliable channel's leftovers go in at 0..k-1. The second channel's first leftover then goes in at index k of an empty array, which panics. A host with two UNRELIABLE channels (for example, positions and voice) panics in Update as soon as more than 16 packets' worth is queued. Debug and release builds behave the same. No test uses more than one unreliable channel. The program needs tests/programs/net_game/lossy.fb copied to ./net_game/lossy.fb.

```
// A host of two unreliable channels, with more on them than one Flush's 16 packets take: what's
// left should wait for the next Flush on each channel.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

Local network = LossyNetwork(1)
Local two = GameSettings([ChannelKind.UNRELIABLE, ChannelKind.UNRELIABLE])
Local server = GameHost(network.Link(5000), two, 1)
Local client = GameHost(network.Link(6000), two, 2)
Local toServer = client.Connect(server.LocalAddress)
For i = 0 Until 100
    Try Tick(network, [server, client])
    If toServer.State == PeerState.CONNECTED Then Break
End For

Local message: UInt8[] = []
For i = 0 Until 1000
    message.Add(7)
End For

For i = 0 Until 20
    toServer.Send(0, message)
    toServer.Send(1, message)
End For

Local got = [0, 0]
For i = 0 Until 20
    For event In Try Tick(network, [server, client])
        If event.Kind == GameEventKind.RECEIVE Then got[event.Channel] += 1
    End For
End For

Print("received", got[0], got[1], "sent", toServer.PacketsSent)
```

Expected: No panic: a line "received N M sent K", with the leftovers of both channels sent over the next Flushes.

Actual: panic: index 4 is out of range for 0 elements at std/net/game/peer.fb:282:21 (exit code 101), in debug and release builds alike

Suggested fix: Keep the insertion index per channel: count leftovers in a Map<Int, Int> keyed by part.Channel, or collect each channel's leftovers in its own list and put them in front of that channel's Fresh after the loop. Add a test with two UNRELIABLE channels that overflows a Flush.

Skeptic: I ran the reviewer's program (rv-game/peers/p1.fb, with lossy.fb copied in). Debug and release both print "panic: index 4 is out of range for 0 elements at std/net/game/peer.fb:282:21" and exit 101. In the code, Flush keeps one `unsent` counter for every channel and does `Outbound[part.Channel].Fresh.Insert(unsent, part)`, but Due already set each UNRELIABLE channel's Fresh to []. Once the first channel's leftovers have raised `unsent`, the second channel's first leftover is inserted past the end of its empty Fresh.

## peer-2 (high): A fragment refused for MaxBuffered is still acked, so a reliable message is lost and its channel stalls

Status: fixed in 1536f5a2 (net_game: "receiver's MaxBuffered 4000, sender's 4 MiB", 20 of 20 in order and whole, and the MaxBuffered 4000, 64 KiB, and 0 runs, ordered and unordered, all of their messages): InChannel.Take says when it refused a reliable part for room, and GamePeer.Receive then neither notes nor acks its packet, so the sender sends it again. A sender also keeps what's in flight to its MaxBuffered, from each channel's oldest message not acked, which is all the receiver may hold, or a message alone, so a receiver of the same settings needn't refuse anything: without that, 64 KiB took 5000 ticks for 79 of 100.

Where: std/net/game/peer.fb:334

Receive records the packet in the ack bits (Note) before it looks at the messages, and it acks the packet whatever InChannel.Take does with them. When a reliable channel refuses a fragment because the connection's MaxBuffered is used up (Whole returns Null when bytes.Count > room for a message that isn't the next), the packet is acked anyway. The sender marks the part acked and never sends it again. The receiver never gets it, and a RELIABLE_ORDERED channel stalls at that id for good. A peer that calls Disconnect also never finishes, because nothing can deliver the message. The sender's Unacked shows 0 even though 19 of the 20 messages never arrived. MaxBuffered is a documented setting, and this happens as soon as out-of-order fragmented data goes past it. The program needs tests/programs/net_game/lossy.fb copied to ./net_game/lossy.fb.

```
// A receiver whose MaxBuffered is full drops a fragment of a message that isn't the next, but its
// packet is still acked, so the sender never sends it again: a reliable-ordered channel's 20
// messages should all arrive, if later than with more room.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

Local network = LossyNetwork(3)
network.Drop = 0.2
Local tight = GameSettings()
tight.MaxBuffered = 4000
Local server = GameHost(network.Link(5000), tight, 1)
Local client = GameHost(network.Link(6000), tight, 2)
Local toServer = client.Connect(server.LocalAddress)
For i = 0 Until 100
    Try Tick(network, [server, client])
    If toServer.State == PeerState.CONNECTED Then Break
End For

Local message: UInt8[] = []
For i = 0 Until 3000
    message.Add((i % 256) As UInt8)
End For

For i = 0 Until 20
    toServer.Send(0, message)
End For

Local got = 0
For i = 0 Until 900
    For event In Try Tick(network, [server, client])
        If event.Kind == GameEventKind.RECEIVE Then got += 1
    End For
End For

Print("received", got, "of 20; client's unacked", toServer.Unacked, toServer.State)
```

Expected: received 20 of 20; client's unacked 0 CONNECTED

Actual: received 1 of 20; client's unacked 0 CONNECTED (in debug and release builds)

Suggested fix: Don't ack a packet whose reliable part was refused. Have InChannel.Take report a refusal (as opposed to a duplicate or a part that is still incomplete), and in Receive clear that sequence from Bits/Latest, or check for duplicates without marking and mark only when nothing reliable was refused. The sender then resends the part. Add a test with a small MaxBuffered over a lossy link.

Skeptic: p2.fb (MaxBuffered 4000, 20% drop) prints "received 1 of 20; client's unacked 0 CONNECTED" in debug and release. The same program with MaxBuffered 4000000 prints "received 20 of 20". In the code, Receive calls Note(sequence) before it reads the messages, and the packet is acked whatever happens to them. InChannel.Whole returns Null for a fragment of a message that isn't the next one (room >= 0, bytes.Count > room), and nothing records that it was refused. The sender's TakeAcks then marks the part acked and never sends it again. The reliable-ordered channel stalls for good, which breaks stdlib.md's promise of "each message once and in order".

## tests-1 (high): Fragments refused for MaxBuffered are still acked, so reliable messages are silently lost and an ordered channel stalls for good

Status: fixed in 1536f5a2 (net_game: "receiver's MaxBuffered 4000, sender's 4 MiB", 20 of 20 in order and whole, and the MaxBuffered 4000, 64 KiB, and 0 runs, ordered and unordered, all of their messages): InChannel.Take says when it refused a reliable part for room, and GamePeer.Receive then neither notes nor acks its packet, so the sender sends it again. A sender also keeps what's in flight to its MaxBuffered, from each channel's oldest message not acked, which is all the receiver may hold, or a message alone, so a receiver of the same settings needn't refuse anything: without that, 64 KiB took 5000 ticks for 79 of 100.

Where: std/net/game/channel.fb:279

InChannel.Whole refuses a fragment of a message that isn't next when it would go past `room` (MaxBuffered minus what's held). Refusing it is meant to make the sender send it again later. But the packet that carried it is still acked at the packet level (GamePeer.Note/AckPending), so the sender marks the part acked and never sends it again. That message never arrives. On RELIABLE_ORDERED, every message after it then waits forever. On RELIABLE_UNORDERED, the refused ones are simply gone. The sender's Unacked drops to 0 all the same. No test sets MaxBuffered or fills it, so the done-when's "each once, in order" is never checked with a buffer limit in force. With the default 4 MiB, this needs a long run of losses behind big messages. With any smaller MaxBuffered, it happens at once. (The program goes in tests/programs/ beside net_game/lossy.fb.)

```
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

Function Run(buffered: Int, channel: Int, count: Int, size: Int, seed: Int64): String Throws NetError
    Local network = LossyNetwork(seed)
    network.Drop = 0.2
    network.Reorder = 0.25
    Local settings = GameSettings()
    settings.MaxBuffered = buffered
    Local server = GameHost(network.Link(5000), settings, 1)
    Local client = GameHost(network.Link(6000), settings, 2)
    Local peer = client.Connect(server.LocalAddress)
    For i = 0 Until 300
        Try Tick(network, [server, client])
        If peer.State == PeerState.CONNECTED Then Break
    End For

    Local payload: UInt8[] = []
    For i = 0 Until size
        payload.Add(7)
    End For
    For i = 0 Until count
        peer.Send(channel, payload)
    End For
    Local got = 0
    For i = 0 Until 3000
        For event In Try Tick(network, [server, client])
            If event.Kind == GameEventKind.RECEIVE Then got += 1
        End For
    End For
    Return "got {got} of {count}, unacked {peer.Unacked}, {peer.State}"
End Function

Print("ordered, MaxBuffered 64 KiB:", Try Run(65536, 0, 200, 5000, 31))
Print("unordered, MaxBuffered 64 KiB:", Try Run(65536, 1, 200, 5000, 31))
```

Expected: ordered, MaxBuffered 64 KiB: got 200 of 200, unacked 0, CONNECTED
unordered, MaxBuffered 64 KiB: got 200 of 200, unacked 0, CONNECTED

Actual: ordered, MaxBuffered 64 KiB: got 13 of 200, unacked 0, CONNECTED
unordered, MaxBuffered 64 KiB: got 21 of 200, unacked 0, CONNECTED
(the same in --release; with 2000-byte messages and MaxBuffered 0, 4 of 200 arrive)

Suggested fix: Don't ack what was refused. One way is to give Receive a 'refused' result for a reliable part, and leave the packet's sequence out of Note/AckPending when one is refused, so the sender's RTO resends it. Another is per-message acks for reliable parts. Then add a net_game.fb case that sets a small MaxBuffered on the lossy network and checks that all ordered and unordered messages arrive.

Skeptic: Ran the program (tv5/t1.fb, with tests/programs/net_game/lossy.fb copied beside it) in debug and --release. Both print 'ordered, MaxBuffered 64 KiB: got 13 of 200, unacked 0, CONNECTED' and 'unordered, MaxBuffered 64 KiB: got 21 of 200, unacked 0, CONNECTED'. The same run with the default 4 MiB gets 200 of 200. In the code, GamePeer.Receive (peer.fb ~334-363) calls Note(sequence) and sets AckPending = True for every well-formed part. InChannel.Whole (channel.fb ~279-286) returns Null when a fragment would go past room, and nothing records that refusal. The packet is still acked, TakeAcks marks its parts Acked, and they are never resent. The sender's Unacked is 0 while 187 or 179 messages never arrive. That breaks stdlib.md's 'RELIABLE_ORDERED, each message once and in order; RELIABLE_UNORDERED, each once'. No test sets MaxBuffered.

## wire-1 (high): A reliable fragment dropped for MaxBuffered is still acked, so the message is lost for good and an ordered channel stalls

Status: fixed in 1536f5a2 (net_game: "receiver's MaxBuffered 4000, sender's 4 MiB", 20 of 20 in order and whole, and the MaxBuffered 4000, 64 KiB, and 0 runs, ordered and unordered, all of their messages): InChannel.Take says when it refused a reliable part for room, and GamePeer.Receive then neither notes nor acks its packet, so the sender sends it again. A sender also keeps what's in flight to its MaxBuffered, from each channel's oldest message not acked, which is all the receiver may hold, or a message alone, so a receiver of the same settings needn't refuse anything: without that, 64 KiB took 5000 ticks for 79 of 100.

Where: std/net/game/channel.fb:279

InChannel.Whole returns Null without storing a fragment when it would go past `room` (lines 279 and 288). That happens for any reliable message that isn't the one the channel waits for next. GamePeer.Receive has already noted the packet's sequence and set `any`/AckPending, so the packet is acked anyway. The sender gets the ack, marks the part Acked, and never sends it again. The message never arrives, and on a RELIABLE_ORDERED channel every later message waits in Ready forever. The sender's Unacked drops to 0, so it can't tell anything is wrong. With MaxBuffered = 1000, one lost packet is enough. With the default 4 MiB, it happens once more than 4 MiB of fragments arrive ahead of a lost message: dropping a small message's packet for about 3 s while 8 x 1 MiB messages go delivered 5 of 10 messages, with Unacked 0. No test sets MaxBuffered.

```
// A reliable-ordered message whose fragments come while the receiver's MaxBuffered is full is
// dropped but acked, so it never comes, and the channel stalls.
Import std.core
Import std.collections
Import std.time
Import std.net
Import std.net.game

Class Net
    Field Clock: Int64
    Field Queue: (UInt8[], SocketAddress, SocketAddress)[] = []
    Field Dropped: Int
    Field Max: Int

    // Drops the first DATA datagram from 6000 that carries a message.
    Method Carry(bytes: UInt8[], from: SocketAddress, to: SocketAddress)
        If bytes.Count > Max Then Max = bytes.Count
        If Dropped == 0 && from.Port == 6000 && bytes.Count > 17 && bytes[3] == 4
            Dropped += 1
            Return
        End If

        Queue.Add((bytes.Clone(), from, to))
    End Method

    Method Take(to: SocketAddress): (UInt8[], SocketAddress)?
        For i = 0 Until Queue.Count
            If Queue[i].2 == to
                Local got = Queue[i]
                Queue.RemoveAt(i)
                Return (got.0, got.1)
            End If
        End For

        Return Null
    End Method
End Class

Class NetLink Implements GameLink
    Field N: Net
    Field Address: SocketAddress

    New(n: Net, port: UInt16)
        N = n
        Address = SocketAddress(IpAddress.Loopback, port)
    End New

    Property LocalAddress: SocketAddress
        Get => Address
    End Property

    Method Send(bytes: UInt8[], to: SocketAddress) Throws NetError
        N.Carry(bytes, Address, to)
    End Method

    Method Receive(timeout: Duration): (UInt8[], SocketAddress)? Throws NetError
        Return N.Take(Address)
    End Method
End Class

Local net = Net()
Local settings = GameSettings()
settings.MaxBuffered = 1000
Local server = GameHost(NetLink(net, 5000), settings, 1)
Local client = GameHost(NetLink(net, 6000), settings, 2)
Local peer = client.Connect(server.LocalAddress)
Local got: Int[] = []
Local big: UInt8[] = []
big.Resize(3000, 7)
For tick = 1 To 300
    If tick == 5
        Print("state:", peer.State)
        peer.Send(0, "a")
        peer.Send(0, big)
        peer.Send(0, "c")
    End If

    net.Clock += 10000000
    For host In [server, client]
        For event In Try host.Update(Instant.FromNanoseconds(net.Clock))
            If event.Kind == GameEventKind.RECEIVE Then got.Add(event.Bytes.Count)
        End For
    End For
End For

Print("dropped", net.Dropped, "received sizes", got, "client unacked", peer.Unacked)
```

Expected: state: CONNECTED
dropped 1 received sizes [1, 3000, 1] client unacked 0

Actual: state: CONNECTED
dropped 1 received sizes [1] client unacked 0   (same in debug and release; the 3000-byte message and "c" never arrive)

Suggested fix: A part the receiver doesn't keep must not be acked. Either check room in GamePeer.Receive before Note/AckPending and skip the whole packet, leaving it unnoted and unacked so the sender resends it, or keep every in-window reliable fragment and enforce MaxBuffered on the sender's side. Add a test that sets MaxBuffered small and loses the packet before a fragmented message.

Skeptic: I ran the finding's program (rv-game/wire/w1.fb) in debug and release. Both print 'state: CONNECTED' then 'dropped 1 received sizes [1] client unacked 0', so the 3000-byte message and "c" are lost for good on a RELIABLE_ORDERED channel and the sender has nothing left to resend. Code: the dropped packet carried "a" and fragment 0. Fragments 1 and 2 arrived first, and since the channel was waiting for "a", channel.fb:218 passed room = MaxBuffered (1000). Whole then returned Null at line 279 because 1163 > 1000, without storing anything. GamePeer.Receive (peer.fb:334-363) had already called Note(sequence) and sets any/AckPending for every parsed message whatever Take does, so those fragments were acked. The resent fragment 0 is kept (ahead == 0, so room is -1), but fragments 1 and 2 never come back, and Next sticks at the 3000-byte message forever. This breaks stdlib.md's 'RELIABLE_ORDERED, each message once and in order'.

## peer-3 (medium): RoundTripTime is inflated whenever a side sends ack-only or keepalive packets: about 6x when idle, 2.5x for a client that only receives

Status: fixed in 1536f5a2 (net_game's "rtt busy", "rtt idle", "rtt one way", and "rtt never busy", all 40ms on a 20 ms each way path): a packet with messages, or a keepalive, has DATA's new ASK_FLAG, which the other side acks at its next Update, and only those packets' acks time a round trip; a packet that only acks doesn't ask, so the two sides don't ack each other's acks. A keepalive goes KeepAlive after the last packet that asked.

Where: std/net/game/peer.fb:492

TakeAcks times a round trip from every newly acked newest record, including packets that carry no messages: keepalives and ack-only packets. The receiving side sets AckPending only for packets that have messages (`any`). A packet with no messages is therefore acked only by the other side's next send, which can be up to KeepAlive (250 ms) later or at the other side's own send rate. That wait goes into the RTT sample. With a 20 ms each-way link and 10 ms ticks, the real round trip is 40 ms, and busy traffic measures exactly that. After idling, both sides report about 269 ms. When a server sends state every 100 ms and the client only acks, the client reports 100 ms. ResendTime is built on Smoothed and Variance, so resends after any quiet stretch also go far later than they should. The program needs tests/programs/net_game/lossy.fb copied to ./net_game/lossy.fb.

```
// A clean network whose datagrams take 20 ms each way, ticked each 10 ms: a round trip is about
// 40 to 50 ms, idle or not.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Local network = LossyNetwork(1)
Local server = GameHost(network.Link(5000), GameSettings(), 1)
Local client = GameHost(network.Link(6000), GameSettings(), 2)
Local toServer = client.Connect(server.LocalAddress)
Local toClient: GamePeer? = Null
Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

For i = 0 Until 30
    For event In Try Tick(network, [server, client])
        If event.Kind == GameEventKind.CONNECT && event.Peer.Address == client.LocalAddress Then toClient = event.Peer
    End For
End For

// Busy: a message each tick each way.
For i = 0 Until 200
    toServer.Send(0, "x")
    (toClient ?? toServer).Send(0, "y")
    Try Tick(network, [server, client])
End For

Print("busy: client rtt", toServer.RoundTripTime, "server rtt", (toClient ?? toServer).RoundTripTime)
// Idle: only keepalives.
For i = 0 Until 1000
    Try Tick(network, [server, client])
End For

Print("idle: client rtt", toServer.RoundTripTime, "server rtt", (toClient ?? toServer).RoundTripTime)
```

Expected: busy: client rtt 40ms server rtt 40ms
idle: client rtt about 40ms server rtt about 40ms

Actual: busy: client rtt 40ms server rtt 40ms
idle: client rtt 268.740949ms server rtt 268.740949ms
(A one-way variant, where the server sends on channel 2 every 10 ticks and the client sends nothing, prints "server's rtt 40ms client's rtt 100ms".) Debug and release builds give the same output.

Suggested fix: Time only packets the other side acks at once. Mark a SentRecord as timed when its packet carried messages, and call Measure only for those. Alternatively, have the receiver set AckPending for every packet, keepalives included, though that adds ack traffic. Add a test of RoundTripTime after an idle stretch and with one-way traffic.

Skeptic: p3.fb prints "busy: client rtt 40ms server rtt 40ms" then "idle: client rtt 268.740949ms server rtt 268.740949ms", the same in debug and release. In the code, Receive sets AckPending only when `any` is true (the packet carried messages), so a keepalive is acked only when the other side next sends, up to KeepAlive (250 ms) later. TakeAcks still calls Measure(now - record.Sent) for the newest acked record whatever it carried, so RoundTripTime and ResendTime include that wait. It's a real mismeasurement: RoundTripTime is documented as the round-trip time and ResendTime is built on it. I rate it medium, not high, because nothing is lost and the estimate recovers under steady traffic. The real effects are ping readouts after a quiet stretch and late resends of the first message after one.

## peer-4 (medium): MaxPeers counts the host's own outgoing peers, though Connect's doc says it limits only peers that connect to it

Status: fixed in 1536f5a2 (net_game's "MaxPeers 1, out and in": CONNECTED CONNECTED NONE 2): OnConnect counts only the peers that connected to the host, IncomingCount, against MaxPeers.

Where: std/net/game/host.fb:314

OnConnect refuses with FULL when Connections.Count >= Settings.MaxPeers. Connections holds every peer, including the ones this host connected out to. Connect's doc says "MaxPeers limits only the peers that connect to it." A host with MaxPeers 1 that has connected to one other host therefore refuses the first host that connects to it. The program needs tests/programs/net_game/lossy.fb copied to ./net_game/lossy.fb.

```
// MaxPeers limits only the peers that connect to a host, as Connect's doc says; a host of
// MaxPeers 1 that connects out to one host should still take one that connects to it.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Local network = LossyNetwork(1)
Local one = GameSettings()
one.MaxPeers = 1
Local a = GameHost(network.Link(5000), one, 1)
Local b = GameHost(network.Link(6000), GameSettings(), 2)
Local c = GameHost(network.Link(7000), GameSettings(), 3)
Local toB = a.Connect(b.LocalAddress)
Local fromC = c.Connect(a.LocalAddress)
For i = 0 Until 100
    network.Advance(10.Milliseconds)
    For host In [a, b, c]
        Try host.Update(network.Now)
    End For
End For

Print("a to b:", toB.State, "c to a:", fromC.State, fromC.Reason, "a's peers:", a.Peers.Count)
```

Expected: a to b: CONNECTED c to a: CONNECTED NONE a's peers: 2

Actual: a to b: CONNECTED c to a: DISCONNECTED FULL a's peers: 1 (in debug and release builds)

Suggested fix: Count only incoming peers against MaxPeers: in OnConnect, count the Connections values with Incoming set (or keep a counter of them). Add a test of a host that connects out and also takes a peer at MaxPeers 1.

Skeptic: p4.fb prints "a to b: CONNECTED c to a: DISCONNECTED FULL a's peers: 1" in debug and release. In host.fb, OnConnect line 314 checks `Connections.Count >= Settings.MaxPeers`, and Connections includes the outgoing peer that Connect made. That contradicts Connect's doc ("MaxPeers limits only the peers that connect to it") and GameSettings' doc ("the most peers that can connect to it").

## peer-5 (medium): Two hosts that Connect to each other at the same time never connect and both time out

Status: fixed in 1536f5a2 (net_game's "at once": a CONNECT each, both CONNECTED, and a message each way): a CONNECT from an address this host is asking breaks the tie, the host of the larger salt, or else of the larger address, answering with its own peer, which takes the asker's salt and first sequence, and the other waiting for that ACCEPT. stdlib.md says so.

Where: std/net/game/host.fb:303

Peers are keyed by address. When a CONNECT arrives from an address where this host already has an outgoing CONNECTING peer, OnConnect drops it, because peer != Null and peer.Incoming is False. If A connects to B while B connects to A, which is common in peer-to-peer games and hole punching, each side ignores the other's CONNECT. Neither ever sends an ACCEPT, so both give DISCONNECT TIMEOUT after the full Timeout. The docs don't say that simultaneous connects fail. The program needs tests/programs/net_game/lossy.fb copied to ./net_game/lossy.fb.

```
// Two hosts that connect to each other at once, as peers of a game often do.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Local network = LossyNetwork(1)
Local quick = GameSettings()
quick.Timeout = 1.Seconds
Local a = GameHost(network.Link(5000), quick, 1)
Local b = GameHost(network.Link(6000), quick, 2)
Local toB = a.Connect(b.LocalAddress)
Local toA = b.Connect(a.LocalAddress)
For i = 1 To 200
    network.Advance(10.Milliseconds)
    For host In [a, b]
        For event In Try host.Update(network.Now)
            Print(i, host.LocalAddress.Port, event)
        End For
    End For
End For

Print("a to b:", toB.State, toB.Reason, "b to a:", toA.State, toA.Reason)
```

Expected: Each host gives one CONNECT event, and the program ends with "a to b: CONNECTED NONE b to a: CONNECTED NONE".

Actual: 101 5000 DISCONNECT 127.0.0.1:6000, TIMEOUT
101 6000 DISCONNECT 127.0.0.1:5000, TIMEOUT
a to b: DISCONNECTED TIMEOUT b to a: DISCONNECTED TIMEOUT

Suggested fix: Break the tie when a CONNECT comes from an address that has an outgoing CONNECTING peer. For example, the side with the larger salt (or address) answers it: that side turns its peer into the incoming one, taking the asker's salt and first sequence, opening its Inbound, and sending an ACCEPT. The other side keeps waiting for that ACCEPT. Either way, document the result and add a test.

Skeptic: p5.fb prints "101 5000 DISCONNECT 127.0.0.1:6000, TIMEOUT" and "101 6000 DISCONNECT ...", then "a to b: DISCONNECTED TIMEOUT b to a: DISCONNECTED TIMEOUT", in debug and release. In host.fb, OnConnect returns at once when `peer != Null` and peer.Incoming is False, so each side drops the other's CONNECT and neither sends an ACCEPT. Two live hosts that each asked for a connection both fail. The reason given, TIMEOUT, is documented as "nothing came for the host's Timeout", yet CONNECTs did come. stdlib.md says a host "takes those others ask for", and nothing documents an exception for this case.

## tests-2 (medium): RoundTripTime on an idle connection includes the other side's keepalive wait (256 ms reported for a 64 ms path)

Status: fixed in 1536f5a2 (net_game's "rtt busy", "rtt idle", "rtt one way", and "rtt never busy", all 40ms on a 20 ms each way path): a packet with messages, or a keepalive, has DATA's new ASK_FLAG, which the other side acks at its next Update, and only those packets' acks time a round trip; a packet that only acks doesn't ask, so the two sides don't ack each other's acks. A keepalive goes KeepAlive after the last packet that asked.

Where: std/net/game/peer.fb:492

TakeAcks times a round trip from the newest acked record. A keepalive (ack-only) packet doesn't set the receiver's AckPending, so it is acked only by the receiver's own next keepalive, up to KeepAlive (250 ms) later. That wait goes into the RFC 6298 sample. On a connection with no messages yet, at a 60 Hz tick over a 20 ms link with default settings, RoundTripTime settles at 256 ms, not about 64 ms. ResendTime is built from it too. The same happens whenever the two sides' KeepAlives differ: a server with KeepAlive 1 s and clients at 250 ms reads about 250 ms. The tests only check the RTT in the lossy run's golden output; nothing checks it against a known clean path.

```
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(16.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

// A clean network, 20 ms each way, updated at 60 Hz: a round trip is 64 ms of ticks.
Local network = LossyNetwork(1)
Local server = GameHost(network.Link(5000), GameSettings(), 1)
Local client = GameHost(network.Link(6000), GameSettings(), 2)
Local peer = client.Connect(server.LocalAddress)
For i = 0 Until 20
    Try Tick(network, [server, client])
End For
Print("connected:", peer.State, peer.RoundTripTime)
For i = 0 Until 300
    Try Tick(network, [server, client])
    If i % 100 == 0 Then Print(i, "rtt", peer.RoundTripTime)
End For
```

Expected: connected: CONNECTED 100ms
0 rtt 64ms (about)
100 rtt 64ms
200 rtt 64ms

Actual: connected: CONNECTED 100ms
0 rtt 256ms
100 rtt 256ms
200 rtt 256ms (debug and release)

Suggested fix: Don't take a round-trip sample from a record that carried no reliable parts, i.e. an ack-only or keepalive packet. Or have a keepalive set the receiver's AckPending so it is answered at the next Update. Or send ack delay as QUIC does and subtract it. Add a test that checks RoundTripTime on a clean, fixed-latency LossyNetwork, idle and busy.

Skeptic: Ran tv5/t2.fb in debug and release: 'connected: CONNECTED 100ms', then 'rtt 256ms' at ticks 0, 100 and 200, on a clean path whose round trip is 64 ms of 16 ms ticks. A busy variant (t2b.fb) shows the idle value is 256ms, var 40.5ms, and the first busy reading is still 256ms. It reaches 64ms only after messages flow (about 64.36ms by tick 50). In the code, TakeAcks (peer.fb 484-492) measures from the newest acked record whatever it carried. A keepalive packet has no parts, so Receive's 'any' stays False and AckPending is not set. The keepalive is answered only by the other side's own next keepalive (KeepAlive 250 ms), and that wait goes into the sample. So an idle connection reports about 4x its RTT, and the RTO after idle is inflated. No test checks RTT against a known clean path.

## tests-3 (medium): MaxPeers counts the host's own outgoing connections, though Connect's doc says it limits only peers that connect to it

Status: fixed in 1536f5a2 (net_game's "MaxPeers 1, out and in": CONNECTED CONNECTED NONE 2): OnConnect counts only the peers that connected to the host, IncomingCount, against MaxPeers.

Where: std/net/game/host.fb:314

OnConnect refuses with FULL when Connections.Count >= MaxPeers. Connections also holds the peers this host made with Connect. Connect's doc says "MaxPeers limits only the peers that connect to it", and GameSettings says "the most peers that can connect to it". So a host with MaxPeers 1 that has connected out to another host refuses its first incoming peer as FULL. The FULL test only uses a host that never connects out.

```
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

Local network = LossyNetwork(1)
Local one = GameSettings()
one.MaxPeers = 1
Local a = GameHost(network.Link(5000), one, 1)
Local b = GameHost(network.Link(6000), GameSettings(), 2)
Local c = GameHost(network.Link(7000), GameSettings(), 3)
Local out = a.Connect(b.LocalAddress)
For i = 0 Until 20
    Try Tick(network, [a, b, c])
End For
Print("a to b:", out.State)
Local incoming = c.Connect(a.LocalAddress)
For i = 0 Until 50
    For event In Try Tick(network, [a, b, c])
        Print(event)
    End For
End For
Print("c to a:", incoming.State, incoming.Reason)
```

Expected: a to b: CONNECTED
CONNECT 127.0.0.1:7000
CONNECT 127.0.0.1:5000
c to a: CONNECTED NONE

Actual: a to b: CONNECTED
DISCONNECT 127.0.0.1:5000, FULL
c to a: DISCONNECTED FULL

Suggested fix: Count only incoming peers (peer.Incoming) against MaxPeers in OnConnect, or change Connect's and GameSettings' docs to say outgoing peers count too. Add the case to net_game.fb's handshake tests.

Skeptic: Ran tv5/t3.fb in debug and release: 'a to b: CONNECTED', then 'DISCONNECT 127.0.0.1:5000, FULL' and 'c to a: DISCONNECTED FULL'. host.fb OnConnect (~314) refuses when Connections.Count >= Settings.MaxPeers, and Connections also holds peers made by Connect (host.fb ~141-147). That contradicts Connect's own doc comment, 'MaxPeers limits only the peers that connect to it', and GameSettings' 'the most peers that can connect to it' (settings.fb 13-14). The FULL test uses only a host that never connects out.

## tests-4 (medium): Broadcast to a channel the host lacks, or of a message past MaxMessageSize, panics only once a peer is connected

Status: fixed in 1536f5a2 (panic_net_game_broadcast, with no peers, and net_game's "broadcast"): Broadcast checks the channel and the size first, through CheckMessage in settings.fb, which a peer's Send uses too.

Where: std/net/game/host.fb:152

Broadcast is @PanicsAtCaller. docs/stdlib.md says "A bad channel, a message too big ... panic at the caller". But the checks run only inside peer.Send, so with no CONNECTED peer a bad Broadcast does nothing. A game server that broadcasts every tick hides the bug until the first client connects, and then panics. No test covers Broadcast at all.

```
Import std.core
Import std.collections
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Local network = LossyNetwork(1)
Local settings = GameSettings()
settings.MaxMessageSize = 100
Local server = GameHost(network.Link(5000), settings, 1)
Local big: UInt8[] = []
big.Resize(101, 0)
server.Broadcast(7, "no channel 7".Bytes)
server.Broadcast(0, big)
Print("no panic")
```

Expected: panic: a host of 3 channels has no channel 7 at <file>:13:1

Actual: no panic

Suggested fix: Check the channel and the size in Broadcast itself, before the loop, with the same messages as GamePeer.Send. Add a panic_net_game_broadcast test and a Broadcast case in net_game.fb, since Broadcast has none.

Skeptic: Ran tv5/t4.fb in debug and release. It prints 'no panic' for Broadcast(7, ...) on a 3-channel host and for Broadcast(0, 101 bytes) with MaxMessageSize 100. Broadcast (host.fb ~151-156) is @PanicsAtCaller but checks nothing itself. It calls peer.Send only for CONNECTED peers, and the channel and size checks are only in GamePeer.Send (peer.fb 192-200). stdlib.md says 'A bad channel, a message too big ... panic at the caller'. GamePeer.Send panics for these whatever the peer's state, so Broadcast doesn't match it either. No test covers Broadcast.

## wire-2 (medium): Check's fragment-count test overflows Int for a MaxMessageSize near Int's maximum: debug panics with an overflow, release accepts it and truncates messages

Status: fixed in 1536f5a2 (panic_net_game_fragments, MaxMessageSize 2147483647): Check compares MaxMessageSize with 65535 parts in Int64, so it panics with its message in either build.

Where: std/net/game/settings.fb:289

Int is 32-bit, so `settings.MaxMessageSize + partSize - 1` overflows for any MaxMessageSize within partSize of 2147483647. Users may well write that to mean 'no limit'. In debug, New panics with "integer overflow in '+'" instead of Check's message. In release, the wrapped sum is negative, so the check passes and the host accepts a MaxMessageSize that needs more than 65535 fragments. Sending a message past 65535 fragments then writes `count` through U16, which wraps. The receiver puts together only the first `count & 0xFFFF` fragments and delivers a truncated message as whole. A debug/release difference plus a wrong result.

```
// A MaxMessageSize too large for 65535 fragments should panic at the GameHost's New; release
// takes it, and a message past 65535 fragments arrives cut short.
Import std.core
Import std.collections
Import std.time
Import std.net
Import std.net.game

Class Net
    Field Clock: Int64
    Field Queue: (UInt8[], SocketAddress, SocketAddress)[] = []

    Method Carry(bytes: UInt8[], from: SocketAddress, to: SocketAddress)
        Queue.Add((bytes.Clone(), from, to))
    End Method

    Method Take(to: SocketAddress): (UInt8[], SocketAddress)?
        For i = 0 Until Queue.Count
            If Queue[i].2 == to
                Local got = Queue[i]
                Queue.RemoveAt(i)
                Return (got.0, got.1)
            End If
        End For

        Return Null
    End Method
End Class

Class NetLink Implements GameLink
    Field N: Net
    Field Address: SocketAddress

    New(n: Net, port: UInt16)
        N = n
        Address = SocketAddress(IpAddress.Loopback, port)
    End New

    Property LocalAddress: SocketAddress
        Get => Address
    End Property

    Method Send(bytes: UInt8[], to: SocketAddress) Throws NetError
        N.Carry(bytes, Address, to)
    End Method

    Method Receive(timeout: Duration): (UInt8[], SocketAddress)? Throws NetError
        Return N.Take(Address)
    End Method
End Class

Local net = Net()
Local settings = GameSettings()
settings.Mtu = 128
settings.MaxMessageSize = 2147483647
Local server = GameHost(NetLink(net, 5000), settings, 1)
Local client = GameHost(NetLink(net, 6000), settings, 2)
Local peer = client.Connect(server.LocalAddress)
Local got: Int[] = []
Local big: UInt8[] = []
big.Resize(6700000, 7)
For tick = 1 To 5000
    If tick == 5 Then peer.Send(0, big)
    net.Clock += 10000000
    For host In [server, client]
        For event In Try host.Update(Instant.FromNanoseconds(net.Clock))
            If event.Kind == GameEventKind.RECEIVE Then got.Add(event.Bytes.Count)
        End For
    End For
End For

Print("sent 6700000 bytes, received sizes", got, "client unacked", peer.Unacked)
```

Expected: In both builds: panic: a GameHost's MaxMessageSize must be from 1 to 65535 packets' worth, not 2147483647 (at the GameHost(...) line)

Actual: debug: panic: integer overflow in '+' (at the GameHost line). release: sent 6700000 bytes, received sizes [80901] client unacked 0

Suggested fix: Compute the fragment count without overflow, e.g. `settings.MaxMessageSize > 65535 * partSize` in Int64 (`(settings.MaxMessageSize As Int64) > 65535 * (partSize As Int64)`). Add a panic test with MaxMessageSize = 2147483647.

Skeptic: I ran the program (w2.fb). Debug: 'panic: integer overflow in '+'' at the GameHost(...) line, not Check's message. Release: 'sent 6700000 bytes, received sizes [80901] client unacked 0'. 80901 = (66337 - 65536) * 101, which shows the fragment count wrapped in U16 and a truncated message was delivered as whole. Code: settings.fb:61 computes (settings.MaxMessageSize + partSize - 1) in 32-bit Int, which overflows near 2147483647. In release the wrapped value is negative and passes the > 65535 test. peer.fb:300 then writes part.Count through U16, which keeps the low 16 bits. So a debug/release difference and a silently wrong result.

## wire-3 (medium): An unreliable channel drops every new message for a while after more than 32767 of its messages are lost in a row

Status: fixed in 1536f5a2 (net_game's "outage": 400 unreliable messages a tick through a second's outage, then all 72400 after it, the first at tick 112): an unreliable id more than WINDOW behind the last given, in a packet after the one that brought it, is past an outage, and newer; a stale one comes in an older packet.

Where: std/net/game/channel.fb:254

InChannel.Take for UNRELIABLE keeps only ids IsAfter the last one given, using RFC 1982 on 16 bits. Unreliable ids advance once per message whether or not it arrives, and nothing bounds how far ahead the sender gets. So when 32768 or more unreliable messages are lost in a row, every new message reads as older than the last one given and is dropped until the ids wrap round again. That can happen in a short outage well inside the 10 s Timeout, with a game sending a few thousand small updates a second. In the program, a 1 s outage at 400 messages a tick means nothing arrives for 0.64 s after the network is back, and 21536 fresh messages are dropped. Reliable channels avoid this through their 1024-message window. The unreliable channel has nothing like it.

```
// After a second's outage on a busy unreliable channel, fresh messages should come again at once.
Import std.core
Import std.collections
Import std.time
Import std.net
Import std.net.game

Class Net
    Field Clock: Int64
    Field Queue: (UInt8[], SocketAddress, SocketAddress)[] = []
    Field Dropped: Int
    Field Outage: Bool

    // Drops all of 6000's datagrams during the outage.
    Method Carry(bytes: UInt8[], from: SocketAddress, to: SocketAddress)
        If Outage && from.Port == 6000
            Dropped += 1
            Return
        End If

        Queue.Add((bytes.Clone(), from, to))
    End Method

    Method Take(to: SocketAddress): (UInt8[], SocketAddress)?
        For i = 0 Until Queue.Count
            If Queue[i].2 == to
                Local got = Queue[i]
                Queue.RemoveAt(i)
                Return (got.0, got.1)
            End If
        End For

        Return Null
    End Method
End Class

Class NetLink Implements GameLink
    Field N: Net
    Field Address: SocketAddress

    New(n: Net, port: UInt16)
        N = n
        Address = SocketAddress(IpAddress.Loopback, port)
    End New

    Property LocalAddress: SocketAddress
        Get => Address
    End Property

    Method Send(bytes: UInt8[], to: SocketAddress) Throws NetError
        N.Carry(bytes, Address, to)
    End Method

    Method Receive(timeout: Duration): (UInt8[], SocketAddress)? Throws NetError
        Return N.Take(Address)
    End Method
End Class

Local net = Net()
Local server = GameHost(NetLink(net, 5000), GameSettings(), 1)
Local client = GameHost(NetLink(net, 6000), GameSettings(), 2)
Local peer = client.Connect(server.LocalAddress)
Local before = 0
Local after = 0
Local resumed = 0
For tick = 1 To 300
    // A second's outage, from tick 10 to 110, while 400 unreliable messages a tick go.
    net.Outage = tick >= 10 && tick < 110
    If peer.State == PeerState.CONNECTED
        For i = 0 Until 400
            peer.Send(2, "u")
        End For
    End If

    net.Clock += 10000000
    For host In [server, client]
        For event In Try host.Update(Instant.FromNanoseconds(net.Clock))
            If event.Kind != GameEventKind.RECEIVE Then Continue
            If tick < 10 Then before += 1
            If tick >= 120 Then after += 1
            If tick >= 110 && resumed == 0 Then resumed = tick
        End For
    End For
End For

Print(peer.State, "unreliable before the outage", before, "in the 180 ticks after it", after, "of", 180 * 400, "first after it at tick", resumed)
```

Expected: CONNECTED unreliable before the outage 2400 in the 180 ticks after it 72000 of 72000 first after it at tick 110 (or 111)

Actual: CONNECTED unreliable before the outage 2400 in the 180 ticks after it 50464 of 72000 first after it at tick 174 (debug and release alike)

Suggested fix: Judge an unreliable message's freshness by something that can't run a half-round ahead unseen. Either take an id that is 'before' the last given by more than a small window (for example WINDOW) as a wrap and accept it, or order unreliable messages by the packet sequence they came in, which the acks keep in step. Add a test with an outage of more than 32768 unreliable messages.

Skeptic: I ran the program (w3.fb) in debug and release. Both print 'CONNECTED unreliable before the outage 2400 in the 180 ticks after it 50464 of 72000 first after it at tick 174'. So after the network came back at tick 110, no unreliable message got through for 64 ticks and 21536 fresh ones were dropped. Code: channel.fb:254 keeps only ids that IsAfter(id, Next) under RFC 1982 on 16 bits. Unreliable ids advance once per message whether or not it arrives (channel.fb:86), and nothing bounds the gap, so after 40000 lost messages every new id reads as older than the last one given. That contradicts stdlib.md's promise that 'what comes is the freshest'. It needs about 3300 or more unreliable messages a second through an outage shorter than Timeout, which is unusual but plausible, so medium.

## peer-6 (low): Broadcast doesn't panic for a bad channel or an oversized message when no peer is connected

Status: fixed in 1536f5a2 (panic_net_game_broadcast, with no peers, and net_game's "broadcast"): Broadcast checks the channel and the size first, through CheckMessage in settings.fb, which a peer's Send uses too.

Where: std/net/game/host.fb:152

stdlib.md says "A bad channel, a message too big, or settings that make no sense panic at the caller." Broadcast checks these only through each connected peer's Send. With no peers connected, or with peers that are still connecting, Broadcast(99, bytes) returns quietly. The same mistake then panics only once a peer has connected, so the bug shows up late.

```
// Broadcast on a channel the host doesn't have should panic, as Send does, peers or not.
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Local network = LossyNetwork(1)
Local host = GameHost(network.Link(5000), GameSettings(), 1)
host.Broadcast(99, "x".Bytes)
Print("no panic")
```

Expected: panic: a host of 3 channels has no channel 99 (at the Broadcast call)

Actual: no panic

Suggested fix: In Broadcast, check the channel against Settings.Channels.Count and bytes.Count against Settings.MaxMessageSize, with the same messages as GamePeer.Send, before the loop. Add a panic_ test.

Skeptic: p6.fb prints "no panic" for host.Broadcast(99, ...) with no peers, in debug and release. In host.fb, Broadcast (line 152) is @PanicsAtCaller but checks the channel and size only through each CONNECTED peer's Send. stdlib.md says "A bad channel, a message too big ... panic at the caller", so whether the same mistake panics depends on whether a peer happens to be connected.

## tests-5 (low): Settings whose KeepAlive is not under Timeout don't panic, and idle connections between live hosts time out

Status: fixed in 1536f5a2 (panic_net_game_keepalive): Check panics for a KeepAlive or a ConnectRetry that isn't under the Timeout.

Where: std/net/game/settings.fb:67

Check panics for "settings that make no sense" (docs/stdlib.md) but accepts KeepAlive >= Timeout. Then an idle connection between two live hosts ends with TIMEOUT on both sides, because nothing is sent before the timeout passes. panic_net_game_settings covers only Mtu.

```
Import std.core
Import std.time
Import std.net
Import std.net.game
Import "./net_game/lossy"

Function Tick(network: LossyNetwork, hosts: GameHost[]): GameEvent[] Throws NetError
    network.Advance(10.Milliseconds)
    Local events: GameEvent[] = []
    For host In hosts
        For event In Try host.Update(network.Now)
            events.Add(event)
        End For
    End For

    Return events
End Function

Local settings = GameSettings()
settings.Timeout = 1.Seconds
settings.KeepAlive = 2.Seconds
Local network = LossyNetwork(1)
Local server = GameHost(network.Link(5000), settings, 1)
Local client = GameHost(network.Link(6000), settings, 2)
Local peer = client.Connect(server.LocalAddress)
For i = 0 Until 300
    For event In Try Tick(network, [server, client])
        Print(i, event)
    End For
End For
```

Expected: panic: a GameHost's KeepAlive must be under its Timeout ... at the GameHost(...) line

Actual: 2 CONNECT 127.0.0.1:6000
4 CONNECT 127.0.0.1:5000
104 DISCONNECT 127.0.0.1:5000, TIMEOUT
106 DISCONNECT 127.0.0.1:6000, TIMEOUT

Suggested fix: In Check, panic when KeepAlive >= Timeout (and maybe ConnectRetry >= Timeout). Add those cases to the settings panic tests.

Skeptic: Ran tv5/t5.fb in debug and release: '2 CONNECT 127.0.0.1:6000', '4 CONNECT 127.0.0.1:5000', '104 DISCONNECT 127.0.0.1:5000, TIMEOUT', '106 DISCONNECT 127.0.0.1:6000, TIMEOUT'. Both hosts are live, and the idle connection still times out because nothing is sent within the Timeout. CheckSettings (settings.fb ~55-75) checks only that Timeout, KeepAlive and ConnectRetry are above 0, so KeepAlive 2 s with Timeout 1 s is accepted. stdlib.md says 'settings that make no sense panic at the caller', and a keepalive slower than the timeout makes no sense: every idle connection drops. Low, because a user has to choose these values.

## wire-4 (low): A CONNECT can be larger than Mtu, though GameSettings calls Mtu the most bytes a packet takes

Status: fixed in 1536f5a2 (panic_net_game_connect_mtu, 255 channels and an Mtu of 128): Check panics for an Mtu that can't hold a CONNECT of the channels, 15 bytes and one a channel.

Where: std/net/game/settings.fb:284

GameSettings documents Mtu as 'the most bytes a packet takes', and Check accepts Mtu from 128 with up to 255 channels. But SendConnect writes 15 bytes plus one per channel, so with 255 channels a CONNECT is 270 bytes, past an Mtu of 128. DATA respects Mtu exactly (a run with every fragment boundary topped out at 1200 with Mtu 1200). On a path whose real MTU is the configured one, the handshake can then never get through.

```
// A CONNECT with 255 channels and an Mtu of 128 should fit in 128 bytes, or Check should refuse it.
Import std.core
Import std.collections
Import std.time
Import std.net
Import std.net.game

Class Net
    Field Clock: Int64
    Field Queue: (UInt8[], SocketAddress, SocketAddress)[] = []
    Field Max: Int

    Method Carry(bytes: UInt8[], from: SocketAddress, to: SocketAddress)
        If bytes.Count > Max Then Max = bytes.Count
        Queue.Add((bytes.Clone(), from, to))
    End Method

    Method Take(to: SocketAddress): (UInt8[], SocketAddress)?
        For i = 0 Until Queue.Count
            If Queue[i].2 == to
                Local got = Queue[i]
                Queue.RemoveAt(i)
                Return (got.0, got.1)
            End If
        End For

        Return Null
    End Method
End Class

Class NetLink Implements GameLink
    Field N: Net
    Field Address: SocketAddress

    New(n: Net, port: UInt16)
        N = n
        Address = SocketAddress(IpAddress.Loopback, port)
    End New

    Property LocalAddress: SocketAddress
        Get => Address
    End Property

    Method Send(bytes: UInt8[], to: SocketAddress) Throws NetError
        N.Carry(bytes, Address, to)
    End Method

    Method Receive(timeout: Duration): (UInt8[], SocketAddress)? Throws NetError
        Return N.Take(Address)
    End Method
End Class

Local net = Net()
Local kinds: ChannelKind[] = []
For i = 0 Until 255
    kinds.Add(ChannelKind.UNRELIABLE)
End For

Local settings = GameSettings(kinds)
settings.Mtu = 128
Local server = GameHost(NetLink(net, 5000), settings, 1)
Local client = GameHost(NetLink(net, 6000), settings, 2)
Local peer = client.Connect(server.LocalAddress)
For tick = 1 To 20
    If tick == 5 Then peer.Send(0, "hi")
    net.Clock += 10000000
    For host In [server, client]
        Try host.Update(Instant.FromNanoseconds(net.Clock))
    End For
End For

Print(peer.State, "Mtu", settings.Mtu, "largest datagram", net.Max)
```

Expected: CONNECTED Mtu 128 largest datagram 128 (or a panic at GameHost's New saying the Mtu is too small for 255 channels' CONNECT)

Actual: CONNECTED Mtu 128 largest datagram 270

Suggested fix: In Check, require `settings.Mtu >= 15 + settings.Channels.Count` (HEADER + salt + first + largest + count + kinds), or document that the handshake's packets aren't bound by Mtu. Add a panic test or a size check.

Skeptic: I ran the program (w4.fb) in debug. It prints 'CONNECTED Mtu 128 largest datagram 270'. Code: host.fb:433-443 SendConnect writes a 4-byte header, salt (4), first (2), MaxMessageSize (4), count (1) and one byte per channel, which is 270 bytes for 255 channels. Check (settings.fb:48-58) accepts Mtu 128 with 255 channels, while GameSettings' doc (settings.fb:14) calls Mtu 'the most bytes a packet takes'. DATA respects Mtu but the handshake doesn't. This only shows up with an unusual Mtu and channel count, so low.
