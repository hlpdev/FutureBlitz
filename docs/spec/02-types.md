# 02 · Types, Declarations, Conversions

## Built-in types

| Type | Notes |
|---|---|
| `Bool` | `True` / `False`. Never implicitly converted to or from integers; `b As Int` is `1` or `0` |
| `Int8` `Int16` `Int32` `Int64` | signed integers |
| `UInt8` `UInt16` `UInt32` `UInt64` | unsigned integers |
| `CLong` `CULong` `CWChar` | C's `long`, `unsigned long`, and `wchar_t`, the target's sizes, for `Extern` Functions (see below) |
| `Float32` `Float64` | IEEE floats |
| `String` | immutable UTF-8, refcounted |
| `Char` | a Unicode scalar value: a code point from U+0000 to U+10FFFF that isn't a surrogate, 32 bits, which a literal like `'a'` makes; it compares by its code, and its zero is U+0000 |
| `CString` | C's `const char*`, for `Extern` Functions; `Null`, C's NULL, is its zero (see [12-ffi.md](12-ffi.md)) |
| `Void` | "no value"; only a Ptr's type, `Ptr<Void>`, which points at anything |

The math value types, `Vec2` to `Vec4`, `Vec2i`, `Vec3i`, `Quat`, `Mat3`, `Mat4`, `Color`, and
`Color32`, aren't built in: they're std.math's Structs, which a program imports with
`Import std.math`.

Aliases:

| Alias | Type |
|---|---|
| `Byte` | `UInt8` |
| `Int` | `Int32` |
| `Long` | `Int64` |
| `Float` | `Float32` |
| `Double` | `Float64` |

A program declares aliases of its own the same way, each another name for a type:

```vb
Public Alias MetersPerSecond = Quantity<1, 0, -1>
Alias Scores = Int[]
```

- An alias is the type it names, everywhere: a value of one is a value of the other, and
  messages show the type itself, as they show `Int32` for `Int`. `MetersPerSecond(3)` makes the
  Struct or Class it names, that instance, and `MetersPerSecond.Zero()` calls its Static.
- It's declared at the top level, takes `Public`, `Package`, or `Private` like any declaration,
  and nothing else, and it isn't generic, so it takes no type arguments. A Public one can't name
  a type that code outside its module can't see.
- It can name types and aliases declared anywhere in what its module sees, later ones too, but
  not itself: `Alias A = B[]` with `Alias B = A` is an error, "'A' names itself through 'B', so
  it's no type".

C's integers are as big as the target's C has them: on Windows, a `CLong` is an `Int32`, a
`CULong` a `UInt32`, and a `CWChar` a `UInt16`; elsewhere they're an `Int64`, a `UInt64`, and an
`Int32`, or a `UInt32` on AArch64 outside Apple's targets. Each is a type of its own, which
converts without `As` only where it would on every target, so code that compiles on one
compiles on all: an `Int32` or a `UInt16` widens to a `CLong`, a `UInt32` to a `CULong`, a
`UInt8` or `UInt16` to a `CWChar`, and a `CLong` or `CWChar` to an `Int64`, a `CULong` to a
`UInt64`, and a `CWChar` to a `CLong` or a `Float64`; anything else, an `Int64` to a `CLong`
or a `CLong` to an `Int32`, needs `As`. A literal of one fits every target's, so a `CLong`'s is
an `Int32`'s and a `CWChar`'s from 0 to 65535, and a `CWChar` isn't negated, since it's
unsigned on Windows. Otherwise they're integers like any: arithmetic, comparisons, `As`, Hash,
and strings work on them as on the target's, and a Const of one is worked out as the target
programs are built for has it, as is `SizeOf`. An Enum's integer type is one of the others.

Composite and special types:

