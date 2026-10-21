# Standard Library

Status: what std/ holds today, then [M6's design](#m6s-design), planned 2026-10-20 and built
in M6's 20 steps (see [milestones.md](milestones.md#m6-build-order)). The design is planned,
not built yet.

## Split

- **`std`**: the pure language library; works with or without a window.
- **`engine`**: the game engine modules (see [engine.md](engine.md)).

`Print(String)` is std.core's, in std/core/print.fb, over the runtime's `fb_print` through
`Extern "runtime"`, which only std/ can use. Panic, Assert, RefEquals, the timing Functions, and
the concurrency types stay built in, since the compiler checks and lowers them itself.

## As modules

Each `.fb` file under std/ is a module named by its path: std/collections/map.fb is
`std.collections.map`. Every module imports std/core/'s without saying so, and
std.collections.map is part of every program, so a `{key: value}` literal makes its Map
anywhere; the rest, like `Stack` or array.fb's `Sort`, need `Import std.collections` (or a From
line), and an error says so. A program's own names hide std's for its own code, so it can
declare its own `Stack`, `Map`, or `Error`, and std's code keeps using its own. A std module
only declares things: Functions, Structs, Classes, Enums, Globals, and Consts, and imports what
it uses, as set.fb imports std.collections.map and std.collections.array. A program gets only
the library code it uses. What a program uses is Public: Error, its Message, and its New, the
Interfaces, Iterator, each collection with its Methods and Properties, the iterator Structs
their `Iterate()`s give, with their `Next`, `Handle<T>`'s ReadOnly Index, and std.ffi's `Mem`
and CString Methods. Their Fields and helpers, like `MergeSort`, a Map's `Grow`, or a Handle's
Generation, are private to their files, and what std's files share, like map.fb's `JoinedText`
and `IterateAs`, which set.fb uses, is Package, std's own.

Adding or removing a value while a `For ... In` goes over a collection panics at its next value,
but for a Pool, whose slots stay put. What gives a `T?` for "none", like `Get`, `Find`, and
`Next`, gives a `T??` for a collection of a `T?`, so a stored Null isn't "none".

So far, std/ holds:

| File | Contents |
|---|---|
| std/core/error.fb | `Error`, the Class every error extends |
| std/core/interfaces.fb | `Equatable`, `Comparable`, `Hashable`, `Printable`, and `Numeric`, which `==`, `<`, strings, and generic code go through, and `Sendable`, which the compiler decides |
| std/core/iterator.fb | `Iterator<T>`, which `For ... In` goes over |
| std/core/print.fb | `Print`, over the runtime's `fb_print` |
| std/collections/array.fb | `Remove`, `Sort`, `Filter`, `Map`, and `Find` on `T[]` |
| std/collections/map.fb | `Map<K, V>`, which `{key: value}` makes |
| std/collections/set.fb | `Set<T>` |
| std/collections/stack.fb, queue.fb, deque.fb | `Stack<T>`, `Queue<T>`, and `Deque<T>` |
| std/collections/priority_queue.fb | `PriorityQueue<T>` |
| std/collections/pool.fb | `Pool<T>` and `Handle<T>` |
| std/ffi/mem.fb | `Mem`, raw memory for Unsafe code, over the runtime's `fb_mem_*` |
| std/ffi/cstring.fb | `CString.From`, `Length`, and `Free`, over the runtime's copies |

What the language itself uses, `Error` for Throw and Catch, the Interfaces, `Iterator<T>`, and
`Map<K, V>` for `{key: value}`, is always std's, whatever a program calls by those names.

## M6's design

std is as cross-platform as it can be: its `.fb` files have no platform code, and there are
no modules for one OS. The OS layer is the C++ runtime's, behind `Extern "runtime"` `fb_*`
functions that mean the same on every OS, over bundled C libraries. Every target is 64-bit.

### Bundled C libraries

