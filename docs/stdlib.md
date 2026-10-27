# Standard Library

Status: what std/ holds today, then [M6's design](#m6s-design), planned 2026-10-20 and built
in M6's 32 steps (see [milestones.md](milestones.md#m6-build-order));
[history.md](history.md) says which have landed, and the rest are planned.

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
| std/core/error.fb | `Error`, the Class every error extends, with its `Cause` and `Place`; `Place`; and each area's Error and its Kind: `ArgumentError`, `IOError`, `ParseError`, `FormatError`, `TimeoutError`, `CancelledError`, `NetError`, `DatabaseError` |
| std/core/assert.fb | `AssertEqual`, an Assert that says what it compared |
| std/core/interfaces.fb | `Equatable`, `Comparable`, `Hashable`, `Printable`, and `Numeric`, which `==`, `<`, strings, and generic code go through; `Formattable`, which takes format specs; `Cloneable` and `Defaultable`; and `Sendable`, which the compiler decides |
| std/core/iterator.fb | `Iterator<T>`, which `For ... In` goes over, with its lazy adapters and its consumers as Final defaults; `Iterable<T>`, which std's collections are; `Repeated` and `Empty` |
| std/core/range.fb | `Range<T>`, which `0 Until 10`, `1 To 10`, and `Step` make |
| std/core/print.fb | `Print`, `Write`, `PrintError`, `WriteError`, `Flush`, `Input`, and `InputAsync`, over the runtime's `fb_write` and its I/O thread |
| std/core/format.fb | `Format`, which fills a template by the runtime's `fb_format_split`, with `Formatted<T>`, the object a value that isn't one goes in; `Dump`; and `DebugElements`, `DebugEntries`, and `ElementsOf`, through which a collection's debug form shows its elements or entries |
| std/core/parse.fb | `Parse` on each integer type, each Float type, and Bool, in an Extend of each, over the runtime's `fb_parse_int`, `fb_parse_uint`, and `fb_parse_float` |
| std/core/values.fb | `Min`, `Max`, `Clamp`, `Swap`, and `HashCombine` |
| std/core/value.fb | `Value`, data of any shape, and `ValueKind`, over the runtime's `fb_value_` functions for its text and paths |
| std/core/load.fb | The attributes Load<T> and Save read, `Rename`, `Skip`, `Flatten`, and `Renamed`; `Loadable` and `Saveable`, for a type that converts itself; and the Package Functions the compiler's code for each type's Load and Save calls |
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
| std/math/numbers.fb | `Bounded`, `Integer`, and `FloatingPoint`, the Interfaces the number types implement for std.math's generic Functions, over C's libm, with `Bounded`'s `DoubleValue`; `MinValue<T>` and `MaxValue<T>`; a Float's and a Double's `ToBits` and `FromBits` |
| std/math/constants.fb | `Pi`, `Tau`, `E`, and `Epsilon` as Floats, `Pi64`, `Tau64`, `E64`, and `Epsilon64` as Doubles, and `Inf` and `NaN` |
| std/math/scalar.fb | `Abs`, `Sign`, `Floor`, `Ceil`, `Round`, `Trunc`, `Fract`, `Mod`, `Rem`, `Gcd`, `Lcm`, `IntSqrt`, `Lerp`, `InverseLerp`, `Remap`, `SmoothStep`, `Wrap`, `PingPong`, `MoveTowards`, `IsClose`, `Sqrt`, `Cbrt`, `Exp`, `Exp2`, `Log`, `Log2`, `Log10`, the trig and hyperbolic Functions, `Atan2`, `IsNaN`, `IsFinite`, `CopySign`, `Fma`, `Deg`, `Rad`, `ToRadians`, and `ToDegrees` |
| std/math/bits.fb | std.math.bits: `PopCount`, `LeadingZeros`, `TrailingZeros`, `RotateLeft`, `RotateRight`, `ByteSwap`, `IsPowerOfTwo`, `NextPowerOfTwo`, `AlignDown`, `AlignUp`, and the integers' `SaturatingAdd`, `SaturatingSubtract`, and `SaturatingMultiply` |
| std/math/vector.fb, int_vector.fb | `Vec2`, `Vec3`, `Vec4`, `Vec2i`, and `Vec3i`, with `Dot`, `Cross`, `Length`, `LengthSquared`, `Distance`, `DistanceSquared`, `Normalize`, `Reflect`, `Project`, `Angle`, `Rotate`, `Perpendicular`, `ManhattanDistance`, `ChebyshevDistance`, and the component-wise `Lerp`, `Min`, `Max`, `Clamp`, `Abs`, `Floor`, `Ceil`, `Round`, `MoveTowards`, and `IsClose` |
| std/math/quat.fb | `Quat`, with `Slerp`, `Lerp`, `Inverse`, `Normalize`, `Dot`, `Length`, `Angle`, and `IsClose` |
| std/math/matrix.fb | `Mat3` and `Mat4`, with `Transpose`, `Determinant`, `Inverse`, `TryInverse`, and `IsClose` |
| std/math/color.fb | `Color` and `Color32`, with `Lerp` and `IsClose` |
| std/math/geometry.fb | `Rect`, `AABB`, `Sphere`, `Ray`, `Plane`, and `Frustum` |
| std/math/polygon.fb | `PolygonArea`, `PointInPolygon`, `ConvexHull`, `Triangulate`, `Union`, `Intersection`, `Difference`, and `Offset` |
| std/math/curves.fb | `QuadraticBezier`, `CubicBezier`, `Hermite`, `CatmullRom`, and `BSpline`, in 2D and 3D, and `Curve2` and `Curve3` |
| std/math/random.fb | `Random` |
| std/math/noise.fb | `Noise` |
| std/math/easing.fb | `EaseIn`, `EaseOut`, and `EaseInOut` of `Sine`, `Quad`, `Cubic`, `Quart`, `Quint`, `Expo`, `Circ`, `Back`, `Elastic`, and `Bounce`, as `EaseInSine` |
| std/math/stats.fb | `Mean`, `Median`, `Variance`, `StdDev`, `PopulationVariance`, `PopulationStdDev`, `Percentile`, and `Histogram` |
| std/math/fixed.fb | `Fixed32` and `Fixed64`, with `Sqrt`, `Sin`, and `Cos` |
| std/math/units.fb | `Quantity<L, M, T>`, the base and derived units as its Aliases, from `Meters` to `KilogramsPerCubicMeter`, and `Kilometers` to `KilometersPerHour`, which make them from other units, with `Abs` |
| std/math/big/bigint.fb | `BigInt`, an integer of any size, over libtommath |
| std/math/big/decimal.fb | `Decimal`, over mpdecimal, and `RoundingMode` |
| std/math/big/rational.fb | `Rational`, a fraction of two BigInts |
| std/math/big/complex.fb | `Complex`, of two Doubles |
| std/algo/search.fb | `Bfs`, `BfsOrder`, `Dfs`, `DfsOrder`, `Dijkstra`, `DijkstraAll`, `AStar`, `Path<N, C>`, and `ShortestPaths<N, C>` |
| std/algo/grid.fb | `GridNeighbors`, `GridPath`, and `FlowField` |
| std/algo/union_find.fb | `UnionFind` |
| std/algo/topological.fb | `TopologicalSort` and `FindCycle` |
| std/algo/combinatorics.fb | `Permutations`, `Combinations`, their `PermutationIterator<T>` and `CombinationIterator<T>`, `Factorial`, and `Binomial` |
| std/algo/delaunay.fb | `Delaunay`, `Voronoi`, and `LloydRelax` |
| std/ffi/mem.fb | `Mem`, raw memory for Unsafe code, over the runtime's `fb_mem_*`: `Alloc`, `AllocUninitialized`, `AllocAligned`, `Resize`, `Free`, `FreeAligned`, `Copy`, `Fill`, and `Compare` |
| std/ffi/cstring.fb | `CString.From`, `Length`, and `Free`, over the runtime's copies |
| std/ffi/cdata.fb | `CData`, what C code has, which the compiler decides, so a Ptr's T can be a type parameter |
| std/ffi/buffer.fb | `Buffer<T>`, native memory that C fills through its `Pointer()` and safe code reads with `[]`, with `Slice` views that keep it alive, `CopyTo`, `ToArray`, `Buffer<T>.From` an array, and `Buffer<T>.View` of C's memory; `BufferIterator<T>` |
| std/io/streams.fb | std.io's `Reader`, `Writer`, and `Seeker`, with the Async twins `ReadAsync`, `WriteAsync`, and `FlushAsync`, Writer's `WriteBytes` and `WriteText`, and Seeker's `Position`; `SeekOrigin`; `Copy`, `ReadAll`, `ReadAllText`, and `ReadExact`, and `CopyAsync`, `ReadAllAsync`, and `ReadExactAsync` |
| std/io/memory.fb | `MemoryStream`, `StringReader`, and `StringWriter` |
| std/io/buffered.fb | `BufferedReader`, with `ReadByte`, `ReadLine`, and their Async twins, and `BufferedWriter` |
| std/io/binary.fb | `BinaryReader` and `BinaryWriter`, each number's bytes in a `ByteOrder` |
| std/crypto/hashes.fb | `Hasher`, and `Md5`, `Sha1`, `Sha256`, and `Sha512`, over mbedTLS through the runtime's `fb_crypto_` functions |
| std/crypto/derive.fb | `Hmac` and `Pbkdf2`, each by SHA-1, SHA-256, or SHA-512, and `Hkdf`, by SHA-256 or SHA-512 |
| std/crypto/random.fb | `SecureRandom` and `ConstantTimeEquals` |
| std/crypto/blake3.fb | `Blake3`, over BLAKE3 through `fb_blake3_` |
| std/crypto/checksums.fb | `Crc32`, in FutureBlitz, and `XxHash`, over xxHash through `fb_xxhash_` |
| std/crypto/passwords.fb | `Argon2id` and `Bcrypt`, over Argon2 through `fb_argon2_` and crypt_blowfish through `fb_bcrypt_` |
| std/crypto/ciphers.fb | `Aes256Gcm` and `ChaCha20Poly1305` |
| std/crypto/signatures.fb | `Ed25519`, over Monocypher through `fb_ed25519_`, and `EcdsaP256`, `Rsa`, and `X25519` |
| std/crypto/pem.fb | `KeyKind`, `Key`, and `Certificate` |
| std/db/database.fb | `Database`, `Statement`, `Transaction`, `Row`, `ColumnType`, `SqlValue`, and `Nullable`, with `Query<T>` over `Load<T>`, over SQLite through the runtime's `fb_db_` functions |
| std/uuid/uuid.fb | `Uuid` |
| std/data/text.fb | What std.data's formats share in text, over the runtime's `fb_data_` functions |
| std/data/toml.fb | `Toml` |
| std/data/csv.fb | `Csv` |
| std/data/ini.fb | `Ini` |
| std/data/xml.fb | `XmlNode`, `XmlKind`, `Xml`, and `XmlWriter` |
| std/data/xml_reader.fb | `XmlReader`, `XmlEvent`, and `XmlEventKind` |
| std/data/xml_path.fb | `XmlPath` |
| std/data/yaml.fb | `Yaml` |
| std/data/yaml_scanner.fb | YAML's tokens, for `Yaml` |
| std/concurrent/locks.fb | `Semaphore`, `Barrier`, `WaitGroup`, `Once`, and `RwLock<T>`, Shared Classes over the runtime's locks through `fb_semaphore_` and the like |
| std/concurrent/concurrent_map.fb | `ConcurrentMap<K, V>` and its `ConcurrentMapIterator<K, V>`, in 16 parts of ImmutableMaps |
| std/concurrent/concurrent_queue.fb | `ConcurrentQueue<T>`, over a Channel that grows |
| std/concurrent/tasks.fb | `TaskSource<T>`, over the runtime's signals through `fb_signal_`, `ThrowingTaskSource<T, E>`, `WhenAll`, and `WhenAny` |
| std/concurrent/lazy.fb | `Lazy<T>` |
| std/concurrent/parallel.fb | `ParallelForEach`, `ParallelMap`, `ParallelReduce`, and `ParallelSort`, each a Parallel For |
| std/concurrent/cancellation.fb | `Cancellation`, over the runtime's flag through `fb_cancel_`, and `Sleep(time, cancel)` |
| std/concurrent/channels.fb | `Receive(channel, cancel)`, `Send(channel, value, cancel)`, and `Selector`, over the job pool's generation through `fb_pool_` |
| std/events/event.fb | `Event<T>` and `Subscription` |
| std/log/level.fb | `LogLevel`, `ParseLogLevel`, and `LogFilter` |
| std/log/output.fb | `LogField`, `LogRecord`, and `LogOutput` |
| std/log/sinks.fb | `ConsoleSink`, `JsonLinesSink`, `LogLine`, and `JsonLine`, with the runtime's `fb_log_colors` |
| std/log/files.fb | `FileSink` and `RotatingFileSink` |
| std/log/logger.fb | `Logger` and `DefaultLogOutput`, which reads FB_LOG through `fb_log_setting` |
| std/time/duration.fb | `Duration`, and the numbers' `Nanoseconds` to `Days`, as in `2.Seconds` |
| std/time/clock.fb | `Instant`, `Stopwatch`, `Measure`, and `Sleep`, on the monotonic clock |
| std/time/timestamp.fb | `Timestamp`, a point in time in UTC |
| std/time/calendar.fb | `Date`, `TimeOfDay`, `LocalDateTime`, and `Weekday` |
| std/time/zone.fb | `TimeZone`, over the embedded IANA tz database through `fb_tz_`, `ZonedDateTime`, and `Disambiguation` |
| std/time/format.fb | ISO 8601, RFC 2822, and the patterns `Format` and `Parse` take |
| std/time/timers.fb | `Timer`, on the I/O thread's loop, `Deadline`, `FixedStep`, `RateLimiter`, and `Debounce` |
| std/compress/codec.fb | What the formats share: `Coder`, which each Compressor and Decompressor is, a Decompress's limit, and the IOErrors the runtime's results mean |
| std/compress/stream.fb | `CoderWriter` and `CoderReader`, which stream std.io's bytes through a Coder |
| std/compress/deflate.fb | `Deflate`, `Zlib`, and `Gzip`, with `DeflateCompressor`, `DeflateDecompressor`, `ZlibCompressor`, `ZlibDecompressor`, `GzipCompressor`, and `GzipDecompressor`, over zlib-ng through `fb_zlib_` |
| std/compress/zstd.fb, lz4.fb, xz.fb | `Zstd`, `Lz4`, and `XZ`, with their Compressors and Decompressors, over zstd, LZ4, and liblzma through `fb_zstd_`, `fb_lz4_`, and `fb_xz_` |
| std/compress/archive.fb | `ArchiveEntry` and `ArchiveEntryKind` |
| std/compress/zip.fb | `Zip`, `ZipReader`, `ZipWriter`, and `ZipMethod`, over minizip-ng through `fb_zip_` |
| std/compress/tar.fb | `Tar`, `TarReader`, and `TarWriter`, through `fb_tar_` |
| std/compress/vfs.fb | `ZipSource`, a zip as std.io's `VfsSource`, and Vfs's `MountZip` |
| std/compress/seven_zip.fb | `SevenZip` and `SevenZipReader`, over liblzma through `fb_xz_7z_` |
| std/debug/debugger.fb | `Breakpoint` and `IsDebuggerAttached`, through the runtime's `fb_debug_` functions |
| std/net/address.fb | `IpFamily`, `IpAddress`, and `SocketAddress` |
| std/net/url.fb | `Url` and `QueryParams`, as the WHATWG URL Standard has them |
| std/net/dns.fb | `Dns`, over libuv's getaddrinfo through the runtime's `fb_io_` functions |
| std/net/http/headers.fb | `Headers`, a request's or response's header fields, whose names ASCII's letter cases don't change, with `HeadersIterator` |
| std/net/http/status.fb | `StatusCode`, the named statuses, with `Reason`, `IsSuccess`, and `IsRedirect` |
| std/net/http/client.fb | `HttpMethod`, `Body`, `HttpResponse`, `HttpClient`, and `Http`'s one-call requests, over libcurl through the runtime's `fb_http_` functions |
| std/net/socket.fb | What the sockets share: the runtime's `fb_io_net_` functions, an address's bytes, the errors their codes mean, and the streams' and listeners' sockets |
| std/net/tcp.fb | `TcpListener` and `TcpStream` |
| std/net/udp.fb | `UdpSocket` and `Datagram` |
| std/net/ipc.fb | `IpcListener` and `IpcStream`, named pipes on Windows and Unix sockets elsewhere |
| std/net/lan.fb | `LanAnnouncer`, `Lan`, and `LanService`, over mDNS and DNS-SD |
| std/net/game/host.fb | std.net.game: `GameHost`, `GameEvent`, and `GameEventKind` |
| std/net/game/peer.fb | `GamePeer` and `PeerState` |
| std/net/game/settings.fb | `GameSettings` and `DisconnectReason` |
| std/net/game/channel.fb | `ChannelKind`, and each channel's sending and receiving sides |
| std/net/game/link.fb | `GameLink`, which a UdpSocket is, through `GameHost.Bind`, or a test's own link |
| std/net/game/packet.fb | The wire's packets: CONNECT, ACCEPT, REFUSE, DATA, and DISCONNECT |
| std/text/string.fb | std.text: a String's `Length`, `CharCount`, and `GraphemeCount`, and its Methods by byte position: `IsEmpty`, `IsBlank`, `Contains`, `StartsWith`, `EndsWith`, `Find`, `FindLast`, `FindAll`, `Count`, `Slice`, `Before`, `After`, `SplitOnce`, `Split`, `SplitN`, `SplitLines`, `SplitWhitespace`, `Join`, `Replace`, `ReplaceFirst`, `Trim`, `TrimStart`, `TrimEnd`, `TrimPrefix`, `TrimSuffix`, `ToUpper`, `ToLower`, `Repeated`, `PadLeft`, `PadRight`, `Center`, `Compare`, `EqualsIgnoreCase`, `NaturalCompare`, `Lines`, and `Graphemes`, with `LineIterator` and `GraphemeIterator`; `String.FromUtf8`; over the runtime's `fb_text_` functions, and its `fb_unicode_` ones for case and grapheme clusters |
| std/text/char.fb | A Char's `Code`, `Utf8Length`, `IsLetter`, `IsDigit`, `IsNumber`, `IsLetterOrDigit`, `IsUpper`, `IsLower`, `IsPunctuation`, `IsSymbol`, `IsControl`, `IsWhitespace`, `IsAscii`, `ToUpper`, and `ToLower`, and `Char.From` |
| std/text/builder.fb, scanner.fb | `StringBuilder` and `Scanner` |
| std/text/unicode.fb | std.text.unicode: `UnicodeCategory` and `Category`, `NormalForm`, `Normalize`, and `IsNormalized`, `CaseFold`, and `DisplayWidth`, over utf8proc through `fb_unicode_` |
| std/text/encoding.fb | std.text.encoding: `TextEncoding`, `Encode`, `Decode`, `DetectBom`, and `DecodeAny`; `Base64`, `Hex`, `Base32`, `Percent`, and `Html`; and `Escape` and `Unescape` |
| std/text/regex.fb | std.text.regex: `Regex`, with `Parse`, `Escape`, `MatchLimit`, and `MemoryLimit`; `RegexFlags`, `RegexError` and `RegexErrorKind`, and `Match`; and String's `Matches`, `ReplaceAll`, and `SplitBy`; over PCRE2 through the runtime's `fb_regex_` functions |
| std/text/glob.fb | std.text.glob: `Glob` and `Glob.Match` |
| std/text/layout.fb | std.text.layout: String's `WordWrap`, `Dedent`, and `Indent`, and `Levenshtein` |
| std/text/fuzzy.fb | std.text.fuzzy: `Fuzzy`'s `Match`, `Score`, and `Rank`, and `FuzzyMatch` |
| std/text/humanize.fb | std.text.humanize: `Humanize`'s `Plural`, `Relative`, `Ordinal`, `Words`, `SnakeCase`, `KebabCase`, `CamelCase`, and `PascalCase`; `FormatBytes` and `FormatDuration` |
| std/text/diff.fb | std.text.diff: `Diff`'s `Lines`, `Chars`, `Unified`, and `Merge`, and `DiffKind`, `DiffRun`, and `Merged` |
| std/text/markdown.fb | `MarkdownToHtml` and `ParseMarkdown`, over md4c through `fb_markdown_`, with `MarkdownExtensions`, `MarkdownNode`, `MarkdownKind`, and `MarkdownAlignment` |
| std/text/template.fb | `Template` and `Templates` |
| std/data/protobuf.fb | `ProtoWriter` and `ProtoReader`, Protocol Buffers' wire format, `ProtoText`, its text format on one line, `ProtoBox<T>`, and `ProtoIsZero`, `ProtoKeepEnum`, and `ProtoPanic`, which the modules fb makes from .proto files go through; and `ProtoTag` and the `WIRE_` constants, for code that writes the wire format itself |
| std/data/proto/google/protobuf/ | protobuf's well-known types, as protoc 36.2 has them, under protobuf's BSD-3-Clause license, in its LICENSE: any, api, descriptor, duration, empty, field_mask, source_context, struct, timestamp, type, and wrappers.proto, which a .proto or a program imports as `"google/protobuf/timestamp.proto"` |
| std/term/console.fb | std.term: `StandardStream`, `IsTerminal`, `TerminalSize`, `ReadKey`, and `ReadPassword`, over the runtime's `fb_term_` functions |
| std/term/style.fb | `ColorLevel`, `ColorKind`, `TermColor`, `TextAttributes`, `Style`, `ConsoleColors`, `Paint`, `StripStyles`, and `VisibleWidth` |
| std/term/input.fb | `KeyCode`, `KeyModifiers`, `KeyEvent`, `MouseButton`, `MouseAction`, `MouseEvent`, `InputKind`, `InputEvent`, and `InputParser` |
| std/term/terminal.fb | `Terminal`, the console's and a program's own, and `FakeTerminal` |
| std/term/widgets.fb | `Alignment`, `BorderStyle`, `Aligned`, `Truncated`, `Table`, `Box`, `ProgressParts`, `ProgressBar`, and `Spinner` |
| std/term/prompts.fb | `Confirm`, `Ask`, and `Choose` |
| std/term/region.fb | `Region`, `ConstraintKind`, and `Constraint` |
| std/term/screen.fb | `Cell`, `Canvas`, and `Screen` |
| std/term/views.fb | `TextInput` and `ListView` |
| std/image/codec.fb | What the formats share: a Decode's limit, the runtime's images, and the IOErrors their results mean |
| std/image/image.fb | `Image`, `ImageFormat`, and `ResizeFilter`, with `Image.Decode` of any format, `Image.Load` of a file, `FormatOf`, and the transforms, through `fb_image_` |
| std/image/animation.fb | `Animation` and `Frame` |
| std/image/png.fb | `Png`, PNG and APNG, over stb_image through `fb_stb_` and the runtime's encoders over zlib-ng through `fb_zlib_png_` and `fb_zlib_apng_` |
| std/image/jpeg.fb | `Jpeg`, over stb_image and stb_image_write through `fb_stb_` |
| std/image/bitmap.fb | `Bmp` and `Tga`, over them too, and `Qoi`, through `fb_image_qoi_` |
| std/image/gif.fb | `Gif`, through `fb_image_gif_` |
| std/image/hdr.fb | `HdrImage`, over a `Buffer<Float32>`, which converts to and from an `Image`, and `Hdr`, over stb_image and stb_image_write |
| std/image/webp.fb | `WebP`, over libwebp through `fb_webp_` |
| std/image/svg.fb | `Svg`, over nanosvg through `fb_svg_` |
| std/image/canvas.fb | `Canvas`, `LineCap`, and `LineJoin`, drawing through the runtime's `fb_image_` functions |
| std/image/font.fb | `Font`, TrueType and OpenType fonts the runtime reads itself, through `fb_font_`, from bytes or a file |
| std/io/path.fb | `Path` and `PathStyle` |
| std/io/file.fb | `File`'s one-call Static Methods and their Async twins, `Metadata`, `FileKind`, and `LineEnding`, over libuv through the runtime's `fb_io_file_` functions |
| std/io/file_stream.fb | `FileStream`, the Reader, Writer, and Seeker `File.Open` gives, and `OpenOptions`, through the runtime's `fb_io_stream_` functions |
| std/io/dir.fb | `Dir`'s Static Methods and their Async twins, and `DirEntry` |
| std/io/temp.fb | `TempFile` and `TempDir` |
| std/io/lock.fb | `FileLock` |
| std/io/mapped.fb | `MappedFile` |
| std/io/watcher.fb | `FileWatcher`, `FileEvent`, and `FileEventKind` |
| std/io/request.fb | the runtime's requests, std's own |
| std/io/vfs.fb | `Vfs`, `VfsSource`, `FolderSource`, and `EmbeddedSource` |
| std/io/preferences.fb | `Preferences` |
| std/os/env.fb | `Env`'s `Get`, `Set`, `Unset`, `All`, and `Expand`, over libuv through the runtime's `fb_os_` functions, and `Load`, which sets what a `.env` file names |
| std/os/system.fb | `System`'s `Platform`, `Arch`, `CpuCount`, `TotalMemory`, `Hostname`, and `UserName`, with the `Platform` and `Arch` Enums |
| std/os/folders.fb | `Folders`' `Executable`, `Home`, `Config`, `Data`, `Cache`, `Temp`, and `Documents`, and `OpenDefault` |
| std/os/process.fb | `Process`'s `Id` and `Exit` |
| std/net/tls.fb | `TlsClient`, `TlsServer`, `TlsStream<S>`, and `TlsListener`, over mbedTLS's TLS through the runtime's `fb_tls_` functions |

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
| libcurl | curl (MIT-style), with two files under ISC | std.net.http's client, over mbedTLS's TLS, zlib-ng's and zstd's decoding, and nghttp2's HTTP/2 |
| nghttp2 | MIT | std.net.http's HTTP/2, through libcurl |
| mbedTLS | Apache-2.0 | std.net.tls, std.net.http's TLS, std.crypto's hashes, HMAC, HKDF, PBKDF2, ciphers, signatures, key exchange, PEM keys and certificates, and secure random |
| PCRE2 | BSD | std.text.regex, and the compiler's check of regex literals |
| utf8proc | MIT, with Unicode's data under the Unicode license | std.text.unicode: categories, case, normalization, graphemes, display width; and the String and Char Methods that need them, like `ToUpper` and `GraphemeCount` |
| zlib-ng (zlib API) and minizip-ng | zlib | std.compress's Deflate, Zlib, Gzip, and Zip, and curl's gzip and deflate |
| zstd, lz4 | BSD | std.compress's Zstd and Lz4, and curl's zstd |
| liblzma (xz-utils) | 0BSD | std.compress's XZ/LZMA and 7z reading |
| xxHash | BSD | std.crypto's fast non-cryptographic hashes |
| BLAKE3, Argon2 | CC0 / Apache-2.0 | std.crypto's BLAKE3 and Argon2id |
| crypt_blowfish | public domain, or else a permissive license | std.crypto's Bcrypt |
| Monocypher (its Ed25519) | CC0 / BSD-2-Clause | std.crypto's Ed25519, which mbedTLS doesn't have yet |
| libtommath | Unlicense | std.math.big's BigInt |
| mpdecimal | BSD-2-Clause | std.math.big's Decimal |
| SQLite | public domain | std.db |
| stb_image, stb_image_write | public domain / MIT | std.image's PNG decoding, JPEG, BMP, TGA, and HDR; its fonts are the runtime's own, which checks each offset, as stb_truetype doesn't |
| libwebp | BSD | std.image's WebP |
| nanosvg | zlib | std.image's SVG |
| md4c | MIT | std.text.markdown |

Each is fetched with FetchContent in third_party/ and built by the C compiler that builds the
runtime, for the build's target, as a separate archive, beside an archive of the runtime's
functions over it (the `fb_io_` ones over libuv), so a program that calls none of those links
none of the library. After a build that links any, `fb` prints which ones and their licenses;
shipping the notices is the programmer's job. IANA's time-zone data, the tz database (2026e, in
the public domain), is compiled by its own zic as the runtime builds and embedded in an archive
beside the runtime's `fb_tz_` functions (only programs that call them link it), so every OS has
the same zones.

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
  `INTERRUPTED`, `NOT_A_DIRECTORY`, `IS_A_DIRECTORY`, `NOT_EMPTY`, `BUSY`, `NO_SPACE`,
  `INVALID_PATH`, `OTHER`, and `Path: String?`), `ParseError` (`SYNTAX`, `UNEXPECTED_END`,
  `WRONG_TYPE`, `OUT_OF_RANGE`, `MISSING`, `OTHER`, and `Line`, `Column`, and for Load's,
  `Path: String?`, where in the Value it is), `FormatError` (`BAD_TEMPLATE`, `MISSING_VALUE`,
  `BAD_VALUE`, `OTHER`), `TimeoutError` and `CancelledError` (one kind each), `NetError`
  (`REFUSED`, `RESET`, `UNREACHABLE`, `HOST_NOT_FOUND`, `CLOSED`, `TLS`, `OTHER`, and
  `Address: String?`), `DatabaseError` (`CONSTRAINT`, `BUSY`, `QUERY`, `CORRUPT`, `OTHER`).
  Each Kind is an Enum named for its Error, like `IOErrorKind`, and a later step can add to
  its area's.
