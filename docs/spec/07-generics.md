# 07 · Generics

## Syntax

`<T>` type parameters work on functions, methods, classes, structs, and interfaces.

```vb
Function Max<T: Comparable>(a: T, b: T): T
    Return a > b ? a : b
End Function

Class Cache<K, V>
    Where K: Hashable & Equatable
    Where V: Sendable
    Field items: Map<K, V>
End Class

Max(1, 2)              // T inferred
Max<Float>(1, 2.5)     // explicit
```

- A generic Struct or Class is always written with its type arguments: `Box<Int>`, not `Box`.
- A generic Method has type parameters of its own, after its type's: `Method Map<U>(f: Func(T):
  U): Stack<U>`. It has a copy for each set of type arguments, so it isn't in the Method table:
  it can't be `Abstract` or `Override`, and a call never goes through an override.
- A type parameter can't share a name with a type, or with a type parameter it would hide.

## Inference

A call can leave out its type arguments when the compiler can tell them:

1. from the types of the arguments: `First(names)` with `names: String[]` gives `T = String`;
2. from literals passed for a type parameter: `Max(1, 2)` gives `Int`, and `Max(1, 2.5)` gives
   `Float`, since one of them has a decimal;
3. from lambdas, once the type arguments their parameters need are known: `Transform(xs,
   Function(x) => "{x}")` takes `T` from `xs` and `U` from the lambda's result;
4. from the type expected: `Local s: Stack<Int> = Stack()`.

The arguments of a generic type's Method come from the object it's called on. When something
can't be told, the call is an error that asks for the type arguments. A Function used as a value
takes its type arguments from the Func expected, or from what's written: `Local f: Func(Int[]):
Int = First`.

When a generic overload and one that isn't generic fit a call equally well, the one that isn't
generic is chosen.

## Constraints: inline and `Where`

Both forms exist. The **formatter enforces** which one is used, so there is still only one
correct way to write any signature:

- **Inline** `<T: X>` for a single, simple constraint.
- **`Where`** for multiple constraints, multiple constrained parameters, or anything inline
  can't express.

`&` combines constraints: `Where K: Hashable & Equatable`.

### Things only `Where` can express

**Conditional methods**, available only when the type argument qualifies:

```vb
Class List<T>
    Method Sum(): T
        Where T: Numeric           // ints.Sum() works; enemies.Sum() is a compile error
        ...
    End Method
End Class
```

**Conditional conformance:**

```vb
Extend List<T> Implements Printable
    Where T: Printable
    Method ToString(): String => "[" + Join(Self, ", ") + "]"
End Extend
```

- An Extend's Where lines constrain the type parameters it names: everything it adds is
  conditional, and so is each Interface it lists. Without them, it adds to every instance.
- A Class that implements an Interface only through an Extend's Where is a value of that
  Interface for the instances whose type arguments meet it: each one's objects get the table.

**Relations between parameters**, through a generic Interface:

```vb
Function Collect<S, E>(items: S): E[]
    Where S: Iterator<E>
```

- A Where line goes at the top of the body. On a generic type, it constrains the type's type
  parameters. On a Method, one for its type's type parameter makes the Method conditional: only
  the instances whose type argument meets it have it, and it isn't in the Method table.
- Each call, `New`, and type written with type arguments meets the constraints. A type argument
  meets `T: I` when it's a Struct or Class that implements I, an Interface that is I or extends
  it, or a type parameter whose own constraints say so.
- A call infers a type argument from a constraint on another one: `Collect(numbers)` takes `E`
  from the instance of `Iterator` that the type of `numbers` implements.

## Constraints are interfaces

Interfaces used as constraints may require statics, operators, and `Self`:

```vb
Interface Numeric
    Static Method Zero(): Self
    Operator +(a: Self, b: Self): Self
    Operator *(a: Self, b: Self): Self
End Interface

Function Sum<T: Numeric>(xs: T[]): T
    Local total = T.Zero()
    For x In xs
        total += x
    End For
    Return total
End Function
```

### Built-in interfaces

`Equatable`, `Comparable`, `Hashable`, `Numeric`, and `Printable` are in std/core/interfaces.fb,
and `Iterator<T>`, which `For ... In` goes over, is in std/core/iterator.fb (see
[04-control-flow.md](04-control-flow.md)). `Sendable` comes with concurrency (M4).

```vb
Interface Equatable
    Operator ==(a: Self, b: Self): Bool
End Interface

Interface Comparable Extends Equatable
    Operator <(a: Self, b: Self): Bool
    Operator >(a: Self, b: Self): Bool => b < a      // and <= and >=, from <
End Interface

Interface Hashable Extends Equatable
    Method Hash(): UInt64
End Interface

Interface Printable
    Method ToString(): String
End Interface

Interface Numeric
    Static Method Zero(): Self
    Operator +(a: Self, b: Self): Self                // and -, *, and /
