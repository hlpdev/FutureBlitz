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
