# 14 · Strings and Collections

## Strings

- UTF-8, **immutable**, refcounted. A string is always valid UTF-8.
- `+` only joins `String` to `String`. Everything else uses interpolation.

```vb
Print("HP: {hp}/{MAX_HP}")              // interpolation; {{ for a literal brace
Print("a\tb\nc \\ \" \0 \x41 \u{1F600}") // escapes (see 01-lexical)
Local path = r"C:\raw\no\escapes"       // raw string
Local text = """
    multi-line string;
    the closing quotes' indentation is trimmed
    """
```

| Form | Escapes | Interpolation | Notes |
|---|---|---|---|
| `"..."` | yes | yes | single line; a lone `}` is an error (write `}}`) |
| `r"..."` | no | no | single line; cannot contain `"` |
| `"""..."""` | yes | yes | multi-line |
| `r"""..."""` | no | no | multi-line; may contain `"` |

### Interpolation

`{expr}` accepts any expression, including string literals: `"{name ?? "nobody"}"`. There
are no format specifiers in v0. Numbers print in the shortest form that reads back as the same
value (`0.1`, not `0.100000001`), and a `Bool` prints as `True` or `False`. A tuple prints as
`(1, a)`, and an array as `[1, 2, 3]`. A Struct, Class, or Interface's object that implements
`Printable` prints what its `ToString` gives. A Task, Thread, Channel, Scheduler, Atomic, or
Mutex has no text, so it can't be shown; a Task's result can, through Await or `Wait()`. In a
single-line string, an interpolation must end on the same line.

### Multi-line strings

- The opening `"""` ends its line (only spaces or tabs may follow it); the closing `"""`
  stands on its own line.
- The closing `"""`'s indentation is removed from every line. A non-blank line that doesn't
  start with exactly that indentation is an error. A line that starts inside an
  interpolation isn't string text, so it isn't checked.
- The line breaks right after the opening `"""` and right before the closing `"""` are not
  part of the string. Line breaks inside the string are always `\n`, whatever the source
  file uses.

### Indexing

There is **no** `s[i]` integer indexing (it would be wrong or slow with UTF-8).

```vb
For c In s.Chars ... End For    // Unicode code points
For b In s.Bytes ... End For    // raw bytes
```

- `s.Chars` is a new `Int[]` of the String's code points, and `s.Bytes` a new `Byte[]` of its
  UTF-8 bytes.

## Arrays

```vb
Local nums: Int[] = [1, 2, 3]        // dynamic array (reference type)
nums.Add(4)
Local grid: Float[64, 64]            // fixed-size, multi-dimensional (value type)
Local mid = nums[1..3]               // slice: a new Int[] of elements 1 and 2
Local copy = nums.Clone()
```

| Form | Semantics |
|---|---|
| `T[]` | dynamic, growable, **reference** |
| `T[N]`, `T[W, H]` | fixed size, **value** (stored inline) |
| `arr[a..b]` | slice: a new `T[]` of the elements from `a` up to `b` |

Out-of-bounds access panics.

- An array literal takes the type it's used as, like `Local a: Long[] = [1, 2]`, or else its
  elements' common type, with literals taking it: `[1, 2.5]` is a `Float[]`. Where an
  `Iterable<T>` of a known T is expected, it's a `T[]`, so `[1, 2]` and `[]` go to a
  `Set<Long>`'s AddAll. `[]` needs a type from where it's used. A `T[N]` can come from a literal of N elements; a `T[W, H]` can't.
- A `T[]` has `Count` (an `Int`) and the Methods `Add(value)`, `Insert(index, value)`,
  `RemoveAt(index)`, `Clear()`, `Contains(value)`, and `Clone()`. `Clone` copies the elements,
  not what they refer to. `Contains` compares with `==`. A fixed array has `Count` only.
- A `T[]`'s `Capacity` is how many elements it has room for before it needs more memory;
  `Reserve(capacity)` makes room for that many in all, and `TrimExcess()` gives back what's
  beyond its Count. Neither changes the elements. A negative capacity panics.
