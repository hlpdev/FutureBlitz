# 05 · Functions and Closures

## Declaration

```vb
Function Add(a: Int, b: Int): Int
    Return a + b
End Function

Function Log(msg: String)          // no return type = returns nothing
    Print(msg)
End Function

@Inline Function Square(x: Float): Float => x * x   // expression-bodied
```

Named functions are declared at the top level of a file, not inside other blocks. Lambdas
(below) cover functions inside functions.

## Parameters

```vb
Function Spawn(pos: Vec3, hp: Int = 100, name: String = "grunt"): Enemy  // defaults
Spawn(Vec3(0, 1, 0), name: "boss")                                       // named args

Function Swap(ByRef a: Int, ByRef b: Int)       // pass by reference
Function Sum(nums: Int...): Int                 // variadic; nums is Int[]
```

- Parameters are read-only inside the function. To change one, copy it into a `Local`.
- Default values must be known at compile time.

### Variadic parameters

- `nums: Int...` takes the rest of a call's arguments as an `Int[]`: `Sum()`, `Sum(1)`, and
  `Sum(1, 2, 3)` pack theirs into a new array. One argument that's an array already, a `T[]` or
  an array literal, is passed as it is: `Sum(xs)`. Named, it takes an array: `Sum(nums: xs)`.
- Only the last parameter can take the rest of the arguments. It can't be `ByRef` or have a
  default, since without any it's an empty array, and an Operator can't have one.
- A generic one infers its type argument from each argument: `Largest(1.5, 2, 3)` with `rest:
  T...` gives `Float`.
- `values: Printable...`, with std.core's Printable, takes values of any types a string can
  show, mixed, and each becomes its text at the call, as `"{x}"` makes it, so `values` is a
  `String[]`: that's how `Print(score, name)` takes them. An array argument is a
  value like any other, so it's shown; named, `values:` takes a `String[]` of texts. A value a
  string can't show, like a T?, is an error that names the function: "String? can't be shown by
  'Print', since it may be Null; give a value for Null with '??'".
- `values: Formattable...`, with std.core's Formattable, takes values of any Formattable types,
  mixed, as Format's does, so `values` is a `Formattable[]`: an object goes in as it is, and a
  value that isn't one, a number, Bool, String, Enum, or Struct, in an object of std's that
  holds it, whose Format and ToString are its value's. One `Formattable[]` argument is passed
  whole. A value of a type that isn't Formattable is an error that names the parameter: "P isn't
  Formattable, which 'values' takes".
- Where two overloads fit a call as well as each other, one that takes the arguments as they are
  beats one that packs them into its variadic parameter, as C#'s do.

### ByRef

A `ByRef` parameter is the caller's own variable: assigning to it changes the caller's
variable. The call says `ByRef` too, so it's visible that the call can change it:

```vb
Swap(ByRef x, ByRef y)
Swap(x, y)                 // ✗ error: Swap takes a ByRef; write ByRef x
```

- Only something assignable can be passed `ByRef`: a `Local`, a `Global`, a `ByRef` parameter,
  or a field of one. Not a `Const`, a literal, a loop variable, a read-only parameter, a computed
  expression, or an array's element, which can move when the array grows.
- The argument's type must match the parameter's exactly, and it must already be assigned.
- The same variable can't be passed `ByRef` twice in one call.
- A `ByRef` parameter can't have a default value, can't be captured by a closure, and can't be
  passed to another thread.

## Overloading

Functions may be overloaded by parameter types, or by how many type parameters of their own
they have (see [07-generics.md](07-generics.md)).

```vb
Function Print(value: Int)
Function Print(value: String)
```

When several overloads fit a call, one that matches every argument's type exactly beats one
that needs a lossless conversion. If that still leaves more than one, the call is ambiguous and
an error.

A literal argument matches its default type (`Int` or `Float`) exactly, and any other type it
fits by conversion. So with `Show(v: Long)` and `Show(v: Double)`, `Show(5_000_000_000)` is
ambiguous; write `5_000_000_000 As Long`.

## Returns

- A function with a return type must return a value on every path; reaching the end without
  one is a compile error.
- A function without a return type returns nothing. It can use a bare `Return`, but not
  `Return value`.

## Tuples and multiple returns

```vb
Function MinMax(xs: Int[]): (Int, Int)
    ...
    Return (lo, hi)
End Function

Local lo, hi = MinMax(data)     // destructuring
Local pair = MinMax(data)       // (Int, Int)
Print("{pair.0}")               // elements are numbered from 0
a, b = (b, a)                   // swap
```

- A tuple type is two or more types in parentheses: `(Int, String)`, `(Int, Int)?`,
  `(String, Int)[]`. `(T)` is just `T`.
- A tuple is a value, copied on assignment like a Struct (see [08](08-memory.md)). Its elements
  can be of any type but `Weak<T>`.
- `(a, b)` makes one. Where a tuple is expected, each value converts to its element's type:
  `Local p: (Long, String?) = (1, Null)`. Any other tuple converts only to its own type, or to
  its `T?`.
- `t.0`, `t.1`, and so on are its elements, which can be assigned. `t.0.1` is an element of an
  element.
- `Local lo, hi = value` declares a Local for each element, of the element's type. There's a
  name for every element, and `_` skips one. Only a `Local` takes a tuple apart.
- `a, b = value`, or `(a, b) = value`, assigns each element to a target, converting it to the
  target's type. The whole value is made first, then each target is assigned, from the left, so
  `a, b = (b, a)` swaps.
