# Standard Library

Status: what std/ holds today, then [M6's design](#m6s-design), planned 2026-10-20 and built
in M6's 32 steps (see [milestones.md](milestones.md#m6-build-order)). Steps 1 to 4 are built;
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
and CString Methods. Their Fields and helpers, like a Map's `Grow` or a Handle's Generation, are
private to their files, and what std's files share, like map.fb's `JoinedText` and `IterateAs`,
which set.fb uses, or iterator.fb's `MergeSort`, which array.fb's Sort uses, is Package, std's
own.

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
| std/collections/array.fb | `T[]`'s `Remove`, `Sort`, `SortUnstable`, `SortBy`, `SortByDescending` (with `Sorting<T>`'s `ThenBy` and `ThenByDescending`), `Filter`, `Map`, `Find`, `FindLast`, `FindIndex`, `IndexOf`, `LastIndexOf`, `BinarySearch`, `Any`, `All`, `First`, `Last`, `RemoveLast`, `TryRemoveLast`, `AddAll`, `InsertAll`, `RemoveRange`, `RemoveAll`, `Resize`, `Fill`, `Reverse`, `Swap`, `Distinct`, `Join`, `Fold`, and `Reduce`; `Iterate()`, which makes a `T[]` an `Iterable<T>`, through `ArrayIterator<T>` |
| std/collections/view.fb | `ArrayView<T>`, which `items.View(a, b)` gives, and `ReadOnlyArray<T>`, `ReadOnlyMap<K, V>`, and `ReadOnlySet<T>`, which `AsReadOnly()` gives |
| std/collections/map.fb | `Map<K, V>`, which `{key: value}` makes |
| std/collections/set.fb | `Set<T>` |
| std/collections/stack.fb, queue.fb, deque.fb | `Stack<T>`, `Queue<T>`, and `Deque<T>` |
| std/collections/priority_queue.fb | `PriorityQueue<T>` |
| std/collections/pool.fb | `Pool<T>` and `Handle<T>` |
| std/collections/sorted.fb | `SortedMap<K, V>` and `SortedSet<T>`, B-trees, with their iterators |
| std/collections/ring_buffer.fb | `RingBuffer<T>` |
| std/collections/grid.fb | `Grid<T>`, `Grid3<T>`, and `GridIterator<T>` |
| std/collections/bit_set.fb | `BitSet` |
| std/collections/lru_cache.fb, counter.fb, multi_map.fb | `LruCache<K, V>`, `Counter<T>`, and `MultiMap<K, V>`, over a Map |
| std/collections/trie.fb | `Trie<V>` |
| std/collections/linked_list.fb | `LinkedList<T>` and `LinkedListNode<T>` |
| std/collections/interval_tree.fb | `IntervalTree<K, V>` |
| std/collections/spatial.fb | `SpatialHash<T>`, `QuadTree<T>`, and `Octree<T>` |
| std/collections/immutable.fb | `ImmutableArray<T>`, `ImmutableMap<K, V>`, and `ImmutableSet<T>`, over Shared nodes |
| std/math/numbers.fb | `Bounded`, `Integer`, and `FloatingPoint`, the Interfaces the number types implement for std.math's generic Functions, over C's libm; `MinValue<T>` and `MaxValue<T>`; a Float's and a Double's `ToBits` and `FromBits` |
| std/math/constants.fb | `Pi`, `Tau`, `E`, and `Epsilon` as Floats, `Pi64`, `Tau64`, `E64`, and `Epsilon64` as Doubles, and `Inf` and `NaN` |
| std/math/scalar.fb | `Abs`, `Sign`, `Floor`, `Ceil`, `Round`, `Trunc`, `Fract`, `Mod`, `Rem`, `Gcd`, `Lcm`, `IntSqrt`, `Lerp`, `InverseLerp`, `Remap`, `SmoothStep`, `Wrap`, `PingPong`, `MoveTowards`, `IsClose`, `Sqrt`, `Cbrt`, `Exp`, `Exp2`, `Log`, `Log2`, `Log10`, the trig and hyperbolic Functions, `Atan2`, `IsNaN`, `IsFinite`, `CopySign`, `Fma`, `Deg`, `Rad`, `ToRadians`, and `ToDegrees` |
| std/math/bits.fb | std.math.bits: `PopCount`, `LeadingZeros`, `TrailingZeros`, `RotateLeft`, `RotateRight`, `ByteSwap`, `IsPowerOfTwo`, `NextPowerOfTwo`, `AlignDown`, `AlignUp`, and the integers' `SaturatingAdd`, `SaturatingSubtract`, and `SaturatingMultiply` |
| std/math/vector.fb, int_vector.fb | `Vec2`, `Vec3`, `Vec4`, `Vec2i`, and `Vec3i`, with `Dot`, `Cross`, `Length`, `LengthSquared`, `Distance`, `DistanceSquared`, `Normalize`, `Reflect`, `Project`, `Angle`, `Rotate`, `Perpendicular`, `ManhattanDistance`, `ChebyshevDistance`, and the component-wise `Lerp`, `Min`, `Max`, `Clamp`, `Abs`, `Floor`, `Ceil`, `Round`, `MoveTowards`, and `IsClose` |
| std/math/quat.fb | `Quat`, with `Slerp`, `Lerp`, `Inverse`, `Normalize`, `Dot`, `Length`, `Angle`, and `IsClose` |
| std/math/matrix.fb | `Mat3` and `Mat4`, with `Transpose`, `Determinant`, `Inverse`, `TryInverse`, and `IsClose` |
| std/math/color.fb | `Color` and `Color32`, with `Lerp` and `IsClose` |
| std/math/geometry.fb | `Rect`, `AABB`, `Sphere`, `Ray`, `Plane`, and `Frustum` |
| std/algo/search.fb | `Bfs`, `BfsOrder`, `Dfs`, `DfsOrder`, `Dijkstra`, `DijkstraAll`, `AStar`, `Path<N, C>`, and `ShortestPaths<N, C>` |
| std/algo/grid.fb | `GridNeighbors`, `GridPath`, and `FlowField` |
| std/algo/union_find.fb | `UnionFind` |
| std/algo/topological.fb | `TopologicalSort` and `FindCycle` |
| std/algo/combinatorics.fb | `Permutations`, `Combinations`, their `PermutationIterator<T>` and `CombinationIterator<T>`, `Factorial`, and `Binomial` |
| std/algo/delaunay.fb | `Delaunay`, `Voronoi`, and `LloydRelax` |
| std/ffi/mem.fb | `Mem`, raw memory for Unsafe code, over the runtime's `fb_mem_*` |
| std/ffi/cstring.fb | `CString.From`, `Length`, and `Free`, over the runtime's copies |
| std/crypto/hashes.fb | `Hasher`, and `Md5`, `Sha1`, `Sha256`, and `Sha512`, over mbedTLS through the runtime's `fb_crypto_` functions |
| std/crypto/derive.fb | `Hmac` and `Pbkdf2`, each by SHA-1, SHA-256, or SHA-512, and `Hkdf`, by SHA-256 or SHA-512 |
| std/crypto/random.fb | `SecureRandom` and `ConstantTimeEquals` |
| std/crypto/blake3.fb | `Blake3`, over BLAKE3 through `fb_blake3_` |
| std/crypto/checksums.fb | `Crc32`, in FutureBlitz, and `XxHash`, over xxHash through `fb_xxhash_` |
| std/crypto/passwords.fb | `Argon2id` and `Bcrypt`, over Argon2 through `fb_argon2_` and crypt_blowfish through `fb_bcrypt_` |
| std/crypto/ciphers.fb | `Aes256Gcm` and `ChaCha20Poly1305` |
| std/crypto/signatures.fb | `Ed25519`, over Monocypher through `fb_ed25519_`, and `EcdsaP256`, `Rsa`, and `X25519` |
| std/crypto/pem.fb | `KeyKind`, `Key`, and `Certificate` |
| std/concurrent/locks.fb | `Semaphore`, `Barrier`, `WaitGroup`, `Once`, and `RwLock<T>`, Shared Classes over the runtime's locks through `fb_semaphore_` and the like |
| std/concurrent/concurrent_map.fb | `ConcurrentMap<K, V>` and its `ConcurrentMapIterator<K, V>`, in 16 parts of ImmutableMaps |
| std/concurrent/concurrent_queue.fb | `ConcurrentQueue<T>`, over a Channel that grows |
| std/concurrent/tasks.fb | `TaskSource<T>`, over the runtime's signals through `fb_signal_`, `WhenAll`, and `WhenAny` |
| std/events/event.fb | `Event<T>` and `Subscription` |
| std/compress/codec.fb | What the formats share: a Decompress's limit, and the IOErrors the runtime's results mean |
| std/compress/deflate.fb | `Deflate`, `Zlib`, and `Gzip`, with `DeflateCompressor`, `DeflateDecompressor`, `ZlibCompressor`, `ZlibDecompressor`, `GzipCompressor`, and `GzipDecompressor`, over zlib-ng through `fb_zlib_` |
| std/compress/zstd.fb, lz4.fb, xz.fb | `Zstd`, `Lz4`, and `XZ`, with their Compressors and Decompressors, over zstd, LZ4, and liblzma through `fb_zstd_`, `fb_lz4_`, and `fb_xz_` |
| std/compress/archive.fb | `ArchiveEntry` and `ArchiveEntryKind` |
| std/compress/zip.fb | `Zip`, `ZipReader`, `ZipWriter`, and `ZipMethod`, over minizip-ng through `fb_zip_` |
| std/compress/tar.fb | `Tar`, `TarReader`, and `TarWriter`, through `fb_tar_` |
| std/compress/seven_zip.fb | `SevenZip` and `SevenZipReader`, over liblzma through `fb_xz_7z_` |