| Library | License | Backs |
|---|---|---|
| libuv | MIT | std.io, std.os, std.net's sockets, IPC, and DNS, std.time's timers, Async I/O |
| libcurl | curl (MIT-style) | std.net.http's client |
| mbedTLS | Apache-2.0 | std.net.tls, std.crypto's hashes, HMAC, HKDF, PBKDF2, ciphers, and secure random |
| PCRE2 | BSD | std.text.regex |
| utf8proc | MIT | std.text.unicode: categories, case, normalization, graphemes, display width |
| zlib-ng (zlib API) and minizip-ng | zlib | std.compress's Deflate, Zlib, Gzip, and Zip, and curl's gzip |
| zstd, lz4 | BSD | std.compress's Zstd and Lz4 |
| xxHash | BSD | std.crypto's fast non-cryptographic hashes |
| libtommath | Unlicense | std.math.big's BigInt |
| mpdecimal | BSD | std.math.big's Decimal |

Each is fetched with FetchContent and built by the C compiler that builds the runtime, for the
build's target. IANA's time-zone data is embedded in the runtime (only programs that use time
zones link it), so every OS has the same zones.

The platform pieces that are truly necessary, all in the runtime: the OS certificate store,
the local time zone's name, the standard folders (config, data, cache, documents), and opening a
file or URL in the default app.

### Conventions

- **Verbs**: the same word means the same thing on every type: `Count`, `Add`, `Remove` (gives
  whether there was one), `Contains`, `Clear`, `Clone`, `Iterate()`. A lookup gives a `T?`.
