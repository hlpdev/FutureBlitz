# The review of M6 step 5: format specifiers in interpolation checked at compile time, Formattable, Format(template, values...) throwing FormatError, Int/Float/Bool.Parse giving a T?, invariant float text, the debug form "{x:?}" and Dump(x) for every value, and the fix that an Extend's Methods keep their default values

24 confirmed findings: 4 high, 10 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S5-debug-1 (high): A cycle through arrays overflows the stack: only objects shown through fb_debug_object are tracked, never a T[]

Status: fixed in f42f6116 (a debug form cuts a cycle through arrays, and reads a fixed array where it is), with the Struct graph and the array that holds itself in tests/programs/debug_form.fb

Where: compiler/src/lower.cpp:6244

debug_of() sends a T[] straight to its generated Debug function (debug_body's is_any_array branch), and only Class and Interface values go through fb_debug_object, which keeps the thread's `showing` list. So a cycle that runs through arrays and Struct copies, with no Class in it, is never cut off. A graph of Structs whose Edges arrays hold each other (a normal way to build one, since a T[] is a reference) recurses until the stack overflows, in debug and release builds. The docs promise that an object already being shown in a cycle shows as `Node { … }`, and spec 14 says a T[] is a reference. Smaller symptom of the same gap: a Printable[] that holds itself shows twice before the cut, `[[Printable[] { … }]]`, because the array at the top isn't tracked, and the marker for an array is the Struct-style `{ … }`.

```
Import std.core

// Two nodes of a graph, each in the other's edges: a cycle through arrays, with no Class.
Struct Node
    Field Name: String
    Field Edges: Node[]
End Struct

Local a = Node(Name: "a", Edges: [])
Local b = Node(Name: "b", Edges: [a])
a.Edges.Add(b)
Print("{a.Edges.Count} {b.Edges.Count}")
Flush()
Print("{a:?}")
a.Edges.Clear()
```

Expected: 1 1, then a's debug form with the cycle cut where a.Edges comes back, e.g. Node { Name: "a", Edges: [Node { Name: "b", Edges: [Node { Name: "a", Edges: Node[] { … } }] }] }

Actual: 1 1, then "error: f1.exe exited with code 0xC00000FD" (stack overflow), the same with --release. Also, `Local items: Printable[] = []` / `items.Add(items)` / `Print("{items:?}")` prints [[Printable[] { … }]], the array twice.

Suggested fix: Track a dynamic array as being shown, as fb_debug_object does for an object. For example, give every shown T[] type's fb_class_t a debug slot and route a T[]'s debug form through fb_debug_object, or add runtime enter and leave calls around debug_body's array branch. Show the cut array with an array-style marker. Add the Struct-graph cycle to debug_form.fb.

Skeptic: I ran the finding's program, rev-scratch/s5/dbg/f1.fb. Debug and --release both printed `1 1`, then `error: f1.exe exited with code 0xC00000FD` (stack overflow). f1b.fb (a Printable[] holding itself) printed `[[Printable[] { … }]]`. In the code, debug_of() (lower.cpp:6079) sends only is_class/is_interface values to DEBUG_OBJECT. Arrays, tuples, and Structs go straight to the generated DEBUG function, so the thread_local `showing` list in fb_debug_object (runtime/src/format.cpp:147-162) never sees a T[] at the top or reached through a Struct Field. An array is cut off only when an interface value brings it back, and then the marker is `{ … }`. Spec 14 says `{x:?}` shows any value and that a cycle is cut. A crash when printing a Struct graph built through T[] references contradicts that.

## S5-debug-2 (high): Any debug form crashes the compiler in a program whose Class holds a large fixed array

Status: fixed in f42f6116 (a debug form cuts a cycle through arrays, and reads a fixed array where it is), with a Class's UInt8[256, 256] Field in tests/programs/debug_form.fb; a debug function takes what holds a fixed array ByRef. Passing a large fixed array by value elsewhere still crashes codegen, which lane 2 reported with the P1 lowering batch

Where: compiler/src/lower.cpp:1173

Once a program shows any debug form, lowering makes a Debug function for every declared Class. A Class's Debug passes each fixed-array Field by value to that array type's generated Debug (MIR shows `call UInt8[16384].Debug(Int32)(_0.Tiles, _3)`). Large fixed arrays passed by value already crash the compiler: a user function taking a UInt8[256, 256], or == on a Struct holding one, segfaults fb too. But this step makes an unrelated "{n:?}" on an Int trigger it for any Class with such a Field, even one the program never shows, like a game's tile map. The cost grows before the crash: a UInt8[128, 128] Field makes the debug build take 1.7s instead of 0.4s, and UInt8[256, 256] segfaults fb in debug and release builds. `fb check` passes.

```
Import std.core

// A tile map, which the program never shows.
Class TileMap
    Field Tiles: UInt8[256, 256]
End Class

Local map = TileMap()
map.Tiles[3, 4] = 7
Local n = map.Tiles[3, 4]
Print("{n:?}")
```

Expected: 7

Actual: fb run (and fb build, debug or --release) segfaults while compiling: exit 139, no executable, no output. With "{n}" in place of "{n:?}" it prints 7.

Suggested fix: Don't copy fixed arrays (or Structs holding them) into the generated Debug functions: pass them by reference, or have debug_body walk the Field's place directly. Fix the underlying codegen of large fixed arrays by value too, which also crashes `Function Corner(tiles: UInt8[256, 256])` and == on a Struct with one. Add a program test with a large fixed-array Field and a debug form.

Skeptic: f2.fb (a Class with a UInt8[256, 256] Field, plus Print("{n:?}") on an Int): the debug build exits 139 during `Compiling`, with no output. The --release build was still compiling when my 200 s timeout killed it (exit 124). f2b.fb, the same program with "{n}", prints 7. f2c.fb with UInt8[128, 128] builds in 1.41s and prints 7. f2d.fb, `Function Corner(tiles: UInt8[256, 256])`, also exits 139 without any debug form, which confirms the codegen crash for large fixed arrays passed by value already existed. lower.cpp:1170-1186 is new in this step: once m_debug_used is set, it makes a DEBUG function for every class in m_module.classes whose definition the program declared, so any debug form anywhere exposes that crash for such a Class.

## S5-format-parse-1 (high): An Interface's default Methods lose their default values when called on the type that implements them

Status: fixed in 63d7f3ee (an interface's default methods and inherited news keep their parameters' default values), with tests/programs/copied_defaults.fb

Where: compiler/src/sema.cpp:5209

take_default() copies an Interface default's parameter symbols for each implementing type. It does this while declarations are made, before the check_defaults pass (sema.cpp:419) sets `value` / `empty_default` on the Interface's own parameters. Those copies never get a value, so lowering's default_of() silently falls back to zero_value. This happens for Final and non-Final defaults, on a Class, a Struct, and a type whose Extend lists the Interface. A call through the Interface type still works, because it uses the Interface's own symbol. The step fixed the same bug for an Extend's Methods (check_defaults now walks extend_stmt_t), but not these copies. The bug is older than this step.

```
Import std.core

Interface Shape
    Method Area(): Int
    Method Scaled(by: Int = 5): Int => Area() * by
    Final Method Named(prefix: String = "shape"): String => "{prefix}:{Area()}"
End Interface

Class Square Implements Shape
    Method Area(): Int => 3
End Class

Struct Dot
    Field X: Int
End Struct

Extend Dot Implements Shape
    Method Area(): Int => 4
End Extend

Local through: Shape = Square()
Print("{through.Scaled()} {through.Named()}")
Print("{Square().Scaled()} {Square().Named()}")
Print("{Dot(1).Scaled()} {Dot(1).Named()}")
```

Expected: 15 shape:3
15 shape:3
20 shape:4

Actual: 15 shape:3
0 :3
0 :4

Suggested fix: After the check_defaults pass, copy `value` and `empty_default` from each original parameter to the copies take_default() made. One way is to record (copy, original) pairs in take_default. Another is to give the copy a link to its source parameter and have default_of() follow it. Add a program test that calls a default and a Final default Method, without arguments, on a Class, a Struct, and an Extend-listed type.

Skeptic: Ran f1.fb (the finding's program) with build-rev3 fb, debug and --release: prints '15 shape:3 / 0 :3 / 0 :4'. A call through the Interface works, but Square().Scaled(), Square().Named(), Dot(1).Scaled(), and Dot(1).Named() silently get 0 and "". Code: take_default (sema.cpp:5200) copies each parameter symbol with add_symbol(std::move(parameter)) while declaring (declare_extension/link_extensions), and check_defaults runs later (sema.cpp:419, after link_extensions at 408). check_defaults (12808) sets symbol(parameter).value only on the function's own parameters, so the copies keep has_default with no value, and lower.cpp default_of (2384) falls back to zero_value. Silent wrong values, so high.

## S5-format-parse-2 (high): Inherited News lose their default values

Status: fixed in 63d7f3ee (an interface's default methods and inherited news keep their parameters' default values), with tests/programs/copied_defaults.fb

Where: compiler/src/sema.cpp:6118

inherit_constructors() copies each base New's parameter symbols during declaration, before check_defaults runs, so the copies have has_default but no value. Derived() then compiles, but every left-out argument is zero: 0, an empty String, and so on. This is the same root cause as S5-format-parse-1 and the Extend bug this step fixed. Generic bases are affected too (Extends Holder<Int>). The bug is older than this step.

```
Import std.core

Class Base
    Field N: Int
    Field S: String
    New(n: Int = 5, s: String = "base")
        N = n
        S = s
    End New
End Class

Class Derived Extends Base
End Class

Class Holder<T>
    Field Count: Int
    New(count: Int = 9)
        Count = count
    End New
End Class

Class IntHolder Extends Holder<Int>
End Class

Print("{Base().N}|{Base().S}|{Derived().N}|{Derived().S}|{Derived(7).S}|{IntHolder().Count}")
```

Expected: 5|base|5|base|base|9

Actual: 5|base|0|||0

Suggested fix: Same as S5-format-parse-1: once check_defaults has run, carry `value` and `empty_default` from the base New's parameters to the inherited copies, or have the copies refer to their source. Add a program test of an inherited New called without its defaulted arguments, with a plain base and a generic one.

Skeptic: Ran f2.fb, debug and release: prints '5|base|0|||0' where the expected output is '5|base|5|base|base|9'. Derived() and IntHolder() get 0 and "" for the left-out arguments. Code: inherit_constructors (sema.cpp:6089) copies the base New's parameter symbols (add_symbol(std::move(taken))) during link_constructors(), which runs at sema.cpp:417, before check_defaults at 419. The copies have has_default but no value, so lower.cpp default_of gives zero_value. This has the same root cause as finding 1. Silent wrong values, so high.

## S5-debug-3 (medium): A width on a number's debug form aligns left at run time but right when folded or through Format

Status: fixed in db34fdef (a number's padded debug form goes right, and std's box shows as the value in it), with cases in tests/programs/debug_form.fb

Where: compiler/src/lower.cpp:5998

With kind '?' and a width, lowering turns the number into its text and pads it through FORMAT_TEXT with string=false, the TEXT subject, which aligns left. sema's constant folding (formatted()) and the runtime's fb_format_int/fb_format_float (padding_of) pad it as a number, which aligns right. So the same spec gives different text depending on whether the string folds. "[{5:6?}]" alone folds to right-aligned, but the same field in a string that also interpolates an Enum (which sema doesn't fold) comes out left-aligned. A Local gives left, and Format(r"[{:6?}]", x) gives right. This breaks the step's rule that folded and run-time text match through one formatter, and the docs' "numbers go right without one". It's the same in debug and release.

```
Import std.core

Enum Suit
    Hearts
End Enum

Local x = 5
Print("[{x:6?}]")
Print("[{5:6?}]")
Print("[{5:6?}] {Suit.Hearts}")
Try
    Print(Format(r"[{:6?}]", x))
Catch e As FormatError
    Print(e.Message)
End Try
```

Expected: [     5]
[     5]
[     5] Hearts
[     5]

Actual: [5     ]
[     5]
[5     ] Hearts
[     5]

Suggested fix: In the '?' path of lowering, pad a number's debug form as a number. For example, for numeric types call FORMAT_INT/FORMAT_UINT/FORMAT_FLOAT with the spec, whose runtime already strips the kind with padding_of, rather than TO_TEXT then FORMAT_TEXT(false). Test a width on a number's '?' in a Local, a folded literal, and Format.

Skeptic: f3.fb printed `[5     ]`, `[     5]`, `[5     ] Hearts`, `[     5]`, the same in debug and --release. In add_formatted (lower.cpp ~5996-6010), the '?' path calls debug_of, which for a number is TO_TEXT, then pads with FORMAT_TEXT(string=false), so it aligns left like text. fb_format_int/float use padding_of and keep numeric (right) alignment, and the folded literal is right-aligned too. Spec 14's table says "numbers go right without one". The same field gives different text depending on whether the string folds.

## S5-debug-4 (medium): A value passed through a Formattable... parameter shows std's Formatted<T> box in its debug form

Status: fixed in db34fdef (a number's padded debug form goes right, and std's box shows as the value in it), with a case in tests/programs/debug_form.fb

Where: std/core/format.fb:15

A value that isn't an object is boxed in std's Package class Formatted<T> for a `values: Formattable...` parameter. "{v}" shows the value, but "{v:?}" shows the box's Fields by its Class's generated Debug, which exposes an internal Package type the program never wrote. Any logging helper that takes Formattable... and shows debug forms gets this.

```
Import std.core

Function Shown(values: Formattable...): String
    Local text = ""
    For v In values
        text += "{v:?} "
    End For

    Return text
End Function

Print(Shown(5, "a", True))
```

Expected: 5 "a" True 

Actual: Formatted<Int32> { Value: 5 } Formatted<String> { Value: "a" } Formatted<Bool> { Value: True } 

Suggested fix: Make the box transparent to the debug form: in debug_body, for the type whose definition is m_analysis.formatted_box, return the debug form of its Value Field at the same indent. Add the case to debug_form.fb.

Skeptic: f4.fb printed `Formatted<Int32> { Value: 5 } Formatted<String> { Value: "a" } Formatted<Bool> { Value: True } `. Formatted<T> is a `Package Final Class` in std/core/format.fb:15, which the program can't name. Spec 14 doesn't mention the box anywhere, and it says `{x:?}` shows a value "as code would write it". Here the code wrote 5, "a", and True, so the internal box showing up contradicts that.

## S5-debug-5 (medium): Format(r"{:?}", x) throws BAD_VALUE for every program type, though '?' is every value's debug form

Status: fixed in 874f6d79 (format's debug fields give a program's own type its debug form, which its format doesn't take), with cases in tests/programs/format_template.fb

Where: std/core/format.fb:67

Format passes a field's spec to value.Format(spec). The numbers, String, Bool, and Enums answer '?' and '#?' with their debug forms (the runtime's padding_of). A Struct or Class of the program's own that implements Formattable gets "?" in its own Format, gives Null, and Format throws BAD_VALUE. In interpolation the debug form is the compiler's for every value (spec 14: "Its debug form, `?`, is still the compiler's"), so the same field works as "{m:?}" but fails through Format.

```
Import std.core

Struct Money Implements Formattable
    Field Cents: Int

    Method ToString(): String => "${Cents}"

    Method Format(spec: String): String?
        If spec == "c" Then Return "{Cents}c"
        Return Null
    End Method
End Struct

Local m = Money(Cents: 5)
Print("{m:?}")
Try
    Print(Format(r"{:?} {:?}", 5, "a"))
    Print(Format(r"{:?}", m))
Catch e As FormatError
    Print("error: {e.Message}")
End Try
```

Expected: Money { Cents: 5 }
5 "a"
Money { Cents: 5 }

Actual: Money { Cents: 5 }
5 "a"
error: value 0, $5, doesn't take the format spec '?'

Suggested fix: In Format, give a spec whose kind is '?' the value's debug form, "{value:?}" or "{value:#?}", padded by the spec's fill, align, and width, rather than calling value.Format. This needs S5-debug-4's fix so boxed values don't show Formatted<T>. Otherwise, say in spec 14 that Format's '?' is only the built-in types'. Add a test.

Skeptic: f5.fb printed `Money { Cents: 5 }`, `5 "a"`, then `error: value 0, $5, doesn't take the format spec '?'`. Format (std/core/format.fb:67) calls value.Format(specs[i]) for every non-empty spec, and Money's own Format returns Null for "?". Spec 14's kind row says `?` is for "any value", and its Formattable section says a program type's debug form "is still the compiler's". Format's BAD_VALUE is only for "a spec its value doesn't take", so throwing for '?' on a program type contradicts the spec. The same '?' on the built-in types works through Format.

## S5-format-parse-4 (medium): Format's "{:?}" throws BAD_VALUE for a program's Formattable type instead of giving its debug form

Status: fixed in 874f6d79 (format's debug fields give a program's own type its debug form, which its format doesn't take), with cases in tests/programs/format_template.fb

Where: std/core/format.fb:67

In an interpolation, "{m:?}" is the compiler's debug form for every value, a program's Formattable types too (spec 14: "Its debug form, ?, is still the compiler's"). format.fb says Format's fields take interpolation's specs, and spec 14's kind table lists '?' for any value. But Format passes "?" to the value's own Format, so a Struct or Class with its own specs throws FormatError BAD_VALUE. Builtins pass only because their Format handles '?'.

```
Import std.core

Struct Money Implements Formattable
    Field Cents: Int64

    Method ToString(): String => "{Cents} cents"

    Method Format(spec: String): String? => spec == "c" ? "{Cents}c" : Null
End Struct

Local m = Money(5)
Print("{m:?} {5:?} {"a":?}")
Try
    Print(Format(r"{:?} {:?} {:?}", m, 5, "a"))
Catch e As FormatError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: Money { Cents: 5 } 5 "a"
Money { Cents: 5 } 5 "a"

Actual: Money { Cents: 5 } 5 "a"
BAD_VALUE: value 0, 5 cents, doesn't take the format spec '?'

Suggested fix: In Format, when a field's spec kind is '?' ("?" or "#?", with any fill, align, or width), give "{value:?}" or "{value:#?}" padded by the spec through the runtime's text formatter, rather than calling value.Format. Give Formatted<T> its Value's debug form (see S5-format-parse-8) so the box doesn't show. Add the case to format_template.fb. Or, if this is intended, say in spec 14 that Format's '?' goes to the value's own Format.

Skeptic: Ran f4.fb, debug and release: prints 'Money { Cents: 5 } 5 "a"', then 'BAD_VALUE: value 0, 5 cents, doesn't take the format spec '?''. std/core/format.fb's Format calls value.Format(specs[i]) for every spec that isn't empty, "?" too, so a program's Formattable that returns Null for '?' throws. That contradicts format.fb's own header ('Format, whose fields take interpolation's specs'). It also contradicts spec 14: the kind table lists '?' for 'any value', the Formattable section says the debug form '?' 'is still the compiler's', and BAD_VALUE is only 'for a spec its value doesn't take'. Builtins pass only because their Format handles '?'.

## S5-format-parse-5 (medium): A precision of .999 on a Float64 of 1e100 or more writes 1100 NUL bytes

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp

Where: runtime/include/fb/text_format.hpp:244

float_chars formats into a std::array<char, 1100> and ignores to_chars' error. Fixed notation with the allowed maximum precision of 999 needs up to 309 integer digits + '.' + 999 (two more for '%'), which is 1311 characters. From 1e100 up, to_chars fails with value_too_large and returns ptr == end, so the text is the whole zeroed buffer. Both the run-time path (Format and fb_format_float) and the compiler's constant folding produce the same garbage.

```
Import std.core

Local big: Float64 = 1e100
Local text = Try? Format(r"{:.999}", big) ?? "error"
Print(text == "{big:.999}")
Print(text)
```

Expected: True
10000000000000000159028911097599180468360808563945281389781327557747838772170381060813469985856815104.000... (1101 characters: the 101 digits, '.', and 999 zeros)

Actual: True
(1100 NUL bytes)

Suggested fix: Size the buffer for the worst case: at least 309 + 2 ('%' times 100) + 1 + 999 characters, so 1400 is safe. Or check result.ec and retry with a larger buffer. Add text_format_tests cases for 1.7976931348623157e308 with ".999" and ".999%".

Skeptic: Ran f5.fb, debug and release, with output sent to a file. The first line is 'True', then the text is 1100 NUL bytes (od -c shows \0 throughout; the file is 1179 bytes, and 79 after tr -d '\0'). text_format.hpp float_chars uses std::array<char,1100> and returns {buffer.data(), result.ptr} without checking result.ec. For 1e100 with .999, fixed notation needs 101+1+999 = 1102 chars, so to_chars fails with ptr == end, and the whole zeroed buffer becomes the text. The interpolation and Format agree (True) because both use the same header. The spec allows precision up to 999, and this gives corrupt text.

## S5-format-parse-6 (medium): '%' writes "inf%" for a large finite float, and "Inf" with no '%' for infinity

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp

Where: runtime/include/fb/text_format.hpp:625

format_float's '%' case formats magnitude * 100 in the value's own type. Above about 1.8e306 for a Float64, or about 3.4e36 for a Float32, that overflows to infinity, which std::to_chars writes as lowercase "inf". That is wrong for a finite value and breaks the invariant "Inf" text. Separately, the non-finite branch returns before the suffix is added, so infinity and NaN with '%' lose the '%'.

```
Import std.core

Local huge: Float64 = 1e307
Local big: Float32 = 1e37
Local inf = Float64.Parse("Inf") ?? 0
Print(Try? Format(r"{:%} {:%} {:%}", huge, big, inf) ?? "error")
```

Expected: 1 followed by 309 zeros, then '%'; then 1 followed by 39 zeros, then '%'; then Inf%

Actual: inf% inf% Inf

Suggested fix: For '%', format the magnitude's own digits and move the decimal point two places right in the text. This is exact and can't overflow. Alternatively, multiply in a wider type and handle overflow. Add the '%' suffix after NaN and Inf too ("Inf%", "NaN%"), and add these to text_format_tests.

Skeptic: Ran f6.fb, debug and release: prints 'inf% inf% Inf'. Ran my own g6.fb: '{ratio:.1%}' with ratio = 0/0 prints 'NaN', '{(1/zero):%}' prints 'Inf', and a Float64 1e307 prints 'inf%' at run time and as a folded Const. text_format.hpp format_float: the non-finite branch returns before suffix '%' is added, and case '%' computes magnitude * 100 (cast to float for Float32), which overflows to infinity, and std::to_chars writes it as lowercase 'inf'. This contradicts spec 14, which says '%' is 'the value times 100, then %' and that text is invariant 'NaN', 'Inf', '-Inf'. A NaN ratio losing its '%' is a plausible case.

## S5-specs-1 (medium): A large Float with a long precision prints 1100 NUL bytes

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp

Where: runtime/include/fb/text_format.hpp:244

float_chars writes into a fixed 1100-byte buffer and never checks to_chars' result. A Float64 at or above about 1e100 with a precision near the allowed maximum of 999 needs up to 309 + 1 + 999 characters. to_chars then fails with value_too_large and returns ptr == last, so the function returns the whole zero-filled buffer. Because the compiler's folding uses the same formatter, folded constants, run-time values, Format, and debug and release builds all print the same garbage. 'f', '%' and the default fixed form with a precision are all affected.

```
Import std.core

Const FOLDED: Float64 = 1e200

Function Show(big: Float64)
    Print("{big:.999}")
    Print("{FOLDED:.999}")
End Function

Show(1e200)
```

Expected: Each line is 1200 characters long: the 200 digits of the double nearest 1e200 ("99999999999999996973312221251036165947..."), a '.', and 999 zeros. Python's format(1e200, '.999f') gives the same text.

Actual: Each line is 1100 NUL bytes (od -c shows only \0), with no digits.

Suggested fix: Size the buffer for the worst case: sign, 309 integer digits, '.', and 999 decimals, so about 1400 bytes. Also check result.ec and treat a failure as an internal error, so it can't return the buffer's contents silently.

Skeptic: I ran s1.fb (the finding's program) from rev-scratch/s5/specs. In both the debug and the --release build, each line is 1100 NUL bytes: the output holds 2200 NULs and no digits after 'Running'. text_format.hpp:244 declares `std::array<char, 1100> buffer{}`, and line 255 returns `{ buffer.data(), result.ptr }` without ever checking result.ec. For 1e200 with .999, to_chars needs 201+1+999 chars, fails with value_too_large, and returns ptr == last. Spec 14 allows precision up to 999, so this is valid input that silently comes out as garbage. The folded Const line comes out the same, because the compiler uses the same header.

## S5-specs-2 (medium): '%' without a precision prints float noise for common ratios

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp; 'f' without a precision writes the shortest digits too, which spec 14 promises

Where: runtime/include/fb/text_format.hpp:625

With '%', format_float multiplies the value by 100 in double and writes the shortest text of that product. The product's error shows up in the output: 0.57 becomes 56.99999999999999 and 0.07 becomes 7.000000000000001. Spec 14 says "f, e, E, and % without a precision write the shortest digits that read back". The shortest digits of 0.57 are "57", so the percent should be 57%, in the same way that "{x}" prints 0.57 and not 0.56999999999999995. Normal code like "{ratio:%}" hits this. Format and the folded constant path go through the same formatter, so they print the same noise.

```
Import std.core

Function Show(hit: Float64, tax: Float64, crit: Float64)
    Print("{hit:%} {tax:%} {crit:%}")
    Print(Try? Format(r"{:%}", hit) ?? "-")
End Function

Show(0.57, 0.07, 0.29)
```

Expected: 57% 7% 29%
57%

Actual: 56.99999999999999% 7.000000000000001% 28.999999999999996%
56.99999999999999%

Suggested fix: When '%' has no precision, take the value's shortest round-trip digits and exponent (to_chars scientific, shortest), add 2 to the exponent, and write that decimal in fixed form. This moves the decimal point exactly instead of multiplying in floating point. With a precision, the same exact shift avoids double-rounding too.

Skeptic: I ran s2.fb in debug and release. Both print '56.99999999999999% 7.000000000000001% 28.999999999999996%' and '56.99999999999999%', exactly as reported. text_format.hpp:626 does `chars(magnitude * 100, std::chars_format::fixed)`, so it writes the shortest text of the rounded double product. Spec 14:61-62 says f, e, E, and % without a precision write 'the shortest digits that read back'. For 0.57, '57' is shorter and reads back exactly (57/100 rounds to the same double), so the noisy output contradicts the spec and the 0.1-not-0.100000001 intent at spec 14:28.

## S5-specs-4 (medium): An interpolation with a format spec skips the main-thread check of the code it runs

Status: fixed in 18838775 (a value shown by a format spec counts what it runs, for work on another thread), with a case in tests/compiler/sema_tests.cpp

Where: compiler/src/sema.cpp:16071

check_expr(string_expr_t) calls note_shown(type, span) only when there is no format spec. note_shown is what records that showing a value runs its ToString, so that work on another thread that reaches main-thread-only state is rejected. With any spec, the check is lost. This covers a ToString padded by a spec, a program Formattable's own Format, a tuple of Printables, and the debug form's call into a @DebugElements type's Iterate through ElementsOf. All of these compile and run on the spawned thread, reading and writing the main thread's Global. The same value without a spec is a compile error.

```
Import std.core
Import std.collections

Global score = 0

Struct Shown Implements Printable
    Field n: Int
    Method ToString(): String => "{score}"
End Struct

Struct Money Implements Formattable
    Field n: Int
    Method ToString(): String => "m"
    Method Format(spec: String): String? => "{score}"
End Struct

@DebugElements
Class Bag Implements Iterable<Int>
    Field Items: Int[] = []
    Method Iterate(): ArrayIterator<Int>
        score += 1
        Return Items.Iterate()
    End Method
End Class

// Without a spec, the first is an error; each of the others reaches 'score' just the same.
// Local plain = Spawn(Function() => "{Shown(1)}")
Local a = Spawn(Function() => "{Shown(1):>5}")
Local b = Spawn(Function() => "{Money(1):x}")
Local c = Spawn(Function() => "{(Shown(1), 2):>9}")
Local d = Spawn(Function() => "{Bag():?}")
Print(a.Wait() != "", b.Wait() != "", c.Wait() != "", d.Wait())
```

Expected: Four compile errors, one at each Spawn. Each should match what the commented-out line gives when it's uncommented: "this work runs on another thread, but it calls 'Shown.ToString', which uses the Global 'score', which only the main thread can, unless it's an Atomic or a Mutex". For the others, the error names Money.Format and Bag.Iterate.

Actual: It compiles and runs: "True True True Bag []". The spawned work read and wrote the main thread's Global 'score'.

Suggested fix: In check_expr(string_expr_t), note what the formatted value runs. Call note_shown(type, span_of(inner)) for a spec whose kind isn't '?'. For a program Formattable type, also note_thread_use its Format method. For the debug form of a type with @DebugElements or @DebugEntries, note ElementsOf and its Iterate. Add a sema test next to the existing "Showing a value runs its ToString" cases.

Skeptic: s4.fb compiles and prints 'True True True Bag []': all four Spawns that carry a spec ran on another thread and reached the Global 'score'. s4b.fb is the same program with the commented-out spec-less line enabled, and it fails with 'this work runs on another thread, but it calls 'Shown.ToString', which uses the Global 'score'...'. In sema.cpp:16070-16076, check_expr(string_expr_t) calls note_shown only in the else branch where interpolation.format is absent. check_format (sema.cpp:16130-16172) does no note_shown or note_thread_use, so a padded ToString, a program Formattable's Format, a tuple's parts, and the @DebugElements ElementsOf/Iterate call all skip the main-thread check.

## S5-specs-5 (medium): ',' grouping and '0' padding on a round Float write the exponent form

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp

Where: runtime/include/fb/text_format.hpp:632

With no kind and no precision, format_float uses shortest_text. That is to_chars' shortest form, and it switches to an exponent for round values from 1e5 up (1e5, 2e6, 3e5 for a Float32). Grouping and zero padding are then applied to that form, so "{gold:,}" prints 2e6 for 2000000 but 2,000,001 for 2000001. "{gold:012}" prints 0000000002e6. Spec 14 says ',' goes "between each 3 digits of a decimal", and Python gives 2,000,000.0 here. The result changes from value to value in ordinary code like "{gold:,}".

```
Import std.core

Function Show(gold: Float64, more: Float64, small: Float32)
    Print("{gold:,}|{more:,}|{gold:012}|{more:012}|{small:,}")
End Function

Show(2000000, 2000001, 300000)
```

Expected: 2,000,000|2,000,001|000002000000|000002000001|300,000

Actual: 2e6|2,000,001|0000000002e6|000002000001|3e5

Suggested fix: When a spec has grouping or '0' (or a width) but no kind and no precision, write the shortest digits in fixed form, as 'f' already does (chars(magnitude, fixed) with no precision), instead of shortest_text's exponent form.

Skeptic: I ran s5.fb in debug and release. Both print '2e6|2,000,001|0000000002e6|000002000001|3e5'. In format_float's default case (text_format.hpp:631-632), no kind and no precision means shortest_text, and to_chars' shortest picks '2e+06' over '2000000'. The grouping step (636-641) then groups only the mantissa before 'e'. An extra probe gave '{gold:f}' = 2000000 and '{gold:,f}' = 2,000,000, so only the spec without a kind is wrong. Thousands grouping is one of milestone row 5's features, and spec 14:57 says ',' goes between each 3 digits of a decimal. In practice '{x:,}' gives no grouping for any round Float of 1e5 or more (1.2e6, 3e5), while its neighbours get grouped.

## S5-debug-10 (low): No test checks the multi-dimensional fixed array's one-list debug form, or '?' through Format

Status: fixed in 924d9e9c (a flags value's debug form shows the bits no member has, and the debug form of a fixed array of several dimensions has a test), with fixed arrays in tests/programs/debug_form.fb; Format's '?' fields are in format_template.fb, from the S5-debug-5 fix

Where: tests/programs/debug_form.fb:90

Two behaviours of this step have no test. The lane's call that a fixed array of several dimensions shows its elements in one list isn't in debug_form.fb, mir_tests, or sema_tests: debug_form.fb has no fixed array at all. The runtime's '?' handling in fb_format_int, fb_format_float, and fb_format_text (padding_of and the quoted String), which Format(r"{:?}") goes through, isn't tested either. format_template.fb has no '?' field, and S5-debug-3 and S5-debug-5 are both in that path.

```
Import std.core

Local grid: Int[2, 3]
grid[0, 0] = 1
grid[1, 2] = 6
Print("{grid:?}")
Try
    Print(Format(r"[{:4?}] [{:?}]", 1.5, "a"))
Catch e As FormatError
    Print(e.Message)
End Try
```

Expected: These outputs pinned by a program test: [1, 0, 0, 0, 0, 6] and [ 1.5] ["a"]

Actual: Prints [1, 0, 0, 0, 0, 6] and [ 1.5] ["a"], but no test checks either

Suggested fix: Add a fixed Int[2, 3] (alone, in a Struct, and with Dump) and Format '?' fields (number, String, Enum, padded, and a program type once S5-debug-5 is decided) to debug_form.fb or format_template.fb.

Skeptic: f10.fb prints `[1, 0, 0, 0, 0, 6]` and `[ 1.5] ["a"]`, but no test checks either. debug_form.fb's only array literals are dynamic ([3, 1], [10, 20]), and it has no fixed array. The step's new tests have no multi-dimensional fixed-array debug case. format_template.fb has no '?' field, and runtime_tests.cpp doesn't call fb_format_int, fb_format_float, or fb_format_text with '?'. So the runtime's padding_of and quoted-String paths, which Format(r"{:?}") uses, have no test, and S5-debug-3 and S5-debug-5 are bugs in exactly that area. The project's rule that every behavior is pinned by a test makes this a real gap.

## S5-debug-7 (low): Spec 14 says an Atomic's or Mutex's debug form can be shown, but "{x:?}" on one is an error

Status: fixed in b2021f99 (spec 05, spec 14, and stdlib.md say what step 5's review fixed), docs only: spec 14 says an Atomic's or Mutex's debug form is inside what holds it, and lists Scheduler, CString, and Ptr<T>

Where: docs/spec/14-strings-collections.md:32

Spec 14 says a Task, Thread, Channel, Scheduler, Atomic, or Mutex "has no text, so it can't be shown, though its debug form can", and the debug-form section (line 138) says an Atomic or Mutex shows its type's name. An Atomic or Mutex can only be a Global, Static Field, or Class Field, and "{hits:?}" on any of them is a copy error. Its debug form appears only inside what holds it, as Counter { Hits: Atomic<Int32> }. The debug-form list also leaves out Scheduler, CString, and Ptr<T>, which show their type's names too ("{sc:?}" gives Scheduler, a CString gives CString, and a Ptr<Int32> gives Ptr<Int32>).

```
Import std.core

Class Counter
    Field Hits: Atomic<Int>
End Class

Global hits = Atomic<Int>(3)
Local c = Counter()
Print("{c:?}")
Print("{hits:?}")
```

Expected: Per spec 14: Counter { Hits: Atomic<Int32> } then Atomic<Int32>

Actual: error at 10:9: Atomic<Int32> stays where it's made, so it can't be copied; use its Methods, like Load()

Suggested fix: Say in spec 14 that an Atomic's or Mutex's debug form shows only inside what holds it (or let "{x:?}" read one without a copy). Add Scheduler, CString, and Ptr<T> to the list of values that show their type's name.

Skeptic: f7.fb fails with `10:9: error: Atomic<Int32> stays where it's made, so it can't be copied; use its Methods, like Load()` on Print("{hits:?}"). f7b.fb shows `Holder { Guard: Mutex<Int32> }`, `Scheduler`, and `Ptr<Int32>`, so these show only inside what holds them, or as their type's name. The diff shows that spec 14's line "A Task, Thread, Channel, Scheduler, Atomic, or Mutex has no text, so it can't be shown, though its debug form can" is new in this step, and for an Atomic or Mutex "{x:?}" is an error. The debug-form list (line 138) also leaves out Scheduler, which line 32 includes, and Ptr<T>. The behavior is right; the docs are wrong.

## S5-debug-8 (low): A @Flags value with bits outside its members shows the same debug form as a member

Status: fixed in 924d9e9c (a flags value's debug form shows the bits no member has, and the debug form of a fixed array of several dimensions has a test), with a case in tests/programs/debug_form.fb

Where: compiler/src/lower.cpp:6094

debug_of() shows an Enum through add_text, which shows a @Flags value's member flags and drops any bit no member has. So `9 As Layer` (World plus bit 8) and Layer.World both show as World, though they're !=. A debug form should tell different values apart. The same text comes from "{x}", so the root is in the Enum text, which predates this step, but the debug form inherits it.

```
Import std.core

@Flags Enum Layer
    World, Player, Trigger
End Enum

Local odd = 9 As Layer
Print("{odd:?} {Layer.World:?} {odd == Layer.World}")
```

Expected: A form that keeps bit 8, e.g. World | 8, then World False

Actual: World World False

Suggested fix: Show a @Flags value's leftover bits, e.g. as a number after the member flags (World | 8). Test it in debug_form.fb.

Skeptic: f8.fb printed `World World False`. A more natural case, f8b.fb with `Local m = ~Layer.World`, printed `Player | Trigger False`: the debug form looks like Player | Trigger, but the value isn't equal to it. Spec 06 lets As make any @Flags value, and ~ sets bits that no member has. debug_of (lower.cpp ~6090) reuses add_text for Enums, which drops those bits. Spec 14 says the debug form shows a value "as code would write it", and "World" written in code is a different value. The text came from the existing Enum code, but the debug form inherits it, so low.

## S5-debug-9 (low): Grid and Grid3, std collections, don't show their elements as the docs say std's collections do

Status: fixed in b2021f99 (spec 05, spec 14, and stdlib.md say what step 5's review fixed), docs only: spec 14 and stdlib.md say a Grid and a Grid3 show their Fields, their cells and sizes, which their elements alone would leave out

Where: std/collections/grid.fb:9

Spec 14 and stdlib.md say std's collections show their elements, as Stack<Int32> [1, 2]. Every Iterable Class in std/collections has @DebugElements or @DebugEntries except Grid<T> and Grid3<T>. Those show their private Fields instead: the Cells array, and Columns and Rows (plus Layers for Grid3).

```
Import std.core
Import std.collections

Local stack = Stack<Int>()
stack.Push(1)
Local grid = Grid<Int>(2, 1, 7)
Print("{stack:?}")
Print("{grid:?}")
```

Expected: Stack<Int32> [1]
Grid<Int32> [7, 7]

Actual: Stack<Int32> [1]
Grid<Int32> { Cells: [7, 7], Columns: 2, Rows: 1 }

Suggested fix: Add @DebugElements to Grid<T> and Grid3<T>. If showing the size is intended, say in spec 14 and stdlib.md that a Grid shows its Fields.

Skeptic: f9.fb printed `Stack<Int32> [1]` and `Grid<Int32> { Cells: [7, 7], Columns: 2, Rows: 1 }`. grep over std/collections shows that every public Iterable Class has @DebugElements or @DebugEntries except Grid<T> (grid.fb:9) and Grid3<T> (grid.fb:140). Spec 14 and stdlib.md say std's collections show their elements, as Stack<Int32> [1, 2], with no exception for Grid. Either the code or the docs needs to change.

## S5-format-parse-12 (low): Spec 05 doesn't document `values: Formattable...` parameters, which box their arguments

Status: fixed in b2021f99 (spec 05, spec 14, and stdlib.md say what step 5's review fixed), docs only: spec 05 has a values: Formattable... bullet; its decisions.md row is in lane 2's report

Where: docs/spec/05-functions.md:42

symbol_t::formats gives any function with a `values: Formattable...` parameter a new rule: values of any Formattable types, with non-objects boxed in std's Formatted<T>, and an error "T isn't Formattable, which 'values' takes". Spec 05's variadic section documents only `values: Printable...`, spec 14 gives only Format's signature, and decisions.md has no row for it. A program's own function gets the behavior without any documentation.

```
Import std.core

Function Log(values: Formattable...): String
    Local out = ""
    For v In values
        out += "[" + (v.Format(">3") ?? "?") + "]"
    End For
    Return out
End Function

Print(Log(1, 2.5, "x", True))
```

Expected: [  1][2.5][  x][True], as documented in spec 05

Actual: [  1][2.5][  x][True], but spec 05 says nothing about a Formattable... parameter

Suggested fix: Add a `values: Formattable...` bullet to spec 05's variadic section, beside Printable...'s: any Formattable types, non-objects boxed, a lone Formattable[] passed whole, and the error's text. Add a matching row to docs/decisions.md.

Skeptic: Ran f12.fb: prints '[  1][2.5][  x][True]'. sema.cpp:7016 gives any program function's 'values: Formattable...' parameter the 'formats' rule: non-object arguments are boxed in std's Formatted<T>, with the error 'T isn't Formattable, which ...'. Under spec 05's general variadic rule (arguments are checked against the element type) and g10a.fb's result ('Local x: Formattable = 3' is an error), Log(1, ...) would be rejected. So the code applies a language rule the spec doesn't state. Spec 05 documents the matching 'values: Printable...' special case, but has nothing for Formattable...; spec 14 gives only Format's signature, and decisions.md has no row. This is a documentation gap in the source of truth, so low.

## S5-format-parse-3 (low): Float64.Parse and Float32.Parse give Null for a number that underflows to zero

Status: fixed in c26b4e1b (parse reads a float too small for its type as 0, and -0 as an unsigned 0), with cases in tests/programs/parse.fb

Where: runtime/src/format.cpp:230

fb_parse_float rejects any fast_float result whose ec isn't errc{}. fast_float reports result_out_of_range on underflow as well as overflow, so very small decimals give Null: "1e-400" and "2e-324" for Float64, and "1e-50" and "1e-46" for Float32. A slightly larger "3e-324" reads as 5e-324, so the cutoff is arbitrary. Float32.Parse's doc comment and parse.fb's header both say Null is for a number "too big" for its type. Python, Rust, C#, Java, and JS all read these as 0.

```
Import std.core

Function Shown<T: Printable>(value: T?): String => value == Null ? "-" : "{value}"

Print("{Shown(Float64.Parse("1e-400"))} {Shown(Float64.Parse("2e-324"))} {Shown(Float64.Parse("3e-324"))}")
Print("{Shown(Float32.Parse("1e-50"))} {Shown(Float32.Parse("1e-46"))} {Shown(Float32.Parse("1e-45"))}")
```

Expected: 0 0 5e-324
0 0 1e-45

Actual: - - 5e-324
- - 1e-45

Suggested fix: In both branches of fb_parse_float, accept result_out_of_range when the parsed number isn't infinite (an underflow to 0), and reject only an overflow to infinity. Add the underflow cases to parse.fb. If underflow is meant to be Null, say so in spec 14 and in Float32.Parse's comment instead.

Skeptic: Ran f3.fb, debug and release: prints '- - 5e-324 / - - 1e-45'. Float64.Parse("1e-400"), Float64.Parse("2e-324"), Float32.Parse("1e-50"), and Float32.Parse("1e-46") all give Null. runtime/src/format.cpp fb_parse_float rejects any result whose ec isn't errc{}, and fast_float reports result_out_of_range for underflow as well as overflow. This contradicts Float32.Parse's own comment in std/core/parse.fb:105 ('Null for text that isn't a float or one too big for a Float32'). The cutoff is also arbitrary: 3e-324 rounds and is accepted, while 2e-324 gives Null. Spec 14's 'a number its type can't hold' is ambiguous, but values that underflow are valid number text. Rare input, so low.

## S5-format-parse-7 (low): An unsigned type's Parse gives Null for "-0"

Status: fixed in c26b4e1b (parse reads a float too small for its type as 0, and -0 as an unsigned 0), with a case in tests/programs/parse.fb

Where: runtime/src/format.cpp:186

fb_parse_uint rejects any text with a '-' sign, even when the magnitude is 0. Spec 14 says an integer is "a sign, then digits", and Null is for text that isn't one or a number the type can't hold. -0 is 0, which every unsigned type holds, and Int.Parse("-0") and UInt8.Parse("+0") both give 0.

```
Import std.core

Print("{UInt64.Parse("-0") ?? 99} {UInt8.Parse("+0") ?? 99} {Int.Parse("-0") ?? 99}")
```

Expected: 0 0 0

Actual: 99 0 0

Suggested fix: In fb_parse_uint, reject a negative sign only when the magnitude isn't 0. Or, if unsigned types take no '-', say so in spec 14's Parsing section. Add the case to parse.fb either way.

Skeptic: Ran f7.fb, debug and release: prints '99 0 0'. UInt64.Parse("-0") gives Null, while UInt8.Parse("+0") and Int.Parse("-0") give 0. runtime/src/format.cpp fb_parse_uint returns 0 whenever negative is set, even for a magnitude of 0. Spec 14 says an integer is 'a sign, then digits of the base', and Null is only for text that isn't one 'or a number its type can't hold'. -0 is 0, which every unsigned type holds. Edge case, so low.

## S5-specs-3 (low): '%' writes lowercase "inf%" when the product overflows, and drops the '%' for Inf and NaN

Status: fixed in be577422 (a float's fixed text is its shortest digits, a percent moves the point, and the longest text fits), with cases in tests/compiler/text_format_tests.cpp

Where: runtime/include/fb/text_format.hpp:600

format_float checks isfinite before it multiplies by 100. A finite Float64 at or above about 1.8e306, or a Float32 above about 3.4e36 (whose product is cast back to float), becomes infinite after the multiply, and to_chars writes it as "inf". FutureBlitz's invariant text never uses that spelling. A value that is already Inf or NaN takes the early non-finite branch, which never appends the '%' suffix. Spec 14 says '%' is "the value times 100, then %".

```
Import std.core

Function Show(huge: Float64, inf: Float64, nan: Float64, big: Float32)
    Print("{huge:%}|{inf:%}|{inf:.1%}|{nan:%}|{big:%}|{inf}")
End Function

Local zero: Float64 = 0
Show(1e307, 1 / zero, zero / zero, 1e37)
```

Expected: The first and fifth fields are the values' digits with the point moved two places, a '1' followed by zeros, then '%'. The Inf and NaN fields are Inf%|Inf%|NaN%. The line ends with |Inf.

Actual: inf%|Inf|Inf|NaN|inf%|Inf

Suggested fix: Move the decimal point by digits instead of multiplying (see S5-specs-2), so the product can't overflow. In the non-finite branch, append the '%' suffix when the kind is '%'.

Skeptic: I ran s3.fb in debug and release. Both print 'inf%|Inf|Inf|NaN|inf%|Inf'. format_float's non-finite branch (text_format.hpp:600-602) returns 'NaN'/'Inf' before the '%' suffix is set, so Inf and NaN lose the '%'. Line 626 multiplies by 100 after the isfinite check, so 1e307 (and the Float32 1e37, cast back to float) overflow to infinity, which to_chars writes as lowercase 'inf'. That breaks the invariant spelling Inf in spec 14:29 and '% is the value times 100, then %' in spec 14:62. An extra check printed '{inf:+%}' as '+Inf', again without the '%'.

## S5-specs-7 (low): An empty spec on a program's Formattable type panics, though Format shows its ToString

Status: fixed in c3d4838a (an empty format spec is none, a program's formattable type's too), with a case in tests/programs/format_specs.fb

Where: compiler/src/lower.cpp:6019

"{m:}" parses as an empty spec, and add_formatted passes it to the type's own Format(""). For a type whose Format doesn't take "", it panics. Format's field "{0:}" treats an empty spec as no spec (format.fb line 67) and shows ToString. The empty spec is a no-op for every built-in type, so the two paths disagree only for program types.

```
Import std.core

Struct Money Implements Formattable
    Field Cents: Int64
    Method ToString(): String => "{Cents} cents"
    Method Format(spec: String): String? => spec == "c" ? "{Cents}c" : Null
End Struct

Local m = Money(5)
Print(Try? Format(r"{0:}", m) ?? "thrown")
Print("{m}")
Print("{m:}")
```

Expected: 5 cents
5 cents
5 cents

Actual: 5 cents
5 cents
panic: Money doesn't take the format spec '' at t20.fb:12:11

Suggested fix: In add_formatted (and in sema's check_format), treat an empty spec as no spec and use add_text/ToString, the way Format does.

Skeptic: s7.fb prints '5 cents', '5 cents', then 'panic: Money doesn't take the format spec '' at s7.fb:12:11'. An extra probe, s7b.fb, shows that an empty spec is a no-op everywhere else: Int, Float, String, Bool, a Printable struct, and a generic T: Formattable ('[42] [1.5] [p3] [s] [True]', '[42] [x]'). Format's template treats '{0:}' as no spec (format.fb:67, `specs[i] == "" ? "{value}" : ...`), and format.fb:1 says Format's fields take interpolation's specs. lower.cpp's add_formatted (6015-6018) sends every spec, the empty one included, to own_format for a program Formattable. So the same spec text on the same value gives ToString in Format and a panic in interpolation.

## S5-specs-8 (low): Format's "{:?}" throws BAD_VALUE for a program's Formattable type, though its fields "take interpolation's specs"

Status: fixed in 874f6d79 (format's debug fields give a program's own type its debug form, which its format doesn't take), with cases in tests/programs/format_template.fb

Where: std/core/format.fb:67

format.fb says Format's fields take interpolation's specs, and in an interpolation '?' is the compiler's debug form for every value, a program's Formattable types included (spec 14: "Its debug form, ?, is still the compiler's"). Format passes every spec to value.Format(spec). That works for built-in values, whose Format handles '?', but a program type's Format gets "?" and gives Null, so Format throws BAD_VALUE.

```
Import std.core

Struct Money Implements Formattable
    Field Cents: Int64
    Method ToString(): String => "{Cents} cents"
    Method Format(spec: String): String? => spec == "c" ? "{Cents}c" : Null
End Struct

Function Filled(template: String, values: Formattable...): String
    Try
        Return Format(template, values)
    Catch e As FormatError
        Return "{e.Kind}: {e.Message}"
    End Try
End Function

Local m = Money(5)
Print("{m:?}")
Print(Filled(r"{:?} {:?}", 7, "a"))
Print(Filled(r"{:?}", m))
```

Expected: Money { Cents: 5 }
7 "a"
Money { Cents: 5 }

Actual: Money { Cents: 5 }
7 "a"
BAD_VALUE: value 0, 5 cents, doesn't take the format spec '?'

Suggested fix: For a field whose spec has kind '?', have Format write the value's debug form instead of calling its Format. For example, Formatted<T> could answer '?' with "{Value:?}" padded by the spec, and an object could use its Class's debug form. Otherwise, say in format.fb and spec 14 that '?' in a Format template goes to the value's own Format.

Skeptic: s8.fb prints 'Money { Cents: 5 }', '7 "a"', then 'BAD_VALUE: value 0, 5 cents, doesn't take the format spec '?''. Spec 14:59 lists '?' as a kind 'any value' takes, and 14:95 says a program Formattable's 'debug form, ?, is still the compiler's'. Spec 14:109 has Format throw BAD_VALUE only for 'a spec its value doesn't take', and format.fb:1 says Format's fields take interpolation's specs. Yet format.fb:68 sends '?' to value.Format, and Formatted<T>.Format (format.fb:24) forwards it to Money.Format, which returns Null. Built-in values handle '?' in Format, but a program's own type never does.

## Refuted by the skeptics (no action)

- S5-specs-6: A ternary's ':' at the end of a line in a multi-line string's interpolation starts a format spec. It reproduces: s6.fb gives the three errors listed. But this is the documented rule, and it doesn't break any doc. Spec 14:69 says 'a ':' ... that has no space or tab after it starts the spec', and spec 03:22 says 'a ternary's ':' has a space after it, since a ':' without one starts a format spec'. lexer.cpp:243-244 implements exactly that (peek(1) != ' ' && peek(1) != '\t'), and it's the lane's listed decision. Even if it ran to the '}', the spec text would be invalid anyway. Treating a line break as a space would be a nice extension, but it isn't a bug against the spec.
- S5-specs-9: Spec 14 lists the space sign, but an interpolation's spec can't start with it. It reproduces: s9.fb fails at line 6 with 'a format spec comes right after its ':', with no space'. Without that line, Format(r"[{: }] [{: 5}]") and '{n:> }' / '{n:> 5}' both print '[ 42] [   42]'. That's how the documented rules combine, not a contradiction. Spec 14:69 (and 03:22) say a ':' with a space after it isn't a spec, the grammar [[fill]align][sign] allows a space sign after an align, and Format templates take it as written. The behavior matches the spec and the lane decision, and the error says why. A sentence in the docs would help, but there's no wrong behavior and no contradiction.
- S5-format-parse-8: The debug form of a Formattable... parameter shows std's Formatted<T> box. Ran f8.fb, debug and release: prints [Formatted<Int32> { Value: 1 }, Formatted<String> { Value: "a" }, Formatted<Float32> { Value: 2.5 }]. The output is as reported, but it matches the spec: spec 14 says the debug form shows an object 'with every Field, by its object's own Class', and the elements of a Formattable... array really are Formatted<T> objects, under the lane's listed boxing decision. Format's own '{:?}' on a boxed value goes through Value.Format, so the box doesn't show there. This is a cosmetic consequence of an intended design, not a contradiction of the spec, stdlib.md, or the milestones row. Refuted.
- S5-format-parse-9: Inside a Formattable type, Format(template, ...) resolves to its own Format Method, with a misleading error. Ran f9.fb: it gives the reported warning and the error 'too many arguments to 'Format''. Ran my own g9a.fb, a Class with Method Pick(a) that calls Pick(1, 2) while a top-level Function Pick(a, b) exists: it gives the same 'too many arguments to 'Pick''. So a type's own Method hiding a top-level Function is the language's general rule. Spec 06:222 makes an exception only for what Interfaces' defaults give, and Format is the type's own Method, not a default. Ran g9b.fb with core.Format(...) in the Method: prints '212.0F'. The warning is accurate for the Method the call resolved to. This is a documentation and usability suggestion, not a bug. Refuted.
- S5-format-parse-10: An array literal passed to a Formattable... parameter isn't boxed, unlike the same values passed loose. Ran f10.fb: Format(r"{} {}", 3, 4) is fine, and [3, 4] reports 'expected Formattable, found Int32'. Spec 05's variadic rule says 'One argument that's an array already, a T[] or an array literal, is passed as it is', and sema bind() does exactly that (passes_array). Ran g10a.fb ('Local x: Formattable = 3') and g10b.fb ('Local xs: Formattable[] = [3, 4]'): both give the same error, so a non-object is never a Formattable value outside packed variadic arguments. The behavior follows spec 05 and the listed lane decision (only a Formattable... parameter's packed arguments are boxed). Refuted.
- S5-format-parse-11: No panic program tests that Parse panics, at the caller, for a base outside 2 to 36. Ran f11.fb (Int.Parse("1", 37)), g11.fb (UInt8.Parse("1", base=1)), and g11b.fb (Int64.Parse("1", 40)), debug and release. Each prints 'panic: Parse takes a base from 2 to 36, not N at <file>:<line>:7', at the user's call, exit 101. The behavior is right, and there's no contradiction of the spec, stdlib.md, or the milestones row. It's only a missing test (tests/programs has panic_format_generic and panic_format_own only). Refuted under the rule that a finding needs wrong behavior or a plain contradiction.
- S5-debug-6: @DebugElements and @DebugEntries on an Enum, Function, or Field are silently accepted. f6.fb builds and prints `Hearts Holder { Items: [] }`, as the finding says. But @DebugElements and @DebugEntries are ordinary `Public Attribute` declarations in std/core/format.fb, and spec 13 (docs/spec/13-compile-time.md:122-123) says a declared attribute "goes on anything declared outside a body: a Function, Method, Property, Struct, Class, Interface, Enum, Field, Global, or Const". Accepting them silently on an Enum, a Function, or a Field follows that rule, and reflection can still read them. The extra check in check_debug_attributes (sema.cpp:11624) for type statements is std-specific, and nothing in the spec requires it to cover other declarations. Refuted as a design choice, not a contradiction.
