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
`Printable` prints what its `ToString` gives. In a single-line string, an interpolation must end
on the same line.

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
  elements' common type, with literals taking it: `[1, 2.5]` is a `Float[]`. `[]` needs a type
  from where it's used. A `T[N]` can come from a literal of N elements; a `T[W, H]` can't.
- A `T[]` has `Count` (an `Int`) and the Methods `Add(value)`, `Insert(index, value)`,
  `RemoveAt(index)`, `Clear()`, `Contains(value)`, and `Clone()`. `Clone` copies the elements,
  not what they refer to. `Contains` compares with `==`. A fixed array has `Count` only.
- std/collections/array.fb gives a `T[]` the rest of the collections' Methods (below): `Remove`,
  `Sort`, `Filter`, `Map`, and `Find`.
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
panics; `Count` says whether there is one.

| Type | Members besides those |
|---|---|
| `T[]` | `Remove(value)`, the first equal one, giving whether there was one; `Sort()` when T is `Comparable`, or `Sort(before)`, in place and stable; `Filter(keep)`, `Map(change)`, and `Find(test)`, which gives a `T?` |
| `Map<K, V>` | `Add(key, value)` adds or replaces; `Get(key)` gives a `V?`; `Remove(key)`; `Keys()`; `Values()`; `Filter(keep)` with a `Func(K, V): Bool` |
| `Set<T>` | `Add(value)` gives whether it's new; `Remove(value)`; `Filter(keep)`; `Find(test)` |
| `Stack<T>` | `Push`, `Pop`, and `Peek`, at the top; it goes from the top down |
| `Queue<T>` | `Enqueue` at the back, `Dequeue` and `Peek` at the front |
| `Deque<T>` | `AddFirst`, `AddLast`, `RemoveFirst`, `RemoveLast`, `First`, and `Last`; `Add` is `AddLast` |
| `PriorityQueue<T>` | `Pop` and `Peek` give the first: the smallest when T is `Comparable`, or what `PriorityQueue<T>(before)` says comes first |
| `Pool<T>` | `Add(value)` gives a `Handle<T>`; `Get(handle)` gives a `T?`; `Set(handle, value)`; `Remove(handle)` |

- A Map or Set needs its keys to be `Hashable`. Its entries are in the order they were added;
  removing one moves the last into its place. A Map or Set whose keys and values are
  `Printable` is too: `{a: 1, b: 2}`, `{1, 2}`.
- `{key: value, ...}` is a new `Map<K, V>`. K and V are the expected Map type's, or else the
  keys' and the values' common types, as in an array literal. `{}` needs a type from where it's
  used, like `Local m: Map<String, Int> = {}`. Each entry goes to the Map's `Add`, in order, so
  a repeated key keeps its last value.
- `For key, value In map` takes each entry apart; `For entry In map` gives each as a tuple.
- A `Pool<T>` reuses removed values' slots. Each slot counts how many times it has been reused,
  and a `Handle<T>` has that count, so an old Handle finds nothing in a reused slot. Going over a
  Pool gives the values in the order of their slots, and removing one while going over them is
  fine.
