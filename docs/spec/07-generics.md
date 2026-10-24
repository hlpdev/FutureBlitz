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
- Generic code is compiled once for each set of type arguments it's used with, so code that
  calls itself, or each other, with a type holding its own type parameter, like `Deep<T>`
  calling `Deep([x])` with a `T[]`, would need instances without end, and is an error at the
  call: "'Deep' gets T[] for T here, which leads back to this code, so it would need a larger
  instance each time, without end". A Const type argument computed from its own parameter needs a
  different instance each time, as `Down<N - 1>` does, unless `/` or `%` computes it: then it can
  come back to one already made, as `Bits<N / 2>` comes to `Bits<0>`, which needs itself, so only
  the chain of instances below stops one that doesn't. A type that only names a larger instance,
  like a Field of `Node<Node<T>>?`, is fine, and so is making one with the New a Struct gets, when
  its Fields' defaults are known at compile time, since that runs none of its code: a
  `Quantity<L + L2>` in Quantity's own Operator. An Interface's default is code of this kind too,
  with what each call gives it for Self, and a Class's copies of the defaults in the Interface's
  table run when one of its objects is made, so a default that wraps Self in a Class implementing
  the Interface, `Holder<Self>(Self)`, is an error where the wrapped one runs it again: a call of it
  on the Holder, or the Holder's table holding it. One the compiler can't follow there, like a
  Static default, or one that only tests for a Holder, which still makes its table, is an error
  where the compiler makes the instance that keeps growing: one more than 50 instances of the same
  function or Class deep in a chain of instances made one from another, or one whose type arguments
  are made of more than 65536 types, as an argument that doubles each time, like `Pair<Self, Self>`,
  soon is. A type nested more than 1000 deep, or made of more than 65536 types, is an error even
  where nothing grows. A Field's type that grows that way is left out past those limits, as it's
  never reached.
- A Const type argument an instance works out, like `N * 2` or `N - 1`, has to fit its type
  parameter's type, and one that's a fixed array's size has to be from 1 to 2147483647, or it's
  an error where the instance is made, as one that divides by 0 is.

## Inference

A call can leave out its type arguments when the compiler can tell them:

1. from the types of the arguments: `First(names)` with `names: String[]` gives `T = String`,
   and an argument whose Class, Struct, or array implements an instance of a generic Interface,
   or whose Interface extends one, gives that instance's: `Total(set)` for `values: Iterable<T>`
   with a `Set<Int>` gives `T = Int`;
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
  conditional, and so is each Interface it lists, with the defaults it gives. Without them, it
  adds to every instance.
- What a Where line makes conditional implements an Interface only where the Interface is
  listed for instances that meet it: a type that lists `Sized` for every instance implements it
  with Sized's default, and the one a conditional `Doubled Extends Sized` gives is what a call
  through Doubled runs. A conditional Method can't implement a member of an Interface listed for
  every instance. A call on an instance that has both reaches the one every instance has, as it
  would a type's own Method before an Extend's, and an Override in a Class extending it
  overrides that one; a call through Doubled still runs the one the Extend gives, which nothing
  overrides.
- A conditional `Operator ==` is what `==` means for the instances that meet its Where, in a
  direct `==` and inside the arrays, tuples, Structs, and T?s that hold them; the others have no
  Operator == of their own and compare the built-in way.
- A Class extending an instance that doesn't meet a Where, like `Base<Rock>`, has none of what it
  makes conditional, so it can declare Methods and Properties of those names. One extending an
  instance that meets it, or one with the Class's own type parameters in it, like `Base<T>`,
  can't, as for any Extend's member.
- A Class that implements an Interface only through an Extend's Where is a value of that
  Interface for the instances whose type arguments meet it: each one's objects get the table, and
  the tables of the Interfaces it extends.

**Relations between parameters**, through a generic Interface:

```vb
Function Collect<S, E>(items: S): E[]
    Where S: Iterator<E>
```

