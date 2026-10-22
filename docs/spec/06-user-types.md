# 06 · Structs, Classes, Interfaces, Enums

## Struct (value type)

Copied on assignment, stored inline (stack or inside its container). Use for small data:
vectors, particles, records.

```vb
Struct Particle
    Field pos: Vec3
    Field vel: Vec3
    Field life: Float
End Struct

Local p = Particle(pos: Vec3(0, 0, 0), vel: Vec3(0, 1, 0), life: 1.0)
```

- Every struct gets a constructor whose parameters are its fields, in declaration order, with
  the fields' defaults. Like any call, it takes positional or named arguments. Declaring a `New`
  replaces it.
- A method sees `Self` and its fields read-only. A `ByRef Method` can change them, so it can only
  be called on something assignable: a `Local`, a `Global`, a `ByRef` parameter, or a field of
  one, or an element of a fixed array. A `T[]`'s elements can move when the array grows, so not
  one of those.

```vb
Struct Counter
    Field count: Int

    Method Doubled(): Int => count * 2      // reads Self
    ByRef Method Bump()                     // changes Self
        count++
    End Method
End Struct

Local c = Counter(1)
c.Bump()
```

## Class (reference type)

Heap-allocated, refcounted, shared by reference.

```vb
Class Enemy Implements Damageable
    Public Field name: String
    Field hp: Int = 100

    New(name: String)               // constructor
        Self.name = name
    End New

    Method Hurt(amount: Int)
        hp -= amount
    End Method

    Method Destroy()                // runs the moment the refcount hits 0
        Print("{name} freed")
    End Method
End Class
```

- A Class without a `New` gets one without parameters, which leaves every field at its
  default.
- A field can refer to a Class, its own included. One that can't be `Null` has no default, so
  the Class needs a `New` that assigns it on every path before reading it or using `Self`.
- A Class is a reference: copies share one object. Its fields can change through any reference,
  even a read-only parameter or a `Const`. A Method can always change them, so `ByRef Method` is
  only for Structs, and `Self` can't be replaced.
- `Destroy()` takes no parameters and returns nothing. It runs when the last reference goes
  away, before the fields are released. It can't be called, and it may not keep a reference to
  its object: storing `Self` somewhere panics.
- `Class` replaces Blitz's `Type` keyword.
- Blitz's implicit per-type global lists (`Each`, `First`, `Last`, `After`, `Before`,
  `Insert`, `Delete`) **do not exist**. Store objects in collections (or a `Pool<T>`).

## Inheritance

Single inheritance plus any number of interfaces.

```vb
Class Boss Extends Enemy
    Override Method Hurt(amount: Int)
        Super.Hurt(amount / 2)
    End Method
End Class
```

- `Abstract` classes/methods, `Final` classes/methods.
- `Override` is required when overriding.
- A Class inherits the fields and Methods of the one it extends, and can't declare a field with
  an inherited member's name. A Method with the parameter types of an inherited one overrides it
  and returns the same type; other parameter types make an overload.
- Methods are virtual: a call runs the override of the object's Class. A `Final` Method can't be
  overridden, and a `Final` Class can't be extended.
- An `Abstract` Method is only its first line, in an `Abstract` Class, which can't be made. A
  Class that isn't Abstract overrides every Abstract Method it inherits.
- `Super.Hurt(5)` calls the Method of the Class extended, on `Self`. An Abstract one can't be
  called that way.
- A Class's `New` starts with `Super.New(...)`, or else the `New` of the Class it extends that
  takes no arguments runs first. Until `Super.New` runs, `Self` can't be used.
- A Class without a `New` of its own takes the parameters of each `New` of the Class it extends,
  and their Throws, and passes them on: `Class FileError Extends Error` makes
  `FileError("missing")`.
- A Class can extend an instance of a generic Class: `Class Names Extends Container<String>`, or
  with its own type parameters, `Class Sorted<T> Extends Container<T>`. What it inherits has the
  instance's type arguments in it, and its overrides take those types.
- Every Class's `Destroy` runs, the most derived first, so `Destroy` is never `Override`.
- An object converts to any Class its Class extends. `x Is T` tests its Class at run time,
  `x As T` panics when it isn't a `T` ("expected Boss, found Grunt"), and `x As? T` gives `Null`
  instead. Classes that aren't related are an error with each.
- A Struct can't extend anything, but it can implement Interfaces.

## Interfaces

```vb
Interface Damageable Extends Shown
    Method Hurt(amount: Int)
    ReadOnly Property Health: Int

    Method Kill()                   // default implementation
        Hurt(Health)
    End Method
End Interface

Class Enemy Implements Damageable
    ...
End Class
```

