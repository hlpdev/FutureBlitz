# The review of M6 step 12, std.ffi's additions: the CData constraint and Ptr<T> of a type parameter, Buffer<T: CData> and its views, CLong, CULong, and CWChar with fb-bindgen writing them, Mem.AllocAligned, FreeAligned, and AllocUninitialized, and Extern "stdcall" removed

14 confirmed findings: 1 high, 3 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S12-buffer-1 (high): Reflection's f.Set reaches Buffer's private Fields, so safe code can double free a Buffer's memory or index past it

Status: fixed in 10115a63

Where: std/ffi/buffer.fb:17

Spec 13 says `For Field f In T` covers Private members too, and its `f.Set` is refused only on a ReadOnly Field. Buffer's First, Length, Viewed, and Owned are plain Fields. Generic safe code can therefore copy one Buffer's Fields onto another. Then both Buffers own the same memory, and the second Destroy frees it again: heap corruption. Copying only Length lets `[]` read and write far past the memory, because Check compares against the copied Length. This breaks decisions.md's rule that safe code never sees freed memory it didn't promise about. All four Fields are set only in a New, so they can be ReadOnly. I checked that ReadOnly works on a Buffer-shaped generic Class with a Ptr<T> Field: Assign over it becomes the compile error "'First' is ReadOnly, so f.Set can't change it".

```
Import std.core
Import std.ffi

// A generic helper that makes into's Fields what from's are, in safe code.
Function Assign<T>(ByRef into: T, from: T)
    For Field f In T
        f.Set(ByRef into, f.Get(from))
    End For
End Function

Local a = Buffer<Int32>(4)
Local b = Buffer<Int32>(4)
Assign(ByRef a, b)
Print("assigned {a.Count}")
a = Buffer<Int32>(0)
Print("a's gone")
b = Buffer<Int32>(0)
Print("b's gone")
```

Expected: Either a compile error at f.Set ("'First' is ReadOnly, so f.Set can't change it"), or, if reflection could reach the Fields, no shared ownership: "assigned 4", "a's gone", "b's gone".

Actual: Debug and release both: "error: reflect2.exe exited with code 0xC0000374" (STATUS_HEAP_CORRUPTION, a double free), and none of the program's lines are printed. A variant that copies only Length (`If f.Name == "Length" Then f.Set(...)`) from a Buffer<Int32>(100000000) onto a Buffer<Int32>(2) prints "100000000" and then reads small[99999999] with no panic. One release run of that variant died with 0xC0000005.

Suggested fix: Declare Buffer's Fields `ReadOnly Field First: Ptr<T>`, `ReadOnly Field Length: Int64`, `ReadOnly Field Viewed: Buffer<T>?`, and `ReadOnly Field Owned: Bool`. Every assignment to them is already in a New. Add a sema or program test that `For Field f In Buffer<Int32>` with f.Set is an error.

