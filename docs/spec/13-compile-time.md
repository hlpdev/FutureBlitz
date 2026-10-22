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
- A top-level `Const` known at compile time can be used anywhere in its file, even above its
  line, and in a declaration's type, like a Field's `Int[SIZE]`. One computed at run time is
  initialized in source order with the `Global`s. A `Const` that needs its own value is an
  error.

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

- A Const Function is a top-level `Function` that can't throw. It takes and gives numbers, Bools,
  Strings, and Enums (or a `T?` of one). A generic one, like `Const Function Largest<T:
  Comparable>(a: T, b: T): T`, runs at compile time when its type arguments are those types, and a
  built-in Interface's Operators on its type parameters are the built-in ones. Its body can use
  Locals of those, and Structs, tuples, and fixed arrays of them, and calls only Const Functions,
  `Assert`, `Panic`, and the New a Struct gets. It doesn't use Globals, or Consts known only when
  the program runs, and makes no objects, `T[]`s, or function values.
- A call whose arguments are all known runs in the compiler, and its value is the call's. With
  any argument that isn't known, it's an ordinary call that runs with the program.
- A panic, an overflow, a division by zero, an index out of range, more than 10 million steps,
  calls more than 10,000 deep, or variables that hold more than 256 MiB of text and parts while
  it runs is a compile error at the call, pointing at where in the Const Function it happened.
- A size in a declaration's type, like a Field's `Int[Cells()]`, can come from a Const Function
  anywhere in the file, since Const Functions are declared first. While types are still being
  declared, one whose code reaches a Struct or Class whose members aren't declared yet can't run
  there, which is an error.

## Attributes

```vb
@Inline Function Lerp(a: Float, b: Float, t: Float): Float
@Deprecated("use Spawn2") Function Spawn(...)
@Flags Enum Layer ...
@CLayout Struct JPH_Vec3 ...
@MainThread Function RenderWorld()
@PanicsAtCaller Method Pop(): T
@Serialize Class SaveData ...
```

| Attribute | Effect |
|---|---|
| `@Inline` | force inlining |
| `@Deprecated(msg)` | warning at use sites |
| `@Flags` | enum values are powers of two |
| `@CLayout` | C-compatible struct layout |
| `@MainThread` | callable only from the main thread |
| `@PanicsAtCaller` | its panics report where it's called |
| `@Serialize` | generate serialization code via compile-time reflection |

- An attribute goes before a declaration, on its line or alone on the line above.
- `@Inline` goes on a Function, Method, Operator, or Property with a body. Every call to it is
  inlined, in debug builds too.
- `@Deprecated` and `@Deprecated("message")` go on anything declared outside a body: a
  Function, Method, Property, Struct, Class, Interface, Enum, Field, Global, or Const. Each use
  outside it, and outside its type's own code, gets a warning: "'Spawn' is deprecated: use
  Spawn2".