- An Interface holds Methods, `ByRef Method`s, Static Methods, Operators, and Properties. A
  Method or Operator that's only its first line is required, and one with a body is a
  **default**. A Property is only its first line: it needs a Get, and a Set unless it's
  `ReadOnly`.
- In an Interface, `Self` is a type: the type that implements it. A Method's `Self` is a value
  of that type.
- `Interface A Extends B, C` has the members of B and C too, and what implements A implements
  them. A member of A with the name, kind, and types of one of B's, A's Self for B's,
  redeclares it: the two are one member, and A's default, if it has one, is what a type gets
  for both, like a `>` that an Interface extending Comparable gives in Comparable's place.
- `Class Boss Extends Enemy Implements Damageable, Shown`, or `Struct Money Implements Numeric`.
  Each of an Interface's members is implemented by one of the type's, its own or one it
  inherits, with the same name, kind, parameter types (ByRef included), and result, with the
  type for Self. A required `ByRef Method` is implemented by a Struct's ByRef Method or a Class's
  Method, and one that isn't ByRef by one that isn't. A Property is implemented by a Property of
  its type, which has a Set if the Interface's does. What implements a member doesn't throw.
- A type that doesn't have a default's Method, its own or one it inherits, gets the default.
  Where its own declaration, or a Class's it extends, lists the Interface, the default is a
  Method of its own, which a Class extending it can override, and which is checked against what
  the Class inherits like a Method written there. Where an Extend lists it, the default is the
  Extend's, like the Extend's own Methods: a member only where the Extend's module is seen,
  hidden by a member of the type's own of its name, and overriding nothing, while the type
  implements the Interface everywhere. A Method it has that takes the
  default's parameters but gives another result is an error where the Interface is listed. An
  Interface with a default takes its own, never another Interface's of the same name and types
  that the type gets itself, so a type can get both: each Interface calls its own, and a call or
  an Override that could mean either is ambiguous, as is an Interface's member without a default
  that both would implement. The type's own Method implements both, and so does one it has from
  a Class it extends, or an instance from its generic type, whichever Interface's default it is,
  except that a default the Class it extends took gives way to the default of an Interface the
  Class's own declaration lists that redeclares it, which overrides it, as in Java. An
  Interface's member with the name and parameters of one of an Interface it extends gives what
  that one does. Another Interface's default, refining or not, can't take the place of an
  Interface's own default where it's seen less far than the type and that member, as a private
  Interface's `>` in place of Comparable's on a Public type would be: code that sees the type
  couldn't call what calls through Comparable run.
  A default an Extend with a Where gives is conditional, like the Extend's own Methods (see
  [07-generics.md](07-generics.md)). Two Interfaces' default `Operator ==`s, neither refining the
  other, make `==` on the type ambiguous wherever it's used, in a direct `==` and inside arrays,
  tuples, and Structs, which is an error at the type unless it has an `Operator ==` of its own.
- An Interface is a type of its own unless it needs Statics, Operators (but `[]` and `[]=`), or
  generic Methods, or uses Self in its members' types: those are only constraints (see
  [07-generics.md](07-generics.md)). Its values
  are references to objects of Classes that implement it, and calls on them run what the
  object's Class gives. A Class's object converts to the Interfaces its Class implements, and an
  Interface's to the ones it extends. `Is`, `As`, `As?`, and `Case x As I` test for an
  Interface, and from one for a Class.
- A Struct implements Interfaces for generic code: it's never an Interface's value, which would
  have to put it in an object.
- A Class extending one that implements an Interface implements it too, whether the other lists
  it or an Extend gives it, with a Where line too, unless the Interface is only a constraint,
  whose Self would be the other Class.
- An Interface's values have no `==`: `Equatable` uses Self, so it's only a constraint, and an
  Interface extending it is too. `RefEquals` tells whether two are the same object.
- An Interface can be generic, like `Iterator<T>` (see [07-generics.md](07-generics.md)).
- An Interface can't be made, and has no Fields or News.
- Its Methods can throw: what implements one throws only what it can, or less. A call through an
  Interface's value needs Try, as any call that can throw does.
- Its Methods can be generic. A generic default runs as written wherever it's called: on a
  type, on a type parameter, and on an Interface's value, whose Self it is then, so its calls on
  Self go through the value's object. A generic Method without a default can't be in a table,
  since each set of type arguments makes a Method of its own, so an Interface that needs one, or
  extends one that does, is only a constraint. What implements it has as many type parameters,
  and constrains them no more.
