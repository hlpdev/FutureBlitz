# Standard Library

Status: what std/ holds today, then [M6's design](#m6s-design), planned 2026-10-20 and built
in M6's 32 steps (see [milestones.md](milestones.md#m6-build-order)). Step 1's part is built;
the rest is planned.

## Split

- **`std`**: the pure language library; works with or without a window.
- **`engine`**: the game engine modules (see [engine.md](engine.md)).

The console Functions, `Print` and the rest, are std.core's, in std/core/print.fb, over the
runtime's `fb_write` and its I/O thread's `fb_io_` functions through `Extern "runtime"`, which
only std/ can use. Panic, Assert, DebugAssert, Unreachable, Todo,
RefEquals, the timing Functions, and the concurrency types stay built in, since the compiler
checks and lowers them itself.

## As modules

Each `.fb` file under std/ is a module named by its path: std/collections/map.fb is
`std.collections.map`. Every name needs its Import, std.core's too, so hello world is
`Import std.core` and then `Print("hi")`, and an error says which Import a name needs.
std.collections.map is part of every program, so a `{key: value}` literal makes its Map
anywhere; the rest, like `Stack` or array.fb's `Sort`, need `Import std.collections` (or a From
line). A program's own names hide std's for its own code, so it can
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
| std/core/error.fb | `Error`, the Class every error extends, with its `Cause` and `Place`; `Place`; and each area's Error and its Kind: `ArgumentError`, `IOError`, `ParseError`, `FormatError`, `TimeoutError`, `NetError`, `DatabaseError` |
| std/core/assert.fb | `AssertEqual`, an Assert that says what it compared |
| std/core/interfaces.fb | `Equatable`, `Comparable`, `Hashable`, `Printable`, and `Numeric`, which `==`, `<`, strings, and generic code go through; `Cloneable` and `Defaultable`; and `Sendable`, which the compiler decides |
| std/core/iterator.fb | `Iterator<T>`, which `For ... In` goes over, with its lazy adapters and its consumers as Final defaults; `Iterable<T>`, which std's collections are; `Repeated` and `Empty` |
| std/core/range.fb | `Range<T>`, which `0 Until 10`, `1 To 10`, and `Step` make |
| std/core/print.fb | `Print`, `Write`, `PrintError`, `WriteError`, `Flush`, `Input`, and `InputAsync`, over the runtime's `fb_write` and its I/O thread |
| std/core/values.fb | `Min`, `Max`, `Clamp`, `Swap`, and `HashCombine` |
| std/collections/array.fb | `T[]`'s `Remove`, `Sort`, `SortUnstable`, `Filter`, `Map`, `Find`, `FindLast`, `FindIndex`, `IndexOf`, `LastIndexOf`, `BinarySearch`, `Any`, `All`, `First`, `Last`, `RemoveLast`, `TryRemoveLast`, `AddAll`, `InsertAll`, `RemoveRange`, `RemoveAll`, `Resize`, `Fill`, `Reverse`, `Swap`, `Distinct`, `Join`, `Fold`, and `Reduce` |
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

Planned with the user on 2026-10-20 and, module by module, on 2026-10-21. std is batteries
included: it covers what Python's standard library and its most common packages do together, so
a program needs FFI only for a C library of its own choosing. Packages (M7's registry) are for
niche domains, like PDF or cloud SDKs.

std is as cross-platform as it can be: its `.fb` files have no platform code, and there are no
modules for one OS. The OS layer is the C++ runtime's, behind `Extern "runtime"` `fb_*`
functions that mean the same on every OS, over bundled C libraries. Every target is 64-bit.

Every std name needs an Import, std.core's too, so hello world is `Import std.core` and then
`Print("hi")`. The language's own features (Throw and Catch, `For ... In`, `==`,
`{key: value}`) work without one, and the compiler's built-ins (Panic, Assert, Spawn, SizeOf,
and the rest) aren't std's.

### Bundled C libraries