- std/collections/array.fb gives a `T[]` the rest of the collections' Methods (below), and
  makes it an `Iterable<T>`, whose `Iterate()` gives its elements, so it goes where an
  `Iterable<T>` is expected, as it does where a `Printable` is, when its elements are.
- An index can be any integer. A `T[W, H]` takes one per dimension, `grid[x, y]`, and stores
  its elements row by row. An index out of range panics: "index 5 is out of range for 3
  elements". A fixed array's index known at compile time is checked then.
- A `T[]` is a reference, so its elements change through any reference to it. A fixed array is
  a value, copied on assignment, and starts with every element at zero; a `Local` of one is
  ready when declared.
- A `T[]` has no zero. A `Global` of one needs a value, and a field needs a default or a `New`
  that assigns it. `[]` is a default anywhere: each use gets a new empty array.
- A `T[]` only holds `T`: a `Boss[]` isn't an `Enemy[]`, since adding an `Enemy` to it would
  break it.
- `==` compares two arrays' elements, when they can be compared; `RefEquals` tells whether two
  `T[]`s are the same array.
- An element can't be passed `ByRef`, and a `ByRef Method` can't change a `T[]`'s element in
  place, since the elements move when the array grows.
- A fixed array's sizes are integers known at compile time: literals, or `Const`s in code.
- A slice, `arr[a..b]`, is a new `T[]` with copies of the elements from `a` up to `b`, without
  the one at `b`. `arr[a..]` goes to the end, and `arr[..b]` starts at the start. It slices a
  `T[]` or a fixed array of one dimension.
- A slice's bounds are integers from 0 to the count, with the start no more than the end. Out
  of range, it panics: "slice 1..4 is out of range for 3 elements". Known bounds are checked at
  compile time.

## Collections (`std.collections`)

| Type | Notes |
|---|---|
| `T[]` | dynamic array (the "list" type) |
| `Map<K, V>` | hash map; literal `{"a": 1, "b": 2}` |
| `Set<T>` | hash set |
| `Queue<T>`, `Stack<T>`, `Deque<T>` | |
| `PriorityQueue<T>` | |
| `Pool<T>` | game-oriented object pool with stable handles; replaces what Blitz `Type` lists were used for |

All collections are reference types and share a common method vocabulary: `Add`,
`Remove`, `Contains`, `Count`, `Clear`, `Sort`, `Filter`, `Map`, `Find`, `Clone`. Each name
means the same thing wherever a collection has it.

```vb
Local hp: Map<String, Int> = {"grunt": 50, "boss": 500}
For name, value In hp ... End For
Local seen = Set<Int>()
```

They're written in FutureBlitz, in std/collections/. Each has a `Count` Property, `Add`,
`Clear`, `Contains`, and `Clone`, which copies the collection but not what its values refer to,
and goes in a `For ... In`. Taking a value from an empty Stack, Queue, Deque, or PriorityQueue
panics; `Count` says whether there is one. Adding or removing a value while a `For ... In` goes
over the collection panics at the loop's next value, rather than skipping or repeating some,
except in a Pool; a Map's key can get a new value meanwhile, and reading never changes a
collection.