- `@MainThread` goes before a Function or Method, which work on another thread can't call (see
  [10-concurrency.md](10-concurrency.md#main-thread-rules)).
- `@PanicsAtCaller` goes before a Function, Method, Operator, New, or Property, whose panics then
  report where it's called (see [09-errors.md](09-errors.md#panics)), not Destroy or an Extern
  Function, and of an Interface's members only a Final, generic, or conditional default, which
  a call runs directly.
- `@CLayout` lays a Struct out as C does (see [12-ffi.md](12-ffi.md)), and `@Serialize` comes
  with the serialization library (M6); until then it's reported as not supported yet.

### Declaring attributes

```vb
Attribute Skip
Attribute Rename(Name: String)
Attribute Range(Low: Int = 0, High: Int = 100)

Struct Player
    @Rename("hp") @Range(High: 200)
    Field Health: Int
    @Skip
    Field Cache: Int
End Struct
```

- `Attribute Name(parameters)` declares an attribute, which reflection reads. It's a Struct whose
  Fields are its parameters, so `f.Attribute(Rename)` gives a `Rename?`.
- Its parameters are numbers, Bools, Strings, and Enums. A default is known at compile time. It
  can't take a built-in attribute's name.
- A use takes an argument for each parameter without a default, positional ones first, then
  named ones, as in a call. Each argument is known at compile time, like a Const's value.
- It goes on anything declared outside a body: a Function, Method, Property, Struct, Class,
  Interface, Enum, Field, Global, or Const. A declaration has each attribute at most once.
  `Attribute` is only a keyword before a name.
- An attribute from a module imported with As goes after its qualifier, as other names do:
  `@C.Rename("hp")`, and `f.Has(C.Rename)` in reflection. A qualified one is never
  a built-in, so `@C.Inline` is C's own: an Attribute can take a built-in's name, which code
  reaches only through a qualifier, since `@Inline` alone is always the built-in.

## Reflection

**Compile-time only.** Code can inspect types (fields, methods, attributes) during
compilation to generate code, e.g. serializers. There is **no runtime reflection**, which
keeps binaries small and fast.

```vb
Function Describe<T>(value: T): String
    Local text = ""
    For Field f: F In T
        Where F: Printable
        If f.Has(Skip) Then Continue
        Local name = f.Attribute(Rename)?.Name ?? f.Name
        text = text + "{name}={f.Get(value)} "
    End For
    Return text
End Function

Function Clear<T>(ByRef value: T)
    For Field f: F In T
        Where F: Numeric
        f.Set(ByRef value, F.Zero())
    End For
End Function
```

- `For Field f: F In T` goes over the fields of a Struct, a Class (the inherited ones first), or a
  tuple ("0", "1", and so on), in order, but not an Atomic or Mutex, which can't be copied. The
  compiler makes a copy of the body for each field, in which `F` is that field's type. `: F` can be
  left out when nothing names it.
- The body is checked once, with `F` a type parameter that only its Where lines, at the top of
  the body, say anything about. Over a concrete type, each field's type must meet them. Over
  the function's own type parameter `T`, they're constraints on `T`'s fields, which each call
  checks: `Describe(p)` needs each of `p`'s fields to be Printable. A type parameter's fields
  meet a constraint only when one of its own loops says so.
- The field has `f.Name`, `f.Index`, `f.Get(value)`, which gives an `F`, `f.Set(ByRef value, x)`,
  `f.Has(A)`, and `f.Attribute(A)`, which gives an `A?`, for an Attribute `A`. `f` isn't a value
  of its own.
- `For Method m In T` goes over the Methods of a Struct or Class that take nothing, give nothing,
  and throw nothing (the inherited ones first, each in the place of what it overrides), but not
  Destroy or Unsafe Methods. `m.Call(value)` calls it, through the Method table for a Class. `m`
  has `m.Name`, `m.Has(A)`, and `m.Attribute(A)` too.
- `For Method m: M In T` with `Where M: Func(Int): String` goes over the Methods of that Func
  type instead, and `m.Call(value, 5)` passes their arguments. Its Where line picks the Methods;
  it isn't a constraint to meet.
- `For Property p: P In T` goes over a Struct's or Class's Properties (inherited ones first),
  with `p.Name`, `p.Get(value)`, `p.Has(A)`, and `p.Attribute(A)`, and `For Static Field s: S In
  T` over its own Static Fields, with `s.Get()`. They read; a Property without a Set and a
  ReadOnly Static Field couldn't be told apart, so neither has a Set. Where lines on P or S work
  as on F.
- The loops go over Private members too, and over what an Extend of the type, or of that one
  instance like `Box<Int>`, adds, but only where the loop's file sees the Extend's module, as a
  call there would. A Method or Property that Where lines on the type's parameters make
  conditional is among them only for the instances whose type arguments meet the lines. In
  `For Property`, an Override takes the place of what it overrides, while another Property of its
  name, like one an Extend adds beside a Private one, is a Property of its own.
- A loop can go over the code's own type parameters: its function's, or in a Method, its type's,
  whose Where lines then make the Method conditional, as a Where line on it would. `Self` is the
  type of the code it's in, and in an Interface's default, the type that takes it, which has to
  meet the loop's Where lines.
- A lambda in the loop's body is one for each copy. A lambda in code with type parameters can
  loop over them too, since each instance of the code has its own copy of the lambda.
- `T.Has(A)` and `T.Attribute(A)` read the attributes on a type's own declaration, for a type or
  a type parameter, unless it has a Static member by that name.
- `Continue` goes on to the next copy, and `Break` past the last. Other types, like Int, have no
  fields or Methods, so the loop's body is never run.
- `@Serialize` comes with the serialization library (M6).

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
  `PLATFORM_MACOS`. A build defines `DEBUG` or `RELEASE`, as `fb build --release` says, and the
  platform it's built on; `fb check --release` checks the release build's code. A symbol that
  isn't one of these gets a warning, since it's never defined.
- Code in an inactive branch is skipped by the compiler and does not need to parse. Directives
  in it still count, so its `#If`s and `#End If`s pair up.
- Each `#If` needs an `#End If`, `#ElseIf` and `#Else` come after an `#If`, and nothing comes
  after `#Else` but `#End If`.

## Assertions

```vb
Assert(hp >= 0, "negative hp")    // panics on failure, in debug and release builds alike
```

An Assert is checked in every build, since the code after it counts on its condition: after
`Assert(e != Null)`, `e` is used as non-null. See [09](09-errors.md#panics).