- **Missing, failed, or a bug**: not there, or not a number, is `Null`. A real failure (a file
  that won't open, a reset socket, bad JSON) throws the area's Error. A bug (an empty `Pop`, a bad
  index, a negative size) panics.
- **Try twins**: what panics on empty has a `Try` twin that gives a `T?`: `TryPop`, `TryPeek`.
- **Names**: a noun's operations are Static Methods of a type named for it, like `Mem`:
  `File.ReadText`, `Path.Join`, `Env.Get`. Plain math stays Functions: `Sqrt`, `Lerp`, `Min`.
- **Resources** (`File`, `TcpStream`, `Process`, ...) close in `Destroy()`, which refcounting
  runs right away, and have `Close()` for closing early. `Defer x.Close()` needs no new syntax.
- **Threads**: what std sends across threads is `Sendable`. Blocking I/O has an `Async` twin
  that gives a `Task<T>` (`File.ReadTextAsync`), which libuv's loop completes, and a Wait
  rethrows its error.
- **Comments** are the plain-English style std already uses.

### Modules

#### std.core (every module has it)

- Output: `Print`, `Write` (no line break), `PrintError` and `WriteError` (stderr), `Flush`.
- Errors: `Error` gains `Cause: Error?`. Area Errors, each with a `Kind`: `ArgumentError`,
  `IOError` (`NOT_FOUND`, `PERMISSION_DENIED`, `ALREADY_EXISTS`, `UNEXPECTED_END`,
  `INTERRUPTED`, `OTHER`, and `Path: String?`), `ParseError` (`Line`, `Column`), `FormatError`,
  `TimeoutError`, `NetError`.
- Assertions beside Panic and Assert: `AssertEqual(a, b)`, which shows both, `DebugAssert`
  (left out of release builds), `Unreachable()`, `Todo(message)`.
- Nullables: `value.Expect(message)` panics with the message on Null; `value.OrElse(make)`.
- Iterator adapters on `Iterator<T>`: lazy `Map`, `Filter`, `Take`, `Skip`, `TakeWhile`,
  `SkipWhile`, `Enumerate`, `Zip`, `Chain`, `Flatten`, `StepBy`, `Chunks`, `Windows`; consuming
  `ToArray`, `Count`, `Any`, `All`, `Find`, `First`, `Last`, `Fold`, `Reduce`, `ForEach`; `Sum`
  and `Product` where Numeric; `Min` and `Max` where Comparable; `MinBy`, `MaxBy`. Sources:
  `Range(start, finish, step)`, `Repeat(value, count)`, `Empty<T>()`.
- Interfaces: `Iterable<T>`, `Cloneable`, and `Default` (`Static Method Default(): Self`).
- Small things: `Min`, `Max`, `Clamp`, `Swap`; `HashCombine`; `TypeName<T>()` at compile time.

#### std.collections

- Map and Set keep insertion order through removals (tombstones, compacted when they grow).
- `T[]`: `IndexOf`, `LastIndexOf`, `FindIndex`, `FindLast`, `Any`, `All`, `BinarySearch`;
  `First`, `Last`, `RemoveLast`, `TryRemoveLast`; `AddAll`, `InsertAll`, `RemoveRange`,
  `RemoveAll(test)`, `Resize(count, fill)`, `Fill`; `Reverse`, `Swap`, `Shuffle(random)`,
  `SortUnstable` (in place, no allocation); `Distinct`, `Join(separator)`, `Fold`, `Reduce`;
  `Capacity`, `Reserve`, `TrimExcess`.
- Map: `TryAdd`, `Get(key, fallback)`, `GetOrAdd(key, make)`, `Update(key, change)`, `AddAll`,
  `RemoveWhere`, `Find`, `Any`, `MapValues`, `Reserve`, and `[]`.
- Set: `Union`, `Intersect`, `Difference`, `SymmetricDifference`, and in place `AddAll`,
  `RemoveAll`, `KeepOnly`; `IsSubsetOf`, `IsSupersetOf`, `Overlaps`, `SetEquals`, `ToArray`.
- Stack, Queue, Deque, PriorityQueue: Try twins, `ToArray`, `AddAll` (an O(n) heapify for
  PriorityQueue); Deque's `[]`; PriorityQueue's `Drain()`.
- Pool: `IterateHandles()` (handle and value), `Capacity`, `Reserve`.
- New: `SortedMap` and `SortedSet` (B-trees: `First`, `Last`, `Floor`, `Ceiling`, `Range`),
  `RingBuffer`, `Grid<T>(width, height)` (`[x, y]`, `InBounds`, `Neighbors4`, `Neighbors8`),
  `BitSet`, `LruCache`, `Counter`, `MultiMap`.

#### std.text

- String Methods (byte positions): `IsEmpty`, `IsBlank`, `CharCount`; `Contains`,
  `StartsWith`, `EndsWith`, `Find`, `FindLast`, `FindAll`, `Count`; `Slice(start, finish)`,
  `Before`, `After`, `SplitOnce`; `Split`, `SplitN`, `SplitLines`, `SplitWhitespace`, `Join`;
  `Replace`, `ReplaceFirst`, `Trim`, `TrimStart`, `TrimEnd`, `TrimPrefix`, `TrimSuffix`,
  `ToUpper`, `ToLower`, `Repeat`, `PadLeft`, `PadRight`, `Center`; `Compare`,
  `EqualsIgnoreCase`, `NaturalCompare`; `Lines`, `Graphemes`; `String.FromUtf8(bytes)`.
- `StringBuilder`: `Append`, `AppendLine`, `Insert`, `Remove`, `Replace`, `Clear`, `Count`,
  `Reserve`, `ToString`; it's a `Writer`.
- Formatting helpers: `FormatBytes` ("1.2 MB"), `FormatDuration`, `Ordinal` ("3rd").
- Parsing: `ParseInt(text, base)`, `ParseFloat`, `ParseBool`, each a `T?`; a `Scanner` with
  `Line` and `Column`.
- std.text.unicode (utf8proc): `Rune` categories and case, `Normalize` (NFC, NFD, NFKC, NFKD),
  `CaseFold`, `DisplayWidth`; `Utf8` and `Utf16` encode, decode, validate, repair.
- std.text.encoding: Base64 (standard and URL-safe), Hex, Base32, percent-encoding, C-style
  `Escape` and `Unescape`.
- std.text.regex (PCRE2): `Regex(pattern, flags)`, `IsMatch`, `Find`, `FindAll`, captures by
  number and name, `Replace` (with `$1` or a Function), `Split`, `Escape`.
- `Glob.Match`, `WordWrap`, `Dedent`, `Indent`, `Levenshtein`.

#### std.math

- Constants (`Pi`, `Tau`, `E`, `Inf`, `NaN`, `Epsilon`, ...); `MinValue<T>`, `MaxValue<T>`.
- Scalars: `Abs`, `Sign`, `Floor`, `Ceil`, `Round`, `Trunc`, `Fract`, `Mod` (floored), `Rem`;
  `Gcd`, `Lcm`, `IntSqrt`, `IntPow`; `Lerp`, `InverseLerp`, `Remap`, `SmoothStep`, `Wrap`,
  `PingPong`, `MoveTowards`, `IsClose`; powers, logs, trig, and hyperbolics; `IsNaN`,
  `IsFinite`, `CopySign`, `Fma`, `ToBits`, `FromBits`.
- std.math.bits: `PopCount`, `LeadingZeros`, `TrailingZeros`, `RotateLeft`, `RotateRight`,
  `ByteSwap`, `IsPowerOfTwo`, `NextPowerOfTwo`, `AlignUp`, `AlignDown`; saturating arithmetic
  beside the Checked and Wrapping Methods integers have.
- Linear algebra: `Vec2`, `Vec3`, `Vec4`, `Vec2i`, `Vec3i`, `Quat`, `Mat3`, `Mat4`, `Color`,
  with operators, `Dot`, `Cross`, `Length`, `Normalize`, `Lerp`, `Slerp`, `Inverse`,
  `LookAt`, `Perspective`, `Orthographic`, `Decompose`, and the rest.
- Geometry: `Rect`, `AABB`, `Sphere`, `Ray`, `Plane`, `Frustum` (`Contains`, `Intersects`,
  `Raycast`).
- Random: `Random(seed)` (PCG or xoshiro), with `Int`, `Float`, `Bool`, `Chance`, `Pick`,
  `WeightedPick`, `Shuffle`, `Sample`, `Normal`, `InsideUnitCircle`, `UnitVector3`, `Fork`,
  and a default generator per thread (a `ThreadLocal Global`) behind `Rand`, `RandInt`, and
  `RandFloat`.
- Noise (Perlin, Simplex, Value, Worley, Fbm), easing (In, Out, InOut of each curve), stats
  (`Mean`, `Median`, `Variance`, `StdDev`, `Percentile`).
- std.math.big: `BigInt` (libtommath), `Decimal` (mpdecimal), `Rational`, `Complex`.

#### std.io

- `Reader`, `Writer`, and `Seeker` Interfaces, with `Copy`, `ReadAll`, `ReadExact`; buffered,
  memory, and string readers and writers; `BinaryReader` and `BinaryWriter` with endianness.
- Console: `ReadLine`, `ReadKey`, `ReadPassword`, `IsTerminal`, `TerminalSize`, and styled
  output that turns itself off when it isn't a terminal or `NO_COLOR` is set.
- `File`: `ReadText`, `ReadBytes`, `ReadLines`, `WriteText`, `WriteBytes`, `AppendText`,
  `Exists`, `Copy`, `Move`, `Delete`, `Size`; `File.Open` with options; `Seek`, `Flush`, `Sync`,
  `Lock`, `Metadata`; each with an `Async` twin.
- `Path` (a value: `Join` and `/`, `Parent`, `FileName`, `Stem`, `Extension`, `Normalize`,
  `RelativeTo`, `Absolute`), `/` accepted on every OS.
- `Dir`: `Create`, `CreateAll`, `Remove`, `RemoveAll`, `Entries`, `Walk`, `Glob`, the current
  folder, and temporary files and folders that go away when closed.
- `FileWatcher` (libuv): created, changed, removed, renamed.

#### std.time

- `Duration`, `Instant` (monotonic), `Sleep`, `Stopwatch`, `Measure(f)`.
- `Date`, `Time`, `DateTime` with time zones (embedded IANA data): arithmetic that clamps
  (Jan 31 plus a month is Feb 28 or 29), `StartOfDay` and the like, `Format` and `Parse`
  patterns, ISO 8601 and RFC 2822.
- Timers on libuv's loop (`Timer.After`, `Timer.Every`), `Deadline` for blocking calls,
  `FixedStep(rate)` for fixed-step game loops, `RateLimiter`, `Debounce`.

#### std.os

- `Args()`, and an `ArgParser` (flags, options, positionals, subcommands, generated help),
  which fb's own CLI can use.
- `Env`: `Get`, `Set`, `Unset`, `All`, `Expand`.
- `Process`: `Exit`, `Id`; `Command(program)` with arguments, environment, folder, and piped or
  inherited streams; `Spawn` gives a `Child` (`Wait`, `TryWait`, `Kill`, its streams);
  `Output()` and `Status()`. No shell: a program and its arguments, the same on every OS.
- Signals that every OS has: `OnInterrupt` (Ctrl+C) and `OnTerminate`.
- System: `Platform`, `Arch`, `CpuCount`, `TotalMemory`, `Hostname`, `UserName`.
- Folders: `ExecutablePath`, `HomeFolder`, `ConfigFolder`, `DataFolder`, `CacheFolder`,
  `TempFolder`, `DocumentsFolder`; `OpenDefault(pathOrUrl)`.

#### std.concurrent (beside the built-ins, which stay built in)

- `RwLock<T>`, `Semaphore`, `Barrier`, `WaitGroup`, `Once`, `ConcurrentMap`, `ConcurrentQueue`.
- `WhenAll`, `WhenAny`, `Cancellation` (a token blocking calls and Tasks take).

#### std.data

- `Value` (Null, Bool, Number, String, Array, Object) with typed accessors and `Path("a.b[0]")`.
- JSON: `Parse`, `Stringify(indent)`, a streaming reader and writer over std.io.
- `@Serialize`, with `Rename`, `Skip`, and `Default`, through the same Encoder and Decoder
  Interfaces every format implements.
- TOML, CSV, and INI after JSON.

#### std.crypto and std.compress

- Hashes (mbedTLS): `Sha1`, `Sha256`, `Sha512`, `Md5` (only for checksums, and said so), `Hmac`,
  `Hkdf`, `Pbkdf2`; `SecureRandom`; `ConstantTimeEquals`; non-cryptographic `Crc32`, `XxHash`.
- Compression as Readers and Writers: `Deflate`, `Zlib`, `Gzip`, `Zstd`, `Lz4`; `Zip` archives
  to read and write.

#### std.net

- `IpAddress`, `SocketAddress`, `Dns.Resolve`.
- `TcpListener` and `TcpStream` (Reader and Writer), `UdpSocket`, with timeouts and Async twins.
- `IpcListener.Listen(name)` and `IpcStream.Connect(name)`: one IPC type, named pipes on Windows
  and Unix sockets elsewhere, chosen by libuv, never by the program.
- `Url` and `QueryParams`.
- `TlsStream` over any stream (mbedTLS, the OS's roots by default), and a TLS listener.
- std.net.http: `Method`, `StatusCode`, `Headers`, `Body`; `HttpClient` and `Http.Get` and the
  rest (libcurl: redirects, proxies, cookies, gzip, HTTP/2), `Download(url, path, progress)`;
  `HttpServer` with a `Router`, path parameters, middleware, and static files.
- std.net.ws: WebSocket client and server (the server from an HttpServer handler).

#### std.log and std.debug

- `Logger` with levels, sinks (console, file, rotating file), structured fields, and a default
  logger.
- `Breakpoint()`, `IsDebuggerAttached()`, `Dump(value)`; stack traces wait for debug info
  (spec 09 already says so).

#### std.ffi (after M5's reorganization)

- `Mem.AllocAligned`, `Mem.FreeAligned`, `Mem.AllocUninitialized` (no zeroing).
- `Buffer<T>(count)`: native memory it owns, zeroed, at a stable address, freed when its last
  reference goes; bounds-checked `[]`, `Count`, `Slice(start, finish)` views that keep it alive,
  `CopyTo`, `ToArray`, and `Pointer` for C in Unsafe code. `Buffer<T>.View(pointer, count)`, in
  Unsafe code only, wraps memory C owns, on the promise that it outlives the view. std.io reads
  into and writes from Buffers without copying.
- Loading a library at run time (`Library.Open`) waits for M8's dynamic `[native]` libraries.

### Not in std

- Platform-only modules (no std.os.windows or std.os.posix), `Fork`, the registry, a shell
  command, and signals only one OS has.
- Thread, Task, Channel, Mutex, Atomic, Parallel For, Scheduler, and Async stay built in.
- `std.test` comes with M7's `fb test`.

The standard library is written in FutureBlitz itself, over the runtime where it needs the
OS.
