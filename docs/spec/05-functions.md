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

### ByRef

A `ByRef` parameter is the caller's own variable: assigning to it changes the caller's
variable. The call says `ByRef` too, so it's visible that the call can change it:

```vb
Swap(ByRef x, ByRef y)
Swap(x, y)                 // ✗ error: Swap takes a ByRef; write ByRef x
```

- Only something assignable can be passed `ByRef`: a `Local`, a `Global`, a `ByRef` parameter,
  and later a field or array element. Not a `Const`, a literal, a loop variable, a read-only
  parameter, or a computed expression.
- The argument's type must match the parameter's exactly, and it must already be assigned.
- The same variable can't be passed `ByRef` twice in one call.
- A `ByRef` parameter can't have a default value, can't be captured by a closure, and can't be
  passed to another thread.

## Overloading

Functions may be overloaded by parameter types.

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
```

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

### Capture lists

Under refcounting, a closure stored in an object that captures that object creates a
cycle that is never freed. Capture lists (C++-style) control how values are captured:

```vb
OnOpen = Function[Weak Self]() => Self?.PlaySound()   // Self may be gone, hence ?.
```

- `[Weak x]` captures `x` as a weak reference (its type becomes `x?` inside the closure).
- The compiler **warns** when a closure stored in a field captures `Self` strongly.

### C callbacks

Closures cannot be passed to C. FFI callbacks use `Callback(...)`, which accepts only
non-capturing functions. See [12-ffi.md](12-ffi.md).

## Generic functions

```vb
Function Max<T: Comparable>(a: T, b: T): T
```

See [07-generics.md](07-generics.md).
