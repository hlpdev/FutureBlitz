# The review of M6 step 32's integration program (tests/programs/readings: JSON fetched over HTTPS, kept in SQLite, drawn as a chart, served as JSON, static files, and to its own WebSocket client)

7 confirmed findings: 0 high, 1 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## program-1 (medium): Chart panics when every reading is the same multiple of 5, or the store is empty

Status: fixed in 6768b448 (readings: "flat", 916 colored pixels, and "empty", 0, none outside the plot): Chart's band of degrees is 5 at least, high = Max(Ceil(max / 5) * 5, low + 5), so a flat line, or a store with none, draws inside it.

Where: tests/programs/readings/chart.fb:28

Chart sets the grid from low = Floor(min / 5) * 5 to high = Ceil(max / 5) * 5. When every reading is one multiple of 5 (a flat 5.0, 0.0, or 10.0 series), or the store is empty (Range gives (0, 0) through its ?? 0.0), low == high. At() then divides (high - celsius) by (high - low) = 0, which gives NaN, and Canvas panics on the first grid line. Chart is the module someone would copy to plot a series, and a flat series is an ordinary input.

```
Import std.core
Import std.image
Import "./readings/source"
Import "./readings/store"
Import "./readings/chart"

// A store whose readings are all 5 degrees: a flat line, which the chart should still draw.
Function Drawn(readings: Reading[]): String Throws Error
    Local store = Try Store.Open()
    Try store.Add(readings)
    Local image = Try Chart(store, 480, 270)
    Local colored = 0
    Local bytes = image.ToBytes()
    For i = 0 Until bytes.Count Step 4
        If bytes[i] != bytes[i + 1] || bytes[i] != bytes[i + 2] Then colored += 1
    End For

    Return "{colored} colored pixels"
End Function

Try
    Print("flat:", Try Drawn([Reading("Oslo", 0, 5.0), Reading("Oslo", 1, 5.0)]))
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: flat: <n> colored pixels (a flat line drawn in a band around 5 degrees)

Actual: panic: a point drawn is finite, not (24, NaN) at readings/chart.fb:44:9 (exit 101). Drawn([]) on an empty store panics the same way.

Suggested fix: When high - low is 0, widen the band, for example high = low + 5 (and when the store is empty, return the blank grid or use low = 0, high = 5). Add the flat and empty cases to the test.

Skeptic: Ran the finding's program (rv-readings/v/p1.fb) with a copy of tests/programs/readings/. In both debug and --release it printed 'panic: a point drawn is finite, not (24, NaN) at readings/chart.fb:44:9' and exited 101. Drawn([]) on an empty store (p1e.fb) panics the same way in debug and release. A flat 5.5 series (p1b.fb) draws fine ('flat: 1348 colored pixels'), so the panic comes from low == high: chart.fb:27-28 gives Floor(5/5)*5 == Ceil(5/5)*5, and line 36 then divides by (high - low) = 0. Chart is a public module that crashes on an empty store, which is an ordinary input.

## program-2 (low): Chart scales x by how many readings there are, not by the hours, so sparse or offset hours are drawn off the plot

Status: fixed in 6768b448 (readings: "every 12 hours" and "from hour 30", none outside the plot): Chart maps an hour from the store's first to its last, which Store.Hours gives from SQL, and puts the grid's lines at the hours that are a multiple of 6 between them. The main chart's pixels, and their hash, didn't change.

Where: tests/programs/readings/chart.fb:35

`hours` is the largest Series(city).Count, and At() puts an hour at plot.X + plot.Width * hour / (hours - 1). That's right only when every city's hours are 0, 1, ..., n - 1. Readings every 12 hours (0, 12, 24) put hour 12 at x = 24 + 432 * 12 / 2 = 2616, off the image. The line leaves the plot, and the dot for the warmest reading isn't drawn. The same happens for any series that doesn't start at hour 0. The vertical grid lines (`For hour = 0 Until hours Step 6`) make the same assumption.

```
Import std.core
Import std.image
Import "./readings/source"
Import "./readings/store"
Import "./readings/chart"

// How many of a chart's colored pixels are right of its plot, which ends 24 pixels from the edge.
Function Outside(readings: Reading[]): Int Throws Error
    Local store = Try Store.Open()
    Try store.Add(readings)
    Local image = Try Chart(store, 480, 270)
    Local bytes = image.ToBytes()
    Local outside = 0
    For y = 0 Until 270
        For x = 457 Until 480
            Local i = (y * 480 + x) * 4
            If bytes[i] != bytes[i + 1] || bytes[i] != bytes[i + 2] Then outside += 1
        End For
    End For

    Return outside
