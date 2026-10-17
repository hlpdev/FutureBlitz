# 02 · Types, Declarations, Conversions

## Built-in types

| Type | Notes |
|---|---|
| `Bool` | `True` / `False`. Never implicitly converted to or from integers; `b As Int` is `1` or `0` |
| `Int8` `Int16` `Int32` `Int64` | signed integers |
| `UInt8` `UInt16` `UInt32` `UInt64` | unsigned integers |
| `Float32` `Float64` | IEEE floats |
| `String` | immutable UTF-8, refcounted |
| `Vec2` `Vec3` `Vec4` `Quat` `Mat3` `Mat4` `Color` | built-in math value types with operators |
| `Void` | "no value"; only meaningful in FFI pointer types (`Ptr<Void>`) |

Aliases:

| Alias | Type |
|---|---|
| `Byte` | `UInt8` |
| `Int` | `Int32` |
| `Long` | `Int64` |
| `Float` | `Float32` |
| `Double` | `Float64` |

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
| `Const` | read-only value, computed at compile time whenever possible ([13](13-compile-time.md#compile-time-evaluation)) |
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
top-level code runs. An initializer can only use `Const`s and `Global`s declared above it.

A `Global` without an initializer starts at `0`, `0.0`, `False`, or `""`, like a field.

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

## Nullability

- All types are **non-null by default**.
- `T?` may hold `Null`. The `?` touches the type name (see [03](03-operators.md)).
- After a null check, the compiler narrows `T?` to `T` (flow typing).

```vb
Local target: Enemy? = Null
Local hp = target?.hp ?? 0
If target != Null Then target.Hurt(5)   // target is Enemy here
```

- `Null` takes its type from where it's used: `Local e: Enemy? = Null` works, and
  `Local e = Null` doesn't. A `T` converts to a `T?` without `As`.
- `If`, `ElseIf`, `While`, `? :`, `&&`, `||`, `!`, an early `Return`, and assigning a value that
  can't be `Null` all narrow. Narrowing applies to Locals, Consts, and parameters that aren't
  `ByRef`, and ends when the variable is assigned a `T?` or passed `ByRef`. A loop that changes
  the variable can't rely on a check from before it.
- A Global or a field can change during any call, so it isn't narrowed: check a Local copy.
- `a?.b` is `Null` when `a` is, and a `B?` otherwise; `a?.M()` only calls `M` when `a` holds a
  value. Each `?.` is one check, so a chain is `a?.b?.c`.
- `a ?? b` gives the value in `a`, or `b` when `a` is `Null`; `b` only runs then.
- Two `T?` values are equal when both are `Null` or both hold equal values.
- A `T?` can't go in a string; give a value for `Null` first: `"{name ?? "nobody"}"`.
- `?.`, `??`, or comparing with `Null` on something that can't be `Null` is a warning.

## Conversions

- Only **lossless** conversions happen implicitly (e.g. `Int32` → `Int64`, `Int32` →
  `Float64`, `Float32` → `Float64`, `T` → `T?`).
- Everything else requires an explicit cast: `x As Float`.
- `As?` is a checked downcast that yields `Null` on failure (see [03](03-operators.md)).
- `Bool` never converts implicitly. `As` turns it into an integer: `True As Int` is `1` and
  `False As Int` is `0`. Numbers don't convert to `Bool`; compare instead (`n != 0`).

### Literals

- An integer literal converts to any numeric type that holds its value exactly:
  `Vec3(0, 1, 0)` and `Local d: Double = 1` work, `Local b: Int8 = 300` doesn't.
- A decimal literal converts to `Float` or `Double` only.
- A cast gives a literal its type: `5_000_000_000 As Long`.
- With no expected type, literals made only of integers are `Int`, and literals with a decimal
  among them are `Float`: `1 + 0.5` is a `Float`. A literal too big for `Int` needs an expected
  type or a cast.

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
- Explicit intent is always available: `WrappingAdd`, `CheckedAdd` (returns `T?`), and
  equivalents for the other operations.