- A default's Where line can constrain the Interface's type parameters, which makes it
  conditional: `Method Sum(): T` with `Where T: Numeric` on `Iterator<T>` is only where T is
  Numeric, and a type whose instance can't meet it doesn't have it. A default can also be
  `Final`. A generic, conditional, or Final default isn't in the Interface's table: a call
  through its value runs the default with the value for Self, so a type can't have a Method of
  its own in its place, and a Class can't override the copy it has.

## Properties

```vb
Class Enemy
    Field hp: Int

    Property Alive: Bool
        Get => hp > 0
    End Property

    Property Health: Int
        Get => hp
        Set(value)
            hp = Clamp(value, 0, maxHp)
        End Set
    End Property
End Class
```

- A Property has a Get, `Get => value` or a block ending in `End Get`, and maybe a Set, whose
  parameter takes the value. Each is a Method, and its name is the Property's.
- Reading `e.Health` calls the Get. `e.Health = 5`, `e.Health += 5`, and `e.Health++` call the
  Set, after the Get for the last two. One without a Set can't be assigned.
- In a Struct, the Get can't change Self and the Set can, like a ByRef Method. A Property of a
  Struct type gives a copy, so changing part of it, as in `e.Pos.x = 1`, is an error.
- A Property can't be passed `ByRef`, since it has no variable.
- A Class's Properties are virtual, like its Methods: `Override Property` replaces the Get, and
  the Set if it has one.
- An `Abstract Property` is only its first line, in an Abstract Class. A Class extending it
  overrides its Get, and its Set unless it's `Abstract ReadOnly Property`.
- A `Static Property` belongs to its type, like a Static Field: `Enemy.Count`. Its Get and Set
  have no Self.

### ReadOnly fields

`ReadOnly Field id: Int` can only change in its own Class's `New`, through Self, by name or as
`Self.id`: not later, and not in a Class that extends it. Its parts can't change either, and it
can't be passed `ByRef` outside that `New`. A Global that never changes is a `Const`.

## Statics

```vb
Class Enemy
    Static Field count: Int = 0
    Static Method Spawn(name: String): Enemy
        count++
        Return Enemy(name)
    End Method
End Class

Enemy.Spawn("grunt")
```

- A Static Field is one variable for the type, which starts with its value when the program
  does, in source order with the Globals. A Static Method has no Self.
- Both are used through the type, like `Enemy.count`, or by name in the type's own Methods. A
  Static Method can't use the other members by name, since it has no object.
- Struct and Class both have them. They aren't virtual, so they can't be Abstract, Final, or
  Override. A `Static ReadOnly Field` keeps the value it starts with.
- A generic Struct or Class has its Statics for each instance: `Box<Int>.count` and
  `Box<String>.count` are two variables, and by name in its own code, they're the instance's. A
  generic type's Static Field starts with a value known at compile time, or its zero.

## Operator overloading

```vb
Struct Money
    Field cents: Long
    Operator +(a: Money, b: Money): Money => Money(cents: a.cents + b.cents)
    Operator ==(a: Money, b: Money): Bool => a.cents == b.cents
End Struct
```

- A Struct or Class can define `+ - * / % ** & | ^ << >>` and `== < > <= >=`, and `-`, `!`, and
  `~` with one parameter, and `[]` and `[]=`, which index (see below). `&&`, `||`, `??`, `? :`,
  `Is`, and `As` mean what they always do.
- `!=` is always the opposite of `==`, so it's never defined on its own.
- An Operator is Static, has no Self, and takes a value of its own type as one of its
  parameters. A comparison gives a `Bool`. Its parameters aren't `ByRef` and have no defaults,
  it has no type parameters of its own, and it doesn't throw.
- `a + b` calls the Operator `+` that `a`'s or `b`'s type defines, or inherits from a Class it
  extends, chosen among them like among a function's overloads; a literal takes the type of the
  parameter it's passed for. Comparing with `Null` is never an Operator.
- `x op= y` assigns what `x op y` gives, which must fit in `x`.
- A type with an `Operator ==` uses it for every `==` on its values: in a Struct's or tuple's
  field-wise `==`, a `T[]`'s `Contains`, and between `T?` values. `RefEquals` still compares
  objects themselves.
- A generic type's Operators take its type arguments from their operands.

### Indexing

```vb
Struct Grid
    Field Cells: Int[]
    Field Width: Int

    Operator [](x: Int, y: Int): Int => Cells[y * Width + x]

    Operator []=(x: Int, y: Int, value: Int)
        Cells[y * Width + x] = value
    End Operator
End Struct

grid[1, 2] = 5
grid[0, 0] += grid[1, 2]
```

