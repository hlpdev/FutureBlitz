# Standard Library

Status: **draft layout only**. Intentionally not designed in depth yet; language design and
the compiler come first. Full stdlib is milestone M6.

## Split

- **`std`**: the pure language library; works with or without a window.
- **`engine`**: the game engine modules (see [engine.md](engine.md)).

Until `std.core` exists (M5), the compiler provides a temporary built-in `Print(String)`. It's
removed once `std.core` provides `Print`.

## Until modules

Until M5 turns files into modules, every `.fb` file under std/ is part of every program, and all
of them share the program's namespace. A library file only declares things: Functions, Structs,
Classes, Enums, Globals, and Consts. A program gets only the library code it uses.

So far, std/ holds:

| File | Contents |
|---|---|
| std/core/error.fb | `Error`, the Class every error extends |
| std/core/interfaces.fb | `Equatable`, `Comparable`, `Hashable`, `Printable`, and `Numeric`, which `==`, `<`, strings, and generic code go through, and `Sendable`, which the compiler decides |
| std/core/iterator.fb | `Iterator<T>`, which `For ... In` goes over |
| std/collections/array.fb | `Remove`, `Sort`, `Filter`, `Map`, and `Find` on `T[]` |
| std/collections/map.fb | `Map<K, V>`, which `{key: value}` makes |
| std/collections/set.fb | `Set<T>` |
| std/collections/stack.fb, queue.fb, deque.fb | `Stack<T>`, `Queue<T>`, and `Deque<T>` |
| std/collections/priority_queue.fb | `PriorityQueue<T>` |
| std/collections/pool.fb | `Pool<T>` and `Handle<T>` |

A program declaring one of these names, like its own `Stack`, is an error until modules.

## `std` modules

| Module | Contents |
|---|---|
| `std.core` | auto-imported: `Print`, `Assert`, `Panic`, `String`, `Error`, primitive methods |
| `std.collections` | arrays, `Map`, `Set`, `Queue`, `Stack`, `Deque`, `PriorityQueue`, `Pool` |
| `std.math` | `Vec2/3/4`, `Quat`, `Mat3/4`, `Color`, `Rect`, `Random`, noise, `Lerp`, `Clamp` |
| `std.text` | `StringBuilder`, formatting, Unicode helpers |
| `std.io` | `File`, `Path`, `Dir`, streams, binary reader/writer (sync and async) |
| `std.data` | JSON, binary serialization |
| `std.time` | `Clock`, `Stopwatch`, `DateTime` |
| `std.concurrent` | `Task`, `Spawn`, `Scheduler`, `Channel`, `Mutex`, `Atomic`, `Thread` |
| `std.net` | TCP/UDP sockets, HTTP client |
| `std.os` | args, environment, process, platform info |
| `std.ffi` | `Ptr`, `CString`, `Mem`, unsafe helpers |

The standard library is written in FutureBlitz itself, backed by runtime intrinsics where
needed.
