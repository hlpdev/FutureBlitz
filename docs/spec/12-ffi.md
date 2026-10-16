# 12 · FFI (Foreign Function Interface)

FFI declarations live **in source**, in `Extern` blocks. No separate `.decls` files.

## Declaring C functions

```vb
Extern "C" Lib "joltc"
    Function CreateBody(shape: Ptr<Void>, x: Float, y: Float, z: Float): UInt32 = "JPH_CreateBody"
    Function SetLogger(cb: Callback(CString))
End Extern
```

- `Lib "name"` refers to a library declared in `project.toml` `[native]`.
- `= "symbol"` maps to the real C symbol name (optional when names match).
- Calling conventions: `"C"` (default) and `"stdcall"`.

## C-layout structs

```vb
@CLayout Struct JPH_Vec3          // memory layout guaranteed to match C
    Field x: Float
    Field y: Float
    Field z: Float
End Struct
```

## Type mapping

| FutureBlitz | C |
|---|---|
| `Int8`–`Int64`, `UInt8`–`UInt64` | `int8_t`–`int64_t`, `uint8_t`–`uint64_t` |
| `Float32`, `Float64` | `float`, `double` |
| `Bool` | `bool` |
| `Ptr<T>` | `T*` |
| `Ptr<Void>` | `void*` |
| `CString` | `const char*` |
| `Callback(A, B): R` | function pointer |
| `@CLayout Struct` | struct |

## Strings

A `String` argument auto-converts to `CString` for the duration of the call (temporary
null-terminated buffer).

## Callbacks

`Callback(...)` is a raw C function pointer. Only **non-capturing** functions may be
passed; closures are rejected at compile time.

## Safety

- Calling FFI functions is allowed in safe code.
- Dereferencing pointers returned from C requires `Unsafe` (see [08](08-memory.md)).

## Bindgen

`bindgen joltc.h` reads a C header (via libclang) and generates an `Extern` module
automatically. This is the main way C libraries get used.