- `Operator [](indexes): T` reads an element, and `Operator []=(indexes, value)` sets one, with
  an index or more. Unlike the other Operators, they're Methods of Self, written without the
  object: a Struct's `[]=` changes it in place, as a ByRef Method does, so it's called only on
  what can change, and a Class's can be overridden like any Method. An Interface can require
  them and still have values, and a type parameter it constrains indexes through them. They
  overload by their indexes' types, their parameters aren't ByRef, variadic, or defaulted, they
  have no type parameters of their own, and they don't throw.
- `x[i] op= v`, `x[i]++`, and `x[i]--` read the element through `[]` and set what that gives
  through `[]=`, with the object and the indexes worked out once, the object first. Assigning
  needs `[]=`, and a tuple's element can't be set through it.
- What `[]` gives is a copy: setting a Field of a Struct it gives, passing it ByRef, or calling a
  ByRef Method on it is an error, as for a Property. `Super[i]` calls the Class it extends'.

## Extend

Adds methods or interface conformances to an existing type, including built-ins.

```vb
Extend Vec3 Implements Printable
    Method ToString(): String => "({x}, {y}, {z})"
End Extend

Extend Int
    Method Doubled(): Int => Self * 2
    Static Method Parse(text: String): Int => ...
End Extend

Extend T[]                          // every array
    Method Last(): T => Self[Count - 1]
End Extend
```

- An Extend adds to a Struct, Class, Enum, or built-in type, to a generic Struct or Class
  written with its own type parameters (`Extend Box<T>`), to one instance (`Extend Box<Int>`),
  to every array (`Extend T[]`), or to one array type (`Extend Int[]`). In an Extend of one
  instance, the members have the instance's types: a `Box<Int>`'s field `Value: T` is an Int.
  A Class extending that instance, like `Class Names Extends Box<Int>`, has them too, and an
  Operator one adds beats a generic one the type has for all its instances, in generic code too,
  where the instance implements the type's Interfaces with it. Its `==` is what `==` means for
  the instance everywhere, inside the Structs, arrays, and tuples that hold one. The type's own
  members, with the instance's types, implement what an Extend of the instance lists.
- It holds Methods, Static Methods, Properties, and Operators, and adds them to the type, as
  if the type had declared them. A `ByRef Method` changes a value; a Class's or array's Methods
  can always change it, so theirs aren't ByRef. Only a Struct or Class gets Operators, and an
  array gets no Statics, since an array type can't come before a '.'.
- It can't add Fields, a New, or a Destroy: a type's data and how it's made are the type's own.
- Its members use the type's own by name, as a type's Methods do, like an array's `Count` and
  `RemoveAt` in an `Extend T[]`.
- What it adds isn't virtual. Nothing overrides it, it overrides nothing, and it can't have a
  member the type has already, or one an instance inherits from a Class its generic type
  extends. A built-in type's, Enum's, or array's can't share a name with the members it has
  already, like `ToString` or an array's `Add`.
- `Extend T Implements I` makes the type implement `I`, with members its Extends or its own
  declaration give. A built-in type, Enum, or array comes with the built-in Interfaces that fit
  it, so an Extend gives it others.
- An Extend only goes at the top level of a file.

Conditional extensions use `Where` (see [07-generics.md](07-generics.md)).

## Enums

```vb
Enum State: UInt8
    Idle, Walk, Attack = 10, Dead   // integers auto-increment (Dead = 11)
End Enum

@Flags Enum Layer                   // values 1, 2, 4, 8...
    World, Player, Trigger
End Enum

Local mask = Layer.World | Layer.Player
```

- Optional underlying type after `:` (default `Int`), which must be an integer.
- Members are separated by commas or lines. A member without a value takes the next integer
  after the one above, starting at 0. In a `@Flags` Enum, it takes the next bit after every
  value so far, starting at 1. A value is known at compile time and can use the members above
  it by name, as integers: `Solid = World | Player`. Names and values don't repeat.
- `State.Walk` is a member. Values compare with `==` and `!=`; a `@Flags` Enum's also combine
  with `|`, `&`, `^`, and `~`, and `mask.Has(flags)` tells whether all of `flags` are set.
- `As` turns an Enum into any integer type, and an integer into an Enum. For an Enum without
  `@Flags`, the integer must be one of its values: known ones are checked at compile time, and
  others panic.
- An Enum's zero is its first member, or no flags for a `@Flags` Enum.
- A string shows a member's name. A `@Flags` value that isn't a member shows its flags,
  like `World | Player`.

## Visibility

