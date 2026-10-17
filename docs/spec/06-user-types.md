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
  be called on something assignable: a `Local`, a `Global`, a `ByRef` parameter, or a field or
  element of one.

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
- Every Class's `Destroy` runs, the most derived first, so `Destroy` is never `Override`.
- An object converts to any Class its Class extends. `x Is T` tests its Class at run time,
  `x As T` panics when it isn't a `T` ("expected Boss, found Grunt"), and `x As? T` gives `Null`
  instead. Classes that aren't related are an error with each.
- A Struct can't extend anything. Interfaces come in M3.

## Interfaces

```vb
Interface Damageable
    Method Hurt(amount: Int)

    Method Kill()                   // default implementation
        Hurt(1_000_000)
    End Method
End Interface
```

- Interfaces may provide **default method bodies**.
- Interfaces may require static members, operators, and use `Self` as a type
  (see [07-generics.md](07-generics.md)).
- Structs can implement interfaces too.

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
  the Set if it has one. They can't be Static or Abstract yet.

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

## Operator overloading

```vb
Struct Money
    Field cents: Long
    Operator +(a: Money, b: Money): Money => Money(cents: a.cents + b.cents)
    Operator ==(a: Money, b: Money): Bool => a.cents == b.cents
End Struct
```

- A Struct or Class can define `+ - * / % ** & | ^ << >>` and `== < > <= >=`, and `-`, `!`, and
  `~` with one parameter. `&&`, `||`, `??`, `? :`, `Is`, and `As` mean what they always do.
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

## Extend

Adds methods or interface conformances to an existing type, including built-ins.

```vb
Extend Vec3 Implements Printable
    Method ToString(): String => "({x}, {y}, {z})"
End Extend
```

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
| `Protected` | the declaring class and subclasses |
| `Package` | the whole package |
| `Public` | everyone |

## Equality

| Kind | `==` means |
|---|---|
| Struct | field-by-field comparison, generated automatically |
| Class | `Equatable` implementation if present, otherwise reference identity |

`RefEquals(a, b)` always tests reference identity.
