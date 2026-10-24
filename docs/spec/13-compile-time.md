# 13 · Compile-Time Features

## Compile-time evaluation

Whatever can be computed at compile time is computed at compile time.

- An expression made only of literals, `Const`s, operators, and casts is evaluated by the
  compiler, never at run time.
- `Const` declares a read-only value. If its initializer can be computed at compile time, it
  is; otherwise it's computed once at run time (top-level `Const`s together with the
  `Global`s).
- Places that need a value at compile time (array sizes, const generic arguments, `Case`
  values) need a `Const` whose value is known at compile time. A Const type argument computed
  from Const type parameters, like `N - 1`, is worked out in each instance, where dividing by 0
  or a size below 1 is a compile error ([07](07-generics.md#const-generics)).
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
  Strings, Chars, and Enums (or a `T?` of one). A generic one, like `Const Function Largest<T:
  Comparable>(a: T, b: T): T`, runs at compile time when its type arguments are those types, and a
  built-in Interface's Operators on its type parameters are the built-in ones. Its body can use
  Locals of those, and Structs, tuples, and fixed arrays of them, and calls only Const Functions,
  `Assert`, `Panic`, and the New a Struct gets. It doesn't use Globals, or Consts known only when
  the program runs, and makes no objects, `T[]`s, or function values.
- A call whose arguments are all known runs in the compiler, and its value is the call's. With
  any argument that isn't known, it's an ordinary call that runs with the program.
- A panic, an overflow, a division by zero, an index out of range, more than 10 million steps,
  calls more than 10,000 deep, or variables that hold more than 256 MiB of text and parts while
  it runs is a compile error at the call, pointing at where in the Const Function it happened:
  for a limit, the last call, operation, or Local's declaration it got to, or else the function's
  name.
- A Const Function runs as the build would run it, so a release build leaves its DebugAsserts
  out, as it does the program's.
- A size in a declaration's type, like a Field's `Int[Cells()]`, can come from a Const Function
  anywhere in the file, since Const Functions are declared first. While types are still being
  declared, one whose code reaches a Struct or Class whose members aren't declared yet can't run
  there, which is an error.

## Attributes

```vb
@Inline Function Lerp(a: Float, b: Float, t: Float): Float
@Deprecated("use Spawn2") Function Spawn(...)
@Flags Enum Layer ...
@Open Enum Kind ...
@CLayout Struct JPH_Vec3 ...
@MainThread Function RenderWorld()
@PanicsAtCaller Method Pop(): T
```

| Attribute | Effect |
|---|---|
| `@Inline` | force inlining |
| `@Deprecated(msg)` | warning at use sites |
| `@Flags` | enum values are powers of two |
| `@Open` | an enum holds any value of its integer type |
| `@CLayout` | C-compatible struct layout |
| `@MainThread` | callable only from the main thread |
| `@PanicsAtCaller` | its panics report where it's called |

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
- `@CLayout` lays a Struct out as C does (see [12-ffi.md](12-ffi.md)). `@Serialize` isn't
  needed: Load and Save (below) convert every Struct and Class, so it's an error that says so.

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
- Its parameters are numbers, Bools, Strings, Chars, and Enums. A default is known at compile
  time. It can't take a built-in attribute's name.
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
  of its own. A ReadOnly Field changes only in its Class's New, so `f.Set` on one is an error
  where the loop's copy for it is made, and so is `f.Set` on a Field that the loop's code
  couldn't assign by name: a Private one outside its file, a Package one outside its package,
  and a Protected one outside its file and the code of the Classes extending its Class. What a
  type keeps to itself, like the memory a std.ffi Buffer owns, stays as its own code has it,
  while `f.Get` still reads every Field.
- `For Method m In T` goes over the Methods of a Struct or Class that take nothing, give nothing,
  and throw nothing (the inherited ones first, each in the place of what it overrides), but not
  Destroy, Unsafe, or ByRef Methods, or Async ones, which give a Task. `m.Call(value)` calls it,
  through the Method table for a Class. `m` has `m.Name`, `m.Has(A)`, and `m.Attribute(A)` too.
- `For Method m: M In T` with `Where M: Func(Int): String` goes over the Methods of that Func
  type instead, and `m.Call(value, 5)` passes their arguments. Its Where line picks the Methods;
  it isn't a constraint to meet. A Func type has no ByRef, so a Method with a ByRef parameter is
  never among them.
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
- `TypeName<T>()` is a built-in, like SizeOf: a String of T's declared name, without its module,
  like `"Map<String, Int32>"` or `"(Int32, String?)"`, the same wherever it's asked for, known at
  compile time, and in generic code the
  instance's, so `TypeName<T[]>()` in an instance for Int32 gives `"Int32[]"`. A program's own
  TypeName takes its place.
- `Continue` goes on to the next copy, and `Break` past the last. Other types, like Int, have no
  fields or Methods, so the loop's body is never run.

## Load and Save

```vb
Class Settings
    @Rename("player_name") Field Name: String
    @Optional Field Volume: Double = 0.8
    @Skip Field Cache: Int[] = []
    Field Keys: Map<String, Key>

    Method OnLoaded() Throws ParseError
        If Volume > 1.0 Then Throw ParseError(ParseErrorKind.OUT_OF_RANGE, "Volume is past 1")
    End Method
End Class

Local saved = Save(settings)                    // a Value, which a format writes out
Local loaded = Try Load<Settings>(saved)        // and makes a Settings from again
```

- The built-ins `Load<T>(value)` and `Save(x)`, or `Save<T>(x)` for an x that converts to a T,
  convert between std.core's `Value` and a T, which the compiler checks it can, Field by Field
  through what T is made of, by compile-time reflection. In generic code, each instance's T is
  checked. A module's own Load or Save, or an imported one, takes the name's place.
- The numbers are integers and Doubles, a Bool a BOOL, a String a STRING, and an Enum its member's
  name, as its text has it ("WORLD | PLAYER" for a @Flags Enum's flags, and an integer for a value
  that isn't a member's), which Load also takes in another case, or as an integer. A `T?` is
  Null or a T, a `T[]` an Array, which Load also takes as one Value alone, as XML and a command
  line give one item, a `UInt8[]` bytes, which Load also takes as an Array of integers, as JSON
  has them, and a `Map<String, V>` an Object. A Value is copied, so the T and
  the Value don't share one.
- A Struct or Class is an Object of its Fields, the inherited ones first, by name, but not
  Properties. Load makes one without running its New: each Field starts at its default, then
  takes the Object's entry for its key, and keys the type has no Field for are left alone. Then
  its `OnLoaded()` runs, which can check what was loaded, or work out what wasn't, and throw a
  ParseError. A Class is saved as its own type's Fields, whatever Class extends it, and an
  Abstract one can't be loaded. A Func, an Interface's object, a tuple, a fixed array, a
  Weak, a Ptr, a Task, and the other types like them can't be converted.
- Load also reads a number or a Bool from a String that reads as one, like "8080" or "true", as
  a command line's and an environment's values are; and an integer from a whole Double, and a
  Double from an integer.
- std.core's attributes on a Field change it: `@Rename("key")` gives it another key, `@Skip`
  leaves it out (Load leaves it at its default), `@Optional` lets the Object leave it out, which
  Load leaves at its default too, as it does a `T?` Field, which is Null without one, and
  `@Renamed("old")` reads an older key when the Object doesn't have its own, so what an earlier
  version of a program saved still loads. `@Flatten` puts a Struct's or Class's Field's own
  Fields in the Object with the others, not in an Object of their own. A Field that @Skip or
  @Optional leaves at its default has one, a type with a zero or a default of its own; two
  Fields can't have one key, and a Field's old key is no other's.
- A type that converts itself implements std.core's `Loadable`, with `Static Method
  FromValue(value: Value): Self Throws ParseError`, which Load calls in its place, or
  `Saveable`, with `Method ToValue(): Value`, which Save does.
- `OnLoaded()` is a Method of a Struct or Class that takes and gives nothing, a ByRef Method in a
  Struct, and throws only ParseErrors. It isn't generic, Static, or Async, and an Extend can't
  add one.
- Load throws a ParseError, of `WRONG_TYPE` for a Value of another kind, `OUT_OF_RANGE` for a
  number too big for its type, and `MISSING` for a Field that the Object has no entry for, and
  that isn't @Optional or a `T?`. Its `Path` says where in the Value it is, like
  `"options.sizes[2]"`, or `""` for the Value itself, and its Message starts with it:
  `options.sizes[2]: expected Int32, found "big"`. A key that a path's steps can't tell apart
  from the rest, like one with a dot in it, is quoted in brackets: `["a.b"]`. Its Place is the
  Load<T> call's, and an error that FromValue or OnLoaded throws keeps its own, with the path to
  where it was thrown.
- Load goes 256 levels deep into a Value's Arrays and Objects, and throws a ParseError of
  `OUT_OF_RANGE` past that, and Save goes as deep into Fields and elements, and panics past that,
  as for a Class that holds itself, whose Save would never end.

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
  in it still count, so its `#If`s and `#End If`s pair up. As anywhere, a line inside a block
  comment or a multi-line string is no directive, a block comment after a directive can go on
  past its line, and invalid UTF-8 is an error.
- Each `#If` needs an `#End If`, `#ElseIf` and `#Else` come after an `#If`, and nothing comes
  after `#Else` but `#End If`.

## Assertions

```vb
Assert(hp >= 0, "negative hp")    // panics on failure, in debug and release builds alike
```

An Assert is checked in every build, since the code after it counts on its condition: after
`Assert(e != Null)`, `e` is used as non-null. See [09](09-errors.md#panics).
