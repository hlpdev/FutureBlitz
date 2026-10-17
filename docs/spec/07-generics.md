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
3. from lambdas, once the type arguments their parameters need are known: `Map(xs, Function(x)
   => "{x}")` takes `T` from `xs` and `U` from the lambda's result;
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

**Relations between parameters:**

```vb
Function Convert<A, B>(xs: A[]): B[]
    Where A: Into<B>
```

## Constraints are interfaces

Interfaces used as constraints may require statics, operators, and `Self`:

```vb
Interface Numeric
    Static Function Zero(): Self
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

`Equatable`, `Comparable`, `Hashable`, `Numeric`, `Printable`, `Sendable`.

Primitives and the math types implement the relevant ones out of the box.

## Const generics

```vb
Struct Grid<T, Const W: Int, Const H: Int>
    Field cells: T[W, H]
End Struct
```

## Semantics

- **Monomorphized** (like C++ templates): every instantiation compiles to specialized
  native code. Tradeoff: binary size and compile time.
- **Invariant**: `List<Boss>` is not a `List<Enemy>`. No variance annotations.
- A generic body is checked once, with its type parameters as types of their own, so errors
  point at the generic code rather than at each use. What a `T` can do is what its constraints
  say: without any, it's stored, passed, returned, and put in `T?`, `T[]`, tuples, and Funcs,
  but it has no members, no `==`, and no zero, and can't be shown in a string.
- `T?` with an `Int?` for `T` is `Int?`.

## Parsing `<`

C#'s disambiguation rule: in expression context, `Name<...>` is parsed as type arguments
when the token after the closing `>` is `(`, `.`, `)`, or `,`. Otherwise `<` is a
comparison.

The closing `>>` of nested type arguments is read as two `>`s, so `Map<K, List<V>>` needs no
space.