End Function

Try
    Print("hourly:", Try Outside([Reading("Oslo", 0, 1.0), Reading("Oslo", 1, 9.0), Reading("Oslo", 2, 3.0)]))
    Print("every 12 hours:", Try Outside([Reading("Oslo", 0, 1.0), Reading("Oslo", 12, 9.0), Reading("Oslo", 24, 3.0)]))
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: hourly: 0
every 12 hours: 0

Actual: hourly: 0
every 12 hours: 70

Suggested fix: Scale x by the hours themselves. Take the first and last hours across the cities (in SQL, SELECT min(hour), max(hour)), and map hour to plot.X + plot.Width * (hour - first) / Max(last - first, 1). Put the grid's vertical lines at multiples of 6 within that range.

Skeptic: Ran p2.fb in debug and release. Both printed 'hourly: 0' and 'every 12 hours: 70', so 70 colored pixels land in the right margin past the plot. chart.fb:29-35 sets `hours` to the largest Series().Count and maps an hour to plot.X + plot.Width * hour / (hours - 1). That is only right when hours run 0..n-1, but Reading.Hour is any hour 'from 0'. The vertical grid at line 48 makes the same assumption. The program's own fixed data is hourly from 0, so readings.out isn't affected. I rated it low because the chart draws wrongly but nothing crashes.

## program-3 (low): The "stored:" line calls the reading count cities: "108 cities"

Status: fixed in 6768b448 (readings: "stored: 108 readings of [Cairo, Lisbon, Oslo]"): the line calls the count readings.

Where: tests/programs/readings.fb:85

Store.Count() is how many readings the store has (its own comment says so), but readings.fb prints it as `"stored:", store.Count(), "cities", store.Cities()`. So readings.out reads "stored: 108 cities [Cairo, Lisbon, Oslo]", although there are 3 cities and 108 readings.

```
Import std.core
Import "./readings/source"
Import "./readings/store"

// readings.fb's "stored:" line: Count is how many readings the store has, but the line calls it
// cities.
Try
    Local store = Try Store.Open()
    Try store.Add(Readings(26, 36))
    Print("stored:", Try store.Count(), "cities", Try store.Cities())
    Print("cities really:", (Try store.Cities()).Count)
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: stored: 108 readings, cities [Cairo, Lisbon, Oslo]
cities really: 3

Actual: stored: 108 cities [Cairo, Lisbon, Oslo]
cities really: 3

Suggested fix: Print("stored:", Try store.Count(), "readings of", Try store.Cities()), and update readings.out to match.

Skeptic: p3.fb printed 'stored: 108 cities [Cairo, Lisbon, Oslo]' and 'cities really: 3'. readings.fb:85 prints store.Count(), whose doc in store.fb:45 says 'How many readings it has', next to the word 'cities'. readings.out line 3 has the same mislabel.

## program-4 (low): The "a reading again takes the place of the one before it" check can't tell REPLACE from IGNORE

Status: fixed in 6768b448 (readings: "again, warmer: True then back: True"): the program adds the first reading again 10 degrees warmer, sees the series has it, then adds the first readings back, so the summaries stay; an INSERT OR IGNORE would print False.

Where: tests/programs/readings.fb:84

readings.fb adds taken[0..5] again with the values they already have, so the count stays at 108 and the summaries don't change whether the INSERT replaces or ignores. With store.fb's `INSERT OR REPLACE` changed to `INSERT OR IGNORE`, the whole program's output is byte-for-byte readings.out (I ran it). So the replacing behavior that the comment and the store's doc claim is never tested.

```
Import std.core
Import std.db
Import "./readings/source"

// readings.fb's check that "a reading again takes the place of the one before it", run against
// a table that ignores a reading again instead: it prints the same, so the check can't tell.
Function Kept(verb: String): String Throws DatabaseError
    Local db = Try Database.Open(":memory:")
    Try db.Execute("CREATE TABLE readings (city TEXT NOT NULL, hour INTEGER NOT NULL, " +
                   "celsius REAL NOT NULL, PRIMARY KEY (city, hour))")
    Local readings = Readings(26, 36)
    For pass = 0 Until 2
        For reading In pass == 0 ? readings : readings[0..5]
            Try db.Execute("INSERT OR {verb} INTO readings VALUES (?, ?, ?)", reading.City,
                           reading.Hour As Int64, reading.Celsius)
        End For
    End For

    Local row = (Try db.Rows("SELECT count(*), avg(celsius) FROM readings"))[0]
    Return "{row.Integer(0) ?? 0} {row.Real(1) ?? 0.0}"
