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
value (`0.1`, not `0.100000001`), and a `Bool` prints as `True` or `False`. In a single-line string, an interpolation must end on the same
line.

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

## Arrays

```vb
Local nums: Int[] = [1, 2, 3]        // dynamic array (reference type)
nums.Add(4)
Local grid: Float[64, 64]            // fixed-size, multi-dimensional (value type)
Local mid = nums[1..3]               // slice view, half-open
Local copy = nums.Clone()
```

| Form | Semantics |
|---|---|
| `T[]` | dynamic, growable, **reference** |
| `T[N]`, `T[W, H]` | fixed size, **value** (stored inline) |
| `arr[a..b]` | slice view into an array |

Out-of-bounds access panics.

- An array literal takes the type it's used as, like `Local a: Long[] = [1, 2]`, or else its
  elements' common type, with literals taking it: `[1, 2.5]` is a `Float[]`. `[]` needs a type
  from where it's used. A `T[N]` can come from a literal of N elements; a `T[W, H]` can't.
- A `T[]` has `Count` (an `Int`) and the Methods `Add(value)`, `Insert(index, value)`,
  `RemoveAt(index)`, `Clear()`, `Contains(value)`, and `Clone()`. `Clone` copies the elements,
  not what they refer to. `Contains` compares with `==`. A fixed array has `Count` only.
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
- Arrays don't have `==` yet; `RefEquals` tells whether two `T[]`s are the same array.
- An element can't be passed `ByRef` yet, and a `ByRef Method` can't change a `T[]`'s element
  in place, since the elements move when the array grows.
- A fixed array's sizes are integers known at compile time: literals, or `Const`s in code.
- Slices (`arr[a..b]`) come in M3.

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
`Remove`, `Contains`, `Count`, `Clear`, `Sort`, `Filter`, `Map`, `Find`, `Clone`.

```vb
Local hp: Map<String, Int> = {"grunt": 50, "boss": 500}
For name, value In hp ... End For
Local seen = Set<Int>()
```