| Form | Meaning | See |
|---|---|---|
| `T?` | nullable `T` | below |
| `T[]` | dynamic array | [14](14-strings-collections.md) |
| `T[N]`, `T[W, H]` | fixed-size array | [14](14-strings-collections.md) |
| `(A, B)` | tuple | [05](05-functions.md) |
| `Func(A, B): R` | function type | [05](05-functions.md) |
| `Ptr<T>` | raw pointer (unsafe/FFI) | [08](08-memory.md) |
| `Weak<T>` | weak reference | [08](08-memory.md) |

## Declarations

| Keyword | Meaning |
|---|---|
| `Local` | mutable local variable |
| `Global` | module-level variable |
| `Const` | read-only value, computed at compile time when it can be, and otherwise at run time ([13](13-compile-time.md#compile-time-evaluation)) |
| `ReadOnly` | field that only its Class's `New` can change; a global that never changes is a `Const` |
| `Field` | member of a struct or class |

```vb
Const MAX_HP = 100
Const GRAVITY: Float = -9.81
Global score: Int = 0
Local name = "grunt"          // inferred String
Local hp: Int                 // typed, assigned later
```

### Annotation rules

- **Any variable** (`Local`, `Global`, `Const`, `Field`, `Static Field`) may omit its type when
  it has an initializer; the type is inferred from the initializer.
- A variable **without** an initializer must have a type annotation.
- **Parameters and return types** are always annotated.

### Scopes

- Functions, `Global`s, and top-level `Const`s are visible everywhere in their file, even
  before their declaration.
- `Local`s in top-level code belong to the top-level code only; functions can't see them. Use
  a `Global` to share state.
- A name can't be declared twice in the same scope.
- A local can't hide a local or parameter of an enclosing block in the same function. It may
  hide a file-level name.

### Global initialization

`Global`s and top-level `Const`s are initialized in source order, before the rest of the
top-level code runs. An initializer can only use `Const`s and `Global`s declared above it. A
lambda in one runs later, so it can use one further down whose type is written. A function
that an initializer calls can reach a Global whose initializer hasn't run yet, through any
call, so a debug build panics where it uses it: "'late' is used before its initializer has
run". A release build panics there too when the Global's zero would hold a null object, as a
Class, an Interface, an array, a function value, or a Struct or tuple holding one would, since
using it would crash; any other Global reads as its zero, a value of its type, as overflow
wraps. A Global whose value is known at compile time has it from the start.

When the program ends, once its coroutines and jobs are done, the Globals let go of what they
hold, the last first, so a Destroy that runs then can use the Globals above its object's. Each
String, object, or array a Global let go of reads as its zero after, and a use of a Global
already let go of panics where a use before its initializer would: "'late' is used after it's
released".

A `Global` without an initializer starts at `0`, `0.0`, `False`, or `""`, like a field.

### ThreadLocal Globals

```vb
ThreadLocal Global rolls = Random(42)
ThreadLocal Global seen: Int
```

`ThreadLocal Global` gives each thread a Global of its own, which is as fast as a Global, in the
CPU's own thread-local storage.

- A thread's starts at its zero, or at its initializer's value when that's known at compile
  time. The first time the thread uses it, reading, assigning, or passing it ByRef, its
  initializer runs on that thread, before the use. An assignment first still runs it, and lets
  go of what it made.
- The initializer runs on whichever thread uses the Global first, so it's checked as work on
  another thread ([10](10-concurrency.md#main-thread-rules)): it can't use what only the main
  thread can, a mutable Global's value among them. One that reaches its own Global, through what
  it calls, panics there: "'depth' is used before its initializer has run, since its initializer
  reaches it". An error it lets out ends the program, as top-level code's does. While it runs on
  a pool worker, that worker's waits run only the jobs it starts, not older ones queued there,
  which another worker runs, with its own.
- A thread lets go of its ThreadLocal Globals as it ends, the last made first: a thread of its own
  after its work, before Join returns; a pool worker's when the pool stops, as the program ends;
  and the main thread's once the top-level code and the jobs are done, before the Globals are
  released, so a ThreadLocal's Destroy can use them. A use of one already let go of panics:
  "'later' is used after it's released, since a thread releases its ThreadLocal Globals as it
  ends". A thread C started lets go of its own as it exits. A job a Global's Destroy starts runs
  on a pool worker, with that worker's own, never on the main thread once it's let go of its
  own.
- Code on any thread can use a ThreadLocal Global, which is its own, and its type needn't be
  Sendable, since its value stays on its thread. It can't be an Atomic or a Mutex, which are for
  what threads share. Only a top-level Global is ThreadLocal: not a Local, a Const, or a Static
  Field.

### Definite assignment

Reading a `Local` before it has been assigned on every path is a compile error. (`Global`s
always have a value; see above.)

```vb
Local hp: Int          // ✓ type given, no value yet
Local hp               // ✗ error: needs a type or a value
Print(hp)              // ✗ error: hp read before assignment
```

### Field defaults

- Primitive fields default to `0`, `0.0`, `False`, or `""` when no initializer is given, and
  nullable ones to `Null`.
- A field with no zero, a non-nullable Class or a Struct holding one, must be assigned by every
  constructor (compile-checked): a Struct's generated constructor requires it, and a Class with
  one needs a `New`, which assigns it on every path before reading it or using `Self`.
- A field's default is a value known at compile time, like a parameter's: a number, Bool,
  String, Char, or Enum member, or `Null` for a `T?`, made of literals, Consts known at compile
  time, operators, casts, and Const Function calls (see
  [13](13-compile-time.md#compile-time-evaluation)). The one other default is `[]`, which gives
  each object or value a new empty array. Anything else, like `{}`, `[3, 0, 5]`, or a Struct's
  constructor, is an error, "a field's default must be known at compile time": a Class's `New`
  assigns such a value instead.

## Nullability

- All types are **non-null by default**.
- `T?` may hold `Null`. The `?` touches the type name (see [03](03-operators.md)).
- After a null check, the compiler narrows `T?` to `T` (flow typing).
- A `T?` of a `T?` is a type of its own: an `Int??` is `Null`, or holds an `Int?`, which can be
  `Null` itself. Generic code makes them, like a `Map<String, Int?>`'s `Get`, whose `Null` is a
  missing key and whose `Int?` is a key's value, `Null` or not, and they can be written too.

```vb
Local target: Enemy? = Null
Local hp = target?.hp ?? 0
If target != Null Then target.Hurt(5)   // target is Enemy here
```

- `Null` takes its type from where it's used: `Local e: Enemy? = Null` works, and
  `Local e = Null` doesn't. A `T` converts to a `T?` without `As`. A `T?` converts to a `T??`
  as a value, so its `Null` is held, not the `T??`'s own, and a `T??` needs a check to be a
  `T?`.
- `If`, `ElseIf`, `While`, `? :`, `&&`, `||`, `!`, an early `Return`, and assigning a value that
  can't be `Null` all narrow. Narrowing applies to Locals, Consts, and parameters that aren't
  `ByRef`, and loop variables, and ends when the variable is assigned a `T?` or passed `ByRef`.
  A loop that changes the variable can't rely on a check from before it. Each check takes off
  one `T?`: a `T??` narrows to its `T?`, which a Local copy of it can narrow again.
- A Global or a field can change during any call, so it isn't narrowed: check a Local copy.
- `a?.b` is `Null` when `a` is, and a `B?` otherwise; `a?.M()` only calls `M` when `a` holds a
  value. Each `?.` is one check, so a chain is `a?.b?.c`.
- `a ?? b` gives the value in `a`, or `b` when `a` is `Null`; `b` only runs then. For a `T??`
  it gives a `T?`, as `?.` takes off one `T?` too. Comparing a `T??` with `Null` asks about the
  outer one.
- Every `T?` has two Methods of its own, which work on it whether it holds a value or not:
  `x.Expect(message)` gives the value, or panics with the message where it's called, and
  `x.OrElse(make)` gives the value, or what `make`, a `Func(): T`, gives, called only when `x` is
  `Null`.
- Two `T?` values are equal when both are `Null` or both hold equal values. A `T?` and a `T`
  compare the same way, the `T` as a `T?` holding it, with `T`'s `Operator ==` if it has one.
- A `T?` can't go in a string; give a value for `Null` first: `"{name ?? "nobody"}"`.
- `?.`, `??`, or comparing with `Null` on something that can't be `Null` is a warning.

## Conversions

- Only **lossless** conversions happen implicitly (e.g. `Int32` → `Int64`, `Int32` →
  `Float64`, `Float32` → `Float64`, `T` → `T?`).
- A `String` converts to std.io's `Path` wherever one is expected, and to a `Path?`: a call
  makes the Path with its New, as the program runs, so `File.ReadText("save.dat")` reads
  naturally. It's a conversion in overloads, so one that takes the String exactly wins. A
  `String?` doesn't convert, since its Null isn't a path, and neither does a `String[]` to a
  `Path[]`, though an array literal's elements do. A Path isn't known at compile time, so a
  String isn't a Path parameter's default. No other type gets a conversion of its own.
- Everything else requires an explicit cast: `x As Float`.
- `As?` is a checked downcast that yields `Null` on failure (see [03](03-operators.md)).
- `Bool` never converts implicitly. `As` turns it into an integer: `True As Int` is `1` and
  `False As Int` is `0`. Numbers don't convert to `Bool`; compare instead (`n != 0`).
- A `Char` isn't a number and never converts implicitly. `As` turns it into an integer, its
  code, which keeps its low bits as any integer's do: `'a' As Int` is `97`. `As` turns an
  integer into a `Char` when it's a Unicode scalar value, `98 As Char` is `'b'`, and another,
  like `0xD800` or `-1`, is an error at compile time and a panic at run time.

### Literals

- An integer literal converts to any numeric type that holds its value exactly:
  `Vec3(0, 1, 0)` and `Local d: Double = 1` work, `Local b: Int8 = 300` doesn't.
- A decimal literal converts to `Float` or `Double` only.
- A cast gives a literal its type: `5_000_000_000 As Long`.
- With no expected type, literals made only of integers are `Int`, and literals with a decimal
  among them are `Float`: `1 + 0.5` is a `Float`. A literal too big for `Int` needs an expected
  type or a cast.
- Literals before a member, like `0.1.Seconds` or `5_000_000_000.ToString()`, take the first
  type, from their default up, that holds them and has the member (see
  [01](01-lexical.md#literals)).

### Casts between numbers

- `As` between integer types keeps the low bits (it wraps): `300 As UInt8` is `44`.
- `As` from a float to an integer drops the fraction (toward zero). A value outside the
  integer's range is an error at compile time and a panic at run time.
- `As` to a float rounds to the nearest value it can hold.

### Mixed numeric types

In arithmetic and comparisons between two numeric types, one side must convert to the other's
type losslessly, and the result has that type: `Int + Long` is `Long`, `Int + Double` is
`Double`. Anything else is an error that needs a cast: `Int + Float`. A literal takes the other
side's type when its value fits.

## Integer overflow

- **Debug builds:** overflow traps (panic with location).
- **Release builds:** overflow wraps.
- A shift amount outside 0 to the width minus 1 panics in debug builds and is masked to that
  range in release builds.
- Integer division or remainder by zero, and a float `As` an integer that doesn't fit, panic in
  every build.
- Explicit intent is always available, as an integer's Methods: `a.WrappingAdd(b)`,
  `WrappingSubtract`, `WrappingMultiply`, and `WrappingNegate()` wrap around in every build, and
  `a.CheckedAdd(b)`, `CheckedSubtract`, `CheckedMultiply`, `CheckedDivide`, and `CheckedNegate()`
  give a `T?` that's Null when the operation would overflow or divide by zero. With known values,
  they're worked out at compile time, and a Const Function can use them.
