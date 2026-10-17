# 08 · Memory Model and Unsafe Code

## Value vs reference

| Kind | Semantics |
|---|---|
| Primitives, `Struct`, tuples, fixed arrays `T[N]` | **values**: copied on assignment |
| `Class`, `String`, dynamic arrays `T[]`, collections | **references**: shared; copy explicitly with `.Clone()` |

## Automatic reference counting

- Class instances are **refcounted**. Cleanup is deterministic: no GC pauses mid-frame.
- There is no `Delete`. An object is freed when its last strong reference goes away.
- `Destroy()` (if defined) runs at that moment. See [06](06-user-types.md#class-reference-type).
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
