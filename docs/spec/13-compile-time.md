# 13 · Compile-Time Features

## Compile-time evaluation

Whatever can be computed at compile time is computed at compile time.

- An expression made only of literals, `Const`s, operators, and casts is evaluated by the
  compiler, never at run time.
- `Const` declares a read-only value. If its initializer can be computed at compile time, it
  is; otherwise it's computed once at run time (top-level `Const`s together with the
  `Global`s).
- Places that need a value at compile time (array sizes, const generic arguments, `Case`
  values) need a `Const` whose value is known at compile time.
- Overflow, division by zero, or an invalid cast during compile-time evaluation is a compile
  error, in debug and release builds alike.
- Calls are evaluated at compile time when the function is a `Const Function` (below) and its
  arguments are known.

```vb
Const SECONDS_PER_DAY = 24 * 60 * 60   // 86400, computed by the compiler
Const START = Clock.Now()              // read-only, but computed at run time
Local timeout = 5 * 60 * 1000          // stored as 300000; nothing computed at run time
```

## Const functions

Functions marked `Const` are evaluated by the compiler when called with constant
arguments.

```vb
Const Function Fib(n: Int): Int
    Return n < 2 ? n : Fib(n - 1) + Fib(n - 2)
End Function

Const FIB_20 = Fib(20)            // computed at compile time
Local table: Int[Fib(10)]          // usable in types
```

## Attributes

```vb
@Inline Function Lerp(a: Float, b: Float, t: Float): Float
@Deprecated("use Spawn2") Function Spawn(...)
@Flags Enum Layer ...
@CLayout Struct JPH_Vec3 ...
@MainThread Function RenderWorld()
@Serialize Class SaveData ...
```

| Attribute | Effect |
|---|---|
| `@Inline` | force inlining |
| `@Deprecated(msg)` | warning at use sites |
| `@Flags` | enum values are powers of two |
| `@CLayout` | C-compatible struct layout |
| `@MainThread` | callable only from the main thread |
| `@Serialize` | generate serialization code via compile-time reflection |

## Reflection

**Compile-time only.** Code can inspect types (fields, methods, attributes) during
compilation to generate code, e.g. serializers. There is **no runtime reflection**, which
keeps binaries small and fast.

## Conditional compilation

```vb
#If DEBUG
    DrawDebugOverlay()
#ElseIf PLATFORM_WINDOWS
    ...
#Else
    ...
#End If
```

- A directive must be the first thing on its line.
- Conditions combine symbols with `!`, `&&`, `||`, and parentheses. A symbol is true when it
  is defined.
- Predefined symbols: `DEBUG`, `RELEASE`, `PLATFORM_WINDOWS`, `PLATFORM_LINUX`,
  `PLATFORM_MACOS`.
- Code in an inactive branch is skipped by the compiler and does not need to parse.

## Assertions

```vb
Assert(hp >= 0, "negative hp")    // panics on failure, in debug and release builds alike
```

An Assert is checked in every build, since the code after it counts on its condition: after
`Assert(e != Null)`, `e` is used as non-null. See [09](09-errors.md#panics).