Skeptic: I ran the finding's reflect2.fb as written. Debug and release both died with 'exited with code 0xC0000374' (heap corruption) and printed nothing. A Length-only variant (reflen.fb: AssignLength copies Length from a Buffer<Int32>(100000000) onto a Buffer<Int32>(2), then reads small[99999999] and writes small[50000000]) segfaulted (0xC0000005) in debug. In release it printed '100000000', '0', 'wrote' with no panic, so safe code read and wrote far past the memory. Accessing a.First directly is refused ('First' is private to std/ffi/buffer.fb), but spec 13 says reflection goes over Private members and refuses f.Set only on ReadOnly ones. buffer.fb:17-23 declares First, Length, Viewed, and Owned as plain Fields, and they are assigned only in the two New's. This breaks decisions.md:579 ('safe code never sees freed memory it didn't promise about'). I checked the fix: a Buffer-shaped Class with 'ReadOnly Field First: Ptr<T>' makes the same Assign a compile error, "'First' is ReadOnly, so f.Set can't change it". Outside this step, std/crypto's blake3.fb:16, checksums.fb:58, and hashes.fb:34/60/86/112 have the same plain 'Field State: Ptr<Void>' exposure.

## S12-bindgen-1 (medium): A parameter declared with a function type is no longer bound: the function is skipped as "takes a FunctionProto", and a callback field holding one turns into Ptr<Void> without a warning

Status: fixed in 98c2ab87

Where: tools/bindgen/src/bindgen.cpp:1256

emit_function and map_callback now map each parameter with map_parameter() from the written FunctionProto, not the canonical one. libclang gives a decayed parameter as its original type. map_parameter() turns an array back into a pointer, but it leaves a function type as it is. A parameter written as `int compare(int, int)`, or as a typedef of a function type (`handler_t h`), reaches map_type() as a FunctionProto and fails with "a FunctionProto", which is libclang's own wording. At f9634d9 the canonical parameter was the decayed pointer, so these bound as Callbacks. The same thing happens one level down: a callback that takes a function-typed parameter fails, so the function holding it is skipped. In a struct field, problem_of() still maps the canonical type, so the struct passes. emit_struct() then maps the written type, fails, and its value_or("Ptr<Void>") quietly writes Ptr<Void> with no warning. Verified with this build's fb-bindgen (8363dc7) next to build-lane4's, whose bindgen.cpp is f9634d9's.

```
// sorter.h, bound next to this file with: fb-bindgen sorter.h --lib "" -o sorter.fb
//     void sort_ints(int* values, int count, int compare(int a, int b));
// (The same thing happens with `typedef long handler_t(long); void set_handler(handler_t h);`,
// and with `struct ops { int (*apply)(int value, int transform(int)); int count; };`.)
Import std.core
Import std.ffi
Import "./sorter"

Function Descending(a: Int32, b: Int32): Int32 => b - a

Local values: Int32[3]
Unsafe
    sort_ints(AddressOf(values[0]), 3, Descending)
End Unsafe
Print("bound")
```

Expected: fb-bindgen writes `Function sort_ints(values: Ptr<Int32>, count: Int32, compare: Callback(Int32, Int32): Int32)`, as f9634d9's does, and `fb check main.fb` passes. Likewise `set_handler(handler: Callback(CLong): CLong)`, and `Public Field apply: Callback(Int32, Callback(Int32): Int32): Int32`.

Actual: fb-bindgen warns `sorter.h:1:6: skipped sort_ints, which takes a FunctionProto` and writes only the skip comment. `fb check main.fb` then fails with `'sort_ints' isn't declared`. For the struct, it writes `Public Field apply: Ptr<Void>` and gives no warning.

Suggested fix: In map_parameter(), handle a function type the way an array is handled: when clang_getCanonicalType(declared) is CXType_FunctionProto or CXType_FunctionNoProto, return map_pointee(declared), since C passes it as a pointer. Add tests that a function-typed parameter (written directly and through a typedef), a callback that takes one, and a struct field's callback that takes one each map to Callbacks. Also consider replacing emit_struct's value_or("Ptr<Void>") with a check, so the written and canonical mappings can't drift apart unnoticed again.

Skeptic: Header at rev-scratch/s12/refute-bg/f1/sorter.h: `void sort_ints(int* values, int count, int compare(int a, int b)); typedef long handler_t(long); void set_handler(handler_t h); struct ops { int (*apply)(int value, int transform(int)); int count; };`. build-rev3's fb-bindgen (8363dc7) prints `warning: sorter.h:1:6: skipped sort_ints, which takes a FunctionProto` and the same for set_handler, and writes `Public Field apply: Ptr<Void>` with no warning. build-lane4's fb-bindgen (its bindgen.cpp is identical to f9634d9's, git diff empty) writes `Function sort_ints(values: Ptr<Int32>, count: Int32, compare: Callback(Int32, Int32): Int32)`, `Function set_handler(h: Callback(Int32): Int32)`, and `Public Field apply: Callback(Int32, Callback(Int32): Int32): Int32`. `fb check main.fb` fails with `main.fb:9:5: error: 'sort_ints' isn't declared` on the new module and passes on the old one. The code agrees. map_parameter() (bindgen.cpp about line 1250) turns arrays into pointers but sends a function type to map_type(), which rejects a FunctionProto. problem_of() maps the canonical, already decayed field type, so the struct passes, and emit_struct's `mapped.value_or("Ptr<Void>")` (line 1618) then quietly writes Ptr<Void>. This is a regression from f9634d9, and the struct case is a silent wrong binding.

## S12-cdata-1 (medium): For loops and ranges over a CLong (or a signed CWChar) stop early, or run past their end, when the span is more than the type's signed maximum

Status: fixed in 458c0429

Where: compiler/src/lower.cpp:92

lower.cpp's unsigned_of() handles only INT8 through INT64. For CLONG and CWCHAR its default case returns the signed type itself. A For loop (lower.cpp:3509) and a range expression (lower.cpp:5058) work out the distance left as a wrapping end - variable, then compare it with the step, or divide it by the step, in that signed type. Once the distance is more than the signed maximum, it wraps negative. The loop then stops after its first pass, and a Range gets a huge Left count, so iterating it overflows in std/core/range.fb. On Windows a CLong is an Int32, so a span over 2^31 is enough. On Linux the same happens to a CWChar (an Int32 there) and to a CLong past 2^63. A Const Function's For loop hits it at compile time too: Passes(-2000000000, 2000000000) with Step 1500000000 gives 1, where the Int32 version gives 3. In a release build the range case gives a different wrong answer from debug: debug panics, release prints -294966.

```
Import std.core
Import std.ffi

Local first: CLong = -2000000000
Local last: CLong = 2000000000
For i = first To last Step 1500000000
    Print("up {i}")
End For
For i = last To first Step -1500000000
    Print("down {i}")
End For
Local n: Int64 = 0
For x In first To last Step 1000
    n += 1
End For
Print("range {n}")
```

Expected: up -2000000000
up -500000000
up 1000000000
down 2000000000
down 500000000
down -1000000000
range 4000001
(This is what the same program prints with Int32 in place of CLong.)

Actual: Debug:
up -2000000000
down 2000000000
panic: integer overflow in '+' at std/core/range.fb:29:23
(exit code 101)
Release:
up -2000000000
down 2000000000
range -294966

Suggested fix: Give unsigned_of() cases for C's integers that work on every target: CLONG -> CULONG, and CWCHAR -> UINT32. The wrapping subtraction happens in CWChar first, so casting its bits to UInt32 gives the right distance whether CWChar is a UInt16, an Int32, or a UInt32. Add a c_integers.fb case like the program above, and a Const Function one.

Skeptic: I ran f1.fb (the finding's program) with fb.exe from build-rev3. Debug printed 'up -2000000000', 'down 2000000000', then 'panic: integer overflow in '+' at std/core/range.fb:29:23' (exit 101). Release printed 'up -2000000000', 'down 2000000000', 'range -294966'. The same program with Int32 in place of CLong (f1i.fb) printed the expected 3 up lines, 3 down lines and 'range 4000001' in both builds. On Windows a CLong is an Int32, so the two should match. The Const Function version, Passes(-2000000000, 2000000000) with Step 1500000000 (f1c.fb), gave '1 1' for the Const and the run-time call, against '3 3' for Int32. Code: lower.cpp:92 unsigned_of() handles only INT8..INT64 and its default returns the type unchanged, so for CLONG the For loop's distance check (lower.cpp:3509, a wrapping end - variable cast to distance_type, then compared with the step) stays signed and wraps negative. The range path at lower.cpp:5058 uses the same helper. Programs: C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s12/cdata-v/f1.fb, f1i.fb, f1c.fb, f1ci.fb.

## S12-cdata-2 (medium): 'p - q' on a Ptr at a generic Struct of a CData T is rejected as taking no bytes

Status: fixed in 7dbd14ca

Where: compiler/src/sema.cpp:18576

check_pointer_arithmetic returns early only when the element is itself CData (!why_not_c_data). A generic Struct's instance is never CData, so for Ptr<Wrap<T>> it falls through and measures Wrap<T> with c_layout at compile time. c_layout gives the type parameter T 0 bytes, so Wrap<T> measures 0 bytes. The '-' count check then reports an error, though every instance of Wrap<T> takes at least a byte, since T is CData. The program is otherwise valid: Ptr<Wrap<T>> is allowed, as are 'p + n' on it and SizeOf<Wrap<T>>(), and the same subtraction in non-generic code works. Only an empty generic Struct, like the sema test's Empty<T>, really takes no bytes. The same false error hits any element whose fields are all T-sized, like a Field items: T[4].

```
Import std.core
Import std.ffi

Struct Wrap<T: CData>
    Field v: T
End Struct

Unsafe Function Count<T: CData>(a: Ptr<Wrap<T>>, b: Ptr<Wrap<T>>): Int64 => a - b

Unsafe
    Local p: Ptr<Wrap<Int32>> = Mem.Alloc(64)
    Print("{Count(p + 3, p)} {(p + 3) - p} {SizeOf<Wrap<Int32>>()}")
    Mem.Free(p)
End Unsafe
```

Expected: 3 3 4

Actual: f2.fb:8:77: error: '-' counts the values between two Ptrs, and Wrap<T> takes no bytes
(With Count's body replaced by 0 the rest compiles and prints "0 3 4".)

Suggested fix: When the element has type parameters in it, don't measure it with c_layout in sema. Either measure each instance in lowering, as size_of() does for SizeOf, and report there, or decide 'takes no bytes' by treating each CData type parameter as at least 1 byte. A Struct is 0 bytes only when all its fields are, so Empty<T> still errors and Wrap<T> doesn't. Add a sema test with Ptr<Wrap<T>> next to the Empty<T> one.

Skeptic: f2.fb (the finding's program) fails in debug and release with 'f2.fb:8:77: error: '-' counts the values between two Ptrs, and Wrap<T> takes no bytes'. In the same setup, 'a + 3' on Ptr<Wrap<T>> in generic code compiles and runs, printing 3 (f2b.fb). An Arr<T: CData> with 'Field items: T[4]' gets the same false error (f2c.fb). No CData type can take 0 bytes: 'Int32[0]' is rejected with 'a fixed array's size must be at least 1' (z0.fb), and an empty @CLayout Struct is rejected with 'C has no empty structs' (z1.fb). Code: sema.cpp check_pointer_arithmetic (around 18557) returns early only when the element itself passes why_not_c_data. A generic Struct like Wrap<T> isn't CData, so it falls through to fb::c_layout(...).size == 0. That measures Wrap<T> in the generic code as 0 bytes, but spec 08 says 'A T with type parameters in it is measured for each instance, as it's made, and isn't known at compile time in the generic code'. Spec 08 also allows p - q 'when a T takes some bytes', and every instance here does, so this rejects a valid program.

## S12-bindgen-2 (low): A wide character macro like L'/' isn't a CWChar: it gets a different integer type on each target (UInt16, Int32, UInt32), and no test covers CWChar macros

Status: fixed in db342865

Where: tools/bindgen/src/bindgen.cpp:784

In C, a wide character constant like L'/' has type wchar_t (C11 6.4.4.4p11). Spec 12 says "a number macro of one of these is one where every target's holds its value". Clang, in C mode, gives the literal the target's plain integer type, with no wchar_t typedef around it. So c_integer(written) finds nothing, and the macro is typed by its size and sign: UInt16 on x86_64-w64-mingw32, Int32 on x86_64-unknown-linux-gnu, and UInt32 on aarch64-unknown-linux-gnu. `#define SEPARATOR_CAST ((wchar_t)'/')` correctly becomes `CWChar = 47` on all three. Because an Int32 doesn't convert to a CWChar without As, code that works with the Windows module fails to compile with the Linux one. Separately, nothing in tests/bindgen checks a CWChar macro at all. fits_everywhere()'s CWChar branch (0 to 65535) and the CLong boundaries are untested.

```
// wide.h: #include <stddef.h>
//         #define SEPARATOR L'/'
//         #define SEPARATOR_CAST ((wchar_t)'/')
// fb-bindgen wide.h --lib "" --target x86_64-unknown-linux-gnu writes the two Consts below
// (for x86_64-w64-mingw32 it writes SEPARATOR: UInt16, and for aarch64-unknown-linux-gnu
// SEPARATOR: UInt32).
Import std.core
Import std.ffi

Const SEPARATOR: Int32 = 47
Const SEPARATOR_CAST: CWChar = 47

Local a: CWChar = SEPARATOR_CAST
Local b: CWChar = SEPARATOR
Print("{a} {b}")
```

Expected: fb-bindgen writes `Public Const SEPARATOR: CWChar = 47` for every target, as it does for SEPARATOR_CAST, and the program prints `47 47`.

Actual: It writes `SEPARATOR: UInt16` (Windows), `Int32` (x86-64 Linux), or `UInt32` (AArch64 Linux). With the Linux module, the program fails with `wide_use.fb:10:19: error: expected CWChar, found Int32`.

Suggested fix: In constant_of(), when the macro's expression (inside its parentheses) is a wide character literal (a CXCursor_CharacterLiteral whose token starts with L), take it as CWChar, subject to the same fits_everywhere() check. Add bindgen tests for `L'/'`, `((wchar_t)'/')`, `((wchar_t)65535)`, and `((wchar_t)70000)`, and for a CLong at -2147483648 and 2147483647 and just outside them.

Skeptic: wide.h (f2/) has `#define SEPARATOR L'/'` and `#define SEPARATOR_CAST ((wchar_t)'/')`. fb-bindgen writes SEPARATOR as `UInt16` for x86_64-w64-mingw32, `Int32` for x86_64-unknown-linux-gnu, and `UInt32` for aarch64-unknown-linux-gnu. SEPARATOR_CAST is `CWChar = 47` on all three, and `5L`/`5UL` are CLong/CULong. With each module, `Local b: CWChar = SEPARATOR` builds and prints `47 47` with the Windows one. With the Linux one it fails: `error: expected CWChar, found Int32`. With the AArch64 one: `expected CWChar, found UInt32`. Spec 12 says a number macro is a Const of the type C gives it and counts `1L` as one of these, and C gives L'/' type wchar_t. Clang in C mode types the literal as the target's plain integer, so c_integer() misses it, and the module differs by target in a way the step's cross-target rule exists to prevent. tests/bindgen/bindgen_tests.cpp has no CWChar macro test: grep finds CWChar only in the struct/function case at lines 417-450.

## S12-bindgen-3 (low): A header's own `wchar_t` typedef becomes CWChar even when it isn't the target's wchar_t, so the @CLayout Struct's size is wrong

Status: fixed in db342865

Where: tools/bindgen/src/bindgen.cpp:1118

c_integer() decides CWChar by the typedef's name alone. For long and unsigned long it uses clang's builtin type, which is always the target's. In C, wchar_t is an ordinary typedef. A header that doesn't include stddef.h can define its own, such as `typedef unsigned int wchar_t;` for UTF-32, or a Windows-style `typedef unsigned short wchar_t;` compiled for Linux. bindgen still writes CWChar, which is the target's size (16 bits on Windows, 32 elsewhere). The size doesn't match what the header declared. problem_of() checks the layout only against clang's own sizes, so it doesn't catch this.

```
// glyphs.h, bound next to this file with: fb-bindgen glyphs.h --lib "" -o glyphs.fb (Windows host)
//     typedef unsigned int wchar_t;
//     struct glyph { wchar_t code; wchar_t next; };
//     _Static_assert(sizeof(struct glyph) == 8, "C's glyph is 8 bytes");
// fb-bindgen writes: @CLayout Public Struct glyph / code: CWChar / next: CWChar
Import std.core
Import std.ffi
Import "./glyphs"

// C's struct glyph is 8 bytes, as glyphs.h's _Static_assert (which clang accepted) says.
Print(SizeOf<glyph>())
```

Expected: 8: the fields are written as UInt32, the header's own 32-bit type, so the Struct is laid out as C has it.

Actual: 4: both fields are CWChar, which is a UInt16 on Windows. On Linux, a header's own 16-bit wchar_t gives the reverse: an 8-byte Struct for C's 4.

Suggested fix: In c_integer(), return CWChar for a typedef named wchar_t only when its canonical type has the size and sign of the target's wchar_t (clang's __WCHAR_TYPE__, or the size from on_target()'s table for the target). Otherwise keep peeling or map by size. Add a test with a header that defines its own wchar_t.

Skeptic: glyphs.h (f3/) is `typedef unsigned int wchar_t; struct glyph { wchar_t code; wchar_t next; }; _Static_assert(sizeof(struct glyph) == 8, ...)`. clang accepts the header, and fb-bindgen writes `@CLayout Public Struct glyph` with both fields `CWChar`. `Print(SizeOf<glyph>())` prints 4 in debug and release, where C's struct is 8, so the @CLayout Struct silently has the wrong layout. c_integer() (bindgen.cpp about line 1118) returns CWChar for any typedef named wchar_t without checking its canonical type against the target's wchar_t. problem_of() checks offsets only with clang's canonical sizes, so the mismatch is never caught. The case is rare: a header that defines its own wchar_t without stddef.h. The result is still a wrong layout that can corrupt memory.

## S12-bindgen-4 (low): vmath's Buffered can't tell whether a view's Pointer starts at the view: the sums at offsets 0, 1, and 2 are all 13

Status: fixed in 38342696

Where: tests/packages/vmath/vmath.fb:61

Buffered's comment says it checks that "a view's Pointer is where the view starts, which C reads". This is the only test that passes a view's Pointer() to C (buffer.fb only passes whole Buffers). The values are vm_fill's 6s with a 1 at index 2, which gives [6, 6, 1, 6, 6]. Every window of three numbers in that Buffer sums to 13, because each one holds the single 1. So `vm_sum(values.Slice(1, 4).Pointer(), 3)` prints 13 even if Pointer() returned the parent's start, or overshot by one. The expected "6 6 13 7 8 0" in native_calls.out doesn't check the done-when it claims to. The current code is right (a check shows view.Pointer() - values.Pointer() is 1); only the test can't tell.

```
Import std.core
Import std.ffi

// vm_sum, as vecmath.c has it: the sum of count Int32s from values.
Unsafe Function Summed(values: Ptr<Int32>, count: Int32): Int32
    Local total: Int32 = 0
    For i = 0 Until count
        total += values[i]
    End For

    Return total
End Function

// Buffered's values: vm_fill's 6s, then a 1 at index 2.
Local values = Buffer<Int32>(5)
For i = 0 Until 5
    values[i] = 6
End For

values[2] = 1
Unsafe
    // The view's Pointer, the Buffer's own, and one past the view's start.
    Print("{Summed(values.Slice(1, 4).Pointer(), 3)} {Summed(values.Pointer(), 3)} {Summed(values.Pointer() + 2, 3)}")
End Unsafe
```

Expected: Three different totals, so a wrong view Pointer would change what Buffered prints.

Actual: `13 13 13`: the view's Pointer, the Buffer's own Pointer, and Pointer() + 2 all give the total Buffered prints.

Suggested fix: Use values that make each window's sum different. For example, after vm_fill set values[1] = 1 and values[3] = 100, giving [6, 1, 6, 100, 6]: the view (1, 4) sums to 107, while offset 0 would give 13 and offset 2 would give 112. Then update native_calls.out to match.

Skeptic: Buffered's values after vm_fill(…, 6) and values[2] = 1 are [6, 6, 1, 6, 6], and each 3-window holds the single 1. My program (f4/main.fb) mirrors vm_sum. Summing 3 from the view's Pointer, the Buffer's own Pointer, and Pointer() + 2 prints `13 13 13`, and `view.Pointer() - values.Pointer()` prints 1, so the current code is right. vmath.fb:61 is the only test that passes a view's Pointer to C (grep: buffer.fb passes only whole Buffers). Its expected `6 6 13 7 8 0` would not change if Pointer() returned the parent's start or start + 2, so the test doesn't check what its comment says ("a view's Pointer is where the view starts, which C reads").

## S12-bindgen-5 (low): An array of function pointers in a struct is written as `Callback(CLong): CLong[3]`, which the compiler reads as a Callback that gives an array, so the module doesn't compile (this predates step 12)

Status: fixed in 27b0b4ed

Where: tools/bindgen/src/bindgen.cpp:1405

map_array() writes the element's text followed directly by `[3]`. When the element is a Callback with a result, `Callback(CLong): CLong[3]` parses as a Callback that gives a CLong[3]. The compiler rejects that, so the generated module fails to compile. This breaks bindgen's promise that each header becomes a module that compiles. The parenthesized form, `(Callback(CLong): CLong)[3]`, compiles and has C's layout (SizeOf 32). f9634d9 writes the same broken text (`Callback(Int32): Int32[3]`), so the bug predates this step, but step 12 changed map_array() without catching it.

```
// pipeline.h, bound next to this file with: fb-bindgen pipeline.h --lib "" -o steps.fb
//     struct pipeline { long (*steps[3])(long value); int count; };
// fb-bindgen writes: Public Field steps: Callback(CLong): CLong[3]
Import std.core
Import std.ffi
Import "./steps"

Print(SizeOf<pipeline>())
```

Expected: The module compiles, with `Public Field steps: (Callback(CLong): CLong)[3]`, and the program prints `32`, C's sizeof.

Actual: `steps.fb:5:42: error: a Callback takes and gives what C can: numbers, Bools, Enums, CStrings, Ptrs, and Callbacks, and a Struct through a Ptr, not CLong[3]`. When the line is parenthesized by hand, the program prints 32.

Suggested fix: In map_array(), wrap the element's text in parentheses when it's a Callback with a result, that is, when it starts with "Callback(" and has a ": " after its closing parenthesis. Add a bindgen test with a field that's an array of function pointers.

Skeptic: pipeline.h (f5/) is `struct pipeline { long (*steps[3])(long value); int count; };`. fb-bindgen writes `Public Field steps: Callback(CLong): CLong[3]`, and `fb run main.fb` fails: `steps.fb:5:42: error: a Callback takes and gives what C can: ... not CLong[3]`. build-lane4's (f9634d9) bindgen writes the same shape, `Callback(Int32): Int32[3]`, so the bug predates step 12. Parenthesizing it by hand, `(Callback(CLong): CLong)[3]`, compiles and prints 32, C's sizeof. map_array() appends `[3]` to the element text unparenthesized, which breaks bindgen's promise (milestones row 10) that everything it writes compiles.

## S12-buffer-3 (low): View doesn't bound count by SizeOf<T> as New does, so CopyTo differs between debug and release

Status: fixed in f089c794

Where: std/ffi/buffer.fb:55

New panics when count * SizeOf<T>() would pass Int64's max ("takes more bytes than an Int64 counts"). View only checks for a negative count and a Null pointer. A view whose byte size overflows Int64 can't describe real memory, and CopyTo's `Length * SizeOf<T>()` then overflows. A debug build panics with an internal "integer overflow in '*'" at the caller. A release build wraps to 0 and silently copies nothing.

```
Import std.core
Import std.ffi

Unsafe
    Local memory: Ptr<Int64> = Mem.Alloc(16)
    Local count: Int64 = 2305843009213693952
    Local whole = Buffer<Int64>.View(memory, count)
    Print("{whole.Count}")
    whole.CopyTo(whole)
    Print("copied")
    Mem.Free(memory)
End Unsafe
```

Expected: View panics in both builds, as New does: "a view of 2305843009213693952 Int64s takes more bytes than an Int64 counts".

Actual: Debug: "2305843009213693952", then "panic: integer overflow in '*' at t11.fb:9:5". Release: "2305843009213693952", then "copied".

Suggested fix: In View, add `If count > 9223372036854775807 / SizeOf<T>() Then Panic(...)` with New's message, document it in View's comment, and add a panic program.

Skeptic: t11.fb as given: debug printed '2305843009213693952' then 'panic: integer overflow in '*' at t11.fb:9:5'. Release printed it then 'copied'. A safe-code variant (t11b.fb, View(memory, 2305843009213693953) made in Unsafe, then whole.CopyTo(whole) in safe code) diverged the same way. buffer.fb:30 has New bound count by Int64's max / SizeOf<T>() for exactly this, but View (55-62) checks only a negative count and a Null pointer, while CopyTo (115) multiplies Length * SizeOf<T>(). So std's own arithmetic relies on an invariant that View lets through. Low severity: it takes an Unsafe View whose count can't describe real memory. View already panics on the other lies it can detect, though, and the bound is one line.

## S12-buffer-4 (low): Only the index panic has a panic program; Buffer's other panics have none

Status: fixed in 2c47ce23

Where: tests/programs/panic_buffer_index.fb:1

std's convention is a panic program for each panic: RingBuffer, Grid, Grid3, and BitSet each have one per panic, and AllocAligned's alignment panic got one in this step. Buffer panics in New (negative count, too many bytes, out of memory), View (negative count, Null with a count), Slice (out of range), CopyTo (destination too short), and ToArray (more than an array holds). Only `[]`'s index panic is tested. I ran each one, and they work today; nothing keeps them working.

```
Import std.core
Import std.ffi

Local values = Buffer<Int32>(4)
Print("{values.Slice(1, 4).Count}")
Local part = values.Slice(3, 5)
```

Expected: Panic programs, for example panic_buffer_slice.fb expecting "3" and then "panic: slice 3..5 is out of range for a Buffer of 4 values at panic_buffer_slice.fb:6:14", plus ones for CopyTo, New, and View.

Actual: The program prints that today, but tests/programs has only panic_buffer_index.fb for Buffer.

Suggested fix: Add panic_buffer_slice, panic_buffer_copy ("a Buffer of 4 values doesn't fit in one of 3"), panic_buffer_count (negative and too large), and panic_buffer_view (Null with count 1, negative count) programs with their .out files.

Skeptic: tests/programs has only panic_buffer_index.fb/.out for Buffer. No test anywhere greps for "doesn't fit in one of", "out of range for a Buffer" (Slice), "can't start at a Null", "a view holds a count", "a Buffer holds a count", or Buffer's "takes more bytes than an Int64 counts". RingBuffer, Grid, Grid3, and Mem each have a panic program per panic. Today's messages: 'panic: slice 3..5 is out of range for a Buffer of 4 values at panic_buffer_slice.fb:6:14' (after '3'), 'a Buffer of 4 values doesn't fit in one of 3 at pcopy.fb:5:1', 'a Buffer holds a count of values that isn't negative, not -1 at pnew.fb:5:16', 'a Buffer of 4611686018427387904 Int32s takes more bytes than an Int64 counts at pbig.fb:5:16', and 'a view of 1 Int64s can't start at a Null Ptr at p6.fb:4:15'. They work now, but nothing tests them.

## S12-buffer-5 (low): Mem.Resize's and Free's comments, and fb_mem_resize's and fb_mem_free's, leave out AllocUninitialized

Status: fixed in bb354fa5

Where: std/ffi/mem.fb:76

Mem's header comment and spec 08 say Resize and Free take what Alloc, AllocUninitialized, or Resize gave. Resize's comment ("What Alloc or Resize gave", mem.fb:76) and Free's ("Gives back what Alloc or Resize gave", mem.fb:87) still name only Alloc and Resize. runtime.hpp:149 and 153 say the same of fb_mem_resize and fb_mem_free, without fb_mem_alloc_uninitialized. The code accepts that memory: the program below works.

```
Import std.core
Import std.ffi

Unsafe
    Local memory = Mem.AllocUninitialized(8) As Ptr<Int32>
    memory[0] = 1
    memory = Mem.Resize(memory, 16)
    memory[3] = 4
    Print("{memory[0]} {memory[3]}")
    Mem.Free(memory)
End Unsafe
```

Expected: "1 4", with Resize's and Free's comments saying they take AllocUninitialized's memory.

Actual: It prints "1 4", but the comments on Mem.Resize, Mem.Free, fb_mem_resize, and fb_mem_free name only Alloc (fb_mem_alloc) and Resize (fb_mem_resize).

Suggested fix: Make the comments read "What Alloc, AllocUninitialized, or Resize gave" in mem.fb, and "fb_mem_alloc, fb_mem_alloc_uninitialized, or fb_mem_resize" in runtime.hpp.

Skeptic: uninit.fb prints '1 4', so Resize and Free take AllocUninitialized's memory; the runtime uses malloc/realloc/free. The comments say otherwise: mem.fb:76 'What Alloc or Resize gave', mem.fb:87 'Gives back what Alloc or Resize gave', runtime.hpp:149 'What fb_mem_alloc or fb_mem_resize gave', and runtime.hpp:153 'Gives back what fb_mem_alloc or fb_mem_resize gave'. Mem's own header (mem.fb:18) and spec 08:138-140 include AllocUninitialized.

## S12-buffer-6 (low): cdata.fb's example puts Where on the signature's line, which doesn't parse

Status: fixed in bb354fa5

Where: std/ffi/cdata.fb:3

The comment gives `Function First<T>(p: Ptr<T>): T Where T: CData` as the way to use CData. A Where line goes on its own line after the signature, as in std's own Iterator.Zip and cdata.fb's Extend test. On one line the parser reports "expected end of line, found 'Where'". Reading p also needs Unsafe. Spec 08's form, `Function First<T: CData>(p: Ptr<T>): T`, is the one that works.

```
Import std.core
Import std.ffi

Unsafe Function First<T>(p: Ptr<T>): T Where T: CData
    Return p[0]
End Function

Unsafe
    Local m: Ptr<Int32> = Mem.Alloc(4)
    m[0] = 3
    Print("{First(m)}")
    Mem.Free(m)
End Unsafe
```

Expected: "3", if the comment's form were valid.

Actual: "t25.fb:4:40: error: expected end of line, found 'Where'", plus an error that Ptr<T> needs 'Where T: CData'. With the Where moved to its own line, it prints "3".

Suggested fix: Write the example as `Unsafe Function First<T: CData>(p: Ptr<T>): T`, matching spec 08, or put the Where on its own line.

Skeptic: cd.fb, with cdata.fb:3's form 'Unsafe Function First<T>(p: Ptr<T>): T Where T: CData', fails with 'cd.fb:4:40: error: expected end of line, found 'Where'' plus 'a Ptr points at a value C code has ... unless 'Where T: CData' keeps it to C's'. With the Where on its own line (cd2.fb), it prints '3'. Spec 07:131 says 'A Where line goes at the top of the body', and spec 08:92 writes the example as 'Function First<T: CData>(p: Ptr<T>): T'. The example also leaves out the Unsafe that reading p[0] needs.

## S12-buffer-7 (low): Buffer's View and New messages always pluralize the type name ("a view of 1 Int64s")

Status: fixed in f089c794

Where: std/ffi/buffer.fb:58

Buffer's index, Slice, and CopyTo messages handle a count of 1 with `value{Length == 1 ? "" : "s"}`. The messages built with "{count} {TypeName<T>()}s" at lines 31, 36, and 58 don't, so a count of 1 reads "1 Int64s".

```
Import std.core
Import std.ffi
Unsafe
    Local b = Buffer<Int64>.View(Null, 1)
End Unsafe
```

Expected: "panic: a view of 1 Int64 can't start at a Null Ptr at p6.fb:4:15", or wording that avoids the plural, such as "a view of 1 value of Int64".

Actual: "panic: a view of 1 Int64s can't start at a Null Ptr at p6.fb:4:15"

Suggested fix: Use `{count == 1 ? "" : "s"}` after TypeName<T>() in the three messages, as the other Buffer messages do.

Skeptic: p6.fb prints 'panic: a view of 1 Int64s can't start at a Null Ptr at p6.fb:4:15'. buffer.fb:58 and 36 build '{count} {TypeName<T>()}s' with no count-of-1 case, while the index, Slice, and CopyTo messages (97, 110, 153) use 'value{Length == 1 ? "" : "s"}'. Line 31 can't show 1, since it runs only when count > Int64 max / size, but 36 (out of memory for a single huge T) and 58 can. Appending 's' to names like 'Map<String, Int32>' or 'Int32[4]' also reads oddly, so the 'value(s) of T' wording may be better.

## S12-cdata-3 (low): Generic code steps a Ptr at a T of more bytes than an Int64 counts, which non-generic code and generic SizeOf<T>() both report

Status: fixed in 8b923d70

Where: compiler/src/sema.cpp:18558

Spec 08 says a T of more bytes than an Int64 counts is an error, and a Ptr at one doesn't step. Non-generic code reports it (sema.cpp:18569), and so does a generic SizeOf<T>() for such an instance, where lower.cpp's size_of() gives 'SizeOf measures Big[2147483647] here, which takes more bytes than an Int64 counts'. But the early return in check_pointer_arithmetic for a CData T with type parameters skips the check, and lowering never checks the instance. So 'p + 1' and 'p - q' on Ptr<T> compile for T = Big[2147483647] (about 2^65 bytes), and the address arithmetic wraps.

```
Import std.core
Import std.ffi

@CLayout Struct Big
    Field a: Int64[2147483647]
End Struct

Unsafe Function Count<T: CData>(a: Ptr<T>, b: Ptr<T>): Int64 => (a + 1) - b

Unsafe
    Local p: Ptr<Big[2147483647]> = Null
    Print("{Count(p, p)}")
End Unsafe
```

Expected: A compile error, as for the same code without the generic: "Big[2147483647] takes more bytes than an Int64 counts, so a Ptr at it doesn't step" (the non-generic '(p + 1) - p' gives exactly this).

Actual: Compiles and prints 1.

Suggested fix: Report it once per instance in lowering, as size_of() does: where a pointer_t's element type came from a type with type parameters, check c_layout(...).size > INT64_MAX and set m_module.error. A shared helper could serve both checks. Add a case next to the SizeOf one.

Skeptic: f3.fb (the finding's program, generic Count<T: CData> doing (a + 1) - b with T = Big[2147483647]) compiles and prints 1 in debug and release. The non-generic version (f3n.fb, '(p + 1) - p' on Ptr<Big[2147483647]>) fails as it should, in both builds, with 'Big[2147483647] takes more bytes than an Int64 counts, so a Ptr at it doesn't step'. Spec 08 says 'A T of more bytes than an Int64 counts ... is an error, and a Ptr at one doesn't step. A T with type parameters in it is measured for each instance'. Code: sema.cpp's early return for a CData element with type parameters (around 18557-18562) skips the INT64_MAX size check, and nothing checks the instance in lowering, so the step compiles and its address arithmetic wraps. It's low because the type is contrived: about 2^65 bytes. Programs: C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s12/cdata-v/f3.fb, f3n.fb.

## Refuted by the skeptics (no action)

- S12-buffer-2: A Buffer of Enums hands safe code a value C wrote that isn't one of the Enum's, unchecked. enumbuf.fb prints '7 False [7, Red]' in debug and release, as reported. That matches spec 12, though: an Enum 'that C writes through a Ptr later isn't checked', and a Buffer's memory reaches C only through Pointer(), which only Unsafe code gets. Plain Ptrs already let the same value into safe code: enumptr.fb has Unsafe code read 7 through a Ptr<Color> into a Local, and safe code then has it. In both builds the result is defined, not UB: an exhaustive Select gives 'panic: no Case matched at enumptr.fb:11:5'. Buffer adds no new way in, and the Unsafe code that handed C the Pointer owns the promise. A sentence in spec 12 saying a Buffer's safe reads of C-written Enums aren't checked would help, but the behavior doesn't contradict the spec.
