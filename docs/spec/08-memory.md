# 08 · Memory Model and Unsafe Code

## Value vs reference

| Kind | Semantics |
|---|---|
| Primitives, `Struct`, tuples, fixed arrays `T[N]` | **values**: copied on assignment |
| `Class`, Interface values, `String`, dynamic arrays `T[]`, collections | **references**: shared; copy explicitly with `.Clone()` |

## Automatic reference counting

- Class instances are **refcounted**. Cleanup is deterministic: no GC pauses mid-frame.
- There is no `Delete`. An object is freed when its last strong reference goes away.
- `Destroy()` (if defined) runs at that moment. See [06](06-user-types.md#class-reference-type).
- An object whose last reference was one of another object's fields goes once that object's
  fields are all released, then what it held goes, and so on, one object at a time, so letting
  go of a chain of any length never runs out of stack. The `Destroy()`s run in the same order
  as ever: an object's, then its first field's object's and what that held, then its second's.
  What a `Destroy()` itself lets go of goes at once.
- Normal objects use **non-atomic** refcounts (fast). Only `Shared Class` instances use
  atomic refcounts (see [10-concurrency.md](10-concurrency.md)).

### Cycles

- Break cycles with `Weak<T>` references, or capture lists in closures
  (see [05](05-functions.md#capture-lists)).
- Debug builds include a **cycle detector** that reports leaked cycles.

```vb
Class Node
    Field children: Node[]
    Field parent: Weak<Node>     // doesn't keep the parent alive
End Class
```

A `Weak<T>` is assigned a `T`. Reading it gives a `T?`, which is `Null` once the object is gone,
so `node.parent?.name` works directly.

- The `T` is a Class. A Weak is assigned a `T`, a Class that extends it, a `T?`, or `Null`, which
  refers to nothing. That's also its zero, so a field of one needs no `New`.
- What reading it gives holds a reference of its own, so the object stays while it's used.
- Its object can go at any time, so a null check doesn't narrow a Weak. Read it into a Local,
  and check that.
- A Struct that holds a Weak has no `==`, which would compare the references.
- When an object's last strong reference goes, its `Destroy()` runs and its fields are released
  at once. Its memory waits for the last Weak to it.

### Leak reports

A program that ends with objects or Strings still alive prints how many of each, by Class:
`leak: 2 Node objects were never released`. Debug builds keep track of every object, so the
report also points out each cycle among them, field by field:

```text
leak: a cycle of 2 objects: Node.next -> Node.next -> the first Node
leak: a cycle of 3 objects: Tree.children -> Tree[] element -> Tree.parent -> the first Tree
```

Objects that only a cycle keeps alive are in the counts, not the cycles.

## Engine resources

Entities, textures, and other engine-owned resources keep an explicit `Free()`, because the
scene owns them. Handles are **generational**, so using a freed resource raises a clear
error instead of crashing.

## Pointers and unsafe code

Normal code never needs pointers. They exist for FFI and low-level work.

```vb
Unsafe
    Local buf: Ptr<UInt8> = Mem.Alloc(1024)
    Defer Mem.Free(buf)
    buf[0] = 255
    Local p = AddressOf(particle)      // Ptr<Particle>
    p.Value.life = 1.0
End Unsafe

Unsafe Function Blit(dst: Ptr<UInt8>, src: Ptr<UInt8>, n: Int)
    ...
End Function
```

- Dereferencing, pointer arithmetic, `AddressOf`, and raw memory (`Mem.*`) are only
  allowed inside `Unsafe` blocks or `Unsafe Function`s.
- `Ptr<T>` values can be stored and passed around in safe code; they just can't be
  dereferenced there.
- A `Ptr<T>` points at a value C code has: a number, `Bool`, Enum, `CString`, `Ptr`, Callback, or a
  fixed array or Struct of these, which holds no references; `Ptr<Void>` points at anything, as C's
  `void*`, and `Void` is a type nowhere else. A type parameter can't be the T yet.
- `Null` is a Ptr's zero, C's NULL; `p == Null` and `p == q` compare addresses, in safe code too,
  and `Ptr<T>?` isn't a type you write, since a Ptr can be Null already, so `??` and `?.` don't
  take a Ptr, which is compared with Null instead. A generic's `T?` of a Ptr, like what a
  `Map<Int, Ptr<Node>>`'s `Get` gives, is a T? all the same, whose Null is "none", apart from a
  Ptr it holds that's Null. A `Ptr<Void>` converts to and from any `Ptr<T>` without `As`. A Ptr
  isn't Sendable, or shown in a string; `p As UInt64` is its address.
- What a Ptr points at isn't part of it, so a Struct can point at its own Struct, as a linked
  list's nodes do.
- Only Unsafe code reads or sets `p.Value`, what a Ptr points at, and its parts, like
  `p.Value.life`; indexes it, `p[i]`, with any integer, which can be negative; steps it,
  `p + n` and `p - n`, by T's size, or counts the T's between two of one type, `p - q`, an
  `Int64`, when a T takes some bytes; converts it with `As` to another Ptr, to and from `Int64`
  or `UInt64`, or between a `CString` and a `Ptr<UInt8>`, `Ptr<Int8>`, or `Ptr<Void>`; takes
  `AddressOf`; reads a CString's text with `ToString()`; calls a Callback (see
  [12](12-ffi.md#callbacks)); or calls an Unsafe Function or Method, like `Mem`'s. A `Ptr<Void>`
  has no Value, indexes, or steps.
- A debug build panics where a Null Ptr's Value is used or it's indexed, "the Ptr is Null", and
  where a Null Callback is called, "the Callback is Null".
  `ToString()` on a Null CString panics in every build.
- `AddressOf(x)` takes a Local, ByRef parameter, Global, field, element of a `T[]` or fixed
  array, or a Struct's Self, and gives a `Ptr<T>` that's good while what it points at is there: until the
  Local's block ends, the object goes, or the `T[]` changes size. It can change what it points
  at, so it takes only what code could change there: not a loop variable, a ReadOnly Field or
  Static Field, or a part of a value the statement makes, like `Make().x`; not Self in a lambda
  or a `Parallel For`, which has a copy of its own; and in a `Parallel For`, nothing from outside
  but an outside array's element at the pass's own index. A place through a Ptr, like `p.Value`
  or `p[i]`, is reached before what comes after it is evaluated, as one in a `T[]` is (see
  [03](03-operators.md)), and it can go ByRef, since what a Ptr points at doesn't move, two of
  them in one call too, since C's memory can be shared.
- `SizeOf<T>()` is how many bytes a T takes in memory, as C lays it out, an `Int64` known at
  compile time, for the same types a Ptr points at, whichever comes first in the code: a Struct
  or Enum declared after it is measured whole. A T of more bytes than an Int64 counts, or whose
  size depends on itself, like a Struct with a Field of `Int8[SizeOf<T>()]`, is an error, and a
  Ptr at one doesn't step.
- An `Unsafe Function` or `Unsafe Method` (or `Unsafe Static Method`) is one that only Unsafe
  code calls. It isn't a function value, and it can't be an Operator, Destroy, a Const Function,
  or an Interface's Method, or implement one, or override a Method that isn't Unsafe, since code
  calls those without saying so. A `For ... In` that calls an Unsafe `Iterate()` is in Unsafe
  code, and reflection's `For Method` leaves Unsafe Methods out. An Unsafe Function's defaults
  are its Unsafe code. A lambda made in Unsafe code is Unsafe code too, and safe code can call
  it.
- A Const Function has no Unsafe code, since it runs in the compiler.
- `std.ffi`'s `Mem` has Unsafe Static Methods over the C runtime: `Alloc(bytes)` gives that much
  zeroed memory; `Resize(memory, bytes)` gives what Alloc or Resize gave, or new memory for
  `Null`, in that much memory, keeping what fits, with what it adds not zeroed; `Free(memory)`
  gives it back; `Copy(destination, source, bytes)`, which can overlap; `Fill(destination,
  value, bytes)`; and `Compare(a, b, bytes)`, which gives 0 for the same bytes, or -1 or 1 as
  the first byte that differs is lower at `a` or `b`. Sizes are Int64s, like SizeOf's, a
  Count, and `p - q`, so one that went below 0 panics instead of wrapping around, and Alloc
  and Resize panic when there isn't that much memory. `Mem` itself isn't made.
- `std.ffi` gives a CString more than its Unsafe `ToString()`: `CString.From(text)` makes a
  NUL-terminated copy of a String's UTF-8 that C code can keep, in safe code; `Length()` counts
  its bytes before the first NUL, panicking on `Null`; and `Free()` gives back what From made,
  panicking for any other CString, or one given back already. The copy is the runtime's, so
  leak reports count one that's never freed.