- `==` and `!=` compare element by element, when every element's type has them.
- A tuple shows in a string as its elements: `"{(1, "a")}"` is `(1, a)`.

## Calls

- **Parentheses are always required**, including for zero-argument calls and calls used
  as statements. Blitz-style command calls (`Graphics3D 800, 600`) do not exist.

## Entry point

- Top-level statements in the main file are the program's entry point (script style, as in
  Blitz3D).
- There is no special `Main` function: `Function Main()` is an ordinary function.
- A bare `Return` in top-level code ends the program early.

## Function types and lambdas

```vb
Local twice = Function(x: Int) => x * 2          // expression lambda
Local cb: Func(Int): Int = twice

button.OnClick = Function()                      // block lambda
    Print("clicked")
End Function
```

Lambdas are always written with `Function`: `Function(x) => expr`, or a block ending in
`End Function`. There is no `() =>` shorthand.

- `Func(A, B): R` is the type of a function value that takes an `A` and a `B` and returns an
  `R`; `Func(A)` returns nothing. A `?` or `[]` after the return type belongs to it, so a Func
  that can be Null is `(Func(): Int)?`, and an array of them is `(Func(): Int)[]`.
- A parameter without a type, like the `x` in `Function(x) => x * 2`, takes the type that the
  Func expected there has. With no Func expected, it needs one. Passed to overloads, the lambda
  fits only those whose parameter there is a Func it can be, as `Replace(text, Function(m) =>
  ...)` picks Regex's Replace that takes one over the one that takes a String.
- A lambda returns what its `: Type` says, or else what the Func expected says. Without either,
  `=> value` returns the value's type, and a block returns nothing.
- `Return` in a lambda leaves the lambda. `Break` and `Continue` can't leave it, and neither can
  an error: a Func doesn't throw, so the lambda catches what it calls.
- A block lambda's body ends statements at line breaks, even inside the brackets of a call.
- A Function's name alone is a function value: `Local f: Func(Int): Int = Twice`. With
  overloads, the Func expected picks one. Methods, built-in functions, and Functions that throw
  or take a ByRef parameter aren't values; a lambda can call them.
- A function value is called like a function, `f(1)`, with its arguments in order. It has no
  `==`, but a `Func?` compares with `Null`.

### Sendable Func

`Sendable Func(A): R` is a function value that other threads can call, as Swift's `@Sendable`
closures are (see [10](10-concurrency.md#safety-data-races-are-compile-errors)):

```vb
Function ParallelMap<T: Sendable, R: Sendable>(items: T[], f: Sendable Func(T): R): R[]
```

- A lambda or a Function's name written where one is expected is one. What the lambda captures
  can't change and is Sendable, as for Spawn's work: a Const, a parameter, or a loop variable
  holding a Sendable value, or a Self that's Sendable. Its code is checked as work on another
  thread, so it can't use a Global or call a `@MainThread` Function, and an Async lambda, whose
  code runs on the main thread, isn't one.
- Its parameters and result can be of any type, since each call runs on the thread that makes it.
- It's Sendable itself, so a Shared Class's ReadOnly Field, a Channel, or a Mutex can hold one,
  and Spawn's work and a Parallel For's body can call one. Spawn and Thread.Start take one as
  their work too.
- It converts to the Func of its parameters and result, but a Func doesn't convert to it, since a
  Func's closure stays on the thread that made it: "expected Sendable Func(Int32): Int32, found
  Func(Int32): Int32, which other threads can't call".
- Its closure counts its owners atomically, as a Shared Class's objects do, so whichever thread
  lets go last frees it.

## Closures

A closure is a function that captures variables from the scope where it was created.

```vb
Function MakeCounter(): Func(): Int
    Local count = 0
    Return Function()                // captures count
        count++
        Return count
    End Function
End Function

Local next = MakeCounter()
Print(next())   // 1
Print(next())   // 2
```

- Variables are captured **by reference** (the closure and the outer scope share them).
- A closure is a refcounted object. A captured `Local` lives in a cell that the closure and its
  scope share. A parameter, a loop variable, a `Const`, and `Self` can't change, so the closure
  copies them: each pass of a `For` gives a lambda its own loop variable.
- A `ByRef` parameter can't be captured, since the caller's variable may be gone when the lambda
  runs. Neither can `Self` in a Struct's `New`, which isn't made yet.
- Using a field or Method by name in a lambda captures `Self`.
- A null check doesn't narrow a captured `Local` inside the lambda, and once a lambda that
  assigns a `Local` exists, no check outside narrows it either: any call could change it.

### Capture lists

Under refcounting, a closure stored in an object that captures that object creates a
cycle that is never freed. Capture lists (C++-style) control how values are captured:

```vb
OnOpen = Function[Weak Self]() => Self?.PlaySound()   // Self may be gone, hence ?.
```

- `[Weak x]` captures `x` as a weak reference (its type becomes `x?` inside the closure).
  It refers to the object `x` holds when the closure is made, and like any `Weak<T>`, a check
  can't narrow it.
- With `[Weak Self]`, fields and Methods are reached through `Self?.`, not by name.
- The compiler **warns** when a closure stored in a field captures `Self` strongly.

### C callbacks

Closures cannot be passed to C. FFI callbacks use `Callback(...)`, which accepts only
non-capturing functions. See [12-ffi.md](12-ffi.md).

## Generic functions

```vb
Function Max<T: Comparable>(a: T, b: T): T
```

See [07-generics.md](07-generics.md).