| Type | Members besides those |
|---|---|
| `T[]` | `Remove(value)`, the first equal one, giving whether there was one; `Sort()` when T is `Comparable`, or `Sort(before)`, in place and stable, `SortUnstable()` and `SortUnstable(before)`, a heap sort that makes no array, and `SortBy(key)` and `SortByDescending(key)`, whose `ThenBy(key)` and `ThenByDescending(key)` sort again where the keys so far tie; `Filter(keep)`, `Map(change)`, and `Find(test)` and `FindLast(test)`, which give a `T?`; `FindIndex`, `IndexOf`, `LastIndexOf`, and `BinarySearch`, which give an `Int?`, and `Any` and `All`; `First()` and `Last()`, which give a `T?`, `RemoveLast()`, and `TryRemoveLast()`; `AddAll(values)`, `InsertAll(index, values)`, `RemoveRange(start, count)`, `RemoveAll(test)`, which gives how many went, `Resize(count, fill)`, and `Fill(value)`; `Reverse()` and `Swap(a, b)`; `Distinct()`, `Join(separator)`, `Fold(start, combine)`, and `Reduce(combine)`; `View(start, finish)` and `AsReadOnly()` |
| `Map<K, V>` | `Add(key, value)` adds or replaces, `TryAdd` only adds, and `AddAll(other)` adds another's; `map[key]` reads a value and `map[key] = value` adds or replaces one; `Get(key)` gives a `V?`, `Get(key, fallback)` a V, and `GetOrAdd(key, make)` adds what `make` gives when the key isn't there, as the key's value even if `make` added the key; `Update(key, change)` gives a key's value what `change` gives for it, unless `change` removed the key; `Remove(key)` and `RemoveWhere(test)`; `Keys()`; `Values()`; `Filter(keep)`, `Find(test)`, and `Any(test)` with a `Func(K, V): Bool`; `MapValues(change)`; `Reserve(count)` |
| `Set<T>` | `Add(value)` gives whether it's new; `Remove(value)`; `Filter(keep)`; `Find(test)`; `Union`, `Intersect`, `Difference`, and `SymmetricDifference` make a new Set, and `AddAll`, `RemoveAll`, and `KeepOnly` change it, from any `Iterable<T>`; `IsSubsetOf`, `IsSupersetOf`, `Overlaps`, and `SetEquals`; `ToArray()` |
| `Stack<T>` | `Push`, `Pop`, and `Peek`, at the top, and `TryPop` and `TryPeek`, which give a `T?`; it goes from the top down, and so does `ToArray()` |
| `Queue<T>` | `Enqueue` at the back, `Dequeue` and `Peek` at the front, and `TryDequeue` and `TryPeek`; `ToArray()` goes from the front |
| `Deque<T>` | `AddFirst`, `AddLast`, `RemoveFirst`, `RemoveLast`, `First`, and `Last`, and the Try twins of the last four; `Add` is `AddLast`; `deque[i]` reads and sets the value i from the first; `ToArray()` goes from the first |
| `PriorityQueue<T>` | `Pop` and `Peek` give the first: the smallest when T is `Comparable`, or what `PriorityQueue<T>(before)` says comes first, and `TryPop` and `TryPeek` give a `T?`; `Drain()` takes every value off, in the order Pop would; it goes over its values in no particular order, and so does `ToArray()` |
| `Pool<T>` | `Add(value)` gives a `Handle<T>`; `Get(handle)` gives a `T?`; `Set(handle, value)`; `Remove(handle)`; `IterateHandles()` gives each value with its Handle, `For handle, value In pool.IterateHandles()`; `Capacity` and `Reserve(count)` |

- What finds a value by comparing it, like an array's `Remove(value)`, `IndexOf`, and
  `LastIndexOf`, and the collections' `Contains(value)`, needs the values to be `Equatable`,
  as `==` in any generic code does (07-generics.md). A Class or Struct is when it lists
  `Equatable`, and a Class that does without an `==` of its own compares its objects
  themselves, so `Class Enemy Implements Equatable` is all `enemies.Remove(e)` needs. Only a
  `T[]`'s built-in `Contains` takes any element that has `==`.
- What gives a `T?` for "none", like `Get`, `Find`, and an Iterator's `Next`, gives a `T??` for
  a collection of a `T?`, so a stored `Null` isn't "none": a For ... In over a `Stack<Int?>` goes
  over its Nulls, and `Get` of a `Map<String, Int?>` tells a missing key from a `Null` value.
- A Map or Set needs its keys to be `Hashable`. Its entries are in the order they were added,
  and removing one leaves the rest in order. A Map or Set whose keys and values are `Printable`
  is too: `{a: 1, b: 2}`, `{1, 2}`.
- Two Maps are `==` when they have the same keys, each with an equal value, in any order, and two
  Sets when they have the same values; a Map of values that aren't Equatable compares as the
  same object, like any Class. A Map whose values are `Hashable` is too, and so is a Set, with
  hashes that their order doesn't change, so either can be a key.
