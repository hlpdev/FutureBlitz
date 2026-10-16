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

## Parsing `<`

C#'s disambiguation rule: in expression context, `Name<...>` is parsed as type arguments
when the token after the closing `>` is `(`, `.`, `)`, or `,`. Otherwise `<` is a
comparison.

The closing `>>` of nested type arguments is read as two `>`s, so `Map<K, List<V>>` needs no
space.