| Modifier | Visible to |
|---|---|
| `Private` (default) | the declaring file |
| `Protected` | the declaring file, and the code of the Classes extending its Class |
| `Package` | the whole package: the modules of its project.toml's package, or of the standard library; without a project.toml, a program's own files are its package |
| `Public` | everyone |

```vb
Public Class Enemy
    Protected Field hp: Int = 100       // Enemy's file, and the Classes extending Enemy
    Field cache: Int                    // Private: this file only

    Public New()
    End New

    Public Method Hurt(amount: Int)
        hp -= amount
    End Method
End Class
```

- A visibility goes on top-level Functions, Globals, Consts, Structs, Classes, Interfaces, Enums,
  and Attributes, on the Fields, Static Fields, Methods, Static Methods, Properties, and News of
  Structs and Classes, and on the Methods, Static Methods, and Properties an Extend adds. It goes
  among a declaration's other modifiers, in any order, after its attributes: `@Inline Public Static
  Method`. Members are Private too unless they say otherwise. A Local, or a Const in a function's
  body or another block, is only its own code's, so it takes none.
- `Protected` only goes on a Class's members. The code of a Class extending it, in any file, uses
  one through `Self`, `Super`, or an object of its own Class or one extending it, not through an
  `Enemy` that may be another Class's, and a Protected Static member through any. A Protected
  `New` only starts its News, with `Super.New`, or is one a Class without a New takes, which is
  Protected too. An Extend's code isn't the Class's own.
- The standard library is one package. A program without a `project.toml` is another: its main
  file and the files it imports by path.
- Enum members, Interface members, and Operators have their type's visibility, and take no modifier.
  An Abstract member has its Class's unless it says otherwise, since it's what the Classes extending
  it have to give, as an Interface's member is; a Class that can't use one, like another file's
  Private one, has to be Abstract itself. A default an Interface gives a type has the Interface's,
  so code that can't name the Interface can't call it, even on a Public type an Extend gave it to. A
  default Operator does too, except `==`, which is what `==` means for the type everywhere. What
  implements an Interface's member can be Private: code anywhere calls it through the Interface, as
  a value of it or a type parameter it constrains.
- An `Override` is seen wherever what it overrides is, a default an Interface gives included: a
  Public one's is Public, a Package one's Package or Public, and a Protected one's Protected or
  Public, since a Class extending it in another package sees a Protected member but not a Package
  one, and one that doesn't say its visibility has what it overrides'. A Method that a Class's code
  can't use, like another file's Private one, isn't one it can override, or declare again.
- A Struct's own constructor is as visible as its least visible Field, so a Private Field keeps
  others from calling it. Its zero is still everyone's, in Fields, Globals, and arrays, so a type
  whose values have to come from its own code makes its zero mean nothing, as a Pool's `Handle`
  does: generations start at 1, so a zero Handle finds no value. A Class's own `New` is as visible
  as the Class, and the News it takes from the Class it extends as the less visible of the Class and
  that New, so a Private one stays its file's; a written `New` is Private unless it says otherwise.
- What a Public or Package declaration shows is seen at least as far: a function's parameter,
  result, Throws, and constraint types, a Global's, Const's, Field's, or Property's type, and a
  type's base, Interfaces, and constraints. A member is seen no further than its type.
- What code can't use hides nothing and clashes with nothing: another file's Private `Node` isn't
  ambiguous beside a third's, a Private Method isn't among a call's overloads, and a Class
  extending `Queue<T>`, or an Extend of it, can have a member named like its Private `Head`;
  only a Class's Method that takes a hidden Method's parameters, or its Property named like a
  hidden Property, is an error, as above. Where code can use both, a Class's member hides what
  the Class it extends has of its name, Methods with other parameters aside, and a type's own
  member hides what an Extend adds in its place. Using one that code can't is an error that
  says where it's declared: "'Entries' is private to std/collections/map.fb".
- Visibility is checked where code is written: a Public generic function uses its file's Private
  helpers for every type it's called with. `For Field`, `For Method`, and `For Property` go over
  every member, Private ones too, but what an Extend adds only where the loop's file sees the
  Extend's module, as a call there would (see [13-compile-time.md](13-compile-time.md)).

## Equality

| Kind | `==` means |
|---|---|
| Struct | its Operator `==` if it has one, otherwise field-by-field comparison |
| Class | its Operator `==` if it has one, otherwise reference identity |
| Array | element-by-element comparison, when the elements have `==` |
| Interface | nothing: an Interface's objects have no `==`, since their Classes' could differ |

`RefEquals(a, b)` always tests reference identity. `Equatable` makes `==` work in generic code
(see [07-generics.md](07-generics.md)).