- `{key: value, ...}` is a new `Map<K, V>`. K and V are the expected Map type's, or else the
  keys' and the values' common types, as in an array literal. `{}` needs a type from where it's
  used, like `Local m: Map<String, Int> = {}`. Each entry goes to the Map's `Add`, in order, so
  a repeated key keeps its last value.
- `For key, value In map` takes each entry apart; `For entry In map` gives each as a tuple.
- `map[key]` of a key that isn't there panics where it's read, unless the Map was made with a
  default, as `Map<String, Int>(default: 0)` is, which `[]` gives without adding the key, so
  `counts[word] += 1` counts. The default is one value, which every such read gives: one that's
  an object or an array is shared, so to give each key its own, like an adjacency list's array,
  use `GetOrAdd`.
- Arrays, Stack, Queue, Deque, and PriorityQueue have `AddAll(values)`, adding each in turn,
  which for a PriorityQueue orders its heap once, and arrays `InsertAll(index, values)`. These
  and the Set Methods that take values take any `Iterable<T>`, an array included, and read all
  of it first, so a collection can take its own; a Map's AddAll takes a Map.
- An array's Methods that call a function, like `RemoveAll(test)` or `Sort(before)`, go over
  what's in the array as each step begins, as a `For ... In` does: a function that adds or
  removes elements changes what's left, and an element it takes away that the Method still
  needs panics where the Method was called.
- `items.View(start, finish)` is an `ArrayView<T>` of the elements from start up to finish: it
  reads and sets them in the array, which it keeps alive, so it sees the array's changes, and an
  element the array no longer has panics, naming the view's index. Its `Count` is the view's
  own, which stays as the array changes. It has `view[i]`, `View`, `ToArray()`, and goes in a
  `For ... In`. A slice is still a copy.
- `AsReadOnly()` gives an array, Map, or Set to read but not change, a `ReadOnlyArray<T>`,
  `ReadOnlyMap<K, V>`, or `ReadOnlySet<T>`, which sees the changes of what it shows and keeps it
  alive: `Count`, `[]`, `Contains`, `Get`, `Keys`, `Values`, `For ... In`, and a copy through
  `ToArray()`, `ToMap()`, or `ToSet()`.
- Arrays and std's collection Classes are `Cloneable`, with `Clone()`. The Classes, Map, Set,
  Stack, Queue, Deque, PriorityQueue, and Pool, are also `Defaultable`, whose `DefaultValue()`
  is a new empty one; a PriorityQueue is Defaultable when its values are Comparable. An array
  isn't, since a `T[]` has no zero.
- A `Pool<T>` reuses removed values' slots. Each slot counts how many times it has been reused,
  and a `Handle<T>` has that count, so an old Handle finds nothing in a reused slot. Going over a
  Pool gives the values in the order of their slots, and removing one while going over them is
  fine. A Handle's Index is ReadOnly, and only a Pool makes one, so a Handle is always one the
  Pool gave; Handles are `Hashable`, equal when they're the same value's. A Pool's Clone has the
  same Handles, and a Pool of a `T?` keeps a Null as a value.
- A Map spreads its keys over its buckets by mixing their Hash, so one that differs only in its
  high bits, like `x << 32 | y`, does as well as any.

### More collections