End Function

Try
    Print("REPLACE:", Try Kept("REPLACE"))
    Print("IGNORE: ", Try Kept("IGNORE"))
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: The program's check should give a different result when the store ignores a repeated reading instead of replacing it.

Actual: REPLACE: 108 14.303703703703704
IGNORE:  108 14.303703703703704 (and readings.fb with OR IGNORE prints readings.out exactly)

Suggested fix: Add a reading again with a changed value, for example Reading(taken[0].City, taken[0].Hour, taken[0].Celsius + 10.0), and print that the count is still 108 and that Series(city)[0] now has the new value. Or add the changed reading and then the original back, so the summaries stay as they are.

Skeptic: p4.fb printed 'REPLACE: 108 14.303703703703704' and 'IGNORE:  108 14.303703703703704'. I also copied the program, changed store.fb's INSERT OR REPLACE to INSERT OR IGNORE, and ran readings.fb: its output diffed IDENTICAL to tests/programs/readings.out. readings.fb:84 adds taken[0..5] again with the same values, so the check at line 81 ('a reading again takes the place of the one before it') can't tell REPLACE from IGNORE.

## program-5 (low): `Image.FormatOf(png) ?? ImageFormat.PNG` prints PNG even when FormatOf doesn't recognize the bytes

Status: fixed in 6768b448 (readings: "a png True"): the line prints Image.FormatOf(png) == ImageFormat.PNG, which is False for bytes FormatOf doesn't know.

Where: tests/programs/readings.fb:95