End Interface
```

- The built-in types implement the ones that fit them. Numbers implement all five; Strings all
  but `Numeric`; and Bools and Enums `Equatable`, `Hashable`, and `Printable`. A `T?`, tuple, or
  array implements what its elements do, except that a `T?` is only `Equatable` and `Hashable`,
  and neither tuples nor arrays are `Comparable` or `Numeric`. The math types will in M6.
- A built-in type has the Methods of the ones it implements: `5.ToString()`, `name.Hash()`.
- A Struct or Class implements one by listing it. One that lists `Equatable` without an
  Operator `==` of its own gets the built-in one, which compares a Struct's fields or a Class's
  objects themselves, and one that lists `Hashable` without a `Hash` gets the built-in one,
  which combines a Struct's fields' hashes or comes from a Class's object. The built-in `Hash`
  goes with the built-in `==`, so a type with an Operator `==` of its own needs a `Hash` of its
  own.
- Values that are equal have equal hashes. A `T?` that holds a value hashes like it, and Null's
  hash is 0.
- What implements `Printable` goes in a string through its `ToString`: `"{enemy}"`. An
  Interface's object does too, when the Interface extends `Printable`, and so does a `T` whose
  constraints say it's `Printable`.

## Generic Interfaces

```vb
Interface Source<T>
    Method Take(): T
    Method TakeTwo(): (T, T) => (Take(), Take())
End Interface

Class Counter Implements Source<Int>
    ...
End Class

Struct Constant<T> Implements Source<T>
    Field Value: T
    Method Take(): T => Value
End Struct

Interface Labeled Extends Source<String>
    ReadOnly Property Label: String
End Interface
```

- An Interface's type parameters are types in its members, like its Self. Each set of type
  arguments makes an instance, like `Source<Int>`, whose members have them in their types: a
  Counter's `TakeTwo` gives an `(Int, Int)`.
- A type implements one instance of a generic Interface at most, counting the ones it lists, the
  ones those extend, and the ones it inherits, so its members' types are always known. A type
  parameter's constraints name one instance of each too.
- An instance that can be a type, like `Source<Int>`, has values like any Interface, and an
  Interface can extend an instance.
- An Interface's type parameters can have constraints, `Interface Sorted<T: Comparable>`,
  which hold in its members, and which each instance meets: a constraint `<S: Sorted<E>>` needs
  `E: Comparable` too.

## Const generics

```vb
Struct Grid<T, Const W: Int, Const H: Int>
    Field cells: T[W, H]
    Method Width(): Int => W
End Struct

Function Sum<Const N: Int>(values: Int[N]): Int

Local g: Grid<Float, 64, 64>
Sum(row)                           // N from row's Int[8]
```

- A Const type parameter, `Const N: Int`, stands for a positive integer, a size or a count: in
  the body it's a value of its type, and in types it's a fixed array's size. It has no
  constraints.
- Its argument is an integer literal, a `Const`, or a Const type parameter of the code around
  it, from 1 to a fixed array's limit. Each value makes an instance, like `Grid<Float32, 64,
  64>`.
- An argument or a fixed array's size can be computed from Const type parameters with `+` and
  `*`, which keep it positive: `Function Join<Const N: Int, Const M: Int>(a: Int[N], b:
  Int[M]): Int[N + M]`, or `Grid<W * 2>`. Each instance works it out.
- A call infers it from a fixed array's size: `Sum(row)` with `row: Int[8]` gives `N = 8`.
- A fixed array whose size is a Const type parameter can't come from an array literal, since its
  size is only known in each instance.
- An Interface can have Const type parameters too: `Interface Sized<Const N: Int>`.

## Semantics

- **Monomorphized** (like C++ templates): every instantiation compiles to specialized
  native code. Tradeoff: binary size and compile time.
- **Invariant**: `List<Boss>` is not a `List<Enemy>`. No variance annotations.
- A generic body is checked once, with its type parameters as types of their own, so errors
  point at the generic code rather than at each use. What a `T` can do is what its constraints
  say: without any, it's stored, passed, returned, and put in `T?`, `T[]`, tuples, and Funcs,
  but it has no members, no `==`, and no zero, and can't be shown in a string. With them, it has
  their Methods and Properties, `T.Zero()` calls their Static Methods, and their Operators work
  on it, `==` among them. It still isn't an Interface's value, since it may be a Struct.
- Each instance calls its type argument's own members, so generic code costs what the code it
  stands for would.
- `T?` with an `Int?` for `T` is `Int?`.

## Parsing `<`

C#'s disambiguation rule: in expression context, `Name<...>` is parsed as type arguments
when the token after the closing `>` is `(`, `.`, `)`, or `,`. Otherwise `<` is a
comparison.

The closing `>>` of nested type arguments is read as two `>`s, so `Map<K, List<V>>` needs no
space.
