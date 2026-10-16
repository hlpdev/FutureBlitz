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

## Operator overloading

```vb
Struct Money
    Field cents: Long
    Operator +(a: Money, b: Money): Money => Money(cents: a.cents + b.cents)
    Operator ==(a: Money, b: Money): Bool => a.cents == b.cents
End Struct
```

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

- Optional underlying type after `:` (default `Int`).

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