| Library | License | Backs |
|---|---|---|
| libuv | MIT, with parts under BSD-2-Clause and ISC | std.io, std.os, std.net's sockets, IPC, and DNS, std.time's timers, Async I/O |
| libcurl | curl (MIT-style) | std.net.http's client |
| mbedTLS | Apache-2.0 | std.net.tls, std.crypto's hashes, HMAC, HKDF, PBKDF2, ciphers, signatures, and secure random |
| PCRE2 | BSD | std.text.regex, and the compiler's check of regex literals |
| utf8proc | MIT | std.text.unicode: categories, case, normalization, graphemes, display width |
| zlib-ng (zlib API) and minizip-ng | zlib | std.compress's Deflate, Zlib, Gzip, and Zip, and curl's gzip |
| zstd, lz4 | BSD | std.compress's Zstd and Lz4 |
| liblzma (xz-utils) | 0BSD | std.compress's XZ/LZMA and 7z reading |
| xxHash | BSD | std.crypto's fast non-cryptographic hashes |
| BLAKE3, Argon2 | CC0 / Apache-2.0 | std.crypto's BLAKE3 and Argon2id |
| libtommath | Unlicense | std.math.big's BigInt |
| mpdecimal | BSD | std.math.big's Decimal |
| SQLite | public domain | std.db |
| stb_image, stb_image_write, stb_truetype | public domain / MIT | std.image's codecs and font drawing |
| libwebp | BSD | std.image's WebP |
| nanosvg | zlib | std.image's SVG |
| md4c | MIT | std.text.markdown |

Each is fetched with FetchContent in third_party/ and built by the C compiler that builds the
runtime, for the build's target, as a separate archive, beside an archive of the runtime's
functions over it (the `fb_io_` ones over libuv), so a program that calls none of those links
none of the library. After a build that links any, `fb` prints which ones and their licenses;
shipping the notices is the programmer's job. IANA's time-zone data is embedded in the runtime
(only programs that use time zones link it), so every OS has the same zones.

The runtime's I/O thread runs libuv's loop. It starts with a program's first I/O request and
stops as the program ends, before the leak report, answering what still waits with nothing. A
request that gives a Task finishes it there, and the code that Awaits or Waits for it resumes on
its own thread, as the scheduling rule in spec 10 says.

The platform pieces that are truly necessary, all in the runtime: the OS certificate store,
the local time zone's name, the standard folders (config, data, cache, documents), and opening a
file or URL in the default app.

### Conventions

- **Verbs**: the same word means the same thing on every type: `Count`, `Add`, `Remove` (gives
  whether there was one), `Contains`, `Clear`, `Clone`, `Iterate()`. A lookup gives a `T?`, and
  a `T??` over values that can be Null.