The chart line is meant to show that the encoded bytes are a PNG. Because of the `?? ImageFormat.PNG` fallback, a Null from FormatOf (bytes that aren't a PNG at all) prints "PNG" too, so this part of the line can never fail.

```
Import std.core
Import std.image

// Not a PNG, so the line readings.fb prints should say so; its ?? says PNG anyway.
Local notPng: UInt8[] = [1, 2, 3, 4]
Print("png", Image.FormatOf(notPng) ?? ImageFormat.PNG)
Print("FormatOf itself:", Image.FormatOf(notPng) == Null)
```

Expected: png - (or some other marker that the format wasn't recognized)
FormatOf itself: True

Actual: png PNG
FormatOf itself: True

Suggested fix: Print `Image.FormatOf(png) == ImageFormat.PNG`, or use a fallback that can't be mistaken for success, such as `?.ToString() ?? "-"`, as the Content-Type lines do.

Skeptic: p5.fb printed 'png PNG' and 'FormatOf itself: True'. So `Image.FormatOf(bytes) ?? ImageFormat.PNG` (readings.fb:95) prints PNG even when FormatOf returns Null for bytes that aren't a PNG, and that part of the check can never fail.

## program-6 (low): The /live handler drops the connection while its WebSocket is still CLOSING, without waiting for the client's Close

Status: fixed in 6768b448 (readings: "served: 1 websocket"): Live reads after its CloseAsync until the client's Close comes back and ReceiveAsync gives Null, and counts the WebSocket served only once it's CLOSED; the program waits for that count before it stops the site.

Where: tests/programs/readings/site.fb:82

Live sends its Close with CloseAsync and returns. The WebSocket is released, and its TcpStream with it, while State is still CLOSING. stdlib.md says Close "is CLOSING until the other side's comes", and that ReceiveAsync answers the other side's Close and then gives Null. So the closing handshake never finishes on the server's side, and `served.Value += 1` (which site.Lives reports) counts a WebSocket whose close is still in progress. The client still sees 1000, so readings.out passes, but the handler is the pattern a reader would copy for a WebSocket route.

```
Import std.core
Import std.net
Import std.net.http
Import std.net.ws

// site.fb's Live, which sends a Close and returns, so the server drops the connection while its
// WebSocket is still CLOSING, before the client's Close comes back.
Async Function Live(request: HttpRequest): HttpResponse
    Try
        Local ws = Try Await WebSocket.AcceptAsync(request)
        Try Await ws.SendAsync("summary")
        Await ws.CloseAsync()
        Print("server, as the handler returns:", ws.State)
    Catch e As NetError
        Return HttpResponse.Of(StatusCode.BAD_REQUEST, Body.Text(e.Message))
    End Try

    Return HttpResponse.Of(StatusCode.OK)
End Function

Async Function Run() Throws Error
    Local server = HttpServer()
    server.Get("/live", Function(request: HttpRequest) => Live(request))
    Try server.Listen(SocketAddress(IpAddress.Loopback, 0))
    Local serving = server.Serve()
    Local live = Try Await WebSocket.ConnectAsync("ws://127.0.0.1:{server.LocalAddress.Port}/live")
    Local first = Try Await live.ReceiveAsync()
    Local last = Try Await live.ReceiveAsync()
    Print("client:", first?.Text ?? "-", last == Null, live.CloseCode ?? 0)
    server.Stop()
    Await serving
End Function

Try
    Await Run()
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: server, as the handler returns: CLOSED
client: summary True 1000

Actual: server, as the handler returns: CLOSING
client: summary True 1000

Suggested fix: After `Await ws.CloseAsync()`, read until the client's Close comes back, for example `While (Try Await ws.ReceiveAsync()) != Null`, and only then count the WebSocket served. Optionally print ws.CloseCode on the server's side as well.

Skeptic: p6.fb printed 'server, as the handler returns: CLOSING' and 'client: summary True 1000'. stdlib.md:1554 says Close 'is CLOSING until the other side's comes', and ReceiveAsync answers the other side's Close and then gives Null. websocket.fb:621-623 closes the stream only once the socket is CLOSED; otherwise the stream goes when the WebSocket is released. std's own tests (http_server_websocket.fb:47-48, http_server_many.fb:43-44, net_websocket.fb:72-80) all call CloseAsync and then ReceiveAsync until Null. site.fb:82-83 returns, and counts the WebSocket as served, while the close is still in progress. The client still gets 1000 here, and I found no race that changes the output (the server has no unread data when it drops the connection). It's the pattern a reader would copy, so I rated it low.

## program-7 (low): readings.fb writes its own HexOf, Same, and SameSummaries, but Hex.Encode and == already do the same

Status: fixed in 6768b448 (readings): HexOf, Same, and SameSummaries are gone, for Hex.Encode, taken == readings, and read == summaries, the WebSocket's summary too.

Where: tests/programs/readings.fb:21

The integration program is meant to show idiomatic std use, but 33 lines of it reimplement what's already there. HexOf duplicates std.text's Hex.Encode, and readings.fb already imports std.text. Same and SameSummaries compare field by field, but Struct and array == already compare that way: `taken == readings` and `read == summaries` give the same answers, False included once a field differs.

```
Import std.core
Import std.crypto
Import std.data
Import std.text
Import "./readings/source"
Import "./readings/store"
Import "./readings/feed"

// readings.fb's HexOf, against what std and the language already give.
Function HexOf(bytes: UInt8[]): String
    Local text = ""
    For b In bytes
        text += "{b:02x}"
    End For

    Return text
End Function

Try
    Local readings = Readings(26, 36)
    Local taken = ReadingsOf(Try Json.Read<Feed>(Json.Write(FeedOf(readings))))
    Local store = Try Store.Open()
    Try store.Add(taken)
    Local summaries = Try store.Summaries()
    Local read = Try Json.Read<Summary[]>(Json.Write(summaries))
    Local hash = Sha256.Hash("abc".Bytes)
    Print(taken == readings, read == summaries, HexOf(hash) == Hex.Encode(hash))
    taken[5] = Reading("Oslo", 5, 99.0)
    read[1] = Summary("Lisbon", 0.0, 0.0, 0.0, 0)
    Print(taken == readings, read == summaries)
Catch e As Error
    Print("failed:", e.Message)
End Try
```

Expected: True True True
False False (the built-ins match the hand-written helpers, so the helpers aren't needed)

Actual: True True True
False False

Suggested fix: Drop HexOf, Same, and SameSummaries. Use Hex.Encode(Sha256.Hash(pixels)), `taken == readings`, and `read == summaries` (likewise for the WebSocket's summary).

Skeptic: p7.fb printed 'True True True' and then 'False False' after one field of each was changed. Struct and array == match Same and SameSummaries, and HexOf(hash) == Hex.Encode(hash) gives the same lowercase hex. readings.fb already imports std.text, yet lines 20-53 write their own versions of these, which goes against its purpose as the idiomatic std program.