What the language itself uses, `Error` for Throw and Catch, the Interfaces, `Iterator<T>`, and
`Map<K, V>` for `{key: value}`, is always std's, whatever a program calls by those names.

## M6's design

Planned with the user on 2026-10-20 and, module by module, on 2026-10-21. std is batteries
included: it covers what Python's standard library and its most common packages do together, so
a program needs FFI only for a C library of its own choosing. Packages (M8's registry) are for
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
| mbedTLS | Apache-2.0 | std.net.tls, std.crypto's hashes, HMAC, HKDF, PBKDF2, ciphers, signatures, key exchange, PEM keys and certificates, and secure random |
| PCRE2 | BSD | std.text.regex, and the compiler's check of regex literals |
| utf8proc | MIT | std.text.unicode: categories, case, normalization, graphemes, display width |
| zlib-ng (zlib API) and minizip-ng | zlib | std.compress's Deflate, Zlib, Gzip, and Zip, and curl's gzip |
| zstd, lz4 | BSD | std.compress's Zstd and Lz4 |
| liblzma (xz-utils) | 0BSD | std.compress's XZ/LZMA and 7z reading |
| xxHash | BSD | std.crypto's fast non-cryptographic hashes |
| BLAKE3, Argon2 | CC0 / Apache-2.0 | std.crypto's BLAKE3 and Argon2id |
| crypt_blowfish | public domain, or else a permissive license | std.crypto's Bcrypt |
| Monocypher (its Ed25519) | CC0 / BSD-2-Clause | std.crypto's Ed25519, which mbedTLS doesn't have yet |
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
  (`NOT_FOUND`, `PERMISSION_DENIED`, `ALREADY_EXISTS`, `UNEXPECTED_END`, `INVALID_DATA`,
  `INTERRUPTED`, `OTHER`, and `Path: String?`), `ParseError` (`SYNTAX`, `UNEXPECTED_END`,
  `WRONG_TYPE`, `OUT_OF_RANGE`, `OTHER`, and `Line`, `Column`), `FormatError` (`BAD_TEMPLATE`,
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
  own `Filter` and `Map` stay eager and give a new collection: a `T[]`'s an array, and a Set's or
  a Map's Filter a Set or a Map.
- Interfaces: `Iterable<T>`, `Cloneable`, and `Defaultable` (`Static Method DefaultValue():
  Self`), since `Default` is Select's keyword.
- Small things: `Min`, `Max`, `Clamp`, `Swap`; `HashCombine`; and the built-in `TypeName<T>()`,
  at compile time, like SizeOf.

#### std.collections

- Map and Set keep insertion order through removals (each entry links to the ones added
  before and after it).
  They're `==` by contents, and Hashable when their parts are.
- `T[]`: `IndexOf`, `LastIndexOf`, `FindIndex`, `FindLast`, `Any`, `All`, `BinarySearch`;
  `First`, `Last`, `RemoveLast`, `TryRemoveLast`; `AddAll`, `InsertAll`, `RemoveRange`,
  `RemoveAll(test)`, `Resize(count, fill)`, `Fill`; `Reverse`, `Swap`, `Shuffle(random)`,
  `SortUnstable` (in place, making no array); `Distinct`, `Join(separator)`, `Fold`, `Reduce`;
  `Capacity`, `Reserve`, `TrimExcess`.
- Sorting: `Sort()`, `Sort(before)`, `SortBy(key)`, `SortByDescending(key)`, and `ThenBy` and
  `ThenByDescending` for ties, stable and in place.
- Views that don't copy: `items.View(a, b)` is an `ArrayView<T>` that keeps its array alive, and
  `x.AsReadOnly()` gives a `ReadOnlyArray`, `ReadOnlyMap`, or `ReadOnlySet` to hand out
  (`ReadOnly` is the modifier's keyword). Slices (`items[a..b]`) stay copies.
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

Step 9 built the constants, the scalars, the angles, std.math.bits, the vectors, `Quat`, the
matrices, the colors, and the shapes; the rest is planned. `Import std.math` brings in all of
std/math/, std.math.bits too.

- The math is Functions, as for Floats, beside the scalar ones of their names: `Lerp(a, b, t)`,
  `Dot(a, b)`, `Length(v)`, `Normalize(v)`, `Inverse(m)`. Making a value is a Static Method of
  its type, like `Mat4.Perspective` and `Quat.FromAxisAngle`, and what a value converts to is a
  Method of it, like `q.ToMat4()` and `c.ToColor32()`.
- The scalar Functions are generic over the Interfaces the number types implement, so each works
  in its arguments' type and its literals follow: `Lerp(0, 10, t)` is a Float with `t: Float`, and
  a Double with `t: Double`. `Bounded` is every number type, with `MinValue` and `MaxValue`, for
  `MinValue<T>()`; `Integer` is the eight integer types, with their bits through a UInt64; and
  `FloatingPoint` is Float32 and Float64, with what C's libm works out for each. Generic code of a
  program's own takes them as constraints, as in `Where T: FloatingPoint & Numeric`. Their Static
  Methods are for generic code; on the types themselves, the Functions are the way.
- Constants: `Pi`, `Tau`, `E`, and `Epsilon` (the gap between 1 and the next value) are Floats,
  and `Pi64`, `Tau64`, `E64`, and `Epsilon64` Doubles, since a Float widened isn't a Double's
  precision. `Inf` and `NaN` are Floats, which a Double holds the same. `MinValue<T>()` and
  `MaxValue<T>()`, a float's being its lowest and highest finite values. `Int` stays 32 bits, and
  `Long` is 64; byte sizes are Int64s.
- Scalars: `Abs` and `Sign` for any Numeric and Comparable type; `Floor`, `Ceil`, `Round` (half
  away from 0), `Trunc`, and `Fract`; `Mod` (floored, with the divisor's sign) and `Rem` (as `%`,
  with the dividend's) for integers and floats; `Gcd`, `Lcm`, and `IntSqrt` for integers;
  `Lerp`, `InverseLerp`, `Remap`, `SmoothStep` (GLSL's), `Wrap`, `PingPong`, `MoveTowards`;
  `IsClose(a, b, tolerance)`, and `IsClose(a, b)`, within 0.00001 of the larger's size, or of 1
  near 0; `Sqrt`, `Cbrt`, `Exp`, `Exp2`, `Log` (and `Log(x, base)`), `Log2`, `Log10`; `Sin`,
  `Cos`, `Tan`, `Asin`, `Acos`, `Atan`, `Atan2`, and the hyperbolic six; `IsNaN`, `IsFinite`,
  `CopySign`, and `Fma`. Powers are the `**` operator, for integers and floats, so there's no
  `Pow` or `IntPow`. A Float's `x.ToBits()` gives its IEEE bits, and `Float.FromBits(bits)` and
  `Double.FromBits(bits)` the value back.
- Angles are radians everywhere: trig, rotations, and `Quat`. `Deg(90)` and `ToRadians(90)` are
  90 degrees in radians, and `ToDegrees(x)` x radians in degrees; `Rad(x)` is x itself, for code
  that says its units, as `Deg` does. Each takes an Int too, since degrees are often whole, and
  gives a Float. `quat.ToEulerDegrees()` gives degrees for editors.
- std.math.bits: `PopCount`, `LeadingZeros`, `TrailingZeros`, `RotateLeft`, `RotateRight`
  (counts go around the width, and a negative one turns the other way), `ByteSwap`,
  `IsPowerOfTwo`, `NextPowerOfTwo`, `AlignUp`, and `AlignDown`, for any integer type, a signed
  one's bits being its two's complement; and the integers' `SaturatingAdd`, `SaturatingSubtract`,
  and `SaturatingMultiply` beside their Checked and Wrapping Methods.
- Linear algebra, Float32, right-handed, Y up, -Z forward, column-major, 0..1 depth: `Vec2`,
  `Vec3`, `Vec4` (x, y, z, w), `Vec2i`, `Vec3i`, `Quat` (x, y, z, w), `Mat3`, `Mat4`. The
  vectors' Operators work component by component, as the matrices' + and - do, and a scalar
  scales a vector or a matrix from either side and divides it; `v[i]` is a component and
  `m[column, row]` an element. The vectors are `Numeric`, `Hashable` (-0 as 0),
  `Printable`, and `Defaultable`, the matrices and `Quat` `Equatable` and `Printable`. Vec3 has
  the directions `Up`, `Down`, `Left`, `Right`, `Forward` (-Z), and `Back`, and swizzles `xy` and
  `xyz`. The Functions: `Dot`, `Cross` (a Vec2's gives a Float), `Length`, `LengthSquared`,
  `Distance`, `DistanceSquared`, `Normalize` (a zero vector stays zero), `Reflect`, `Project`,
  `Angle`, `Rotate` and `Perpendicular` in 2D, `ManhattanDistance` and `ChebyshevDistance` on
  grids, and the component-wise `Lerp`, `Min`, `Max`, `Clamp`, `Abs`, `Floor`, `Ceil`, `Round`,
  `MoveTowards`, and `IsClose`. Lengths and distances are worked out in Doubles, as the spatial
  structures' are, so no Float overflows on the way. `Vec2.ToVec2i()` converts toward 0, as As
  does, and `Vec2i.ToVec2()` back.
- `Quat`: `Identity`, `FromAxisAngle`, `FromEuler` (Y, X, Z order: roll, then pitch, then yaw),
  `FromMat3`, `FromTo` (the shortest turn), and `LookRotation`; `ToMat3`, `ToMat4`, `ToAxisAngle`,
  `ToEuler`, and `ToEulerDegrees`; `q * v` turns a vector and `a * b` turns by b, then a; `Slerp`,
  `Lerp` (normalized), `Inverse`, `Normalize`, `Dot`, `Length`, `Angle`, and `IsClose`, which takes
  q and -q as the same rotation.
- `Mat3` and `Mat4`, whose Fields are their columns, x, y, z, and w, where they take each axis:
  `Identity`, `Scale`, `Rotation`, `RotationX`, `RotationY`, and `RotationZ`, and a Mat4's
  `Translation`, `Compose` (scale, then rotate, then move), `LookAt`, `Perspective`, and
  `Orthographic` (glm's right-handed matrices with depth from 0 to 1); `TransformPoint`,
  `TransformVector`, `ProjectPoint`, `Decompose`, `ToMat3`, and `ToMat4`; `Transpose`,
  `Determinant`, `Inverse`, which panics for a matrix with no inverse, and `TryInverse`, which
  gives Null. Release builds don't make SIMD instructions of them yet: Vec4's Operators and
  Mat4's `*` come out as scalar code until codegen runs LLVM's SLP vectorizer.
- `Color` (four Float32s, linear RGB and alpha) and `Color32` (four bytes, sRGB), converting with
  sRGB's gamma both ways, every byte coming back as it was, with named colors (CSS's sRGB values,
  so `Green` is #008000, and `Lime` #00ff00), `Color.Hex("#ff8800")` and `Color32.Hex`, which
  panic on text that isn't hex, `TryHex`, which gives Null, `ToHex`, `Color.Srgb`, HSV and HSL
  (`Color.Hsv`, `Color.Hsl`, `ToHsv`, and `ToHsl`, in sRGB, with a hue from 0 to 1 around, as
  color pickers have it), and OKLab (`Color.OkLab` and `ToOkLab`, from linear light). Math on a
  Color is in linear light, and `Lerp` blends there.
- Geometry: `Rect` (X, Y, Width, Height), `AABB` (Min, Max), `Sphere`, `Ray`, `Plane` (Normal,
  Distance), and `Frustum` (six Planes facing in, from a view-projection matrix), with `Contains`,
  `Intersects`, and `Raycast`, which gives how far along the ray, in lengths of its Direction, 0
  from inside, or Null for a miss. A shape holds the points on its edges. Polygons: triangulation
  with holes, convex hull, point-in-polygon, offsetting, and union, intersection, and difference.
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

Step 18 built the locks, `ConcurrentMap`, `ConcurrentQueue`, `TaskSource`, `WhenAll`, and `WhenAny`;
the rest is planned. `Import std.concurrent` brings in all of std/concurrent/.

- The locks are Shared Classes over the runtime's, so a Const or a parameter shares one with the
  work Spawn and Thread.Start run. What waits blocks its thread and runs nothing else meanwhile, as
  a Channel's Send does, so work on the pool that waits holds its worker: work that waits for other
  work belongs on a Thread. `Semaphore(count)`: `Acquire`, `TryAcquire`, `Release(count)`, and
  `Available`. `Barrier(count)`: `Wait()`, which returns once the round's threads have all come and
  gives True to the last of them, and `Count`. `WaitGroup()`: `Add(count)` and `Done()`, which panic
  where the count would go below 0, `Wait()`, and `Count`. `Once()`: `Do(work)`, which runs the work
  the first time, and the other times returns once it has run, and `IsDone`. `RwLock<T>(value)`, of
  a Sendable T: `Read(reader)` gives what reader makes of the value, other readers reading at once,
  `Write(writer)` gives the value what writer makes of it, with no reader or writer meanwhile, and
  `Value` reads or replaces it; a writer waiting goes before the readers that come after it. Counts
  stay within what an Int holds, and a thread that takes a lock it holds already, as a Once's work
  calling its Do or a Read inside a Write, panics, since it could wait for itself forever.
- `ConcurrentMap<K, V>`, of Sendable keys and values, which any number of threads change at once:
  `Count`, `Add`, `TryAdd`, `Get` (and `Get(key, fallback)`), `GetOrAdd`, `Update`, `[]` and `[]=`,
  `Contains`, `Remove`, `Clear`, `Keys`, `Values`, `ToMap`, and `AddOrUpdate(key, value, change)`,
  which adds value or gives the key's value what change makes of it. Its entries are in 16 parts, by
  the top bits of their hashes once they're mixed, as a Map's buckets are, each an ImmutableMap
  behind a Mutex of its own, so a read copies its part, holding the lock only for that, and looks in
  the copy. GetOrAdd's make, and Update's and AddOrUpdate's change, run outside the lock, so they
  may run again, when another thread changed the key meanwhile, and no change is lost. A For ... In
  goes over a copy of each part, as it comes to it, part by part, and by hash in a part. What a
  change replaces or takes out goes once it holds no lock, so a value's Destroy can use the map, as
  it can a ConcurrentQueue in its Clear, and an RwLock in its Write.
- `ConcurrentQueue<T>`, of Sendable values: `Enqueue` (and `Add` and `AddAll`), `TryDequeue`,
  `TryPeek`, `ToArray`, `Clear`, and `Count`, with no limit but memory and what an Int counts. It
  never waits, for room or for a value; waiting is a Channel's.
- `TaskSource<T>`, of a Sendable T, a Task finished by hand: `Finish(value)`, on any thread, gives
  the result, `TryFinish` tells whether it did, so of threads that try at once one does, and
  `IsDone`. Each read of `Task` gives a new Task of the same result, so code keeps the one it reads:
  done at once when read after Finish, and otherwise at the main thread's first tick after it, as an
  Async Function's Task is once what it awaits is done. `WhenAll(tasks)` gives a Task of the tasks'
  results in their order, or a Task for tasks with none, and `WhenAny(tasks)` a Task of the index of
  the first found done: at a tick, it looks at the tasks in their order, so of those done before it
  looks the first given wins, and one done later in the tick doesn't count. Their Tasks are Async
  Functions', so they're the main thread's.
- Planned: a TaskSource whose Task throws, and WhenAll over tasks that throw, once `Task<T> Throws
  E` can be written as a type; `Cancellation`, a token blocking calls and Tasks take; `Select` over
  several Channels and a timeout; `Lazy<T>` (made once, on first use, from any thread) and
  `ParallelSort`, `ParallelMap`, `ParallelReduce`, and `ParallelForEach` on the job pool, which need
  a function value that another thread can call.

#### std.log

- `Logger` with levels, sinks (console, file, rotating file, JSON lines), and structured fields.
  The default logger prints readable, timestamped lines to stderr, colored only on a terminal,
  and `FB_LOG=debug` or module filters (`FB_LOG=debug,net=trace`) set levels at run time.

#### std.events

Step 18 built `Event<T>` and `Subscription`, in std/events/event.fb; `Import std.events`.

- `Event<T>`, held as a Field: `Emit(value)` calls its handlers in the order they were added, each
  Emit going over the ones it started with, so one added meanwhile waits for the next, and one a
  handler before it cancels isn't called. `Subscribe(handler)` gives a `Subscription`, whose
  `Cancel()` takes the handler off and lets go of it, and whose `IsActive` says whether it's on; it
  holds neither the Event nor the handler, so keeping one keeps neither. `Count` and `Clear()`. An
  Event holds its handlers until they're cancelled, and stays on the thread that made it, as a
  Class's objects do.
- Planned: `Subscribe(owner, handler)`, which ties a handler to an owner held weakly, passed to the
  handler so it captures nothing, and dropped when the owner goes, once a Weak can hold a type
  parameter's objects.

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

- Bytes go in as `UInt8[]`, or a String's bytes, and come out as `UInt8[]`; keys are bytes too.
  A size that can't be right, like a 16-byte key for AES-256, panics at the caller.
- Hashes: `Sha1`, `Sha256`, `Sha512`, and `Md5` (only for checksums, and said so) over
  mbedTLS, and `Blake3` over BLAKE3. Each one's `Hash(data)` hashes at once, and an object of it
  is a `Hasher`, with `Add(data)` and `Finish()`, which gives the hash and starts again. Blake3
  gives any length, and is keyed (`Keyed`, `KeyedHash`) or derives a key for a context
  (`DerivingKey`, `DeriveKey`) too.
- `Hmac.Sha1`, `Sha256`, and `Sha512(key, data)`; `Hkdf.Sha256` and `Sha512(material, salt, info,
  length)`; `Pbkdf2.Sha1`, `Sha256`, and `Sha512(password, salt, iterations, length)`.
- `SecureRandom.Bytes(count)` and `Below(limit)`, from the OS's randomness through mbedTLS's
  generator; `ConstantTimeEquals(a, b)`, for comparing a MAC without telling where it differs.
- Non-cryptographic `Crc32` (zlib's CRC-32) and `XxHash` (XXH3, 64 bits, with a seed), each
  with `Hash`, and `Add` and `Finish`.
- Passwords: `Argon2id` and `Bcrypt`, whose `Hash(password)` picks a new 16-byte salt and the
  parameters (19 MiB, 2 passes, and 1 lane, as OWASP says, and a cost of 12) and gives the
  string to keep, and whose `Verify(password, hash)` checks a password against it.
  `Argon2id.Derive` gives a key's bytes. bcrypt reads only 72 bytes of a password, and none
  past a NUL, so `Bcrypt.Hash` throws an `ArgumentError` for a longer one, or one with a NUL.
- Authenticated encryption: `Aes256Gcm` and `ChaCha20Poly1305`, through `Seal(key, message,
  associated)` and `Open(key, sealed, associated)`, which gives Null for what Seal didn't make
  with that key and associated data. Seal takes a new random 12-byte nonce each time and puts
  it first, so no nonce is ever given, or reused but by a chance of about 1 in 2^33 after 2^32
  messages under one key, as many as a key should seal.
- Keys and signatures: `Ed25519` (RFC 8032, over Monocypher, verifying only the point
  encodings RFC 8032 decodes), `EcdsaP256` (ES256: signatures r then s, deterministic as RFC
  6979 makes them), `Rsa.Verify` (RS256) and `VerifyPss` (PS256), with keys of up to 8192 bits,
  and `X25519` key exchange, whose secret is Null for a public key that makes it 0.
  `Key.FromPem` reads the first private or public key in a PEM, of a `KeyKind` (ECDSA P-256,
  RSA, X25519, or Ed25519), and `ToPem` writes one; `Certificate.FromPem` and `AllFromPem` read
  X.509 certificates, of any of those keys: subject and issuer, as UTF-8 text with RFC 4514's
  escapes, serial number, validity, public key, and DER. Checking a chain of them is
  std.net.tls's.

#### std.compress

Step 22 built the codecs and archives over bytes; their Readers and Writers wait for step 13's
streams. `Import std.compress` brings in all of std/compress/.

- Codecs: `Deflate` (RFC 1951), `Zlib` (RFC 1950), and `Gzip` (RFC 1952) over zlib-ng's zlib API,
  `Zstd` over zstd, `Lz4` (LZ4's frame format) over LZ4, and `XZ` over liblzma. Each one's
  `Compress(data, level)` and `Decompress(data, limit)` work on a `UInt8[]` at once, and its
  Compressor and Decompressor, like `GzipCompressor(level)` and `GzipDecompressor(limit)`, take
  bytes in turn: `Add(data)` gives what came out so far, and `Finish()` the rest, and starts again,
  which is what step 13's streams will wrap. Levels are each library's: 0 to 9 for Deflate, Zlib,
  Gzip, and XZ, 6 by default, 1 to 22 for Zstd, 3 by default, and 1 to 12 for Lz4, 1 by default;
  another panics at the caller. Gzip writes one member with no name, time, or OS in its header, so
  every OS makes the same bytes; a Zstd or Lz4 frame has its content's checksum, and an XZ stream a
  CRC64. Decompress reads Gzip's members, Zstd's and Lz4's frames, and XZ's streams one after
  another, as the tools do; Deflate and Zlib take one stream, and bytes after it are an error.
- Data that's bad throws an IOError: `INVALID_DATA` for bytes that aren't the format or are broken,
  `UNEXPECTED_END` for ones that end too soon, and `OTHER` for ones that would come to more than the
  limit, or use what isn't supported. The limit is the most a UInt8[] holds unless it's given a
  smaller one, which guards against data made to decompress huge; a negative one panics.
- Archives: `Zip.Read(data)` gives a `ZipReader`, `Tar.Read(data)` a `TarReader`, and
  `SevenZip.Read(data)` a `SevenZipReader`, of a copy of the bytes, with `Entries`, `Count`,
  `Find(name)`, `Contains(name)`, `Read(name)`, which gives the last entry of the name, or Null for
  none, and `ReadAt(index)`, which panics out of range. Each `ArchiveEntry` has its `Name`, with '/'
  between folders and none at its end, its `Kind` (`FILE`, `FOLDER`, or `LINK`), its `Size`, and a
  link's `Target`; reading a folder or a link gives no bytes. A name is as the archive has it, so a
  program that makes files of them checks them first. A `ZipWriter` adds files by a `ZipMethod`
  (`STORE`, `DEFLATE`, `LZMA`, `XZ`, or `ZSTD`) at a level, and folders, and a `TarWriter` files and
  folders, and each one's `Finish()` gives the archive and starts again. Names are UTF-8: a zip's
  that don't say so, and aren't, are read as code page 437, and each byte of a tar's that isn't is
  U+FFFD.
- Zip, over minizip-ng: Zip64 where sizes need it, and no encryption; what a ZipWriter makes was
  made by Unix, with modes 644 and 755, and last changed on 1980-01-01, so every program and OS
  makes the same bytes. Tar: ustar, pax's extended headers, GNU's long names and base-256 numbers,
  and old tars, leaving out devices and FIFOs; `Tar.Read` decompresses a .tar.gz, .tar.zst, .tar.xz,
  or .tar.lz4 first, and a TarWriter's tar compresses to one, as `Gzip.Compress(writer.Finish())`
  does. A TarWriter writes ustar, with a pax header for a name ustar can't hold or that isn't ASCII,
  owned by root, at 1970's start. 7z, read over liblzma: its coders stored, LZMA, LZMA2, Delta, and
  the branch filters for x86, PowerPC, IA-64, ARM, ARM Thumb, ARM64, SPARC, and RISC-V, in chains,
  as 7-Zip writes them; BCJ2, PPMd, BZip2, and encryption are IOErrors, and a solid folder is
  decoded once for the files read from it in turn. Writing 7z isn't std's.
- Planned: the Readers and Writers over step 13's streams, and entries' times and permissions, with
  std.time and std.io's files.

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
  shapes, and fills, and text with TrueType fonts. The engine's 2D (M10) shares the image and
  font code.

#### std.term

- Line widgets: progress bars, spinners, prompts (yes/no, choose, input), tables, and boxes.
- Full-screen terminal apps: layouts, lists, text inputs, mouse and resize events, redrawing
  only what changed.

#### std.algo

Built in step 27. `Import std.algo` brings in all of std/algo/.

- Graph search (search.fb): a graph is any values a successors Function steps between, so a
  grid's cells, rooms, and a game's states are graphs alike, and a node is Hashable. `Bfs(start,
  successors, goal)` gives the path with the fewest steps to the first node `goal` says is one,
  `Dfs` a path found depth first, and `BfsOrder` and `DfsOrder` every node reached, in each
  one's order. `Dijkstra(start, successors, goal)` and `AStar(start, successors, heuristic,
  goal)` take each step's weight, of any Numeric and Comparable type (Ints for exact sums), and
  give a `Path<N, C>`, its `Nodes` and `Cost`; `DijkstraAll(start, successors)` gives the
  `ShortestPaths<N, C>` to every node reached, with `Nodes` (the cheapest first), `Contains`,
  `CostTo`, `PathTo`, and `Parent`. A search goes over successors in the order they're given,
  and its queue breaks ties by the least total, then the least guess, then the first added, so a
  run finds what the last did; a weight that's negative or NaN, or a guess that's NaN, panics,
  and what isn't reached is Null. Block lambdas give their result type, like
  `Function(n: Int): (Int, Int)[]`, so the weight's type can come from them.
- Grids (grid.fb), of cells as `Vec2i` from (0, 0): `GridNeighbors(width, height, cell,
  diagonal)`; `GridPath(width, height, start, goal, passable, diagonal)`, A* with the octile or
  Manhattan distance, a step along an axis costing 1 and a diagonal one the square root of 2,
  never cutting a corner a wall is on; and `FlowField(width, height, goals, cost, diagonal)`,
  each cell's cheapest way to its nearest goal, worked out once, from the goals out, where
  entering a cell costs what `cost` gives (Null for a wall), with `Distance`, `Next`,
  `Direction`, and `Reaches`. A size that's negative or past what an Int counts, a cell off the
  grid, or a cost that's negative or NaN panics.
- `UnionFind(count)` (union_find.fb): the Ints from 0 up, in sets that only join, with `Find`
  (halving paths), `Union` (by size), `Connected`, `SizeOf`, `Add`, `Groups`, `Count`, and
  `SetCount`.
- `TopologicalSort(nodes, successors)` (topological.fb), Kahn's algorithm, taking the earliest
  given of the nodes that could come next, which gives the least order by where they're given,
  and keeps nodes that nothing leads to in their order; Null for a cycle, which
  `FindCycle(nodes, successors)` gives.
- `Permutations(items)`, `Permutations(items, count)`, and `Combinations(items, count)`
  (combinatorics.fb), Iterators of new arrays in itertools' order, made as they're asked for;
  `Factorial(n)` and `Binomial(n, k)` as Longs, which panic past what a Long holds.
- `Delaunay(points)` (delaunay.fb): the triangles, counterclockwise, as indices into points,
  Bowyer and Watson's, a point at a time in order of x then y, with a point at infinity so the
  hull comes out whole, in Doubles with Shewchuk's exact predicates, so every distinct point is a
  corner however near the others; fewer than three points or a line of them have none, and of
  equal points only the first is a corner. `Voronoi(points, bounds)`: each point's cell in a
  Rect, counterclockwise, cut by its Delaunay neighbors' bisectors. `LloydRelax(points, bounds,
  iterations)`: each point moved to its cell's centroid, so they spread evenly. A coordinate
  that's infinite or NaN, or bounds that aren't finite or have a negative size, panic.

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