- Assertions beside Panic and Assert: `AssertEqual(a, b)`, which shows both, `DebugAssert`
  (left out of release builds), `Unreachable()`, `Todo(message)`.
- Formatting: interpolation's format specs, `"{price:.2}"`, which the compiler checks (spec
  14); `Formattable`, for a type that takes specs of its own; and `Format(template,
  values...)`, which fills a template the program has only when it runs, and throws a
  FormatError.
- Parsing: Static Methods on the number types give a `T?`: `Int.Parse("42")`,
  `Int.Parse("ff", 16)`, `Float.Parse`, `Bool.Parse`.
- Nullables: `value.Expect(message)` panics with the message on Null; `value.OrElse(make)`.
- Ranges are values: `0 Until 10`, `1 To 10`, and `Step`, stored, passed, and iterated, with
  every adapter.
- A debug form for every value, from reflection: `"{x:?}"` and `Dump(x)`, which indents what's
  nested. Printable stays the form for people. std's collections show their elements, or a
  Map's entries, through `@DebugElements` and `@DebugEntries`, but a Grid and a Grid3 their
  Fields, their cells and sizes.
- Iterator adapters on `Iterator<T>`, lazy: `Map`, `Filter`, `Take`, `Skip`, `TakeWhile`,
  `SkipWhile`, `Enumerate`, `Zip`, `Chain`, `Flatten`, `StepBy`, `Chunks`, `Windows`,
  `OrderBy`, `OrderByDescending`; consuming: `ToArray`, `Count`, `Any`, `All`, `Find`, `First`,
  `Last`, `Fold`, `Reduce`, `ForEach`, `Sum` and `Product` where Numeric, `Min` and `Max` where
  Comparable, `MinBy`, `MaxBy`. Sources: `Repeated(value, count)`, `Empty<T>()`. A collection's
  own `Filter` and `Map` stay eager and give a new collection: a `T[]`'s an array, and a Set's or
  a Map's Filter a Set or a Map.
- Interfaces: `Iterable<T>`, `Cloneable`, and `Defaultable` (`Static Method DefaultValue():
  Self`), since `Default` is Select's keyword.