- **Missing, failed, or a bug**: not there, or not a number, is `Null`. A real failure (a file
  that won't open, a reset socket, bad JSON) throws the area's Error. A bug (an empty `Pop`, a bad
  index, a negative size) panics, at the caller's line (`@PanicsAtCaller`).
- **Try twins**: what panics on empty has a `Try` twin that gives a `T?`: `TryPop`, `TryPeek`.
- **Names**: a noun's operations are Static Methods of a type named for it: `File.ReadText`,
  `Path.Join`, `Env.Get`, `Int.Parse`. Plain math stays Functions: `Sqrt`, `Lerp`, `Min`.
- **Resources** (`File`, `TcpStream`, `Process`, ...) close in `Destroy()`, which refcounting
  runs right away, and have `Close()` for closing early. `Defer x.Close()` needs no new syntax.
- **Threads**: what std sends across threads is `Sendable`. Blocking I/O has an `Async` twin
  (`File.ReadTextAsync`) that gives a `Task<T>`, which libuv's loop completes; the code after an
  Await resumes where it started, and a Wait rethrows its error.
- **Text** is invariant: formatting, `ToString`, parsing, and the data formats use `.` for
  decimals and `,` for thousands on every machine; a `Locale` is only ever explicit.
- **Comments** are the plain-English style std already uses.

### Modules

#### std.core

- Output: `Print(values...)` prints any Printable values with spaces between and a line break;
  `Write` is the same without the break; `PrintError` and `WriteError` go to stderr; `Flush`.
- `Input(prompt)` reads a line, giving a `String?` that's Null at the end of input, and
  nothing else runs meanwhile; `InputAsync(prompt)` gives a `Task<String?>` of the line, which
  the I/O thread finishes. Lines go to both in the order they're asked for.
- Errors: `Error` has `Message`, `Cause: Error?`, and `Place` (file, line, column), which Throw
  fills in debug and release builds. An uncaught error prints its message, place, and Cause
  chain, then exits with code 1. Each area has one Error with a `Kind` and its details:
  `ArgumentError` (`MISSING`, `UNKNOWN`, `INVALID`, `OTHER`, and `Name: String?`), `IOError`
  (`NOT_FOUND`, `PERMISSION_DENIED`, `ALREADY_EXISTS`, `UNEXPECTED_END`, `INTERRUPTED`,
  `OTHER`, and `Path: String?`), `ParseError` (`SYNTAX`, `UNEXPECTED_END`, `WRONG_TYPE`,
  `OUT_OF_RANGE`, `OTHER`, and `Line`, `Column`), `FormatError` (`BAD_TEMPLATE`,
  `MISSING_VALUE`, `BAD_VALUE`, `OTHER`), `TimeoutError` (one kind), `NetError` (`REFUSED`,
  `RESET`, `UNREACHABLE`, `HOST_NOT_FOUND`, `CLOSED`, `TLS`, `OTHER`, and
  `Address: String?`), `DatabaseError` (`CONSTRAINT`, `BUSY`, `QUERY`, `CORRUPT`, `OTHER`).
  Each Kind is an Enum named for its Error, like `IOErrorKind`, and a later step can add to
  its area's.
- Assertions beside Panic and Assert: `AssertEqual(a, b)`, which shows both, `DebugAssert`
  (left out of release builds), `Unreachable()`, `Todo(message)`.
- Parsing: Static Methods on the number types give a `T?`: `Int.Parse("42")`,
  `Int.Parse("ff", 16)`, `Float.Parse`, `Bool.Parse`.
- Nullables: `value.Expect(message)` panics with the message on Null; `value.OrElse(make)`.
- Ranges are values: `0 Until 10`, `1 To 10`, and `Step`, stored, passed, and iterated, with
  every adapter.
- A debug form for every value, from reflection: `"{x:?}"` and `Dump(x)`, which indents what's
  nested. Printable stays the form for people.
- Iterator adapters on `Iterator<T>`, lazy: `Map`, `Filter`, `Take`, `Skip`, `TakeWhile`,
  `SkipWhile`, `Enumerate`, `Zip`, `Chain`, `Flatten`, `StepBy`, `Chunks`, `Windows`,
  `OrderBy`, `OrderByDescending`; consuming: `ToArray`, `Count`, `Any`, `All`, `Find`, `First`,
  `Last`, `Fold`, `Reduce`, `ForEach`, `Sum` and `Product` where Numeric, `Min` and `Max` where
  Comparable, `MinBy`, `MaxBy`. Sources: `Repeated(value, count)`, `Empty<T>()`. A collection's
  own `Filter` and `Map` stay eager and give arrays.
- Interfaces: `Iterable<T>`, `Cloneable`, and `Defaultable` (`Static Method DefaultValue():
  Self`), since `Default` is Select's keyword.
- Small things: `Min`, `Max`, `Clamp`, `Swap`; `HashCombine`; `TypeName<T>()` at compile time.

#### std.collections

- Map and Set keep insertion order through removals (each entry links to the ones added
  before and after it).
  They're `==` by contents, and Hashable when their parts are.
- `T[]`: `IndexOf`, `LastIndexOf`, `FindIndex`, `FindLast`, `Any`, `All`, `BinarySearch`;
  `First`, `Last`, `RemoveLast`, `TryRemoveLast`; `AddAll`, `InsertAll`, `RemoveRange`,
  `RemoveAll(test)`, `Resize(count, fill)`, `Fill`; `Reverse`, `Swap`, `Shuffle(random)`,
  `SortUnstable` (in place, no allocation); `Distinct`, `Join(separator)`, `Fold`, `Reduce`;
  `Capacity`, `Reserve`, `TrimExcess`.
- Sorting: `Sort()`, `Sort(before)`, `SortBy(key)`, `SortByDescending(key)`, and `ThenBy` and
  `ThenByDescending` for ties, stable and in place.
- Views that don't copy: `items.View(a, b)` is an `ArrayView<T>` that keeps its array alive, and
  `ReadOnly(x)` gives a `ReadOnlyArray`, `ReadOnlyMap`, or `ReadOnlySet` to hand out. Slices
  (`items[a..b]`) stay copies.
- Map: `[]`, `TryAdd`, `Get(key, fallback)`, `GetOrAdd(key, make)`, `Update(key, change)`,
  `AddAll`, `RemoveWhere`, `Find`, `Any`, `MapValues`, `Reserve`; `Map<K, V>(default: v)`, whose
  `[]` reads give the default without adding it, so `counts[word] += 1` works.
- Set: `Union`, `Intersect`, `Difference`, `SymmetricDifference`, and in place `AddAll`,
  `RemoveAll`, `KeepOnly`; `IsSubsetOf`, `IsSupersetOf`, `Overlaps`, `SetEquals`, `ToArray`.
- Stack, Queue, Deque, PriorityQueue: Try twins, `ToArray`, `AddAll` (an O(n) heapify for
  PriorityQueue); Deque's `[]`; PriorityQueue's `Drain()`.
- Pool: `IterateHandles()` (handle and value), `Capacity`, `Reserve`.
- More: `SortedMap` and `SortedSet` (B-trees: `First`, `Last`, `Floor`, `Ceiling`, `Range`),
  `RingBuffer`, `Grid<T>` and `Grid3<T>` (`[x, y]`, `InBounds`, `Neighbors4`, `Neighbors8`),
  `BitSet`, `LruCache`, `Counter`, `MultiMap`, `Trie`, `LinkedList` (stable nodes),
  `IntervalTree`; spatial `SpatialHash`, `QuadTree`, and `Octree`; and `ImmutableArray`,
  `ImmutableMap`, and `ImmutableSet`, which share structure on change and are Sendable.

#### std.text

- `s.Length` is the UTF-8 byte count, O(1), and positions are byte offsets; `CharCount` and
  `GraphemeCount` count code points and what people see. Slicing mid-character panics.
- `Char` is a built-in type, a 32-bit code point, which a literal like `'a'` or `'\u{1F600}'`
  makes; `s.Chars` gives a `Char[]`, and a Char has `IsLetter`, `IsDigit`, `IsWhitespace`,
  `IsUpper`, `ToUpper`, `Code`, and the rest.
- String Methods: `IsEmpty`, `IsBlank`; `Contains`, `StartsWith`, `EndsWith`, `Find`,
  `FindLast`, `FindAll`, `Count`; `Slice(start, finish)`, `Before`, `After`, `SplitOnce`;
  `Split`, `SplitN`, `SplitLines`, `SplitWhitespace`, `Join`; `Replace`, `ReplaceFirst`, `Trim`,
  `TrimStart`, `TrimEnd`, `TrimPrefix`, `TrimSuffix`, `ToUpper`, `ToLower` (full Unicode),
  `Repeat`, `PadLeft`, `PadRight`, `Center`; `Compare`, `EqualsIgnoreCase`, `NaturalCompare`;
  `Lines`, `Graphemes`; `String.FromUtf8(bytes)`.
- `StringBuilder`: `Append`, `AppendLine`, `Insert`, `Remove`, `Replace`, `Clear`, `Length`,
  `Reserve`, `ToString`, and `[]`; it's a `Writer`. A `Scanner` reads text with `Line` and
  `Column`.
- std.text.unicode (utf8proc): categories and case, `Normalize` (NFC, NFD, NFKC, NFKD),
  `CaseFold`, `DisplayWidth`.
- std.text.encoding: UTF-8, UTF-16 (LE and BE), UTF-32, ASCII, Latin-1, and Windows-1252, with
  BOM detection; Base64 (standard and URL-safe), Hex, Base32, percent-encoding, HTML escaping,
  and C-style `Escape` and `Unescape`.
- std.text.regex (PCRE2, with match limits that throw instead of hanging): `Regex(pattern,
  flags)` with `IsMatch`, `Find`, `FindAll`, captures by number and by name (`m["year"]`),
  `Replace` (with `$1` or a Function), `Split`, and `Escape`; the String shortcuts
  `s.Matches(pattern)`, `s.ReplaceAll(pattern, with)`, and `s.SplitBy(pattern)` cache recent
  patterns. The compiler checks a literal pattern and reports PCRE2's error at it.
- std.text.template: Mustache-style `{{name}}`, `{{#each items}}`, `{{#if x}}`/`{{else}}`,
  partials, and format specs inside (`{{price:.2}}`), with HTML escaping on for `.html`; values
  from a Map, a Struct (by reflection), or a JSON Value.
- std.text.markdown (md4c): CommonMark with tables and task lists, to HTML, and a document tree
  to walk.
- std.text.diff: line and character diffs (Myers), unified-diff text, and a three-way merge.
- Fuzzy matching (scores and matched positions, like fzf), and `Humanize`: plurals ("3
  enemies"), relative time ("3 minutes ago"), ordinals, number words, and case styles
  (snake_case, camelCase, PascalCase, kebab-case).
- `Glob.Match`, `WordWrap`, `Dedent`, `Indent`, `Levenshtein`; `FormatBytes` ("1.2 MB") and
  `FormatDuration`.

#### std.math

- Constants (`Pi`, `Tau`, `E`, `Inf`, `NaN`, `Epsilon`, ...); `MinValue<T>`, `MaxValue<T>`.
  `Int` stays 32 bits, and `Long` is 64; byte sizes are Int64s.
- Scalars: `Abs`, `Sign`, `Floor`, `Ceil`, `Round`, `Trunc`, `Fract`, `Mod` (floored), `Rem`;
  `Gcd`, `Lcm`, `IntSqrt`, `IntPow`; `Lerp`, `InverseLerp`, `Remap`, `SmoothStep`, `Wrap`,
  `PingPong`, `MoveTowards`, `IsClose`; powers, logs, trig, and hyperbolics; `IsNaN`,
  `IsFinite`, `CopySign`, `Fma`, `ToBits`, `FromBits`.
- Angles are radians everywhere: trig, rotations, and `Quat`. `Deg(90)` and `Rad(x)` convert,
  as do `ToDegrees` and `ToRadians`, and `quat.ToEulerDegrees()` gives degrees for editors.
- std.math.bits: `PopCount`, `LeadingZeros`, `TrailingZeros`, `RotateLeft`, `RotateRight`,
  `ByteSwap`, `IsPowerOfTwo`, `NextPowerOfTwo`, `AlignUp`, `AlignDown`; saturating arithmetic
  beside the Checked and Wrapping Methods integers have.
- Linear algebra, Float32, right-handed, Y up, -Z forward, column-major, 0..1 depth, with SIMD
  where the target has it: `Vec2`, `Vec3`, `Vec4`, `Vec2i`, `Vec3i`, `Quat` (x, y, z, w),
  `Mat3`, `Mat4`, with Operators, `Dot`, `Cross`, `Length`, `Normalize`, `Lerp`, `Slerp`,
  `Inverse`, `LookAt`, `Perspective`, `Orthographic`, `Decompose`, and the rest.
- `Color` (four Float32s, linear RGB and alpha) and `Color32` (four bytes, sRGB), with named
  colors, `Color.Hex("#ff8800")`, HSV, HSL, and OKLab, converting between them with the right
  gamma.
- Geometry: `Rect`, `AABB`, `Sphere`, `Ray`, `Plane`, `Frustum` (`Contains`, `Intersects`,
  `Raycast`); polygons: triangulation with holes, convex hull, point-in-polygon, offsetting, and
  union, intersection, and difference.
- Curves: Bezier (quadratic and cubic), Catmull-Rom, Hermite, and B-splines in 2D and 3D, with
  arc-length sampling.
- Random: `Random(seed)` (PCG), with `Int`, `Float`, `Bool`, `Chance`, `Pick`, `WeightedPick`,
  `Shuffle`, `Sample`, `Normal`, `InsideUnitCircle`, `UnitVector3`, `Fork`, `State`; and `Rand`,
  `RandInt`, and `RandFloat` over a default generator per thread (a `ThreadLocal Global`),
  seeded randomly each run until `SeedRandom(n)`.
- Noise (Perlin, Simplex, Value, Worley, Fbm), easing (In, Out, and InOut of each curve), and
  stats (`Mean`, `Median`, `Variance`, `StdDev`, `Percentile`, `Histogram`).
- Fixed point: `Fixed32` (16.16) and `Fixed64` (32.32), with Operators, and `Sqrt`, `Sin`, and
  `Cos` that give the same bits on every CPU, for lockstep multiplayer and replays.
- Units: typed quantities like `Meters`, `Seconds`, and `MetersPerSecond`, which the compiler
  keeps apart and combines (`Meters / Seconds` is `MetersPerSecond`).
- std.math.big: `BigInt` (libtommath), `Decimal` (mpdecimal), `Rational`, `Complex`.

#### std.io

- `Reader`, `Writer`, and `Seeker` Interfaces, with Async twins, and `Copy`, `ReadAll`,
  `ReadExact`; buffered, memory, and string readers and writers; `BinaryReader` and
  `BinaryWriter` with endianness.
- `Path` is a value: `Join` and `/`, `Parent`, `FileName`, `Stem`, `Extension`, `Normalize`,
  `RelativeTo`, `Absolute`; `/` works on every OS, and a String converts to a Path wherever one
  is expected.
- `File`: `ReadText`, `ReadBytes`, `ReadLines`, `WriteText`, `WriteBytes`, `AppendText`,
  `Exists`, `Copy`, `Move`, `Delete`, `Size`; `WriteTextAtomic` and `WriteBytesAtomic`, which
  write a temporary file and rename it; `File.Open` with options; `Seek`, `Flush`, `Sync`,
  `Metadata`; `FileLock`; each with an Async twin. Reading splits lines on `\n` and `\r\n`, and
  writing uses `\n` on every OS, unless asked for `\r\n`.
- `MappedFile` maps a file as a `Buffer<UInt8>`, read-only or writable.
- `Dir`: `Create`, `CreateAll`, `Remove`, `RemoveAll`, `Entries`, `Walk`, `Glob`, the current
  folder, and temporary files and folders that go away when closed. `FileWatcher` (libuv):
  created, changed, removed, renamed.
- A virtual file system: mount folders, zips, and embedded folders under one tree, read through
  the same API as files; the engine's pak files build on it.
- `Embed("path")`, `EmbedText("path")`, and `EmbedFolder("path")`: built-ins, checked at compile
  time, that put a file's bytes or text, or a read-only folder, in the program.
- `Preferences("MyGame")`: a per-app key-value store in the OS's config folder, saved
  atomically.
- Console: `ReadKey`, `ReadPassword`, `IsTerminal`, `TerminalSize`, and styled output that turns
  itself off when it isn't a terminal or `NO_COLOR` is set. (`Input` is std.core's.)

#### std.time

- `Duration`, written `2.Seconds`, `1.5.Minutes`, `500.Milliseconds`; the timing built-ins take
  one too: `Await Wait(500.Milliseconds)`.
- A type for each meaning: `Timestamp` (a point in time, UTC), `Date`, `TimeOfDay`,
  `LocalDateTime` (no zone), and `ZonedDateTime`, with `TimeZone` (embedded IANA data, and the
  OS's local zone); arithmetic that clamps (Jan 31 plus a month is Feb 28 or 29), `StartOfDay`
  and the like, `Format` and `Parse` patterns, ISO 8601 and RFC 2822.
- `Stopwatch` and `Measure(f)` on the monotonic clock; `Sleep`.
- Timers on libuv's loop (`Timer.After`, `Timer.Every`), `Deadline` for blocking calls,
  `FixedStep(rate)` for fixed-step game loops, `RateLimiter`, `Debounce`.

#### std.os

- Arguments: a Struct with `@Command`, `@Positional`, `@Option`, and `@Flag` attributes, which
  `Args.Parse<T>()` fills by reflection, with generated help, errors, and subcommands; a builder
  underneath for options made at run time. fb's own CLI can use it.
- `Env`: `Get`, `Set`, `Unset`, `All`, `Expand`, and `Env.Load(".env")` for `.env` files.
- `Process`: `Exit`, `Id`; `Command(program)` with arguments, environment, folder, and piped or
  inherited streams; `Spawn` gives a `Child` (`Wait`, `TryWait`, `Kill`, its streams);
  `Output()` and `Status()`. No shell: a program and its arguments, the same on every OS.
- Signals that every OS has: `OnInterrupt` (Ctrl+C) and `OnTerminate`.
- System: `Platform`, `Arch`, `CpuCount`, `TotalMemory`, `Hostname`, `UserName`.
- `Folders`: `Executable`, `Home`, `Config`, `Data`, `Cache`, `Temp`, `Documents`;
  `OpenDefault(pathOrUrl)`.

#### std.concurrent (beside the built-ins, which stay built in)

- `RwLock<T>`, `Semaphore`, `Barrier`, `WaitGroup`, `Once`, `Lazy<T>` (made once, on first use,
  from any thread), `ConcurrentMap`, `ConcurrentQueue`.
- `WhenAll`, `WhenAny`, `TaskSource` (a Task finished by hand), and `Cancellation`, a token
  blocking calls and Tasks take.
- `Select` over several Channels and a timeout.
- `ParallelSort`, `ParallelMap`, `ParallelReduce`, and `ParallelForEach` on the job pool.

#### std.log

- `Logger` with levels, sinks (console, file, rotating file, JSON lines), and structured fields.
  The default logger prints readable, timestamped lines to stderr, colored only on a terminal,
  and `FB_LOG=debug` or module filters (`FB_LOG=debug,net=trace`) set levels at run time.

#### std.events

- `Event<T>`, held as a Field: `Emit(value)` calls its handlers in order. `Subscribe(handler)`
  gives a `Subscription` to Cancel, and `Subscribe(owner, handler)` ties a handler to an owner
  held weakly, passed to the handler so it captures nothing, and dropped when the owner goes.

#### std.data

- `Value` (Null, Bool, Int, Float, String, Array, Object) with typed accessors and
  `Path("a.b[0]")`; JSON integers stay exact (a Long), and others are Float64.
- Serialization is automatic: a Struct or Class whose fields are serializable is, by
  compile-time reflection, every Field included, Private ones too, Properties not. `@Rename`,
  `@Skip`, `@Default`, and `@Flatten` adjust it, a type can write its own Encode and Decode, and
  every format goes through the same Encoder and Decoder Interfaces: `Json.Write(save, indent:
  2)`, `Json.Read<Save>(text)`. Reading into a typed field converts, and throws a ParseError
  when it doesn't fit.
- Formats: JSON (with a streaming reader and writer), TOML, CSV, INI, XML (a tree, a streaming
  reader and writer, and XPath-lite), YAML (1.2's core schema, no arbitrary tags), and
  MessagePack. A save can carry a version, and `@Since(2)` and `@Renamed("old")` keep old saves
  loading after a game changes.
- Protocol Buffers: fb reads `.proto` files at compile time and generates the types and their
  binary encoding.

#### std.crypto

- Hashes (mbedTLS): `Sha1`, `Sha256`, `Sha512`, `Md5` (only for checksums, and said so), and
  `Blake3`; `Hmac`, `Hkdf`, `Pbkdf2`; `SecureRandom`; `ConstantTimeEquals`; non-cryptographic
  `Crc32` and `XxHash`.
- Passwords: `Argon2id` and `Bcrypt`, whose `Hash` and `Verify` pick salts and parameters.
- Authenticated encryption: AES-256-GCM and ChaCha20-Poly1305, through `Seal` and `Open`, which
  won't reuse a nonce.
- Keys and signatures: Ed25519 and ECDSA P-256 sign and verify, RSA verify, X25519 key
  exchange, and PEM keys and certificates.

#### std.compress

- `Deflate`, `Zlib`, `Gzip`, `Zstd`, `Lz4`, and `XZ` as Readers and Writers; `Zip` and `Tar`
  (with .tar.gz, .tar.zst, and .tar.xz) to read and write; `.7z` to read.

#### std.net

- `IpAddress`, `SocketAddress`, `Dns.Resolve`.
- `TcpListener` and `TcpStream` (Reader and Writer), `UdpSocket`, with timeouts and Async twins.
- `IpcListener.Listen(name)` and `IpcStream.Connect(name)`: one IPC type, named pipes on Windows
  and Unix sockets elsewhere, chosen by libuv, never by the program.
- LAN discovery: UDP broadcast and mDNS/DNS-SD.
- `Url` and `QueryParams`.
- `TlsStream` over any stream (mbedTLS, the OS's roots by default), and a TLS listener.
- std.net.http: `Method`, `StatusCode`, `Headers`, `Body`; `HttpClient`, `Http.Get` and the rest
  (libcurl: redirects, proxies, cookies, gzip, HTTP/2), `Download(url, path, progress)`;
  `HttpServer`, whose handlers are Async code on its own loop (no data races, and Globals work),
  with `Workers = n` to spread them over threads once they're Sendable-safe; routes as
  `server.Get(path, handler)` and router groups, or as a Class whose Methods carry `@Get`,
  `@Post`, and the rest, registered with `server.Routes<T>(instance)`, binding path parameters
  and JSON bodies to typed parameters; middleware; static files; and automatic HTTPS, getting
  and renewing Let's Encrypt certificates (ACME).
- std.net.ws: WebSocket client and server (the server from an HttpServer handler).
- std.net.game: reliable UDP, ENet-style: connections, reliable-ordered, reliable-unordered,
  and unreliable channels, fragmentation, and round-trip time.

#### std.db

- SQLite: `Database.Open(path)`, `Execute` with parameters, `Query<T>` reading rows into
  Structs by reflection, transactions, prepared statements, and numbered migrations
  (`Migrate`) that run once each. No query builder and no server databases.

#### std.image

- Decode and encode PNG, JPEG, BMP, TGA, GIF, HDR, QOI, and WebP, rasterize SVG, and read and
  write animated GIF and APNG frames, into an `Image` over a pixel `Buffer`.
- Resize with good filters, crop, rotate, flip, convert, and blit; software drawing of lines,
  shapes, and fills, and text with TrueType fonts. The engine's 2D (M9) shares the image and
  font code.

#### std.term

- Line widgets: progress bars, spinners, prompts (yes/no, choose, input), tables, and boxes.
- Full-screen terminal apps: layouts, lists, text inputs, mouse and resize events, redrawing
  only what changed.

#### std.algo

- Graph search: BFS, DFS, Dijkstra, and A* on grids and graphs, and flow fields; union-find,
  topological sort, permutations and combinations; Delaunay triangulation and Voronoi diagrams,
  with Lloyd relaxation.

#### std.uuid

- `Uuid.V4()` (random) and `Uuid.V7()` (time-ordered), `Parse`, `ToString`.

#### std.debug

- `Breakpoint()`, `IsDebuggerAttached()`; stack traces wait for debug info (spec 09 already says
  so).

#### std.ffi (after M5's reorganization)

- `Mem.AllocAligned`, `Mem.FreeAligned`, `Mem.AllocUninitialized` (no zeroing).
- `CLong`, `CULong`, and `CWChar`: built-in integers the target sizes, for C's `long`,
  `unsigned long`, and `wchar_t`, which widen to `Int64` or `UInt64` without `As`.
- `Buffer<T>(count)`: native memory it owns, zeroed, at a stable address, freed when its last
  reference goes; bounds-checked `[]`, `Count`, `Slice(start, finish)` views that keep it alive,
  `CopyTo`, `ToArray`, and `Pointer` for C in Unsafe code. `Buffer<T>.View(pointer, count)`, in
  Unsafe code only, wraps memory C owns, on the promise that it outlives the view. std.io reads
  into and writes from Buffers without copying.
- Loading a library at run time (`Library.Open`) waits for the engine's dynamic `[native]`
  libraries.

### Not in std

- Platform-only modules (no std.os.windows or std.os.posix), `Fork`, the registry, a shell
  command, and signals only one OS has.
- Thread, Task, Channel, Mutex, Atomic, Parallel For, Scheduler, and Async stay built in.
- `std.test` comes with the tooling milestone's `fb test`.
- Navigation meshes, game AI (behavior trees, steering), and procedural generation wait for the
  engine phase, where levels and entities are.
- Server databases (Postgres, MySQL), SMTP, Brotli, Bzip2, CBOR, EXR, and an HTML parser are
  for packages.

The standard library is written in FutureBlitz itself, over the runtime where it needs the
OS.
