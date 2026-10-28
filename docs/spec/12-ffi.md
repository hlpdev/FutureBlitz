# 12 · FFI (Foreign Function Interface)

FFI declarations live **in source**, in `Extern` blocks. No separate `.decls` files.

## Declaring C functions

```vb
Extern "C" Lib "joltc"
    Function CreateBody(shape: Ptr<Void>, x: Float, y: Float, z: Float): UInt32 = "JPH_CreateBody"
    Function SetLogger(cb: Callback(CString))
End Extern
```

- `Lib "name"` names one of the `[native]` libraries in the project.toml of the package whose
  module declares it (see [11](11-modules.md#packages)), which its symbols are in. Without one,
  they come from the C runtime and the system libraries every program links. A program links
  every `[native]` library of its packages, so `Lib` says where a symbol is, and a C symbol that
  no library defines is an error at the first Extern Function that declares it, saying where it
  should have been; the linker's other errors are shown as it says them.
- `= "symbol"` maps to the real C symbol name (optional when names match).
- Calling conventions: `"C"`, the default, and `"runtime"`. FutureBlitz's targets are all
  64-bit, where C has one convention, so there's no `"stdcall"`. `"runtime"` is the runtime's own
  functions, which only the standard library calls, with values passed as compiled code holds
  them: a String or an array as the runtime's, borrowed for the call, and a String it gives
  owned by the caller. One that gives a `Task<T>` is passed a new Task first, which the runtime
  finishes once the result is in it, so `InputAsync` is one call of `fb_io_input_async`. Print is
  std.core's Function over the runtime's `fb_write`, which takes the values' texts as a
  `String[]`. A program that calls the runtime's functions over a bundled C library, like the
  `fb_io_` ones over libuv, links that library, and only then.
- An Extern block goes at a file's top level and holds only Functions, each its first line
  alone. It takes only a visibility, like `Public Extern`, which its Functions take unless they
  have their own, as Private ones do.
- An Extern Function is called from safe code, and is a Function value like any other, unless
  it's an `Unsafe Function`, which only Unsafe code calls, as for a C function whose misuse breaks
  memory whatever it's given. It has only what C has: no type parameters, Throws, Async,
  defaults, or C varargs, and it isn't Const or `@Inline`, so a Const Function can't call one.
- One C symbol has one signature, however many Extern Functions declare it, in one module or
  several, under other names: they must take and give the same types, where an Enum is its
  integer type and every pointer is one, whatever it points at: a Ptr, a CString, a ByRef
  parameter, and a String or String? it gives. A symbol the program defines itself, `main` or one
  starting with `fb_` or `fb.`, or one of LLVM's, starting with `llvm.`, can't be declared.

## C-layout structs

```vb
@CLayout Struct JPH_Vec3          // memory layout guaranteed to match C
    Field x: Float
    Field y: Float
    Field z: Float
End Struct
```

- `@CLayout` goes on a Struct without type parameters, so never on a generic Struct's instance,
  with a Field at least, whose Fields are numbers, `Bool` (a byte, 0 or 1), Enums, `CString`,
  `Ptr`, other `@CLayout` Structs, and fixed arrays of these. Its Fields are where C puts them:
  each at its own alignment after the last, and its size a multiple of its largest Field's, which
  `SizeOf` gives. It keeps a Struct's constructor, zero, `==`, and Methods.
- A Struct that C code sees is `@CLayout`: one an Extern Function takes or gives by value, takes
  `ByRef`, or reaches through a `Ptr` or a fixed array.
- By value, a Struct goes as the target's C ABI passes it, as clang does. On Windows, one of 1,
  2, 4, or 8 bytes goes as an integer, floats or not, and any other as a pointer to a copy the
  caller makes, or for a result, memory the caller passes. On x86-64 elsewhere (System V), one
  over 16 bytes goes in memory; a smaller one goes in up to two registers, each eightbyte in an
  integer register when anything in it is an integer, or else in an SSE one, and in memory when
  the registers that are left don't take it. On AArch64 (Linux, Windows, and macOS), one of one
  to four `Float`s, or one to four `Double`s, and nothing else goes in that many floating-point
  registers; any other of 16 bytes or less goes in one or two integer registers; and a bigger one
  goes as a pointer to a copy the caller makes, or for a result, memory the caller passes. Other
  targets don't take Structs by value yet, and neither does any target take one of 4 GiB or more;
  a `Ptr` to one works everywhere.

## CData

`CData`, std.ffi's, is what C code has, which the compiler decides, as it does Sendable: numbers,
`Bool`, Enums, `CString`, Ptrs, Callbacks, `@CLayout` Structs, and fixed arrays of these. A type
parameter with it can be a Ptr's T, and SizeOf's, which measures each instance as it's made, so
generic code hands C memory of any C type:

```vb
Import std.ffi

Unsafe Function Fill<T: CData>(count: Int64, value: T): Ptr<T>
    Local items: Ptr<T> = Mem.Alloc(count * SizeOf<T>())
    For i = 0 Until count
        items[i] = value
    End For

    Return items
End Function
```

- It's only a constraint. A type argument that isn't C data is an error saying why: a Struct is
  CData only when it's `@CLayout`, so a generic Struct's instance never is. A Struct that lists
  it, or an Interface that extends it, is checked for it, as for Sendable.
- A Ptr at a CData T steps and counts by T's size, which is a byte at least.

## Buffers

std.ffi's `Buffer<T: CData>` is native memory of T's at an address that doesn't move, which C
code reads and fills through its Pointer and safe code reads and sets with `[]`, so they share it
without copying:

```vb
Import std.ffi

Extern "C" Lib "vecmath"
    Function vm_fill(values: Ptr<Int32>, count: Int32, value: Int32)
End Extern

Local values = Buffer<Int32>(4)
Unsafe
    vm_fill(values.Pointer(), 4, 7)
End Unsafe

Print("{values[0]} {values.Slice(1, 3).ToArray()}")
```

- `Buffer<T>(count)` is a Class with memory of its own for `count` T's, all 0, which it gives
  back once nothing has it. Its `[]` and `[]=` check each index, an Int64, against its `Count`,
  in every build, and `For ... In` goes over its T's. It isn't Sendable, as a Ptr isn't.
- `Slice(start, finish)` is a view of its T's from start up to finish, without the one at finish:
  a Buffer of the same memory, which sees and makes changes in it, and keeps the Buffer it's part
  of alive, so the memory stays after that Buffer's last variable goes, until its last view goes
  too. A view of a view is part of the first Buffer.
- `CopyTo(destination)` copies its T's to the start of another Buffer, which can share memory
  with it, `ToArray()` gives them in a new array, and `Buffer<T>.From(items)` makes a new one of
  an array's. std.io's streams read into Buffers and write from them.
- Only Unsafe code gets its `Pointer()`, which is good while the Buffer, or a view that keeps it
  alive, is, and makes `Buffer<T>.View(pointer, count)`, a view of memory C owns, on the promise
  that C keeps it there while the view is; nothing gives that memory back.

## Type mapping

| FutureBlitz | C |
|---|---|
| `Int8`–`Int64`, `UInt8`–`UInt64` | `int8_t`–`int64_t`, `uint8_t`–`uint64_t` |
| `CLong`, `CULong`, `CWChar` | `long`, `unsigned long`, `wchar_t`, the target's sizes (see [02](02-types.md#built-in-types)) |
| `Float32`, `Float64` | `float`, `double` |
| `Bool` | `bool` |
| `Ptr<T>` | `T*` |
| `Ptr<Void>` | `void*` |
| `CString` | `const char*` |
| `Callback(A, B): R` | function pointer, `R (*)(A, B)` |
| `@CLayout Struct` | struct, by value or through a `Ptr` |

- An Enum goes as its integer type, and one C gives back, as a result or through ByRef, or in a
  Struct it gives or sets ByRef, in its Fields or their fixed arrays, has to be one of its
  values, unless it's `@Flags` or `@Open`, or the program panics at the call, as `As` does, saying where in the Struct it is. One
  that C writes through a Ptr later isn't checked; Unsafe code reads what C put there. Values
  go as the target's C ABI passes them, as clang does: a Bool as a byte that's 0 or 1, and on
  x86-64 outside Windows and on Apple's targets, an integer narrower than 32 bits extended to 32
  bits. A Bool that C gives is its `bool`, 0 or 1: a byte of another value is undefined, as it
  is in C, so what reads it can see either.
- A `Char` isn't C's: C's `char32_t` is a `UInt32`, which `As Char` checks on the way in, since C
  can hold any number there.
- A `ByRef` parameter of one of these passes the variable's address, so an out-parameter like
  `frexp`'s `int*` needs no Unsafe: `Function frexp(x: Float64, ByRef exponent: Int32): Float64`.
- A `CString` is stored and passed in safe code, and its zero is `Null`, C's NULL, which
  `cs == Null` tests. Unsafe code reads its text with `cs.ToString()`, a copy with U+FFFD for each
  byte that doesn't start well-formed UTF-8, which panics for Null, and converts it to and from a
  `Ptr<UInt8>` with `As`. It isn't Printable or Equatable, since only Unsafe code reads
  its text, or Sendable, since nothing keeps C's memory for another thread.
- A parameter that C takes text through is a `CString`, not a `String`. A function that gives
  text can say `String`, or `String?` where it can give NULL: C's `const char*` is copied into a
  new String right after the call, Null for NULL, and a panic at the call for a `String`, as in
  `Function getenv(name: CString): String?`. Each byte that doesn't start well-formed UTF-8, like
  a Windows code page's `é`, becomes U+FFFD.
- A Task's work can't give a CString or a Ptr, or anything holding one, since they point at
  memory that nothing keeps for another thread.

## Strings

A `String` argument auto-converts to `CString` for the duration of the call (temporary
null-terminated buffer): a copy of its text with a NUL after it, made right before the call and
freed right after it, which C reads up to its first NUL and mustn't keep. Leak reports count copies
that were never freed. Only an Extern Function's `CString` parameter takes a String, and only for a
Function that gives, sets ByRef, or writes through a Ptr parameter nothing that could point into
text: a CString, a Ptr to `Int8`s or `UInt8`s, C's `char*`, or a Struct or Ptr holding one, wherever
its type is declared. What it gives could point into the copy, as `strstr`'s, `strchr`'s, and
`strtol`'s end pointer do, so it takes only CStrings, like one that `CString.From(text)` makes,
until its `Free()` (see [08](08-memory.md#pointers-and-unsafe-code)). A `Ptr<Void>`, like an opaque
handle a C library gives, doesn't count, and neither does a buffer C fills, like a `Ptr<UInt8>`
parameter. Declared to give a `String?` instead, `strstr` works on Strings, since its result is
copied before the arguments' copies go.

## Callbacks

`Callback(...)` is a raw C function pointer. Only **non-capturing** functions may be
passed; closures are rejected at compile time.

```vb
Extern "C"
    Function qsort(base: Ptr<Void>, count: UInt64, size: UInt64,
                   compare: Callback(Ptr<Void>, Ptr<Void>): Int32)
End Extern

Function Ascending(a: Ptr<Void>, b: Ptr<Void>): Int32
    ...
End Function

Unsafe
    qsort(AddressOf(numbers[0]), 5, 4, Ascending)
End Unsafe
```

- `Callback(A, B): R` takes and gives what C can: numbers, `Bool`, Enums, `CString`, `Ptr`, and
  other Callbacks; a Struct goes through a `Ptr`. It's C data, so a `@CLayout` Struct's Field or
  a Ptr's T can be one, and it's Sendable.
- Where a Callback is expected, a Function's name gives its code by the C ABI: a top-level
  Function of exactly the Callback's types, after a module's qualifier too, or an Extern
  Function, but not one that's built in, generic, Async, Unsafe, or takes a parameter ByRef or
  throws, or a Method; two imported Functions that fit are ambiguous. So does a lambda of
  exactly its types that captures nothing, not even Self; what it needs can come through a
  `Ptr<Void>` that C passes along. An overload whose parameter is a Callback takes these too.
  Anything else is an error saying why, naming what a lambda captures.
- Its zero is `Null`, C's NULL, and `==` compares addresses: a Function's name or a lambda beside
  a Callback is one, as in `compare == Ascending`. An Extern Function's is C's own function, so
  it's the address C has for it too; a Function's or a lambda's is code that calls it, the same
  for each use. `Callback(...)?` isn't a type, and `??` and `?.`
  don't take one, as they don't take a Ptr; it isn't shown in a string.
- C can call a Callback on any thread, so its code is checked as work on another thread (see
  [10](10-concurrency.md)): it can't reach `@MainThread` code. It can't throw, and a panic in one
  ends the program as any panic does. An Enum C passes it has to be one of its values, as one C
  gives back does, or the program panics, where the Callback's Function or lambda is.
- Only Unsafe code calls a Callback from FutureBlitz: `compare(a, b)`, by the C ABI, with its
  arguments in order, without names or ByRef. An Enum a Callback gives has to be one of its
  values, or the program panics at the call, and a debug build panics on a Null Callback's
  call.

## Loading at run time

A program can load code it doesn't know when it's built: a C library, or a plugin written in
FutureBlitz.

```vb
Import std.ffi

Local lib = Try DynamicLibrary.Open("native/sqlite3")    // sqlite3.dll, libsqlite3.so, ...
Unsafe
    Local version = lib.Function<Callback(): CString>("sqlite3_libversion")
    If version != Null Then Print(version().ToString())
End Unsafe
lib.Close()

Local mod = Try Plugin.Load<GameMod>("mods/tanks")      // tanks.dll, libtanks.so, ...
mod.Start()
```

- `DynamicLibrary.Open(path)` loads a C library through the OS, from a path without the OS's
  prefix and extension, which it adds, or with them. One it can't load throws an `FfiError` of
  `NOT_FOUND`, or of `OTHER` with the OS's reason. Its Unsafe `Function<F>(name)`, where F is a
  Callback type, gives the symbol as an F, or Null when the library has none; nothing checks that
  the symbol takes and gives what F says, so calling it is Unsafe code's, as any Callback's is.
  `Close()` lets the library go; a Callback from it, called after, breaks memory, as a freed
  pointer's does.
- A **plugin** is a FutureBlitz library built against an interface package of its host: a
  library package with the Interfaces the host calls and the types they pass, which the host and
  each plugin depend on. The plugin is built beside, as its own `.dll`, `.so`, or `.dylib`, and
  uses its host's runtime, as a beside library does (see [11](11-modules.md#libraries)).
- A plugin has exactly one `@PluginEntry` Function, `Public`, taking nothing and giving one of the
  interface package's Interfaces, like `@PluginEntry Public Function Load(): GameMod`. A
  plugin without one, or with two, is an error, and so is the attribute on anything else.
- `Plugin.Load<T>(path)` loads the plugin at `path`, the OS's prefix and extension optional, checks
  that it was built against the host's build of the interface package that declares T, by its ABI
  hash, and that its entry Function gives a T, calls the entry, and gives what it gives. It throws
  a `PluginError`: `NOT_FOUND` for a file it can't load, `MISMATCH` for another build of the
  interface package, naming both builds, `NO_ENTRY` for a library without an entry or whose entry
  gives another type, and `OTHER` with the OS's reason.
- A plugin stays loaded until the program ends, since its objects, Methods, and Tasks may be
  anywhere in the program; loading one path twice gives the entry's value again without loading
  it twice.
- `FfiError` and `PluginError` are std.core's, beside IOError and the rest (see
  [stdlib.md](../stdlib.md)), so any code can catch them; `DynamicLibrary` and `Plugin` are
  std.ffi's.

## Safety

- Calling FFI functions is allowed in safe code, but not calling a Callback, or an Extern Function
  that's Unsafe.
- Dereferencing pointers returned from C requires `Unsafe` (see [08](08-memory.md)). An Extern
  Function takes and gives Ptrs, which C reads and writes through, and a `ByRef` Ptr parameter
  is C's `T**`.

## Bindgen

`fb bindgen joltc.h` reads a C header (via libclang) and generates an `Extern` module
automatically. This is the main way C libraries get used.

```
fb bindgen vecmath.h [-o vecmath.fb] [--lib vecmath] [--target <triple>] [--std <standard>]
                   [-I dir] [-D name[=value]]
```

- It runs fb-bindgen, a tool next to `fb`, built where libclang is, so `fb` itself never loads
  libclang. The module and the library its Extern block names are named after the header unless
  `-o` and `--lib` say otherwise.
- It reads the header as the C compiler that links programs does: for the target programs are
  built for, or `--target`'s, in that compiler's C standard with GNU's extensions, or
  `--std`'s, and with its version as GCC's `__GNUC__`, with its C library's include folders
  after `-I`'s and after clang's own headers, like `stddef.h`, `limits.h`, and the intrinsics,
  which stand in for the compiler's, so `long`, `wchar_t`, and enums have the library's sizes.
  A triple names the build's target however it's spelled (`x86_64-w64-mingw32` is
  `x86_64-w64-windows-gnu`). For another target than the build's, the C library's folders come
  through `-I` (its sysroot's), and clang's own headers, like `stdint.h`, stand on their own. A
  header that doesn't parse is an error, a line for each of clang's, and so is a target or C
  standard libclang doesn't know, which the error names.
- The module holds the header's own declarations, those its macros make too, not what it
  `#include`s, each `Public`, after a comment saying what it was generated from and for which
  target, in the header's order: a struct is a `@CLayout` Struct, after what it defines inside it,
  which C names as if it were outside, and one without a name that a field has is named after the
  struct and the field, like `holder_inner`; an enum is an Enum of its integer type, or Consts of
  that type when its values repeat or it's a flag enum (`__attribute__((flag_enum))`), whose values
  are combined, or of C's type for each constant when it has no name, and a macro that's a number a
  Const of the type and value C gives it after the header, as clang works it out: its last
  definition, which can use any macro, those an `#include` or `-D` defines too. A macro is a number
  when clang reads it as one without an error, a comma operator, what C leaves undefined, like an
  overflow or a negative shift, or a value that depends on where it's used, like `__LINE__`'s, and
  an integer is one only where C takes an integer constant expression too, as clang has them:
  `1 << 31` is one, and `INT_MIN / -1`, `-1 << 1`, and `(int)1e10` aren't. Its
  functions are one `Public Extern "C" Lib` block, after the rest, each where it's first declared,
  with the parameters of its last declaration that says what it takes, named by it, or by another
  declaration where it names none.
- Integers map by their size and sign, but for what's written as `long`, `unsigned long`, or
  `wchar_t`, which are `CLong`, `CULong`, and `CWChar` on every target, through a typedef too,
  though not one whose name says its size, like `int64_t`, `size_t`, or `intptr_t`, which are a
  `long` on some targets; a number macro of one of these, a wide character like `L'/'` too, is
  one where every target's holds its value, as `1L` is, and is of its size where it isn't, as
  Linux's `~0UL` is. A header's own `wchar_t` of another size or sign than the target's is of
  its size. An Enum keeps its integer type's size. `float` and `double` map to `Float32` and `Float64`, and
  `bool` to `Bool`. `const char*` is a `CString`, a function pointer a Callback, `void*` and a
  pointer to a struct that isn't one of the header's Structs, like an opaque handle, `Ptr<Void>`,
  and any other pointer a `Ptr<T>`, or a `Ptr<Void>` when `T` doesn't map; an array field
  `T a[2][3]` is a `T[2, 3]`, and an array parameter a pointer, as C passes it.
- C's names stay where they can. One that's a keyword or a type FutureBlitz writes, like `Bool`
  or `Ptr`, gets a `_`, a character a name can't have, like `$`, is a `_`, and a struct's or
  enum's name that a function, Const, or another struct or enum has gets a `_` (C keeps tags
  apart), as do names in one scope that are alike after that, while a C name that's a fine name
  already keeps it. A function whose name changed says its C symbol with `= "symbol"`. A
  parameter without a name is `arg1`, `arg2`, and so on.
- What can't map is a `// skipped:` comment where it was, saying why, and a warning: unions, bit
  fields, packed or aligned structs, flexible array members and arrays without elements,
  varargs, inline, and static functions, a function that doesn't say what it takes (`f()`
  before C23), functions and callbacks of a calling convention other than the target's C one,
  like `__vectorcall` or `__regcall`, C globals, `long double`, 128-bit integers, `main`
  and the `fb_` functions the program defines, function-like macros, macros that aren't numbers
  or that the header `#undef`s (where C reads it, not in an `#if` it leaves out), and whatever
  takes or holds one of these, like a function that takes a union. A macro that only names
  itself, as `#define X X` beside an enum's `X`, an include guard, and a flag stand for nothing,
  and say nothing. Lines stay within 100 columns where names allow, broken inside parentheses
  after a comma, each part lined up after its parenthesis, or a step in when it doesn't fit
  there, and a part that doesn't fit on a line of its own broken the same way.