| Type | What it's for, and its members |
|---|---|
| `SortedMap<K, V>` | keys in order, `K: Comparable`, in a B-tree: `Add(key, value)`, which adds or replaces, `map[key]` and `map[key] = value`, `Get(key)` and `Get(key, fallback)`, `Contains`, `Remove(key)`, `Clear`, `Keys()` and `Values()` in order, `First()`, `Last()`, `Floor(key)`, the largest key not above it, and `Ceiling(key)`, the smallest not below it, which give a `(K, V)?`, and `Range(low, high)`, from low up to high, without high; `==` when its values are `Equatable` |
| `SortedSet<T>` | values in order, `T: Comparable`: `Add(value)` gives whether it's new, `Remove`, `Contains`, `Clear`, `First()`, `Last()`, `Floor`, `Ceiling`, which give a `T?`, `Range(low, high)`, `ToArray()` in order, and `==` |
| `RingBuffer<T>` | the last `Capacity` values added, oldest first, for a history or a window: `RingBuffer<T>(capacity)`, `Add` drops the oldest when it's full (`IsFull`), `AddAll`, `RemoveFirst`, `RemoveLast`, `First`, `Last`, and their Try twins, `ring[i]` and `ring[i] = value` from the oldest, `Contains`, `Clear`, `ToArray()` |
| `Grid<T>`, `Grid3<T>` | a fixed number of cells, `Grid<T>(width, height, fill)` and `Grid3<T>(width, height, depth, fill)`: `grid[x, y]` and `grid[x, y, z]`, read and set, `Width`, `Height`, `Depth`, `InBounds`, `Neighbors4` and `Neighbors8`, or `Neighbors6` and `Neighbors26`, the positions around one that are in the grid, in the order their comments give, `Fill(value)`, `ToArray()` row by row and layer by layer, and `==`; a position outside panics |
| `BitSet` | a Set of Ints from 0 up, a bit each: `Add(value)`, which gives whether it's new and panics at a negative value, `AddAll`, `Remove`, `Contains`, `Clear`, `First()` and `Last()`, which give an `Int?`, `Union`, `Intersect`, `Difference`, and `SymmetricDifference` with another BitSet, `AddAll`, `RemoveAll`, and `KeepOnly` with one in place, `IsSubsetOf`, `IsSupersetOf`, `Overlaps`, `ToArray()` smallest first, `==`, and `Hash` |
| `LruCache<K, V>` | up to `Capacity` entries, `LruCache<K, V>(capacity)`: `Get(key)` gives a `V?` and uses the key, `Peek(key)` doesn't, `Add(key, value)` uses it and gives the least recently used entry when it had to go, `Contains`, `Remove`, `Clear`, `Resize(capacity)`, `Keys()` and `Values()` from the least recently used |
| `Counter<T>` | how many times each value was added: `Add(value)` and `Add(value, times)`, `AddAll`, `Remove(value)` and `Remove(value, times)`, `counter[value]`, which is 0 for one that isn't there, and `counter[value] = count`, `Contains`, `Clear`, `Total`, an `Int64`, `MostCommon(count)`, `Values()`, `==`, and `Hash`; a count that reaches 0 takes its value out, so one that comes back goes last, and a count past what an Int holds panics where it's added |
| `MultiMap<K, V>` | keys with one value or more: `Add(key, value)`, `AddAll(key, values)`, `multi[key]` and `Get(key)`, which give a copy of its values, empty for none, `Contains(key)` and `Contains(key, value)`, `Remove(key, value)`, `RemoveKey(key)`, `Clear`, `KeyCount`, `Keys()`, and `==`; `Count` counts values, and it goes over each key with each value |
| `Trie<V>` | String keys by their bytes: `Add(key, value)`, which adds or replaces, `trie[key]` and `trie[key] = value`, `Get(key)` and `Get(key, fallback)`, `Contains`, `Remove`, `Clear`, `Keys()` and `Values()` in order, `HasPrefix(prefix)`, `KeysWithPrefix(prefix)`, `LongestPrefixOf(text)`, and `==` |
| `LinkedList<T>` | values in nodes linked both ways: `AddFirst`, `AddLast`, `Add`, `AddBefore(node, value)`, and `AddAfter(node, value)`, which give the value's `LinkedListNode<T>`, `AddAll`, `RemoveNode(node)`, `Remove(value)`, `RemoveFirst`, `RemoveLast`, `First`, `Last`, and their Try twins, `Find(test)` and `FindLast(test)`, `NodeOf(value)` and `LastNodeOf(value)`, the `FirstNode` and `LastNode` Properties, `Contains`, `Clear`, `ToArray()`, and `==`; a node has `Value`, which can be set, `Next`, `Previous`, and `List` |
| `IntervalTree<K, V>` | values over intervals from a low to a high, both in them, `K: Comparable`: `Add(low, high, value)`, `Remove(low, high, value)`, `Contains(low, high, value)`, `Clear`, `Overlapping(low, high)` and `Containing(key)`, which give `(K, K, V)`s by low and then by high, `ToArray()`, and `==` |
| `SpatialHash<T>`, `QuadTree<T>`, `Octree<T>` | values at points of `Float`s: `SpatialHash<T>(size)`, in square cells that size across, `QuadTree<T>(minX, minY, maxX, maxY)`, and `Octree<T>` with Zs too: `Add(value, x, y)`, `Remove(value, x, y)`, `Contains(value, x, y)`, `Move(value, x, y, toX, toY)`, `Clear`, `Query(minX, minY, maxX, maxY)`, the values in a box, its edges included, `Near(x, y, radius)`, no more than radius away, and `ToArray()` |
| `ImmutableArray<T>`, `ImmutableMap<K, V>`, `ImmutableSet<T>` | collections that never change, so they're `Sendable` and go to any thread: Structs whose `Add`, `AddAll`, `Set`, `Insert`, `Remove`, and `RemoveAt` give a new one, sharing all but about log(Count) nodes with the old; `From(values)`, `array[i]` and `map[key]`, `Contains`, `Get`, `Keys()`, `Values()`, `ToArray()`, `ToMap()`, `==`, and `Hash` where their values are `Hashable` |