- `Value`, data of any shape, which every data format reads into and writes from, and the
  built-ins `Load<T>` and `Save` turn into a T and back: Null, a Bool, an integer (an `INT`, an
  Int64, or past its largest a `UINT`, a UInt64, so JSON's integers stay exact), a Double, a String,
  bytes, an Array of Values, or an Object of Values by key, in the order they were added. A New
  for each type makes one, `Value()` Null, and `Value.Array()` and `Value.Object()` empty ones;
  `Kind`, `IsNull` to `IsObject`, and `Count` say what it holds, and `AsBool()`, `AsInt()`,
  `AsInt64()`, `AsUInt64()`, `AsDouble()`, `AsString()`, `AsBytes()`, and `AsArray()` give it as
  each, or Null: AsBytes a copy, and AsArray a new array of the Value's own elements, so adding to
  it or taking from it leaves the Value as it is, but a change to an element shows. `Keys()` gives
  an Object's keys, in order, and `Contains(key)` whether it has one. `v["key"]` and `v[0]` give a
  `Value?`, Null where it has none, and `Path("a.b[0]")` steps through several; `[]=`, `Add`,
  `Insert`, `RemoveAt`, and `Remove` change an Array or Object, which shows through every
  reference to it, and `Clone()` copies one whole. None holds itself, which `[]=`, `Add`, and
  `Insert` panic at, though one can sit in several places of another, where a change to it shows
  in each. `==` and `Hash` compare what it holds, an Object's keys in any order, and `ToString`
  writes it as JSON on one line, with bytes, which JSON doesn't have, in hex between angle
  brackets, as `<0aff>`. They and Clone go through a Value however deep without recursing, and
  through a part held in several places once for each place, as Clone copies it into each.
- Small things: `Min`, `Max`, `Clamp`, `Swap`; `HashCombine`; and the built-in `TypeName<T>()`,
  at compile time, like SizeOf.

#### std.collections

- Map and Set keep insertion order through removals (each entry links to the ones added
  before and after it).
  They're `==` by contents, and Hashable when their parts are.
- `T[]`: `IndexOf`, `LastIndexOf`, `FindIndex`, `FindLast`, `Any`, `All`, `BinarySearch`;
  `First`, `Last`, `RemoveLast`, `TryRemoveLast`; `AddAll`, `InsertAll`, `RemoveRange`,
  `RemoveAll(test)`, `Resize(count, fill)`, `Fill`; `Reverse`, `Swap`, `SortUnstable` (in
  place, making no array); `Distinct`, `Join(separator)`, `Fold`, `Reduce`; `Capacity`,
  `Reserve`, `TrimExcess`. std.math adds `Shuffle` and `Sample`.
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
  `Repeated` (`Repeat` is a keyword), `PadLeft`, `PadRight`, `Center`; `Compare`,
  `EqualsIgnoreCase`, `NaturalCompare`; `Lines`, `Graphemes`; `String.FromUtf8(bytes)`.
- `StringBuilder`: `Append`, `AppendLine`, `Insert`, `Remove`, `Replace`, `Clear`, `Length`,
  `Reserve`, `ToString`, and `[]`; it's a `Writer`. A `Scanner` reads text with `Line` and
  `Column`.
- std.text.unicode (utf8proc): categories and case, `Normalize` (NFC, NFD, NFKC, NFKD),
  `CaseFold`, `DisplayWidth`.
- std.text.encoding: UTF-8, UTF-16 (LE and BE), UTF-32, ASCII, Latin-1, and Windows-1252, with
  BOM detection; Base64 (standard and URL-safe), Hex, Base32, percent-encoding, HTML escaping,
  and `Escape` and `Unescape` by FutureBlitz's own string escapes.
- std.text.regex (PCRE2, with match limits that throw instead of hanging): `Regex(pattern,
  flags)` with `IsMatch`, `Find`, `FindAll`, captures by number and by name (`m["year"]`),
  `Replace` (with `$1` or a Function), `Split`, and `Escape`; the String shortcuts
  `s.Matches(pattern)`, `s.ReplaceAll(pattern, with)`, and `s.SplitBy(pattern)` cache recent
  patterns. The compiler checks a literal pattern and reports PCRE2's error at it.
  - A pattern is UTF-8 with Unicode's `\w`, `\d`, `\s`, and `\b`, and `\C`, which could split a
    character, is an error; `RegexFlags` has `IGNORE_CASE`, `MULTILINE`, `DOT_ALL`, and
    `EXTENDED`, and a line ends at `\n`, `\r\n`, or `\r`. A raw string, `r"\d{4}"`, needs no
    escapes, and leaves `${name}` in a replacement alone.
  - A pattern that isn't valid is a bug, so `Regex(pattern)` panics at its caller; one from
    elsewhere goes through `Regex.Parse`, which throws a `RegexError` of `PATTERN` with the byte
    PCRE2 stopped at. The compiler checks any pattern it knows, a Const's too, with the flags
    when it knows them, and points at the character PCRE2 stopped at.
  - Each search for a match takes at most `MatchLimit` steps (10 million), over every place in
    the text it tries, and `MemoryLimit` bytes (64 MiB), past which it throws a `RegexError` of `MATCH_LIMIT` or `MEMORY_LIMIT`, so `IsMatch`, `Find`,
    `FindAll`, `Replace`, `ReplaceFirst`, `Split`, and the shortcuts are `Throws RegexError`.
  - A `Match` has `Value`, `Start`, `Finish`, `GroupCount`, `[]` by number or name, giving a
    `String?` for a group that took no part, and `StartOf` and `FinishOf`. `FindAll` finds an
    empty match once at each place, one right after another match too, as Python does. A
    replacement's `$1`, `${1}`, `${name}`, and `$$` panic on a group the pattern doesn't have.
  - Each thread keeps the last 16 patterns it compiled, which a Regex of one shares, so the
    shortcuts compile a pattern once.
- std.text.template: Mustache-style templates. `Template(text, html)` reads one, and
  `Render(data)` fills it from a `Value`, whose Object's keys are the names, or from the Value
  `Save` makes of any other data, so a Struct's or Class's Fields are names, by the keys Save
  gives them, a Map's keys are, and an Enum is its member's name; `Templates` holds them by
  name, with `Add(name, text)`, `Contains(name)`, and `Render(name, data)`, and they render each
  other as partials. Its tags:
  - `{{name}}` is a value, the first step of a path like `{{user.name}}` looked up in the scopes
    from the innermost out, then each step from what the last gave; `{{this}}` or `{{.}}` is the
    scope itself, and `{{this.name}}` the innermost scope's own name, not looked up outward. A
    String gives its text, a number or Bool what Print shows, Null or a name nothing has
    nothing, and an Array or Object its ToString. A template for HTML escapes `&`, `<`, `>`, `"`,
    and `'`, but never in `{{{name}}}` or `{{& name}}`.
  - `{{price:.2}}`, in any of the three, gives a value by the format spec after the `:`, as
    interpolation's `"{price:.2}"` does: a number's, an integer's by a Double's spec where an
    integer's doesn't take it, so `{{count:.1}}` gives "3.0" for 3, with every digit, past 2^53
    too, where a Double would round; a String's or a Bool's; an Array's or Object's ToString as
    a String's; and nothing for Null or a name nothing has. A spec that nothing takes, or that
    holds a brace, is BAD_TEMPLATE, as a block's name with a `:` is, and one the value doesn't
    take throws a FormatError of BAD_VALUE at Render, which says where its tag is, as
    BAD_TEMPLATE does. HTML escapes what the spec gives.
  - `{{#each name}}` renders its body for each of an Array's elements or an Object's values, in
    a scope of its own, where `@index`, `@key`, `@first`, and `@last` say where the item is;
    `{{#if name}}` renders its body when the value is anything but Null, False, 0, NaN, "", or an
    empty Array or Object, and `{{#unless name}}` when it's one of those. Each can have an
    `{{else}}`, for when there's no item, or the test fails, and closes with `{{/each}}`,
    `{{/if}}`, or `{{/unless}}`.
  - `{{> name}}` renders the template of that name, a partial, in the scopes there. `{{! }}` is
    a comment, and `\{{` is a "{{" of the text.
  - A block's tag, a comment, or a partial alone on its line takes the line with it, and a
    partial's lines take its indentation there, as Mustache's do.
  - A template of `Templates` whose name ends with ".html" or ".htm", in any case, escapes for
    HTML, as a partial too, and so do the partials it renders.
  - A template that isn't one throws a `FormatError` of BAD_TEMPLATE, which says the template's
    name, its line, and its column; Render throws one of MISSING_VALUE for a partial there's no
    template of, and of OTHER for partials more than 1000 deep inside each other. A template
    and a Value nest however deep, since neither is read or rendered with calls, but data that
    Save makes a Value of nests only as deep as Save goes, 256 levels, past which Render panics
    at its caller, as Save does.
- std.text.markdown (md4c): CommonMark, which it passes all 652 of 0.31.2's spec examples of, as
  CommonMark's spec_tests.py compares them, with tables and task lists unless it's told other
  `MarkdownExtensions`: `None` for CommonMark alone, `Tables`, `TaskLists`, `Strikethrough`,
  `Autolinks`, and `GitHub`, all four, as md4c reads them, which is mostly as GitHub does. A
  table's delimiter row sets its columns, up to 128, so a row's cells past them are left out,
  a `|` in a code span doesn't split cells, and `\|` in a cell is a `|`, a code span's too.
  Autolinks are bare http://, https://, and ftp:// URLs, www. addresses, and e-mail addresses,
  whose hosts are letters, digits, `.`, `-`, and `_`, and paths those and `/`, with a query and
  a fragment after; one with a port, a `%`, or a `~`, say, is a link only between `<` and `>`.
  `MarkdownToHtml(text, extensions)` gives HTML, and `ParseMarkdown(text, extensions)` the
  document as a tree of `MarkdownNode`s, each a `Kind` (the blocks: `Document`, `Quote`,
  `BulletList`, `OrderedList`, `ListItem`, `ThematicBreak`, `Heading`, `CodeBlock`, `HtmlBlock`,
  `Paragraph`, and a table's `Table`, `TableHead`, `TableBody`, `TableRow`, `TableHeaderCell`,
  and `TableCell`; the spans: `Emphasis`, `Strong`, `Link`, `Image`, `Code`, and
  `Strikethrough`; and `Text`, `SoftBreak`, `HardBreak`, and `Html`), its `Children`, its
  `Text` (a code block's, a code span's, or an HTML block's too), and what its kind has: a
  heading's `Level`, a list's `Start` and `Tight` (a tight list's items have no `Paragraph`s,
  so each holds its spans and text itself), a task's `Task` and `Done`, a link's or
  image's `Url` and `Title` and an autolink's `Autolink`, a code block's `Info` and
  `Language`, and a cell's `Alignment`. `PlainText()` gives the text in a node without its
  markup, each block in it on lines of its own and a table row's cells apart by a tab, however
  deep it's nested. Entities are decoded, a NUL is U+FFFD, code included, and HTML is HTML5's,
  `<br>` and `<hr>`.
- std.text.diff: line and character diffs (Myers, in linear space, as git's xdiff searches and
  slides its changes), unified-diff text, and a three-way merge. `Diff.Lines` and `Diff.Chars`
  give `DiffRun`s, `Diff.Unified` what `diff -u` and `git diff` write, and `Diff.Merge` what
  `git merge-file` makes: a conflict holds only the lines that differ, two with at most 3 lines
  between them, or lines without an ASCII letter or digit, are one, and its markers end in
  "\r\n" where the texts' lines do.
- Fuzzy matching (scores and matched positions, like fzf), and `Humanize`: plurals ("3
  enemies"), relative time ("3 minutes ago"), ordinals, number words, and case styles
  (snake_case, camelCase, PascalCase, kebab-case).
  - std.text.fuzzy's `Fuzzy.Match` gives a `FuzzyMatch?` with fzf's own score and the byte
    positions; `Score` and `Rank` too. A pattern without capitals ignores case.
  - std.text.humanize's `Humanize.Plural(word)`, `Plural(count, word)`, `Relative(seconds)`
    and `Relative(duration)` (negative is past), `Ordinal`, `Words`, and `SnakeCase`,
    `CamelCase`, `PascalCase`, and `KebabCase`.
- `Glob.Match`, `WordWrap`, `Dedent`, `Indent`, `Levenshtein`; `FormatBytes` ("1.2 MB") and
  `FormatDuration`.
  - std.text.humanize's `FormatBytes(count, binary)` gives a tenth of the largest unit, 1000s
    ("1.2 MB") or 1024s ("1.2 MiB"), and `FormatDuration(d)` each of a Duration's days, hours,
    minutes, and seconds in words ("1 hour, 2 minutes, and 3.5 seconds"), or one unit under a
    second.
  - std.text.glob's `Glob.Match(pattern, text)`, or `Glob(pattern).Matches(text)`: `*` stops
    at `/` and `**` doesn't, `?`, `[a-z]` and `[!a-z]`, `{a,b}`, and `\`.
  - std.text.layout's String Methods `WordWrap(width)`, in a terminal's columns, `Dedent()`,
    and `Indent(prefix)`, and the Function `Levenshtein(a, b)`, by code point.

#### std.math

Step 9 built the constants, the scalars, the angles, std.math.bits, the vectors, `Quat`, the
matrices, the colors, and the shapes, step 10 the polygons, curves, `Random` and the default
generator, noise, easing, stats, and fixed point, and step 11 the typed units and std.math.big;
the rest is planned.
`Import std.math` brings in all of std/math/, std.math.bits too.

- The math is Functions, as for Floats, beside the scalar ones of their names: `Lerp(a, b, t)`,
  `Dot(a, b)`, `Length(v)`, `Normalize(v)`, `Inverse(m)`. Making a value is a Static Method of
  its type, like `Mat4.Perspective` and `Quat.FromAxisAngle`, and what a value converts to is a
  Method of it, like `q.ToMat4()` and `c.ToColor32()`.
- The scalar Functions are generic over the Interfaces the number types implement, so each works
  in its arguments' type and its literals follow: `Lerp(0, 10, t)` is a Float with `t: Float`, and
  a Double with `t: Double`. `Bounded` is every number type, with `MinValue` and `MaxValue`, for
  `MinValue<T>()`, and `DoubleValue`, a value as a Double, for math like a mean; `Integer` is the
  eight integer types, with their bits through a UInt64; and `FloatingPoint` is Float32 and
  Float64, with what C's libm works out for each. Generic code of a program's own takes them as
  constraints, as in `Where T: FloatingPoint & Numeric`. Their Static Methods are for generic
  code; on the types themselves, the Functions are the way.
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
  gives Null. Release builds make SIMD instructions of Vec4's Operators and Mat4's `*` where
  the result goes to memory, like an array's element or a Field, in a loop or not. A result
  whose parts are only read one at a time, as Print does, stays scalar code.
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
  from inside, or Null for a miss. A shape holds the points on its edges.
- Polygons (polygon.fb): a ring is a `Vec2[]`, closed from its last corner to its first, and a
  region a `Vec2[][]` of rings, filled by the nonzero rule, as SVG's, so outer rings go
  counterclockwise and holes clockwise. `PolygonArea` (signed), `PointInPolygon` (edges count),
  `ConvexHull` (Andrew's monotone chain), `Triangulate(outer, holes)` (Mapbox's earcut, its
  triangles as indices into the rings one after another, counterclockwise), `Union`,
  `Intersection`, and `Difference` of regions, and `Offset(region, distance, rounded)`, which
  grows or shrinks one with rounded or mitered corners; the results are counterclockwise outer
  rings and clockwise holes. The math is in Doubles. A corner that isn't finite panics where
  corners are put in order (the hull, triangulation, booleans, and Offset), as does an Offset
  distance that isn't.
- Curves (curves.fb): `QuadraticBezier`, `CubicBezier`, `Hermite`, `CatmullRom`, and `BSpline`
  points in 2D and 3D, and `Curve2` and `Curve3`, from a Function of t from 0 to 1 (measured at
  256 samples unless it says) or as `CatmullRom(points, closed, samples)`,
  `BSpline(points, closed, samples)`, and `Bezier(points, samples)` (3n + 1 points), measured at
  `samples` places along each segment, 64 unless they say, with `Length`, `PointAt(t)`,
  `PointAtDistance(d)`, and `Sample(count)`, points evenly apart along the curve, from a table of
  lengths; a NaN t or distance panics.
- Random (random.fb): `Random(seed)` and `Random(seed, stream)` (a stream from 0 up), PCG32, the
  same numbers in every build and on every CPU, with `Int()`, `Int(count)`, `Int(low, high)`
  (both included), `Float()` and `Double()` from 0 up to 1, `Float(low, high)`, `Bool`, `Chance`,
  `Pick`, `WeightedPick`, `Shuffle` (Fisher and Yates'), `Sample` (none twice),
  `Normal(mean, deviation)`, `InsideUnitCircle`, `UnitVector3`, `Fork`, and `State`, which saves
  and sets it back.
- The default generator (random.fb): `Rand()` from 0 up to 1, `RandInt(count)`,
  `RandInt(low, high)` (both included), and `RandFloat(low, high)`, over each thread's own
  PCG32, a `ThreadLocal Global` Struct seeded from the runtime's `fb_random_seed` (the OS's
  randomness, the clock, and a count) each run, until `SeedRandom(n)` starts every thread's
  again at `Random(n)`'s numbers, from its next draw. It holds no object, so no thread lets go
  of it, and a Destroy can draw as a thread or the program ends. An array's `Shuffle()` and
  `Sample(count)` draw from it, and `Shuffle(random)` and `Sample(count, random)` from the
  generator given.
- Noise (noise.fb): `Noise()`, from Ken Perlin's permutation, or `Noise(seed)`, with `Perlin`
  (his improved noise), `Simplex` (Gustavson's), `Value`, and `Worley` in 2D and 3D, of Double
  coordinates, giving Floats, and `Fbm`, octaves of Perlin noise, by its `Octaves`, `Lacunarity`,
  and `Gain` Fields.
- Easing (easing.fb): `EaseIn`, `EaseOut`, and `EaseInOut` of Sine, Quad, Cubic, Quart, Quint,
  Expo, Circ, Back, Elastic, and Bounce, as easings.net has them, each from 0 at 0 to 1 at 1.
- Stats (stats.fb), over any Bounded type, in Doubles: `Mean`, `Median`, `Variance` and `StdDev`
  of a sample (over n - 1, as Python's statistics module), `PopulationVariance` and
  `PopulationStdDev` (over n), `Percentile` (NumPy's, between the two nearest), and
  `Histogram(values, bins, low, high)` and `Histogram(values, bins)`, equal ranges, a value on an
  edge in the range above it and the last holding its high end. Too few values panic, as a NaN
  does where they're put in order, for a median, a percentile, or a histogram's own range, which
  an infinite value panics for too.
- Fixed point (fixed.fb): `Fixed32` (16.16, in an Int32) and `Fixed64` (32.32, in an Int64),
  `Numeric`, `Comparable`, `Hashable`, `Bounded`, and `Printable` (a Fixed64 from its bits, with
  the fewest digits nearer it than any other), from `FromInt`, `FromDouble`, `FromFloat`, or
  `FromRaw`, with `Raw`, `ToDouble`, `ToFloat`, `ToInt`, `ToFixed64` and `ToFixed32`, `Pi`, and
  `Tau`; `*` and `/` round to the nearest, and `Sqrt`, `Sin`, and `Cos`
  are correctly rounded (`Sin` and `Cos` from a 62-bit series, worked out again in 124 bits where
  that lands too near halfway to tell), all in integers, so they give the same bits on every OS
  and CPU, for lockstep multiplayer and replays. Overflow traps in debug builds and wraps in
  release, as an Int's does.
- Units (units.fb): `Quantity<L, M, T>`, a Double `Value` in meters, kilograms, and seconds, to
  the powers its Const type arguments give, so the compiler keeps the units apart and combines
  them: `Meters / Seconds` is `MetersPerSecond`, and `Meters + Seconds` is an error, "no Operator
  + takes (Quantity<1, 0, 0>, Quantity<0, 0, 1>)". Each unit is a Public `Alias` of its Quantity,
  which makes one, `Meters(5)`, and has its Statics, `Meters.Zero()`: `Meters`, `Kilograms`,
  `Seconds`, `SquareMeters`, `CubicMeters`, `MetersPerSecond`, `MetersPerSecondSquared`, `Hertz`,
  `Newtons`, `NewtonSeconds`, `Joules`, `Watts`, `Pascals`, and `KilogramsPerCubicMeter`.
  `Kilometers`, `Centimeters`, `Millimeters`, `Grams`, `Tonnes`, `Milliseconds`, `Minutes`,
  `Hours`, and `KilometersPerHour` make them from other units. `+` and `-` take the same units,
  `*` and `/` any two (an Operator's own type parameters, from the operands), and a Double
  scales; a ratio of the same units is `Quantity<0, 0, 0>`, whose `Value` is the number. A
  Quantity is Comparable, Hashable, Defaultable, and Printable, as "9.8 m/s^2" or
  "4 kg/(m s^2)", and `Abs` keeps its units.
- std.math.big, whose numbers never change once made, with Operators, an Int64 (or a Double,
  for Complex) on either side of one too, and `ToString`, and `Parse` for all but Complex. Their
  math is Methods, like `x.Sqrt()`, since std.math's Functions are the built-in types'. A
  BigInt, Decimal, or Rational stays on its thread, since it holds the library's number, and
  text takes it to another.
  - `BigInt` (libtommath): an integer of any size, whose operators are an Int's without
    overflow: `/` rounds toward 0, `%` takes the sign of what's divided, `>>` rounds down, and
    `&`, `|`, `^`, and `~` work on two's complement, as wide as needed. `Parse(text, radix)` and
    `ToString(radix)` from 2 to 36, `FromBytes` and `ToBytes` (big-endian), `FromDouble`,
    `ToInt64` (Null outside its range), and `ToDouble` (the nearest, an infinity past the
    largest); `Abs`, `Sign`, `BitLength`, `ModPow`, `ModInverse`, `Gcd`, `Lcm`, `Sqrt`, `DivRem`,
    and `IsProbablePrime` (Baillie-PSW).
  - `Decimal` (mpdecimal): a decimal number that holds what it's made from exactly, from text,
    an Int64, a BigInt, or a Double (as the shortest digits that read back as it, so 0.1 is
    0.1), and whose arithmetic keeps 28 significant digits, rounded half to even, with
    exponents up to 999999, as Python's decimal does by default; a result past that panics.
    `==` doesn't count trailing 0s, which `ToString` shows, as Python's str does, beside
    `ToPlainString`. `Round(places, RoundingMode)`, `Truncate`, `Floor`, `Ceiling`, `Sqrt`,
    `Exp`, `Ln`, `Log10`, `Pow`, and `ToDouble`, `ToInt64`, and `ToBigInt`.
  - `Rational`: a fraction of BigInts in lowest terms, exact; `Floor`, `Ceiling`, `Truncate`,
    and `Round` (half to even) give BigInts; `Parse` takes "3/4" or a Decimal's text,
    `FromDouble` and `FromDecimal` are exact, and `ToDouble` and `ToDecimal` round.
  - `Complex`: two Doubles, as Python's cmath works them out: `Abs`, `Arg`, `Conjugate`, `Exp`,
    `Log`, `Sqrt`, `Pow`, and `FromPolar`. Like a Double, it never panics, and what has no value,
    like dividing by 0, has NaN parts.

#### std.io

Step 14 built `Path`, `File`'s one-call reads and writes, `FileLock`, `MappedFile`, `Dir`,
`TempFile` and `TempDir`, `FileWatcher`, and IOError's Kinds, and step 15 the virtual file system
and `Preferences`; step 13 the streams, and the gaps step 32's audit found `File.Open`, a
MappedFile's `Buffer<UInt8>`, the Zip mount, and typed Preferences. `Import std.io` brings in all of
std/io/.

- `Reader`, `Writer`, and `Seeker` Interfaces, with Async twins, and `Copy`, `ReadAll`,
  `ReadExact`; buffered, memory, and string readers and writers; `BinaryReader` and
  `BinaryWriter` with endianness. Built in step 13: a Reader reads into a `Buffer<UInt8>` and
  gives how many bytes, 0 at its end, and a Writer writes all of one; their Async twins are
  defaults that block as the plain ones do, which a file or socket replaces with one that waits
  on the main thread, and the buffered streams' go through the stream they wrap. Errors are
  IOErrors, an early end UNEXPECTED_END. Seeking is cheap where it isn't plain memory too, so
  Seeker has no Async twin.
- `Path` is a value, a Struct of its `Text` and its `Style` (`PathStyle.POSIX` or `WINDOWS`, the
  target's unless asked for): `Join` and `/`, `Parent`, `FileName`, `Stem`, `Extension` (with its
  dot), `WithExtension`, `Normalize`, `RelativeTo` (a `Path?`), `IsAbsolute`, `Absolute`, and
  `NativeStyle`. They work on the text alone, but for Absolute, which asks for the current
  folder. Its own separator is `/` on every OS, which it writes wherever it joins or rebuilds a
  path, so a program's paths read the same everywhere; by Windows' rules `\` is one too, and a
  path can start with a drive (`C:`, `C:/`) or a share (`//server/share`). Paths are equal and
  hashed by their texts and styles, and sort by their styles, POSIX's first, then their texts'
  bytes; a Path's zero is "" by the target's rules, as `Path("")` is; on Windows, RelativeTo takes
  names that differ in their ASCII letters' case as the same. A String converts to a Path
  wherever one is expected (spec 02), so `File.ReadText("save.dat")` and `saves / "one.dat"`
  read naturally.
- `File`: `ReadBytes`, `ReadText`, `ReadLines`, `WriteBytes`, `WriteText`, `WriteLines`,
  `AppendBytes`, `AppendText`, `AppendLines`, `WriteBytesAtomic` and `WriteTextAtomic` (a new
  temporary file in the same folder, synced to the disk, then renamed over the file, keeping its
  POSIX permissions), `Exists` (a file, not a folder), `Size`, `Metadata` (its `Kind`, `Size`,
  `Modified` and `Accessed` Timestamps, its `Created` Timestamp, which is Null where the OS
  doesn't say, and `IsReadOnly`; through links unless asked), `Copy` and `Move` (which replace a
  file there, and copy a file to another disk; a folder moves only where nothing is, and neither
  puts a file over a folder), `Delete` (gives whether there was one), and
  `SetReadOnly`; each with an Async twin, which libuv's thread pool does from the I/O thread's
  loop, whose Await resumes on the main thread. Text reads as UTF-8, with U+FFFD for a byte that
  doesn't start well-formed UTF-8, and stays as it was, its line breaks and a byte order mark
  too. Reading lines splits them on `\n` and `\r\n`, and writing them puts `\n` after each on
  every OS, unless asked for `LineEnding.CRLF`.
- `File.Open(path)` opens a file to read, `File.Open(path, options)` as an `OpenOptions` says
  (`Read`, `Write`, `Append`, which writes too, `Create`, `CreateNew`, which refuses a file that's
  there with ALREADY_EXISTS, and `Truncate`, all False unless given, as in
  `OpenOptions(Write: True, Create: True)`), and `File.Create(path)` to write, made or emptied
  (once it's open, as Windows won't open a hidden file with O_TRUNC, which no write uses);
  each with an Async twin. They give a `FileStream`, a Reader, Writer, and Seeker, with
  `ReadAsync`, `WriteAsync`, and `FlushAsync` that libuv's thread pool does, `Seek` and `Position`
  from where it is, which each read and write moves on, `Size`, `Sync` and `SyncAsync`, which put
  what's been written onto the disk, `IsReadable`, `IsWritable`, `IsAppending`, `IsOpen`, and
  `Close`, which its last reference does too. An appending one writes at the end wherever it's
  moved to, and a write past the end fills what's before it with 0s; a read past the end gives
  nothing, up to the last place an Int64 counts, and a write farther out than the file system
  lets a file reach is NO_SPACE. It holds nothing back, so Flush has nothing to do, and a
  BufferedReader or BufferedWriter around it takes fewer calls.
  Options no file opens with panic (none of Read, Write, and Append; Create, CreateNew, or
  Truncate without a write; Truncate with Append), and so do a call while an Async one is under
  way, using it once it's closed, reading one not opened to read, and writing one not opened to
  write, at the caller.
- `FileLock.Acquire`, `AcquireShared`, `TryAcquire`, `TryAcquireShared` (a `FileLock?`), and
  `AcquireAsync` and `AcquireSharedAsync`: a lock on a file, made when it isn't there, which other
  FileLocks, in this program or another, wait for or are refused, until `Close` or its last
  reference. It's on one byte far past where files end, so it never keeps anyone from reading
  or writing the file, on Windows too.
- `MappedFile(path, writable)` maps a file into memory whole: `Count`, `[]` and `[]=`,
  `ReadBytes`, `WriteBytes`, `Flush`, `IsOpen`, and `Close`, which unmaps it; misuse panics at the
  caller. `Bytes()` gives its bytes as a `Buffer<UInt8>`, without copying them, which, with each
  view of it, keeps them mapped until it goes, though the MappedFile is closed or gone: a writable
  one's Buffer is the file's bytes, and a read-only one's Buffers share a copy-on-write view of
  their own, made the first time, so writing one changes them all but never the file. Only std
  extends Buffer with memory it keeps, as MappedFile's does: Buffer's view New is Package.
- `Dir`: `Create`, `CreateAll`, `Remove` and `RemoveAll` (which give whether there was one, and
  don't follow links), `Exists`, `Entries`, `Walk`, and `Glob`, which give `DirEntry`s (`Path`,
  `Relative` from the folder listed, `Kind`, `Name`) in the order of their names' bytes, each
  folder before what's in it, without going into links; each with an Async twin. Glob's
  patterns match names between `/`s, and `\`s too by Windows' rules: `*`, `?`, `[abc]`, `[a-z]`,
  `[!abc]`, and a name of `**` for any number of folders, or, last, for everything below, files
  too, and the folder before it, so `sub/**` gives sub and all that's in it; names match exactly,
  in the same case, on every OS. `Current`, `SetCurrent`, and `Temp`, the OS's temporary
  folder.
- `TempFile(prefix)` and `TempDir(prefix)`: a new, empty file or folder in the OS's temporary
  folder, with a name no other has, which `Close`, or its last reference, removes with what's in
  it.
- `FileWatcher(path)` watches a file, or what's in a folder, not the folders in it, over libuv's
  file events: `Next` (waits), `TryNext`, and `NextAsync` give a `FileEvent?` (`Kind`: `CREATED`,
  `CHANGED`, or `REMOVED`, and `Path`), Null once it's closed. It tells what came and went by the
  names it knows are there, since OSes call those differently, so a rename is the old name's
  REMOVED and the new name's CREATED, and a file renamed over another is the other's CHANGED.
  `Close` drops what it hasn't given.
- A virtual file system: a `Vfs` mounts sources at points in a tree of its own (`Mount`,
  `MountFolder`, `MountEmbedded`, `Unmount`) and reads them as files are read (`ReadBytes`,
  `ReadText`, `ReadLines`, each with an Async twin, `KindOf`, `Exists`, `DirExists`, `Entries`,
  `Walk`, `Glob`). Its paths have `/` (or `\`) between names on every OS, and none goes above its
  root. A later mount over a place comes first, so a mod's folder can stand in for some of a
  game's embedded files: a file is the last mount's that has it, and a folder lists what every
  mount has in it, and the mounts under it, by their names' bytes; what two mounts have is the
  last one's, and a mount is a folder, though its folder isn't there yet. Glob's patterns take `/`
  or `\` between names, as its paths do. A `VfsSource` gives a file's bytes, what's at a path, and
  a folder's names: `FolderSource` reads a folder on the disk as it's asked, with nothing outside
  it (by Windows' rules a name with a `:` isn't there), and is Final, so its Async reads are its
  reads and a source that changes them wraps one; `EmbeddedSource` the `Map<String, UInt8[]>`
  EmbedFolder gives, a copy at each read, and panics for paths that aren't files' or make a place
  a file and a folder too; and a program can write its own. An Async read reads the mounts there
  were when it began. A zip mounts through std.compress: `vfs.MountZip(at, data)`, or a
  `ZipSource` of a ZipReader, whose files are its file entries and whose folders are the ones they
  name or go through, with the last of a name's entries the file; an entry whose name goes above
  the zip's root, and a link, aren't in it, and a zip that makes a place a file and a folder too
  is an IOError of INVALID_DATA. The engine's pak files build on it.
- `Embed("path")`, `EmbedText("path")`, and `EmbedFolder("path")`: built-ins, checked at compile
  time, that put a file's bytes (a new `UInt8[]`), its text (a String known at compile time), or
  a folder's files (a new `Map<String, UInt8[]>` by their paths in it), in the program (spec 13).
- `Preferences("MyGame")`: a program's settings, kept between runs in
  `<folder>/MyGame/preferences.txt`, where the folder is the OS's for its users' programs' settings
  (`Preferences.Folder()`: Windows' %APPDATA%, macOS's ~/Library/Application Support, and elsewhere
  $XDG_CONFIG_HOME or ~/.config), or one given (`Preferences(app, folder)`, as tests keep theirs in
  a temporary folder). `Get`, `Set`, `Remove`, `Contains`, `Keys`, `Count`, `Clear`, `IsChanged`;
  nothing is written until `Save` (or `SaveAsync`), which replaces the file atomically;
  `OpenAsync` reads it without blocking. A change made while a save writes leaves `IsChanged`
  True. The file is text a person can read and change, a `key=value` line for each setting with
  `\\`, `\n`, and `\r` escaped, and `\=` and a leading `\#` in a key; a byte order mark before
  it is skipped, and a file where the app's folder goes holds no settings. An app's name that
  can't be a file's on every OS panics: Windows' device names, like `nul` or `COM1.txt`, a name
  ending with a dot or a space, and one over 255 bytes too. Values are Strings, and `GetInt`,
  `GetDouble`, and `GetBool`, with a fallback or giving Null, read them as step 5's Parse does,
  which `SetInt`, `SetDouble`, and `SetBool` write them for: a Double as the shortest text that
  reads back as it, NaN and the infinities too, and a Bool as "True" or "False".
- The console's `ReadKey`, `ReadPassword`, `IsTerminal`, `TerminalSize`, and styled output that
  turns itself off when it isn't a terminal or `NO_COLOR` is set are std.term's (see below), with
  what else reads keys and draws on a terminal. (`Input` is std.core's.)

#### std.time

- `Duration`, written `2.Seconds`, `1.5.Minutes`, `500.Milliseconds`; the timing built-ins take
  one too: `Await Wait(500.Milliseconds)`, and `RealWait`.
- A type for each meaning: `Timestamp` (a point in time, UTC), `Date`, `TimeOfDay`,
  `LocalDateTime` (no zone), and `ZonedDateTime`, with `TimeZone` (embedded IANA data, and the
  OS's local zone, `TimeZone.System()`); arithmetic that clamps (Jan 31 plus a month is Feb 28 or
  29), `StartOfDay` and the like, a `Disambiguation` for times the clocks show twice or skip,
  `Format` and `Parse` patterns, ISO 8601 and RFC 2822.
- `Instant`, `Stopwatch`, and `Measure(f)` on the monotonic clock; `Sleep`. What reads a clock
  also takes an `Instant`, so tests give the time as a value.
- Timers on libuv's loop (`Timer.After`, `Timer.Every`), `Deadline` for blocking calls,
  `FixedStep(rate)` for fixed-step game loops, `RateLimiter`, `Debounce`.

#### std.os

- Arguments: a Struct with `@Command`, `@Positional`, `@Option`, and `@Flag` attributes, which
  `Args.Parse<T>()` fills by reflection, with generated help, errors, and subcommands; a builder
  underneath for options made at run time. fb's own CLI can use it.
- `Env`: `Get`, `Set`, `Unset`, `All`, `Expand` (`$NAME`, `${NAME}`, and `$$`), and
  `Env.Load(".env")` for `.env` files: `NAME=value` lines, with `export ` or not, `#` comments,
  values in `'`s as they are, in `"`s with escapes and over lines, and the rest trimmed; all
  but those in `'`s are expanded. It leaves what's set as it is, unless `replace` is True, and
  a bad line is a ParseError, with its line and column, before anything's set.
- `Process`: `Exit`, `Id`; `Command(program)` with arguments, environment, folder, and piped or
  inherited streams; `Spawn` gives a `Child` (`Wait`, `TryWait`, `Kill`, its streams);
  `Output()` and `Status()`. No shell: a program and its arguments, the same on every OS.
- Signals that every OS has: `OnInterrupt` (Ctrl+C) and `OnTerminate`.
- System: `Platform`, `Arch`, `CpuCount`, `TotalMemory`, `Hostname`, `UserName`.
- `Folders`: `Executable`, `Home`, `Config`, `Data`, `Cache`, `Temp`, `Documents`;
  `OpenDefault(pathOrUrl)`.

#### std.concurrent (beside the built-ins, which stay built in)

Step 18 built the locks, `ConcurrentMap`, `ConcurrentQueue`, `TaskSource`, `ThrowingTaskSource`,
`WhenAll`, `WhenAny`, `Lazy`, the parallel algorithms, `Cancellation`, the Channels' waits that
take one, and `Selector`. `Import std.concurrent` brings in all of std/concurrent/.

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
- `ThrowingTaskSource<T, E>`, of an error type E, a Task that throws, finished by hand: `Finish(value)`
  gives the result, `Fail(error)` the error, which a Wait or an Await on the Task throws, each
  panicking when it has either already, `TryFinish` and `TryFail` tell whether they did, and
  `IsDone`; its `Task` is a `Task<T> Throws E`, read as a TaskSource's is. An error isn't Sendable,
  so it's a Class, which stays on the main thread; work on other threads finishes a TaskSource.
  `WhenAll` and `WhenAny` take Tasks that throw too, with or without results: WhenAll's Task waits
  for every task and then throws the error of the first of them, in their order, that threw,
  whichever threw first, and WhenAny's first can be one that threw.
- `Lazy<T>(maker)`, of a Sendable T, a Shared Class whose `Value` the first read makes, by the
  Sendable Func it was given, on the thread that reads it, while reads on other threads wait for it,
  and each gets that value, a Null one too; `IsMade` says whether it's made, without waiting. A
  maker that reads its own Lazy's Value panics, since it would wait for itself forever.
- `ParallelForEach(items, action)`, `ParallelMap(items, change)`, `ParallelReduce(items, combine)`,
  and `ParallelSort(items)` (and `ParallelSort(items, before)`), over an array of Sendable values and
  a Sendable Func, each a Parallel For on the job pool with the calling thread taking its share.
  What they give is in the items' order, as their plain loops' would be: ParallelMap's results;
  ParallelReduce's value, or Null for no items, from at most 64 parts, each combined left to right
  and then the parts' values left to right, so combine must give the same however its calls are
  grouped, though its values needn't swap places; and ParallelSort's order, stable as Sort's is, by
  parts sorted at once and merged in pairs, each round's merges at once, below 4096 items Sort's on
  the calling thread. ParallelForEach's calls are in no order.
- `Cancellation()`, a Shared Class, is a flag that `Cancel()` sets, on any thread, for good:
  `IsCancelled`, `Check()`, which throws a `CancelledError` once it's set, and `Task`, done once
  it's set, for Async code to await beside its work's, as with WhenAny. The waits that take one
  stop once it's set, before or while they wait, with a CancelledError, and take nothing: a
  Semaphore's `Acquire(cancel)`, a WaitGroup's `Wait(cancel)`, `Sleep(time, cancel)`,
  `Receive(channel, cancel)`, which gives Null once the Channel is closed and empty, and
  `Send(channel, value, cancel)`, which panics at a closed one, as the Channel's own do.
- `Selector()`, a builder, since `Select` is a keyword, waits on several Channels at once:
  `On(channel, handler)` for each, `Timeout(time, handler)`, and `Wait()` (or `Wait(cancel)`),
  which takes a value from the first Channel, in On's order, that has one and gives it to its
  handler, or runs the timeout's once its time passes with none, and gives True; or gives False
  once every Channel is closed and empty. A Channel's `IsClosed()` and `TryReceive()`, which gives
  a T?, are built in for it.

#### std.log

Step 18 built `Logger`, its levels, filters, outputs, and sinks, in std/log/; `Import std.log`.

- `Logger(module)` or `Logger(module, output)`, a Shared Class, writes lines for a module, a name
  like "net.http" whose dots say what it's in, at `LogLevel`s, `TRACE`, `DEBUG`, `INFO`, `WARN`,
  and `ERROR`: `Trace(message)` to `Error(message)`, `Log(level, message)`, and `IsEnabled(level)`.
  `With(name, value)`, of a String, an integer of any of the eight types, a Double, or a Bool,
  gives a Logger whose lines have that field too, after its others, so threads never change one
  another's. A console line quotes a field's name, as it does a value, where it has a space, a
  quote, an '=', or a control character.
- A `LogOutput(filter)`, a Shared Class, gives each `LogRecord` (`Time`, `Level`, `Module`,
  `Message`, and `Fields`, `LogField`s) its `LogFilter` writes to its sinks, in the order `Add`
  gave them; `Clear()`, and `Filter`, which a program can replace as it runs. A sink is a
  `Sendable Func(LogRecord)`, so a program's own is a lambda: `ConsoleSink()` writes `LogLine`s
  to stderr, colored when it's a terminal that shows colors and NO_COLOR isn't set, and
  `ConsoleSink(colors, times)` as it says; `JsonLinesSink(write)` gives write each `JsonLine`.
  Lines from threads at once don't mix. `FileSink(path)` adds each `LogLine`, with its time and
  without colors, to a file's end, made with its folders, each line in one write that no other
  thread's or program's comes into; a relative path is from the folder that's current when the
  sink's made. `RotatingFileSink(path, maxBytes, keep = 5)` does until a line would make the file
  hold more than maxBytes, and then moves it to `<path>.1`, that one to `.2`, and so on, the
  oldest of keep going, or, with a keep of 0, the old lines; a maxBytes that isn't above 0 or a
  keep below 0 panics. Nothing old is deleted or moved until the file's been moved aside, so a
  file that can't be (one another program has open on Windows, or a folder where an old one
  goes) keeps its old ones and gets the lines meanwhile, and the move's tried again once another
  maxBytes are in it. A line that can't be written is lost, and the first since one was says so
  on stderr, as a failed move does.
- `LogFilter.Parse(text)` reads FB_LOG's text: parts between commas, each a level for every
  module, or `module=level`, like "info,net=trace,net.http=off", a later part winning, every
  module at INFO unless a part says otherwise; Null for anything else. A module gets its own
  level, or the nearest module's it's in, or the filter's. `LogFilter.At(level)`, `With`,
  `LevelOf`, `Allows`, and `ToString()`, as FB_LOG has it. `ParseLogLevel(text)` reads a level's
  name in any letter case.
- `DefaultLogOutput()`, which `Logger(module)` writes to, is made the first time it's used: the
  console's sink, at the levels FB_LOG sets, or at INFO, an FB_LOG that isn't a filter said once
  on stderr and left out.

#### std.events

Step 18 built `Event<T>`, `Subscription`, and `Subscribe(owner, handler)`, in std/events/event.fb;
`Import std.events`.

- `Event<T>`, held as a Field: `Emit(value)` calls its handlers in the order they were added, each
  Emit going over the ones it started with, so one added meanwhile waits for the next, and one a
  handler before it cancels isn't called. `Subscribe(handler)` gives a `Subscription`, whose
  `Cancel()` takes the handler off and lets go of it, and whose `IsActive` says whether it's on; it
  holds neither the Event nor the handler, so keeping one keeps neither. `Count` and `Clear()`. An
  Event holds its handlers until they're cancelled, and stays on the thread that made it, as a
  Class's objects do.
- `Subscribe(owner, handler)`, of a Class's owner, ties the handler to the owner, which it holds
  weakly and passes to the handler, as `handler(owner, value)`, so the handler needn't capture it:
  one that did would keep it as long as the Event keeps the handler. Once the owner goes, the
  handler is off, Count leaves it out, and the next Emit or Clear takes it out; its Subscription
  cancels it as any other's does. Since Subscribe is overloaded, its lambda's parameters need
  their types written.

#### std.data

- Every format reads its text into std.core's `Value` and writes one out, so JSON's integers
  stay exact (an Int64, or a UInt64 past it), and its other numbers are Doubles; `Path("a.b[0]")`
  reaches inside one.
- Serialization is automatic: the built-ins `Load<T>(value)` and `Save(x)` turn a Value into a
  T and back (see [spec 13](spec/13-compile-time.md#load-and-save)), for the numbers, Bool,
  String, Enums (by name), `T?`, `T[]`, `UInt8[]` as bytes, `Map<String, V>`, Values, and
  Structs and Classes, by compile-time reflection, every Field included, Private ones too,
  Properties not. Load makes a Struct or Class Field by Field without running its New, each
  from the data's value, else its default, else its zero, else a MISSING error naming it, and
  then its `OnLoaded()`. std.core's `@Rename("key")`, `@Skip`, `@Flatten`, and
  `@Renamed("old")` adjust it, and a type can give its own
  conversion as a `Loadable` or `Saveable`. A format is then a Value's reader and writer, as
  `Json.Write(save, indent: 2)` and `Json.Read<Save>(text)` are Save and Load around it. Load
  takes a number or a Bool from a String that reads as one, as a command line's are, and throws
  a ParseError whose `Path` names where in the Value it is, like `options.sizes[2]`, when a
  value doesn't fit.
- Formats: JSON (with a streaming reader and writer), TOML, CSV, INI, XML (a tree, a streaming
  reader and writer, and XPath-lite), YAML (1.2's core schema, no arbitrary tags), and
  MessagePack. `@Renamed("old")`, and a Field's default or zero for what an older save didn't
  have, keep old saves loading after a game changes, and OnLoaded can work out the rest.
- Protocol Buffers: `Import "game.proto"` reads a .proto file, proto2 or proto3, as the program
  builds, into a module of its types (see spec 11), which read and write protobuf's wire format
  through std/data/protobuf.fb.
  - A message is a Struct, a nested one named after the messages it's in, like `Outer_Inner`,
    with a Field for each of its fields, named as the .proto names it: a proto3 field without a
    label is its value, which isn't written while it's its zero; one with presence, optional,
    proto2's required, or a message, is a `T?`; a repeated one a `T[]`; and `map<K, V>` a
    `Map<K, V>`. A oneof's fields are Properties, and setting one to a value takes the others'
    away; the oneof's name is a Property of an Enum of which one holds a value, or NONE. A
    message that holds its own type, through messages it holds one at a time, keeps it in a
    `ProtoBox` behind a Property, since a Struct can't hold itself. A proto2 default is
    `xOrDefault`'s value while x is Null. A name FutureBlitz reserves takes a '_', and so does a
    field named like a member the Struct has or a type of the module.
  - Each message has a New of its fields by name, with their zeros, `Encode()`,
    `Decode(bytes)`, `Merge(bytes)` and `MergeFrom(other)`, which merge as protobuf does,
    `Clone()`, `IsInitialized`, `==`, and `ToString()`, protobuf's text format on one line, as
    `name: "Ada" phones { number: "555" }`, with an enum's values by their names in the .proto
    and a map's entries by key, as protoc writes it, and `WriteTo`, `ReadFrom`, `WriteText`, and
    `FindMissing`, which the modules' code calls for a message inside another. Decode throws a ParseError for bytes that stop
    inside a field (UNEXPECTED_END), that aren't the wire format, or a string that isn't UTF-8
    (SYNTAX), past protobuf's limits of 100 messages or groups deep and a length an array holds
    (OUT_OF_RANGE), or without a required field (OTHER). Encode panics where it's called for a
    required field without a value. The fields a message doesn't know, and one of a wire type
    its type doesn't have, it keeps as they were written and writes back after its own, which
    it writes by number, proto3's repeated numbers packed. The last value read of a field that
    holds one wins, a message's merges, and a repeated field reads packed or not.
  - An enum is an Enum of Int32: proto3's are `@Open`, keeping the numbers they don't name,
    while proto2's are closed, and a number one doesn't name goes to the message's unknown
    fields, with a whole map entry for a map's value. An alias is a Static Property, a closed
    enum's `FromNumber(n)` gives its value or Null, and `ProtoName(value)` gives a value's name
    in the .proto, or an open enum's number it doesn't name.
  - A service is an Interface of an Async Method for each rpc, which takes its request and
    gives its response, and can throw any Error, as a transport would; a stream's side is an
    array. No transport is std's yet.
  - An extension's fields are read as fields the message doesn't know, and kept; editions and
    message sets aren't read.

Step 20 built TOML, CSV, INI, XML, and YAML, each read into std.core's `Value`s and written from
them, so `Load<T>` and `Save<T>` go through them too. `Import std.data` brings in all of
std/data/. Each reader throws a ParseError with the Line and Column it's at, and says them, where a
line ends with LF, CRLF, or a CR alone, and a byte order mark at the start isn't a column:
UNEXPECTED_END for text that ends inside something, OUT_OF_RANGE for a number its type doesn't
hold, and SYNTAX for the rest that isn't its format. None of the five has bytes: TOML's and
YAML's Write panic at a BYTES Value, and CSV's, INI's, and XML's take only a String, a number, a
Bool, or Null where their text goes.

- `Toml.Read(text)` reads TOML 1.0 into an Object: a table is an Object, an array an Array, an
  integer an INT, which an Int64 holds, a float a DOUBLE, inf and nan too, and a date, a time, or
  both a String of its RFC 3339 text, with 'T' and 'Z', and its fraction of a second without the
  0s at its end. Which tables a document can define, and where, follows TOML 1.0 as tomli reads
  it, and it passes toml-test's cases for TOML 1.0. `Toml.Write(value)` writes an Object's keys
  and values, then its tables under their headers, an Array of Objects as an array of tables and
  an Object in an Array as an inline table, a key quoted where it must be; TOML has no null, so
  it leaves out an Object's Nulls, and panics at a Null in an Array, and at a UINT, past TOML's
  integers. Arrays and inline tables nest 256 deep at most, a ParseError past that when read and
  a panic when written; tables nest however deep.
- `Csv.Read(text, separator)` reads RFC 4180's rows, each a `String[]` of its fields, quoted or
  not, over LF, CRLF, or CR lines, and `ReadRecords` an Array of Objects by the first row's names,
  each field a String, which throws for a name given twice, and for a row of more or fewer fields.
  `Write(rows, separator)` and `WriteRecords(records, separator)` write them, LF after each row,
  and quote a field that holds the separator, a '"', or a line break, starts or ends with a space,
  or starts with a byte order mark. CSV can't hold a row of no fields, which reads as one empty
  field, so Write panics at one, and WriteRecords at records with no keys; no records are no
  text.
- `Ini.Read(text)` reads `[section]` headers, `key = value` and `key: value` lines, and `;` and
  `#` comments into an Object: the keys before any section are its own, and each section is an
  Object, every value a String, its spaces trimmed but in '"'s. A key given again takes its new
  value, and a section named again goes on adding to the first. `Ini.Write(value)` writes them
  back, and panics at what INI can't hold, or wouldn't read back as the same: an Array, an Object
  in a section, a line break, or a key or a section's name with a space or a tab at its end, or a
  key that starts with a byte order mark.
- XML 1.0. `XmlReader(text)`'s `Next()` gives a document's `XmlEvent`s, START, END, TEXT, CDATA,
  COMMENT, and INSTRUCTION, as a non-validating processor reads them: line ends as LFs,
  references replaced, attributes normalized, and the internal subset's entities, attribute
  defaults, and attribute types used. The external subset and external entities aren't read, and
  names keep their prefixes, unresolved. Entities nest 64 deep at most, in attributes as in text,
  and the text they expand to comes to ten times the document's size and a MiB at most, as expat
  and libxml2 cap it, each a ParseError past that. It passes the W3C XML conformance suite's 1627
  cases
  for XML 1.0's fifth edition that need no external entity and are UTF-8, which is run outside
  the repo, as the suite's terms don't let it be copied in parts. `Xml.Read(text)` builds a tree
  of `XmlNode`s over it: elements, with `Name`, `Attributes`, and `Children`, and text, CDATA,
  comments, and processing instructions, with `Content`; a node's `Text` is XPath's string of it,
  and it prints as XML of its kind. An element has `Attribute`, `SetAttribute`, `Element`,
  `Elements`, `Add`, `AddElement`, `AddText`, `Query(path)`, and `QueryFirst(path)`.
  `Xml.Write(root, indent, declaration)` writes XML indented but for what's in an element that
  holds text, so it reads back as the same tree, and an `XmlWriter`, element by element, the same
  but from an element's first text on; both panic at what XML can't hold, an attribute given twice
  too. A tree however deep is read, written, queried, and made a Value without recursing.
- `XmlPath(path)` is XPath-lite: location paths on the child, descendant, descendant-or-self,
  parent, ancestor, ancestor-or-self, following-sibling, preceding-sibling, self, and attribute
  axes, abbreviated or not; tests of a name, `*`, `text()`, `node()`, `comment()`, and
  `processing-instruction()`; predicates of positions, paths, strings, and numbers, compared with
  `=`, `!=`, `<`, `<=`, `>`, and `>=` and joined with `and` and `or`, and the functions `last`,
  `position`, `count`, `not`, `contains`, `starts-with`, `string`, `name`, `true`, and `false`;
  and paths joined with `|`, which binds tighter than the comparisons, as in XPath. A run of text
  and CDATA next to each other is one text node, as XPath's data model has no CDATA, and
  `string()` writes a number without an exponent. A path that isn't one panics at Query's caller.
- `Xml.ToValue(element)` and `Xml.FromValue(name, value)` convert as xmltodict does: an
  element's attributes are keys with '@' before their names, its children keys of their names,
  an Array for a name that comes more than once, and its text the key "#text", or the element's
  Value itself, a String, when it has neither attributes nor children.
- `Yaml.Read(text)` and `ReadAll(text)` read YAML 1.2's documents by its core schema: a mapping
  is an Object, a sequence an Array, a plain scalar Null, a Bool, an integer, an INT or past an
  Int64 a UINT, a DOUBLE, `.inf` and `.nan` and an integer past a UInt64 too, or a String, and a
  scalar in quotes or a block a String. Anchors and aliases, the core schema's tags under any handle a %TAG gives them, and
  documents with their directives and markers are read. A key is its scalar's text, and each key
  of a mapping is its own; a key that's a mapping or a sequence, or a tag that isn't the core
  schema's, on a scalar or a collection, is a ParseError of OTHER, and an `!!int` past a UInt64
  OUT_OF_RANGE. A byte order mark can start any document's prefix, and a quoted scalar holds any
  character but a C0 control other than a tab, as JSON's strings do. It passes yaml-test-suite's
  367 cases for the core schema. `Yaml.Write(value)` and `WriteAll(values)` write block mappings
  and sequences, a scalar plain where the core schema reads it back as the same, else in double
  quotes, and a key of more than 1024 characters explicitly, after '?'.
- Planned: each format's round trip of a Struct through `Load<T>` and `Save<T>`, which step 17
  adds.

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

Step 22 built the codecs and archives over bytes, and a `CoderWriter` and a `CoderReader` stream
std.io's bytes through a codec. `Import std.compress` brings in all of std/compress/.

- Codecs: `Deflate` (RFC 1951), `Zlib` (RFC 1950), and `Gzip` (RFC 1952) over zlib-ng's zlib API,
  `Zstd` over zstd, `Lz4` (LZ4's frame format) over LZ4, and `XZ` over liblzma. Each one's
  `Compress(data, level)` and `Decompress(data, limit)` work on a `UInt8[]` at once, and its
  Compressor and Decompressor, like `GzipCompressor(level)` and `GzipDecompressor(limit)`, take
  bytes in turn: `Add(data)` gives what came out so far, and `Finish()` the rest, and starts again.
  Each is a `Coder`, which a `CoderWriter(writer, coder)` writes through, its `Finish()` writing the
  Coder's rest, and a `CoderReader(reader, coder)` reads through, giving the Coder's rest once the
  Reader ends: `CoderWriter(file, GzipCompressor())` writes a .gz, and `CoderReader(file,
  GzipDecompressor())` reads one. A Coder holds what it hasn't coded, so a CoderWriter's `Flush`
  flushes only its Writer. Levels are each library's: 0 to 9 for Deflate, Zlib,
  Gzip, and XZ, 6 by default, 1 to 22 for Zstd, 3 by default, and 1 to 12 for Lz4, 1 by default;
  another panics at the caller. Gzip writes one member with no name, time, or OS in its header, so
  every OS makes the same bytes; a Zstd or Lz4 frame has its content's checksum, and an XZ stream a
  CRC64. Decompress reads Gzip's members, Zstd's and Lz4's frames, and XZ's streams one after
  another, as the tools do; Deflate and Zlib take one stream, and bytes after it are an error.
  Data made with a preset dictionary isn't supported.
- Data that's bad throws an IOError: `INVALID_DATA` for bytes that aren't the format or are broken,
  `UNEXPECTED_END` for ones that end too soon, and `OTHER` for ones that would come to more than the
  limit, or use what isn't supported. The limit is the most a UInt8[] holds unless it's given a
  smaller one, which guards against data made to decompress huge; a negative one panics. Data that
  needs more memory than that to decompress, as an XZ dictionary or a Zstd window can say, isn't
  supported, and zstd's default window of 128 MiB is its most, so a frame from zstd's `--long`
  past that isn't either. A header's sizes take no memory but what its data gives.
- Archives: `Zip.Read(data)` gives a `ZipReader`, `Tar.Read(data)` a `TarReader`, and
  `SevenZip.Read(data)` a `SevenZipReader`, of a copy of the bytes, with `Entries`, `Count`,
  `Find(name)`, `Contains(name)`, `Read(name)`, which gives the last entry of the name, or Null for
  none, and `ReadAt(index)`, which panics out of range. Each `ArchiveEntry` has its `Name`, with '/'
  between folders and none at its end, its `Kind` (`FILE`, `FOLDER`, or `LINK`), its `Size`, a
  link's `Target`, when it was `Modified`, a `Timestamp?`, and its `Mode`, its Unix permissions,
  like `0o644`, an `Int?`, each Null where the archive doesn't say, as a 7z's aren't read and a zip
  made on Windows has no mode; reading a folder or a link gives no bytes. A name is as the archive
  has it, so a program that makes files of them checks them first. A `ZipWriter` adds files by a
  `ZipMethod` (`STORE`, `DEFLATE`, `LZMA`, `XZ`, or `ZSTD`) at a level, and folders, and a
  `TarWriter` files and folders, each at a `modified` time and with a `mode`, from 0 to `0o7777`,
  which panics past them, or else a fixed one, and each one's `Finish()` gives the archive and
  starts again, panicking for one of more bytes than a UInt8[] holds. Names are UTF-8: a zip's that don't say so, and aren't, are read as
  code page 437, and each byte of a tar's that isn't is U+FFFD. Every entry is checked against its
  CRC-32 where the archive has one.
- Zip, over minizip-ng: Zip64 where sizes need it, and no encryption; what a ZipWriter makes was
  made by Unix, with modes 644 and 755, and last changed on 1980-01-01, unless it's told otherwise,
  so every program and OS makes the same bytes. An entry's time is its extra fields', NTFS's,
  Info-ZIP's extended timestamp, or PKWARE's Unix field, each UTC, or else its DOS date's, read as
  UTC, as nothing says its zone. A ZipWriter's time is a DOS date, an even second from 1980 to 2107,
  and an extended timestamp's exact second from 1970 to 2106. Its names have '/' between folders,
  and a '\' panics; its LZMA and XZ levels go from 1 to 9, as minizip-ng stores an entry at 0. An
  LZMA or XZ entry is decoded over liblzma, with no more memory than XZ.Decompress takes, and only a
  Unix, macOS, or RISC OS entry's mode, or an extra field naming a target, makes a link. Tar: ustar,
  pax's extended headers, GNU's long names and base-256 numbers, a time's before 1970 too, and old
  tars, whose folders are names ending with '/', leaving out devices and FIFOs. A type it doesn't
  know is a file, as POSIX says, and GNU's 'D', an incremental tar's, a folder; GNU's sparse files,
  old and in pax, are listed, and their reads are IOErrors. `Tar.Read` decompresses a .tar.gz,
  .tar.zst, .tar.xz, or .tar.lz4 first, after skippable frames too, and a TarWriter's tar compresses
  to one, as `Gzip.Compress(writer.Finish())` does. A TarWriter writes ustar, with a pax header for
  a name ustar can't hold or that isn't ASCII, or a time, which pax's has to the nanosecond, before
  1970 or past 2242, owned by root, and at 1970's start unless it's told otherwise. 7z, read over
  liblzma: its coders stored, LZMA, with or without its end marker, LZMA2, Delta, and the branch
  filters for x86, PowerPC, IA-64, ARM, ARM Thumb, ARM64, SPARC, and RISC-V, in chains, as 7-Zip
  writes them; BCJ2, PPMd, BZip2, and encryption are IOErrors. A solid folder is decoded once for
  the files read from it in turn, and its links' targets, but one of more bytes than an array holds
  is decoded up to each file read, and kept for none. Writing 7z isn't std's.

#### std.net

Step 28 is built: the addresses, `Url` and `QueryParams`, `Dns`, the sockets, and LAN
discovery. `Import std.net` brings in std/net/.

- `IpAddress`, IPv4 or IPv6, by its `Family`, an `IpFamily`: `V4(a, b, c, d)`, `V6` of eight
  groups, `FromBytes` of 4 or 16, and `Parse(text)`, which takes IPv4's dotted decimals without
  leading zeros and IPv6's groups with one `::` and IPv4's text at the end, as RFC 4291 has
  them, and gives Null for anything else, a zone too. `ToString()` writes IPv6 as RFC 5952 does,
  an IPv4-mapped address's end as IPv4's. `Bytes`, `IsV4`, `IsV6`, `IsLoopback`,
  `IsUnspecified`, `IsPrivate` (RFC 1918's blocks and fc00::/7), `IsLinkLocal`, `IsMulticast`,
  `IsBroadcast`, `IsDocumentation` (RFC 5737's, 3849's, and 9637's blocks), `ToV4()` of an
  IPv4-mapped one, and `ToV6()`; `Loopback`, `LoopbackV6`, `Any`, `AnyV6`, and `Broadcast`.
  Addresses compare IPv4 first, then by bytes.
- `SocketAddress`: an `Address`, a `Port`, and an IPv6 one's `ScopeId`, which `Parse` reads and
  `ToString` writes as `127.0.0.1:80`, `[::1]:80`, and `[fe80::1%3]:80`, a scope by its number.
- `Url`, as the WHATWG URL Standard parses and serializes one: `Url.Parse(text)` and
  `Parse(text, base)`, with a Url or a String as the base, give Null for what isn't one, and
  `url.Resolve(text)` resolves against it. Its `Scheme`, `Username`, `Password`, `Host` (Null
  for none, "" for an empty one), `Port` (Null for none or the scheme's own), `PortOrDefault`,
  `Path`, `HasOpaquePath`, `Query` and `Fragment` (without their ? and #, Null for none), its
  `Origin`, `Params`, and `ToString()`, which reads back as the same Url. Each part
  percent-encodes what the standard says; `Url.Encode` and `Url.Decode` encode and decode a
  component, Decode reading its bytes as the Encoding Standard's UTF-8 decoder does, with one
  U+FFFD for a code point's start that's cut short. It passes web-platform-tests' urltestdata.json, in tests/data, but for the case
  that needs UTS #46's mapping of a host that isn't ASCII, which waits for std.text.unicode:
  until then, such a host's ASCII letters map to lowercase, fullwidth ASCII to ASCII, and the
  ideographic full stops to a dot; its spaces map to a space, which no host has; soft hyphens
  and zero widths are left out; U+FFFD, C1 controls, and noncharacters fail it; and its labels
  become Punycode. An xn-- label has to decode to one that's kept as it is.
- `QueryParams`, a query's pairs as a form sends them (`a=1&b=two+words`): `QueryParams(query)`,
  `Get`, `GetAll`, `Contains`, `Add`, `Set`, `Remove`, `Sort` (a merge sort by UTF-16 code
  units), `Count`, `For ... In` over its (name, value) pairs, and `ToString()`; its names and
  values decoded as Url.Decode does.
- `Dns.Resolve(host)` and `Dns.ResolveAsync(host)`: a host's addresses, each once, in the OS's
  order, through libuv's getaddrinfo on the I/O thread, where "localhost" is the loopback
  addresses and an address's text is itself; a NetError of `HOST_NOT_FOUND` for a name with
  none, an empty one, one with a NUL, or one longer than 255 bytes, and of `OTHER` for a lookup
  that fails another way. ResolveAsync's addresses come back in an array the I/O thread makes,
  so a coroutine that goes before its lookup finishes leaves nothing of its own to it.
- The sockets are the runtime's, over libuv's handles on the I/O thread, each closed when its
  object goes, or at `Close()`, which returns once its port or name is free; the program's end
  closes what's left. A blocking call runs nothing else on its thread meanwhile, and an Async
  one waits on the main thread, keeping the program running, as a timer does, until it's done
  or its socket closes. A timeout is a `Duration?`, Null to wait as long as it takes, and one
  that runs out throws a `TimeoutError`. Errors are NetErrors: `REFUSED` where nothing listens,
  `RESET`, `UNREACHABLE`, and `CLOSED` for a socket that's closed, or that closing stopped.
- `TcpListener.Listen(address, backlog)` or `Listen(host, port)`, port 0 for the OS's pick, with
  `LocalAddress`, `Accept(timeout)` and `AcceptAsync`, giving `TcpStream`s; `TcpStream.Connect`
  and `ConnectAsync` to a SocketAddress, or a host and port, trying its addresses in Dns's order,
  each for the timeout. A TcpStream is a Reader and a Writer, so std.io's Copy, ReadAll,
  ReadExact, buffered and binary streams take one, with `ReadTimeout`, `LocalAddress`,
  `RemoteAddress`, `ShutdownWrite` (and its Async twin), which ends what it writes, a Write
  after it throwing `CLOSED`, and Nagle's algorithm off. A stream reads from when it connects,
  up to 64 KiB ahead of what's taken, so a ReadTimeout of zero takes what's come without
  waiting. A Read or Write that fails throws an IOError, as a Reader's does, whose Cause is the
  NetError or TimeoutError. A listener's backlog is 511 unless it's given, and 1 at least, or a
  panic, as libuv refuses less on Windows.
- `UdpSocket.Bind(address, reuseAddress)` or `Bind(host, port, reuseAddress)`, `Send(bytes,
  target)` and `SendAsync`, of up to 65507 bytes, a NetError of `OTHER` past that on every OS
  (macOS sends 9216 at once unless it's told otherwise), `Receive(timeout)` and `ReceiveAsync`,
  giving a `Datagram`, its `Bytes` and where it came `From`, what came since the bind there for
  a Receive that doesn't wait; `SetBroadcast`, which a broadcast needs, `SetMulticastLoopback`,
  `SetMulticastTtl` of 1 to 255 hops, or a panic, `SetMulticastInterface`, `JoinMulticast`, and
  `LeaveMulticast`.
- `IpcListener.Listen(name)` and `IpcStream.Connect(name)`, one IPC type, a named pipe
  (`\\.\pipe\<name>`) on Windows and a Unix socket (`<name>.sock` in the temporary folder)
  elsewhere, chosen by libuv, never by the program; a name is 1 to 64 ASCII letters, digits,
  '.', '-', and '_', or a panic, and the same whatever its letters' case on every OS, as a named
  pipe's is, so its file's name is in lowercase. An IpcStream is a Reader and Writer as a
  TcpStream is, but for ShutdownWrite, since a named pipe can't end one way only. A listener
  holds a lock on `<name>.sock.lock` beside its Unix socket, which says the name is taken
  without connecting to it; a Unix socket's file that a program left behind is taken over, and
  any other file there stays. A path too long for a Unix socket's address, under a long TMPDIR
  as macOS's is, goes in /tmp. On Windows a name is taken until its connections close too, both
  ends.
- LAN discovery is mDNS and DNS-SD's (RFCs 6762 and 6763), as Bonjour and Avahi have them, over
  IPv4 through each interface but loopback's: `LanAnnouncer.Start(type, name, port, text)`
  offers a service of a type like `"_mygame._udp"`, answering on the main thread until `Stop()`,
  or until it goes; `Lan.Find(type, timeout)` and `FindAsync` give the `LanService`s that answer
  (`Type`, `Name`, `Host`, `Port`, `Addresses`, `Text`), asking twice; `Lan.LocalAddresses()`
  and `Lan.HostName()`. Each takes a discovery port of its own in place of mDNS's 5353, which
  keeps it from other responders. What a LanAnnouncer sends through an interface, or answers a
  querier on that interface's network, gives that interface's address alone, and it doesn't
  answer with what a query says it knows, as RFC 6762 has it; a LanService's Addresses start
  with the one its answer came from, which the finder can reach. Stop's goodbye has other mDNS
  caches drop the service's PTR, SRV, and TXT, but not its host's addresses or its type's place
  in DNS-SD's enumeration, which other services can have. A text has each key and value 255
  bytes at most, 8192 in all, so a response fits in one of mDNS's packets, and each key once
  whatever its letters' case, or a panic; a Find keeps a key's first, as it's written. A
  LanAnnouncer doesn't probe for a name another has, and a blocking Find doesn't find what its
  own thread announces, which only answers while that thread runs Async code. When nothing
  reaches the LAN, as macOS's Local Network privacy has it for a program it doesn't allow (every
  send to the LAN is UNREACHABLE then, broadcasts too), or on a machine without a network,
  discovery goes through loopback's interface, so programs on the machine still find each
  other. UDP broadcast is UdpSocket's.
- std.net.tls (step 29), TLS 1.2 and 1.3 over mbedTLS, through memory, so over any stream that's
  a Reader and a Writer, blocking or Async: `TlsClient`, trusting the OS's roots, as std.net.http
  reads them, or `TrustOnly(roots)` and `TrustOsRoots()`, with `Offer(protocols)` for ALPN and
  `Identify(chain, key)` for a server that asks; `Connect(stream, host)` and `ConnectAsync`,
  whose server's certificate has to be for the host, a name, which goes as the server_name, or
  an IP address, which goes as none (RFC 6066) and has to be in the certificate's
  subjectAltName. `TlsServer(chain, key)`, with `Offer` and `RequireClients(roots)`;
  `Accept(stream)` and `AcceptAsync`. Each gives a `TlsStream<S>`, a Reader and a Writer, with
  `Version` ("TLS 1.2" or "TLS 1.3"), `Protocol`, `PeerCertificate`, `Stream`, and `Close` and
  `CloseAsync`, which send close_notify the first time, after which a write throws an IOError
  whose Cause is a NetError of CLOSED, and what the other side sends can still be read. A TLS
  identity's key is ECDSA P-256 or RSA, its certificates are ones mbedTLS reads, and its first
  certificate's key usage and extended key usage are for its side's signatures, or it panics;
  a host is 1 to 255 bytes without a NUL, and the protocols offered are 1 to 255 bytes each,
  8192 in all with a byte each for its length, which a ClientHello has room for, or they panic.
  `TlsListener.Listen(address, server)`, with `Accept` and `AcceptAsync`, gives TLS over TCP,
  whose timeout covers the TCP connection and its handshake, so a client that says nothing
  holds it no longer; a backlog below 1 panics. A handshake that fails is a NetError of TLS that
  says why, like "the server's certificate has expired" or "the client's certificate isn't for a
  TLS client", or of CLOSED for a stream that ends first, or the stream's own failure: a
  socket's NetError or TimeoutError, or else a NetError of OTHER; the alert that tells the other
  side why is flushed through a stream that holds what's written. Past the handshake, TLS that
  fails is an IOError whose Cause is a NetError of TLS, or an IOError of UNEXPECTED_END for a
  stream that ends without close_notify, and the stream's own failure is thrown as it was. On
  Windows, whose ROOT store holds only the roots Windows has fetched so far of those it trusts,
  a chain whose one flaw is that none of the store's roots signed it is checked as Windows
  checks one for Schannel, by CertGetCertificateChain and its SSL policy, which fetch the root
  it needs, so a server Windows trusts is trusted whatever the machine has run before; the HTTP
  client's TLS does the same. A program over a stream that isn't a socket links no libuv.
- std.net.http's client (step 29), over libcurl, whose TLS is mbedTLS's, trusting the OS's roots:
  Windows' ROOT store, each that's valid now and for servers, and what Windows trusts besides,
  as std.net.tls has it, macOS's system anchors, or the first of the bundle files Linux
  distributions and the BSDs keep, or the one `SSL_CERT_FILE` names. `HttpMethod` (GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS); `StatusCode`, its named
  statuses as Ints, `StatusCode.NOT_FOUND`, with `Reason`, `IsSuccess`, and `IsRedirect`;
  `Headers` (`Add`, `Set`, `Get`, `GetAll`, `Contains`, `Remove`, `Clear`, `Count`, and
  `For name, value In`), whose names keep their letter cases but match in any, and which panic
  for a name that isn't RFC 9110's token, or a value with a control character but a tab; `Body`
  (bytes with a Content-Type, which panics as a header's value does, `Text`, `Json`, and `Form`,
  a Map's fields as application/x-www-form-urlencoded); `HttpResponse` (`Status`, `Headers`,
  `Bytes`, `Text` as UTF-8, `Url` after the redirects, `Version`, "HTTP/1.1" or "HTTP/2",
  `IsSuccess`, `EnsureSuccess`). `HttpClient`: `Get`, `Head`,
  `Post`, `Put`, `Patch`, `Delete`, `Send(method, url, body, headers)`, and `Download(url,
  progress)`, the body's bytes, which tells `progress` of how many came and how many the response
  said, or -1 when it didn't say, and nothing of a redirect's own body, and `Download(url, path,
  progress)`, which writes the body to the file at `path` as it comes, atomically, as
  `File.WriteBytesAtomic` does: into a new temporary file in its folder, renamed over the file,
  onto the disk, once all of it came, so the file holds all of it or what it held before, which a
  status that isn't success, a transfer that fails, and an IOError for a file that can't be
  written (a folder that isn't there before anything's sent) leave; each with an Async twin that
  the I/O thread runs, telling progress on the main thread, and writing the file there;
  `DefaultHeaders`, which the request's own replace by name; `UserAgent`, checked as a header's
  value is; `FollowRedirects` and `MaxRedirects` (10), which the runtime follows as the fetch
  standard says, a 301 or 302 changing a POST to a GET and a 303 any method but HEAD, without the
  body and its headers, and a redirect to another origin sending no Authorization, Cookie, or
  Host of the request's own; `Decompress`, which asks for gzip, deflate, and zstd and decodes
  them, or else takes a body as it comes;
  cookies, kept for each client's requests (`ClearCookies`), which a request's own Cookie header
  joins in one; connections, which its requests reuse; `Proxy`, `""` for none, or else the
  environment's (http_proxy, https_proxy, no_proxy); and `TrustOnly(roots)` and
  `TrustOsRoots()`, for the server and an HTTPS proxy alike. A failure is a NetError of its Kind
  (a URL that isn't HTTP's or HTTPS's is OTHER's, as are a redirect to one, too many redirects,
  and a URL, Proxy, User-Agent, or Cookie with a NUL or longer than libcurl's 8,000,000 bytes,
  which isn't sent), or a TimeoutError;
  `EnsureSuccess`, and what gives only a body, `Download`, `Http.GetText`, and `Http.GetBytes`,
  throw a NetError of `HTTP_STATUS` for a status that isn't success. `Http.Get`, `GetText`,
  `GetBytes`, `Head`, `Post`, `Put`, `Patch`, `Delete`, and `Download`, and `GetAsync`,
  `GetTextAsync`, `GetBytesAsync`, `PostAsync`, and `DownloadAsync`, send one request each
  through a client of their own. A client's `Timeout`, a Duration, or Null for none, as it is to
  start, is how long a request may take, and `ConnectTimeout` (30 seconds) how long its
  connecting may; past either is a TimeoutError. It speaks HTTP/2 where TLS's ALPN agrees on it,
  over nghttp2, and HTTP/1.1 otherwise. A program
  that only makes Headers or a Body links no libcurl.
- std.net.http's server: `HttpServer`, whose handlers are Async code on its own loop (no data
  races, and Globals work), with `Workers = n` to spread them over threads once they're
  Sendable-safe; routes as
  `server.Get(path, handler)` and router groups, or as a Class whose Methods carry `@Get`,
  `@Post`, and the rest, registered with `server.Routes<T>(instance)`, binding path parameters
  and JSON bodies to typed parameters; middleware; static files; and automatic HTTPS, getting
  and renewing Let's Encrypt certificates (ACME).
- std.net.ws: WebSocket client and server (the server from an HttpServer handler).
- std.net.game (built in step 31; `Import std.net.game` brings in std/net/game/): reliable UDP
  for games, ENet-style. A `GameHost` is one end of any number of connections over a
  `GameLink`: a UdpSocket's, through `GameHost.Bind(address, settings)`, or any Class of a
  program's own, as a test's network that loses datagrams on purpose. `Connect(address)` gives a
  `GamePeer`, connecting, and a host takes those others ask for, up to its `MaxPeers`, which
  counts only those: a handshake of salts, whose XOR is the connection's, which refuses a full
  host, or one whose channels or largest message differ (`DisconnectReason.FULL` or
  `MISMATCH`). Two hosts that ask each other at once connect, the one of the larger salt
  answering. A peer's
  `Send(channel, bytes or text)` goes at the next `Update(now)`, which takes what's come, sends
  what's due, and gives the `GameEvent`s since the last: `CONNECT`, `RECEIVE` (a channel and
  its `Bytes` or `Text`), and `DISCONNECT`, with why (`LOCAL`, `REMOTE`, `TIMEOUT`, `FULL`, or
  `MISMATCH`). `Service(timeout)` and `ServiceAsync` wait for events, and `Broadcast` and
  `Close` act on every peer. Channels are `GameSettings.Channels`' `ChannelKind`s:
  `RELIABLE_ORDERED`, each message once and in order; `RELIABLE_UNORDERED`, each once, as soon
  as it can; and `UNRELIABLE`, never after a newer one, so what comes is the freshest. A message
  past a packet's `Mtu` (1200 bytes) goes in fragments, which are put back together, up to
  `MaxMessageSize` (1 MiB), with `MaxBuffered` (4 MiB) held at once: a sender keeps what's in
  flight to it, from each channel's oldest message not acked, and a receiver doesn't ack a
  packet with a reliable part it hasn't room for, so it comes again. Each packet has a 16-bit
  sequence, and acks the latest of the other side's and the 32 before it; a packet with
  messages, or a keepalive, asks to be acked at the other side's next Update, and its ack times
  the round trip. A reliable part goes again once a round-trip time and four times its variance
  have passed, as TCP's RFC 6298 works them out, each time waiting twice as long, up to 8 times. A Flush sends 16 packets at most,
  and a peer acks every 16 it takes, so an ack's loss loses nothing. Reliable channels keep
  1024 messages in flight, and ids and sequences go round; an unreliable id far behind the last
  given, in a newer packet, is past an outage, and newer. `Disconnect` waits for what's sent to
  be acked, `DisconnectNow` doesn't, and nothing heard for the `Timeout` (10 seconds) ends a
  connection, with a keepalive each `KeepAlive` (250 milliseconds). A peer has
  `RoundTripTime`, `RoundTripVariance`, `PacketLoss`, `PacketsSent`, `PacketsReceived`,
  `PacketsLost`, `Resends`, `BytesSent`, `BytesReceived`, and `Unacked`. Time is the host's to be
  given, so a test repeats; a seed for its salts and first sequences does too. A bad channel or
  a message too big, to `Send` or `Broadcast`, panics at the caller, and so do settings that
  make no sense: an Mtu that can't hold a CONNECT of the channels, more than 65535 fragments to
  a message, or a KeepAlive or ConnectRetry that isn't under the Timeout.

#### std.db

Built in step 23, and `Query<T>` over step 17's `Load<T>`. `Import std.db` brings in std/db/.

- SQLite: `Database.Open(path)` opens a file, made when there's none, `":memory:"`, or a URI
  file name, like `"file:data.db?mode=ro"` or `"file:name?mode=memory&cache=shared"`. Foreign
  keys are checked, and a Database waits up to 5 seconds for another connection that holds the
  database. `Close()` closes it early, finalizing its Statements and rolling back an open
  Transaction, so its file and locks go then, and `IsClosed` says so; using it or its
  Statements after that panics, but for a Statement's `Parameters`.
- `Execute(sql)` runs each statement in turn, stopping at the first that fails, and panics at
  one that takes parameters, which would be NULL. `Execute(sql, values...)` and
  `Rows(sql, values...)` run one statement with its parameters (`?`, `?NNN`, `:name`, `@name`,
  `$name`) bound to up to eight values, in order, one each, and `Rows(sql)` one that takes none.
  A value is a `SqlValue`: an integer, a Bool (1 or 0), a Float32 or Float64 (a NaN, which
  SQLite keeps as NULL, throws), a String, a `UInt8[]` as a blob, or `Nullable(value)`, which
  is NULL when its value is Null; a type of the program's is one too when its `BindTo` binds
  one of these. `Changes` and `LastId` are SQLite's changes and last rowid.
- `Rows` reads every row when it's called, as a `Row[]`: `Count`, `Name(column)`,
  `Type(column)`, a `ColumnType`, `IsNull`, and `Integer`, `Real`, `Text`, and `Blob`, by the
  column's index or name, each a `T?` that's Null for NULL, with the columns the statement has
  as it runs. A value of another type panics, but `Real` reads an integer too.
- `Prepare(sql)` gives a `Statement` to run many times: `Bind(index, value)` and
  `Bind(name, value)`, `Clear()`, `Parameters`, and `Execute`, `Rows`, and `Query<T>`, with
  values, or as they're bound.
- `Begin()` gives a `Transaction`, with `Commit()`, `Rollback()`, and `IsOpen`. One let go of
  without a Commit rolls back, as when an error leaves the code holding it, so a failed
  transaction leaves nothing. The outermost is BEGIN IMMEDIATE, and one begun inside another is
  a SAVEPOINT, a part of it, which the outer one's Rollback undoes.
- `Migrate(migrations...)`: each migration's number is its place, from 1, the database's
  `PRAGMA user_version` is the last that ran, and each runs once, in order, in a Transaction of
  its own with the version it makes; a database past the program's last throws, as when another
  connection migrates it further while this one waits to begin.
- A failure throws a DatabaseError of `CONSTRAINT` (SQLite's 19), `BUSY` (5 and 6), `QUERY` (1,
  20, and 25, SQL that isn't one statement where one is taken, and SQL with a NUL), `CORRUPT` (11
  and 26), or `OTHER`, with SQLite's message. A wrong number of values, a column or parameter
  that isn't there, a wrong type read, a Transaction ended twice, and a Commit while a
  Transaction begun inside it is open panic at the caller.
- `Query<T>(sql)` and `Query<T>(sql, values...)`, and a Statement's `Query<T>()` with its
  parameters as they're bound and `Query<T>(values...)`, binding up to eight values as Rows
  does, read each row as a T. The values' types are type arguments after T, inferred from the
  values, as in `db.Query<Player>(sql, id)`. `Load<T>` makes the T of an Object of the row's
  columns by their names, each an integer, a Double, text, bytes, or Null for NULL, as many as
  `Rows` reads: a Struct's or Class's Fields take the columns of their names, or of the keys
  `@Rename` gives them, as `SELECT name AS Name` or `@Rename("name")` does, a Bool Field takes 0
  or 1, as SQLite keeps one, and a Field no column has takes its default, or else its zero; a
  column a T has no Field for, or the second of a name, is left alone. A row that doesn't make a
  T throws a ParseError, whose Path starts with the row's index, as `[2].Level`.
  `Query<T>(sql)`, like `Rows(sql)`, panics for SQL that takes parameters, and each Query for
  another number of values than the SQL takes, as Rows does. No query builder and no server
  databases.

#### std.image

Step 24 built the codecs over bytes, into an `Image`, and its transforms, and step 25 drawing and
fonts; an Image's pixels are a `Buffer<UInt8>`, and images and fonts load from files through
std.io. `Import std.image` brings in all of std/image/.

- `Image(width, height)`: pixels in rows from the top, each RGBA, 8 bits a channel, its alpha
  straight, all transparent black at first: `Width`, `Height`, `[x, y]` and `[x, y] = color` as
  `Color32`s, which panic outside it, `ToBytes()` and `Image.FromBytes(width, height, bytes)`,
  copies of its bytes, 4 a pixel, and `Clone()`, whose pixels are its own. Its bytes are in a
  `Buffer<UInt8>`, `Pixels`, the image's own, which C code reads and writes through its
  `Pointer()`, and `Image.Over(width, height, pixels)` is an image over a Buffer, not a copy, so
  what changes in one changes in the other, until Resize, Crop, or a flip or turn gives the image
  new pixels; it panics at the caller for a Buffer of another count. It holds 536870911 pixels at
  most, as a UInt8[], which ToBytes gives, holds their bytes. An `HdrImage` is the same of
  `Color`s, linear light that can be past 1, over a `Buffer<Float32>`, with `ToFloats()`,
  `HdrImage.FromFloats`, and `HdrImage.Over`, and holds 134217727 pixels at most, as many as
  stb_image reads of an HDR image.
- The formats are each a Struct of Static Methods. `Png`, `Jpeg`, `Bmp`, `Tga`, `Gif`, `Qoi`, and
  `WebP` each `Decode(data, limit)` into an Image, and `Hdr.Decode` into an HdrImage; and they
  encode: `Png.Encode(image, level)`, 0 to 9 and 6 by default, deflated by zlib-ng, RGB when every
  pixel is opaque and RGBA when one isn't; `Jpeg.Encode(image, quality)`, 1 to 100 and 90, baseline,
  without alpha; `Bmp.Encode`, 32 bits a pixel, and `Tga.Encode`, run-length encoded, with alpha;
  `Qoi.Encode`; `Gif.Encode`; `WebP.Encode(image, quality)`, 0 to 100 and 75, lossy, its alpha
  lossless, and `WebP.EncodeLossless(image, effort)`, 0 to 9 and 6, which keeps the colors of
  transparent pixels too; and `Hdr.Encode`, Radiance's RGBE, run-length encoded, without alpha,
  a channel that's negative or NaN as 0, and one past RGBE's most, 255 times 2^119, as that
  most. A level or quality out of range, or an image a format can't hold, with no pixels or a
  side longer than 65535 for JPEG, TGA, or GIF, or 16383 for WebP, panics at the caller, as does
  one of more than 268435456 pixels for PNG or APNG, as many as stb_image reads back.
  `Image.Decode(data, limit)` reads any of them but HDR by how its bytes start, and
  `Image.FormatOf(data)` gives the `ImageFormat` they start as, or Null; a TGA has no signature,
  so it's what starts as a TGA's header does once no other format matches.
- `Image.Load(path, limit)` reads a file's image as Image.Decode reads its bytes, and
  `Font.Load(path)` a file's font as Font.Parse does, each through std.io's `File.ReadBytes`;
  `Image.LoadAsync` and `Font.LoadAsync` are their Async twins, which read the file on libuv's
  thread pool and decode or parse it once the Await resumes. A file that can't be read throws
  File.ReadBytes's IOError, and one that isn't an image or a font what Decode or Parse throws,
  after "can't load" and the path, its `Path` the file's. A file is written by encoding the
  image and handing the bytes to `File.WriteBytes`. Only a program that loads a file links
  libuv.
- stb_image reads PNG, of every color type and bit depth, interlaced or not, 16 bits a channel
  kept to their top 8, JPEG, baseline and progressive, but not 12-bit or arithmetic, BMP, of 1 to
  32 bits a pixel, but not run-length encoded, TGA, and HDR; libwebp reads still WebPs, lossy and
  lossless, and codes them on one thread at a time, as its setup isn't safe on two at once; and
  the runtime itself reads QOI, GIF, and an APNG's frames. Data that's bad throws an IOError:
  `INVALID_DATA` for bytes that aren't the format or are broken, `UNEXPECTED_END` for
  ones that end too soon, where the decoder can tell, and `OTHER` for an image whose pixels, all
  its frames' counted, are more than the limit, or that uses what isn't supported, like a BMP's
  run-length encoding or an animated WebP. The limit is the most an Image holds unless it's given
  a smaller one, which guards against data made to decode huge; a negative one panics.
- `Animation(width, height, plays)`: frames of one size, each a `Frame` of an `Image` and its
  `Delay` in milliseconds, from 0 to 65535000, APNG's longest, with `Add(image, delay)`, which
  keeps the image rather than a copy, `Count`, `Frames`, `[index]`, and `Plays`, how many times
  they play, 0 for forever, as by default.
  `Gif.DecodeAnimation` and `Png.DecodeAnimation` give each frame as it's shown, the frames
  before it drawn and disposed of, on a canvas that starts transparent, as browsers draw them; a
  still image is an animation of one frame that plays once. `Gif.EncodeAnimation` and
  `Png.EncodeAnimation(animation, level)` write each frame as the whole image, and panic at the
  caller for a frame whose image isn't the animation's size since it was added: a GIF's delays
  in hundredths of a second, up to 655350 milliseconds, and its plays up to 65536, by
  NETSCAPE2.0's count of the times after the first; an APNG's delays in milliseconds up to a
  minute, in hundredths up to 655350, and in seconds after, and its first frame the image a
  decoder without animation shows.
- A GIF's frame has a palette of its own: its colors when there are 256 or fewer, counting one
  for the pixels whose alpha is below 128, which are transparent, or else the 256 that median cut
  picks, or 255 and the transparent one when a pixel is, with Floyd-Steinberg dithering.
- `Svg.Parse(text)` reads an SVG at 96 dots an inch, over nanosvg, and throws an IOError of
  INVALID_DATA for one without a size: `Width` and `Height`, in pixels, and `Rasterize(scale)`, its
  size times the scale, each side rounded up, or `Rasterize(width, height)`, as large as it fits,
  in the middle, its edges antialiased. nanosvg leaves out SVG's text, images, filters, masks, and
  clipping, and style sheets but for style attributes.
- Every target gives the same pixels: the libraries and Resize are built without fused
  multiply-adds, the libraries' SIMD code gives what their plain C gives, and Lanczos's sines are
  Resize's own. An SVG's curves go through the C library's math functions, which can differ by a
  little from one OS to another.
- An Image's transforms change it in place. `Resize(width, height, filter)` gives each pixel the
  colors under it by a `ResizeFilter`, `NEAREST`, `BOX`, `BILINEAR`, `BICUBIC`, or `LANCZOS`, by
  default, the filter widened as the image shrinks, as Pillow resamples, across and then down,
  kept from 0 to 255 between; its colors are weighted by their alpha, so a transparent pixel's
  counts for nothing. Its sums are Doubles where Pillow's are fixed-point, and an opaque image's
  bytes are within 1 of Pillow's; a translucent one's colors are more exact than Pillow's, which
  premultiplies alpha in bytes, rounding a faint pixel's color away. `Crop(x, y, width, height)`
  keeps the pixels from (x, y); `FlipHorizontal`, `FlipVertical`, `RotateClockwise`,
  `RotateCounterclockwise`, and `Rotate180` flip and turn it; `Draw(image, x, y)` draws another
  image over it by its alpha, as "over" does, and `Paste(image, x, y)` puts one's pixels, alpha
  too, in place of its own, each leaving out what falls outside, and drawing an image whose
  pixels share memory with its own, itself or another view of the same Buffer, as they were
  before; and `Fill(color)` gives every pixel a color. A size New panics for, resizing an image
  of no pixels to some, or cropping to pixels not all in the image panics at the caller.
  `HdrImage.FromImage(image)` and `ToImage()` convert through sRGB's gamma, as Color32's
  `ToColor` and Color's `ToColor32` do, ToImage keeping each channel from 0 to 255.
- `Canvas(image)` draws on an Image (step 25): `FillRect` and `StrokeRect` of a `Rect`,
  `StrokeLine`, `StrokePolyline` (open), `StrokePolygon` (closed), `FillPolygon` of a ring or of
  rings, `FillCircle`, `StrokeCircle`, `FillEllipse`, and `StrokeEllipse` (by its radii across
  and down), each in a `Color32` drawn over the pixels by its alpha, and its state:
  `Antialias`, on by default, `LineWidth`, 1, `Cap`, a `LineCap` of `BUTT`, `ROUND`, or
  `SQUARE`, BUTT, `Join`, a `LineJoin` of `MITER`, `ROUND`, or `BEVEL`, MITER, and
  `MiterLimit`, 4, as SVG's are. A pixel is the square from its (x, y) to (x + 1, y + 1), so a
  line along y = 5.5 covers row 5. A fill fills its region by the nonzero rule, as std.math's
  polygons do, so a ring the other way around in one is a hole, and one that crosses itself is
  filled where it winds; a stroke fills what its pieces cover together, a rectangle half the
  width either side of each segment, a join on each corner's outside, and a cap at each open
  end. Antialiased, a pixel is drawn by the exact part of its square the region covers, its
  color's alpha times that, rounded to the nearest, a half and what's within a billionth below
  it up; not, it's drawn wholly when the region holds its center, which on the region's edge is
  in it on the edge's right, and on its top but not its bottom. A pixel more than 32 edges pass
  through, which only a font made to be slow has, is measured along 16 lines across it instead,
  so drawing takes time for each pixel's edges, never all of a row's at once. A shape or a line
  more than 2^20 pixels past the image is cut where it crosses that far, worked out to twice a
  Double's bits, so what it draws keeps its pixels however far its points are. An ellipse is a
  polygon within a hundredth of a pixel of it, up to a radius of 34 million pixels, past which it
  has 131072 sides, and an ellipse's stroke its polygon's, with miters, of the sides for its
  stroke's outer radius. The drawing is the runtime's own, in Doubles, without fused
  multiply-adds, with sines of its own, so every target gives the same pixels;
  image_draw/make_data.py works out the same drawings in Python's Fractions, and image_draw.fb
  gets their bytes exactly. A coordinate that isn't finite, a rectangle whose far corner isn't,
  a negative size or radius, a negative line width, or a miter limit below 1 panics at the
  caller.
- `Font.Parse(data)` reads a TrueType or OpenType font (step 25), a font file or a
  collection's first font, through the runtime's own reader, which checks each offset before it
  follows it, so a font that's broken or made to mislead throws or draws boxes but never reads
  past its bytes: its glyphs' outlines, TrueType's, simple and composite (offsets, scales, and
  2-by-2 matrices, 8 deep at most), or CFF's, Type 2 charstrings with their local and global
  subroutines (10 deep at most), arithmetic and storage, and endchar's accented characters, a
  CID font's by its FDSelect; cmap's formats 4 and 12, hmtx's advances, and kern's format 0
  kerning. It throws an IOError of INVALID_DATA for what isn't one, and OTHER for a font of
  CFF2 outlines or none, or of charstrings that aren't Type 2's. A CFF font's glyphs are in
  head's units per em, as OpenType has them, whatever its FontMatrix. `UnitsPerEm`, and at a
  size in pixels an em, `Ascent`, `Descent` (above 0), `LineGap`, `LineHeight`, and
  `Width(text, size)`, its widest line's; `HasGlyph(codePoint)`. A Canvas's `FillText(text,
  font, size, position, color)` fills the text as one region, its first line's baseline from
  the position: each glyph after the last's advance and the kerning between them, unrounded,
  each line feed a line's height down, a code point with no glyph the font's .notdef, and each
  quadratic or cubic cut into the chords that keep it within a hundredth of a pixel, but one
  wholly outside the image, which is one chord. A glyph of more than 65536 pieces, or 65536
  points once its curves are cut, which only a font made to be slow to draw has, isn't drawn.
  It neither shapes (no ligatures, marks, or right-to-left) nor reads GPOS's kerning or hints.
  image_text's fonts are made by image_text/font.py and cff_font.py, so they're under no license
  but the project's, and image_text/make_data.py draws their text from those glyphs apart from
  the runtime; tests/runtime/font_tests.cpp breaks them, cut short and changed at random. A size
  that isn't finite or is negative panics at the caller.
- The engine's 2D (M11) will share the image and font code.

#### std.term

Built in step 26. `Import std.term` brings in all of std/term/.

- The console (console.fb): `IsTerminal(stream)`, for a `StandardStream`, `INPUT`, `OUTPUT`, or
  `ERROR`; `TerminalSize()`, a `(columns, rows)?`; `ReadKey()` and `ReadKey(timeout)`, a key as
  it's typed, without showing it; and `ReadPassword(prompt)`, a line without showing it, which
  BACKSPACE and Ctrl+U edit. Ctrl+C still ends the program, and Ctrl+\ is a key everywhere. The
  runtime puts the console back however the program ends, a signal's end too, on the stream it was
  changed on, and before a panic's or an uncaught error's message, or a leak report, so that shows
  on the screen the program started on. On Windows, it's the console API
  with virtual terminal sequences, and the records it gives otherwise are written as a terminal
  would send them; elsewhere, termios and SIGWINCH, and Ctrl+Z puts the terminal back while the
  program is stopped. From a pipe or a file, each character is a key, and a line break is ENTER.
  Keys go through `Terminal.Console()`, whose state is the main thread's, and which reads ahead,
  as std.core's `Input` does, so a program reads stdin one way or the other.
- Styles (style.fb): a `TermColor` is the terminal's `Normal` one, one of the 16 basic ones
  (`Red`, `BrightCyan`, `Basic(n)`), one of 256 (`Indexed(n)`), or `Rgb(r, g, b)`, or
  `Hex("#ff8800")`. A `Style` is a foreground and a background `TermColor` and `TextAttributes`
  (`BOLD`, `DIM`, `ITALIC`, `UNDERLINE`, `BLINK`, `REVERSE`, `HIDDEN`, and `STRIKETHROUGH`), as
  in `Style(TermColor.Red).Bold().On(TermColor.Blue)`, with `Over`, `Codes`, and `Apply`. A
  terminal shows a `ColorLevel`, `NONE`, `BASIC`, `INDEXED`, or `TRUE_COLOR`, and each color as
  the nearest it has. `ConsoleColors(stream)` reads the environment: none where the stream isn't a
  terminal, `NO_COLOR` is set and isn't empty, or `TERM` is `dumb`, and no styles either; 24-bit color where
  `COLORTERM` says so, or on Windows' console; 256 where `TERM` has `256color` in it; and the
  basic 16 otherwise. `Paint(text, style)` styles text for stdout, or gives it alone where stdout
  shows none. `StripStyles` takes the escape codes out of text, a DCS's or an APC's whole, like a
  sixel image's, and `VisibleWidth` counts its columns without them.
- A `Terminal` (terminal.fb) is where widgets write and keys come from: `Terminal.Console()`,
  stdout's, or `Terminal.Console(StandardStream.ERROR)`, stderr's, which share stdin; a
  `FakeTerminal(columns, rows, colors, interactive)`, for tests, which keeps what's written to it
  (`Output` and `TakeOutput`) and gives what it's fed (`Feed`) as a terminal would send it, with a
  `Resize` or a `Pause` between; or a Class of a program's own. It has `Columns`, `Rows`,
  `Colors`, `IsInteractive`, `Write`, `WriteLine`, `Flush`, and `Paint`; raw mode, which
  `StartRawMode(ctrlCIsKey, mouse)` and `EndRawMode` nest; and `ReadEvent`, `PollEvent(timeout)`,
  `ReadKey`, and `ReadPassword`, each in raw mode while it waits.
- Input (input.fb): an `InputEvent` is a `KeyEvent`, a `KeyCode`, the `Character` it typed for
  `CHAR`, and its `KeyModifiers`; a `MouseEvent`, a `MouseAction` and `MouseButton` at a `Column`
  and `Row`, with modifiers; a resize's `Columns` and `Rows`; or pasted `Text`. An `InputParser`
  makes them from a terminal's bytes: xterm's keys and modifiers and the kitty keyboard
  protocol's, Konsole's and the Linux console's F keys, SGR's and X10's mouse reports, but for
  buttons past the third, like back and forward, Alt before any of them, and bracketed pastes. An
  escape alone waits 50 milliseconds for the rest of a sequence, and then it's ESCAPE, and a
  paste waits a second for its end mark, which a resize doesn't cut short.
- Line widgets (widgets.fb): a `Table` of rows, with a header row or not, in a `BorderStyle`'s
  lines (`NONE`, `ASCII`, `SINGLE`, `ROUNDED`, `DOUBLE`, and `HEAVY`), each column with its own
  `Alignment`, cells of several lines, and a `MaxWidth` its widest columns give way to, their cells
  wrapping; and a `Box` of text, with a title or not. Each `Render(colors)`s its text, or `Print`s
  or `Draw`s itself. A `ProgressBar` of a total, with a label and `ProgressParts` (the percent, the
  count, the time elapsed, an ETA, and the rate), and a `Spinner`, with `Succeed` and `Fail`, draw
  themselves again in place, no more often than their interval, on an interactive terminal, and
  once, at their end, on a pipe or a file. They go to stderr unless they're given a Terminal, and
  what reads the clock also takes an `Instant`, so a test gives the time.
- Prompts (prompts.fb): `Confirm(question, default)`, `Ask(question, default)`, or
  `Ask(question, validate, default)` with a check, and `Choose(question, options, default)`, on the console or on a Terminal given. Where both
  what's written and the keys are a terminal's, keys are read as they're typed: Confirm takes y or
  n at once, Ask edits its line as a shell does, and Choose moves through its options with the
  arrows, among those that match what's typed, by std.text.fuzzy. Otherwise, each reads a line,
  and asks again for one it can't take. Each gives Null for Escape and at the end of the input.
- Full-screen apps: a `Screen` (screen.fb) takes the whole terminal, on its alternate screen, in
  raw mode, with the cursor hidden, pastes as pastes, and the mouse's reports or not, all of which
  `Close` puts back, as its Destroy and the program's end do. A program draws on its `Frame`, a
  `Canvas` of `Cell`s, a grapheme cluster and its look each, two cells for a wide one, with
  `Print`, `Fill`, `SetLook`, and `DrawBox`, each kept to the canvas, or to a `Region` of it (a
  negative size, or a Fill of anything but a grapheme cluster, panics), and
  `Present` writes only the cells that changed since it last did, moving the cursor and setting
  styles only where it must; `ShowCursor` shows the cursor where it should be. `ReadEvent` and
  `PollEvent` give keys, the mouse, pastes, and resizes, after which the next Present draws all of
  it, and `Run(draw, handle)` is that loop. A `Region` (region.fb) splits into rows or columns by
  `Constraint`s: `Fixed`, `Percent`, `Min`, and `Fill` by weight, none negative, and a Percent
  no more than 100. Views (views.fb): a `TextInput`,
  edited as a shell's line is, scrolled sideways to keep its cursor in view, with a placeholder
  and a mask, and a `ListView`, which keys and the mouse move through, scrolled to keep what's
  selected in view.

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

Built in step 23, but for `V7`. `Import std.uuid` brings in std/uuid/.

- `Uuid`, RFC 9562's, a Struct of `High` and `Low`, its 16 bytes in its text's order.
  `Uuid.V4()` is random, from SecureRandom, with RFC 9562's version and variant. `Parse(text)`
  reads 32 hex digits, in either case, with dashes after the 8th, 12th, 16th, and 20th, and gives
  Null for anything else; `ToString()` writes them in lowercase. `Bytes`, `FromBytes`, and
  `Version`; `==`, `<` in the order of their bytes and texts, and Hash.
- `Uuid.V7()` (time-ordered) waits for step 16's clock.

#### std.debug

- `IsDebuggerAttached()`, as the OS says: Windows' IsDebuggerPresent, macOS's P_TRACED, and
  Linux's TracerPid, and False on an OS that can't be asked; `Breakpoint()`, which stops in the
  debugger that's attached, as a breakpoint set there would, and does nothing when none is, so
  one left in a program doesn't end it, nor under a tracer that hands the trap back, as strace
  does. Stack traces wait for debug info (spec 09 already says so).

#### std.ffi (after M5's reorganization)

Built in step 12, as spec 12 has it.

- `Mem.AllocAligned` and `Mem.FreeAligned`, of memory at a multiple of a power of two, and
  `Mem.AllocUninitialized` (no zeroing).
- `CLong`, `CULong`, and `CWChar`: built-in integers the target sizes, for C's `long`,
  `unsigned long`, and `wchar_t`, which widen to `Int64` or `UInt64` without `As`, and convert
  as they would on every target; fb-bindgen writes them.
- `CData`, a constraint the compiler decides, so a `Ptr<T>` and SizeOf can take a type parameter
  that has it.
- `Buffer<T: CData>(count)`: native memory it owns, zeroed, at a stable address, freed when its
  last reference goes; bounds-checked `[]`, `Count`, `Slice(start, finish)` views that keep it
  alive, `CopyTo`, `ToArray`, `Iterate`, and `Pointer()` for C in Unsafe code.
  `Buffer<T>.View(pointer, count)`, in Unsafe code only, wraps memory C owns, on the promise
  that it outlives the view. std.io reads into and writes from Buffers without copying.
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