- A Where line goes at the top of the body. On a generic type, it constrains the type's type
  parameters. On a Method, one for its type's type parameter makes the Method conditional: only
  the instances whose type argument meets it have it, and it isn't in the Method table, so a
  Class extending one of its instances can't override it, or declare a Method of its name and
  parameters.
- Each call, `New`, and type written with type arguments meets the constraints. A type argument
  meets `T: I` when it's a Struct or Class that implements I, an Interface that is I or extends
  it, or a type parameter whose own constraints say so.
- A call infers a type argument from a constraint on another one: `Collect(numbers)` takes `E`
  from the instance of `Iterator` that the type of `numbers` implements.

### `Class` and `Error`

`Class` in a constraint says the type argument is a Class, as C#'s `where T : class` does, so
generic code can refer to its objects without keeping them:

```vb
Function Subscribe<O: Class>(owner: O, handler: Func(O, T)): Subscription
    Local kept: Weak<O> = owner
    ...
End Function
```

- Under it, `Weak<T>` and `[Weak x]` take a T, as they take a Class (see
  [08](08-memory.md#cycles)), and so do RefEquals, `x Is C`, and `Case c As C`, either way round:
  `x Is T` tests for each instance's Class.
- A Class meets it, an instance of a generic Class, and a Shared Class. Nothing else does: not a
  Struct, tuple, fixed array, or built-in type, which are values; not a String, a T[], or a
  function value, which refer to something that isn't a Class's object; not an Interface, whose
  value can be an array too; and not a `T?`, which can be Null.
- It's only a constraint. It gives what every Class has, which is that it's `Equatable`, and `&`
  adds the rest: `Where T: Class & Hashable`. With Sendable, `Where T: Class & Sendable`, the T is
  a Shared Class, so its `Weak<T>` is Sendable too.

`Error`, std's root of errors, in a constraint says the type argument is a Class that extends
it, as Swift's `E: Error` does, so generic code can throw and catch one, and list it after a
function's Throws or a Task's: `Function Rethrow<E: Error>(error: E): Int Throws E` (see
[09](09-errors.md) and [10](10-concurrency.md#async--await)).

- A type parameter in a Throws needs it: "E is in a Throws, so it needs a constraint that says
  it's an error, like <E: Error>".
- What it gives is what `Class` gives, and it converts to an Error, whose members it has through
  that: `Local error: Error = failed`. `Catch e As E` catches each instance's E.

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

`Equatable`, `Comparable`, `Hashable`, `Numeric`, `Printable`, `Cloneable`, and `Defaultable`
are in std/core/interfaces.fb, and `Iterator<T>`, which `For ... In` goes over, is in
std/core/iterator.fb (see
[04-control-flow.md](04-control-flow.md)). `Sendable`, also in std/core/interfaces.fb, says what
can go to another thread, which the compiler decides (see [10-concurrency.md](10-concurrency.md)),
and std.ffi's `CData` says what C code has, which it decides too (see
[12-ffi.md](12-ffi.md#cdata)).

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
    Static Method One(): Self
    Operator +(a: Self, b: Self): Self                // and -, *, and /
End Interface
```

- The built-in types implement the ones that fit them. Numbers implement all but `Cloneable`;
  Strings all but `Numeric` and `Cloneable`; and Bools and Enums `Equatable`, `Hashable`,
  `Printable`, and `Defaultable`. A `T?`, tuple, or
  array implements what its elements do, except that a `T?` is only `Equatable` and `Hashable`,
  and neither tuples nor arrays are `Comparable` or `Numeric`. std.math's vectors are
  `Numeric`, `Hashable`, `Printable`, and `Defaultable`, and its matrices and `Quat` `Equatable`
  and `Printable` (see [stdlib.md](../stdlib.md#stdmath)).
- `Cloneable` (`Method Clone(): Self`) is what makes a copy of itself, which arrays are with
  their own `Clone`, and `Defaultable` (`Static Method DefaultValue(): Self`) what has a value to
  start from, which the numbers, Bools, Strings, and Enums are with their zero: `T.DefaultValue()`
  is 0, False, "", or an Enum's zero, its first value, or no flags for a @Flags Enum. std's
  collection Classes are both.
- A built-in type has the Methods of the ones it implements: `5.ToString()`, `name.Hash()`.
- A Struct or Class implements one by listing it, and every Class is `Equatable` and `Hashable`
  without listing them, so `enemies.Remove(e)` and `Set<Enemy>` work on any Class. One that
  lists `Equatable` without an Operator `==` of its own gets the built-in one, which compares a
  Struct's fields or a Class's objects themselves, and one that lists `Hashable` without a
  `Hash` gets the built-in one, which combines a Struct's fields' hashes or comes from a Class's
  object. The built-in `Hash` goes with the built-in `==`, so a type with an Operator `==` of
  its own needs a `Hash` of its own, however it gets that `==`: written, inherited, from an
  Interface's default, from another file's Extend, or, for one instance, from an Extend of it.
  That `Hash` is `Hash(): UInt64`; one that gives another type is a Method like any other. A
  Class with such an `==` and no `Hash` is `Equatable` but not `Hashable`, unless it lists
  `Hashable`, which is then that error. An instance whose Extend gives it an `==` decides for
  itself: `Box<String>`, given an `==` and a `Hash`, hashes with them, and given only an `==`
  isn't `Hashable`, while `Box<Int32>` hashes its objects. One whose `==` has a Where line, its
  own or an Extend's, implements them as it lists them, as a Struct does.
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
  ones those extend, and the ones it inherits, and for an instance like `Box<Int>`, the ones its
  generic type gives every instance, so its members' types are always known. A type
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

- A Const type parameter, `Const N: Int`, stands for an integer of its type: in the body it's a
  value of that type, and in types a fixed array's size, or an argument for another. It has no
  constraints.
- Its argument is an integer literal, a `Const`, or a Const type parameter of the code around
  it, any value of its type, 0 and below among them, as a unit's exponents are, though a
  `UInt64`'s goes up to 9223372036854775807, since each instance works one out in an Int64.
  Each value makes an instance, like `Grid<Float32, 64, 64>`. Where it's a fixed array's size,
  it has to be from 1 to a fixed array's limit, which each instance checks.
- An argument or a fixed array's size can be computed from Const type parameters with `+`, `-`,
  `*`, `/`, and `%`, which round toward 0 as an Int's do, a `-` before one, and parentheses:
  `Function Join<Const N: Int, Const M: Int>(a: Int[N], b: Int[M]): Int[N + M]`, `Grid<W * 2>`,
  or `Quantity<L1 - L2, (T - 1) / 2>`. Each instance works it out, in an Int64, and one that
  divides by 0, passes what an Int64 holds, doesn't fit its parameter's type, or makes a size
  below 1 is an error where the instance is made, as `Trimmed<1>` is for an `Int[N - 1]`.
- Values computed alike are one value, whatever types they're reached through: `+`, `-`, and `*`
  by a known number combine, so `L - 0` is `L`, `L + L` is `L * 2`, and `M + N` is `N + M`, and
  each instance works out what they combine to. One that would drop a part that can fail, like
  `(10 / N) * 0`, stays as it's written.
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
- `T?` with an `Int?` for `T` is `Int??`, so generic code's `Null` for "none" stays apart from
  a `Null` it holds: `Find` over an `Int?[]` tells an element that's `Null` from one past the end.

## Parsing `<`

C#'s disambiguation rule: in expression context, `Name<...>` is parsed as type arguments
when the token after the closing `>` is `(`, `.`, `)`, or `,`. Otherwise `<` is a
comparison. Type arguments can be qualified, so `F(a < m.B, c > (d))` calls `a<m.B, c>`, as
`F(a < b, c > (d))` does; parenthesize a comparison there: `F((a < m.B), c > (d))`.

The closing `>>` of nested type arguments is read as two `>`s, so `Map<K, List<V>>` needs no
space.