- Each of these goes in a `For ... In`, and the Classes are `Cloneable`, with `Clone()`; the
  immutable ones need neither Clone nor Clear, since a copy of one is the same collection and a
  new one starts empty. Those that start empty are `Defaultable` too: SortedMap,
  SortedSet, BitSet, Counter, MultiMap, Trie, LinkedList, IntervalTree, and the immutable ones.
- A SortedMap holds 15 to 31 keys in a node but the root, so finding, adding, and removing a key
  looks at about log₁₆(Count) nodes, each a binary search, and giving a key a new value or
  removing one that isn't there leaves the tree as it is. Its order is the keys' `<`, which has
  no place for a key that isn't equal to itself, like NaN: one panics where it's added to a
  SortedMap or SortedSet, and finds nothing where it's looked up.
- A RingBuffer, a Grid, and a Grid3 have a fixed size, set when they're made, and a size that
  isn't 1 or more for a RingBuffer, or is negative or has more cells than an Int counts for a
  Grid, panics there; a Grid3 without cells along an axis has none. Giving a value a new value
  while going over them is fine.
- An LruCache keeps its entries in a Map in the order they were last used, so a use moves an
  entry to the end: going over it while using one panics, as adding one does.
- A Trie's node per byte of its keys makes `HasPrefix` and `LongestPrefixOf` go down the prefix
  alone. `HasPrefix("")` is whether the Trie has a key.
- A LinkedList's node is the same object while it's in the list, holds the next node, and holds
  the one before weakly, so the list makes no cycles. A node removed, or of another list, panics
  where it's given to the list, and a removed node, or one of a list that's cleared or gone, has
  no `List`, `Next`, or `Previous`. Its `First`, `Last`, `Find`, and `Remove` give or take
  values, not nodes, and what gives or takes a node says so in its name. Unlike an array's,
  `First` and `Last` panic when the list is empty, as a Deque's do, and their Try twins give a
  `T?`.
- An IntervalTree is a balanced tree of the intervals, each node with the highest high under
  it, so `Overlapping` skips what ends too soon. A high below low, or an end that isn't equal to
  itself, like NaN, panics where it's added, and finds nothing where it's searched for.
- A spatial structure's coordinates are Floats, and its distances and boxes are worked out in
  Doubles, so none overflows; a NaN coordinate panics where it's given, a negative or NaN radius
  finds nothing, and an infinite one finds every point. A SpatialHash covers any space, a point
  far out going in the last cell, while a QuadTree and an Octree hold only the points in their
  box, which has to be finite, a point outside panicking where it's added or moved to, and split
  a node in four or eight past 8 points, up to 16 times. What a query finds, and the order a
  `For ... In` goes in, is no order to count on.
- The immutable collections are Structs over balanced trees of Shared Class nodes, so their
  values, and an ImmutableMap's keys, are `Sendable`, and a key is `Hashable` too. An
  ImmutableMap and an ImmutableSet go by their keys' hashes, and keys of one hash in the order
  they were added.
