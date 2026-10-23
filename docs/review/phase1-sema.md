# Phase 1's review: sema

The review of phase 1's fixes (ecaa974..36ab52a), on 2026-10-23: six reviewers by area, each finding reproduced by a skeptic. 17 findings for lane 1: 0 high, 13 medium, 4 low. Fix each with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why.

## P1-sema-checks-1 (medium): A Const Function's default checked on demand resolves its names in the caller's scope

Status: open

Where: compiler/src/sema.cpp:19185

This comes from the A4-11 fix (7857407). run_at_compile_time() now calls check_defaults() on demand, but it runs it inside whatever context the call is in: the caller's local scopes, its type scope (a generic type's Const type parameters), m_function, m_substitution and m_bounds. declare_early() swaps those out for this reason; check_defaults() doesn't. Because m_defaults_checked means it runs only once, the first caller's scope fixes the default for every call. A default naming a top-level Const N becomes 5 when the first early run is inside a Const Function with a local `Const N = 5`, and every later call, run-time ones too, uses 5. Called from a Field size inside `Struct G<Const N: Int>`, the same default resolves to G's type parameter N and is refused as 'a default value must be known at compile time' (a separate repro).

```
Import std.core
Const N = 10
Const Function Add(a: Int, b: Int = N): Int => a + b
Const Function Outer(): Int
    Const N = 5
    Return Add(1)
End Function
Const X = Outer()
Print(X)
Print(Add(1))
```

Expected: 11
11  (b's default is the top-level Const N = 10)

Actual: 6
6  (debug and release). Variant: `Struct G<Const N: Int> / Field c: Int8[Add(1)]` with `Local g = G<3>()` reports 'a default value must be known at compile time' at the default N; renaming G's parameter to M makes it compile and print 11.

Suggested fix: Check the defaults on demand the way declare_early() declares a type: in the function's own file, with m_scopes, m_type_scope, m_function, m_substitution and m_bounds swapped for the declaration's own empty context, then restored. Add tests: a default that names a top-level Const, first run from inside a Const Function with a local Const of that name, and from a Field size in a generic type with a Const parameter of that name.

Skeptic: semachk/f1.fb (the finding's program) prints 6 then 6, in debug and release; the top-level Print(Add(1)) should be 11. Control f1d.fb (the same code without Const) prints 11 11. f1b.fb (Struct G<Const N: Int> with Field c: Int8[Add(1)]) reports 'a default value must be known at compile time' at the default N. f1c.fb, with the parameter renamed to M, prints 11 11. Code: run_at_compile_time() (sema.cpp:19180-19186) calls check_defaults(*declared.declaration) right where the call is checked. check_defaults() (12712-12755) swaps only m_file. It keeps the caller's m_scopes, m_type_scope, m_function, and m_substitution, and m_defaults_checked.insert makes the first caller's result stick. declare_early() (19059) and check_const_body() (12846) swap that context out.

## P1-sema-checks-2 (medium): A bad default checked on demand inside a quiet overload try is never reported, and the call silently uses 0

Status: open

Where: compiler/src/sema.cpp:12723

This also comes from the A4-11 fix. If the first on-demand check_defaults() happens inside quietly() (choose_overload checks each argument quietly first), its errors go into the scratch bag. m_defaults_checked is set anyway, so neither the later real check of the call nor the check_defaults pass checks the defaults again. An invalid default (a String or 1.5 for an Int) is then never reported. The program builds, and lowering passes 0 for the parameter. With a non-overloaded F, or with the call in a function body, the same default is reported: 'expected Int32, found String'.

```
Import std.core
Const Function Add(a: Int, b: Int = "x"): Int => a + b
Function F(x: Int): Int => x
Function F(x: String): Int => 0
Global g = F(Add(1))
Print(g)
```

Expected: error: expected Int32, found String (at the default "x")

Actual: Builds with no error and prints 1. The MIR has `call Add(Int32, Int32)(1, 0)`. The same happens with `b: Int = 1.5`.

Suggested fix: Report on-demand default checks to m_bag, the diagnostics kept for on-demand work inside quietly(), as other on-demand checks do. Or don't mark the function in m_defaults_checked while m_diagnostics isn't the kept bag. Add a test with an overloaded outer call in a Global initializer.

Skeptic: semachk/f2.fb builds with no diagnostic and prints 1, in debug and release. `fb check --show mir` shows `_0 = call Add(Int32, Int32)(1, 0)`. f2b.fb, with b: Int = 1.5, prints 1 too. Control f2c.fb, with only one F, reports 'expected Int32, found String' at "x". Code: quietly() (sema.cpp:890) swaps m_diagnostics for a scratch bag. check_defaults() reports through m_diagnostics but marks m_defaults_checked first (12723), so the real check and the pass at 412 skip it. check_const_body() avoids this by swapping m_diagnostics for m_bag (12852); check_defaults() doesn't.

## P1-sema-checks-3 (medium): Compiler crash: a generic Const Function whose type argument misses its constraint still runs

Status: open

Where: compiler/src/sema.cpp:18896

This is a neighbor of A4-3. The fix runs a Const Function only when the error count didn't rise during check_call(). But report_bounds(), which reports a type argument that doesn't meet the function's constraints, runs before the count is taken. So `Largest(True, False)` against `T: Comparable` reports nothing new inside check_call(), runs at compile time with T = Bool, and the evaluator's `<` on Bools throws std::get. The error is never shown. The same function without Const reports 'Bool doesn't implement Comparable, which 'Largest' needs for T'. `Largest<Bool>(True, False)` and `Const Function AddN<T: Numeric>(a: T): T => a + a` with `AddN<Bool>(True)` crash the same way.

```
Import std.core
Const Function Largest<T: Comparable>(a: T, b: T): T => a < b ? b : a
Function Main()
    Local x = Largest(True, False)
    Print(x)
End Function
Main()
```

Expected: error: Bool doesn't implement Comparable, which 'Largest' needs for T

Actual: error: fb failed unexpectedly: std::get: wrong index for variant (fb check exits 1, with no diagnostic)

Suggested fix: Take the error count before report_bounds(), or skip run_at_compile_time() when report_bounds() reported anything. Add a sema test of a generic Const Function called with a type argument that misses its constraint, inferred and written.

Skeptic: semachk/f3.fb (Largest(True, False)) and f3b.fb (Largest<Bool>(True, False)) both make fb print 'error: fb failed unexpectedly: std::get: wrong index for variant'. f3d.fb (Const Function AddN<T: Numeric> with AddN<Bool>(True)) does the same in fb check. Control f3c.fb, without Const, reports 'Bool doesn't implement Comparable, which 'Largest' needs for T'. Code: sema.cpp:18889-18902 calls report_bounds() before taking `errors = m_diagnostics->error_count()`, so the run_at_compile_time() guard the A4-3 fix added doesn't see the bounds error. This is a neighbor of A4-3 the fix left open, a compiler crash on invalid code, rated like A4-3.

## P1-sema-checks-4 (medium): Main-thread rules still miss the Operator == run by an array's built-in Contains and by generic == on a Struct that compares its fields

Status: open

Where: compiler/src/sema.cpp:19549

This is a neighbor of A2-4. note_compared() is called only for a written == or !=. A T[]'s built-in Contains also runs the element's Operator == (spec 14: 'Only a T[]'s built-in Contains takes any element that has =='), but check_special_call() only checks comparable() and records no use. Generic code's `a == b` on a T: Equatable reaches only the conformance members. A Struct that lists Equatable without an == of its own compares its fields, and the Operator == of a field type that doesn't list Equatable is reached by no edge. In both cases a worker runs an Operator == that changes a Global, and the program is accepted. A direct `arr == [Eq(1)]` in the same Work is refused ('calls 'Eq.==', which uses the Global 'score'').

```
Import std.core

Global score = 0

Struct Eq
    Field n: Int
    Operator ==(a: Eq, b: Eq): Bool
        score += 1
        Return a.n == b.n
    End Operator
End Struct

Struct Pair Implements Equatable
    Field e: Eq
End Struct

Function Same<T: Equatable>(a: T, b: T): Bool => a == b

Function Work(): Int
    Local found = [Eq(1)].Contains(Eq(1))
    Local same = Same(Pair(Eq(1)), Pair(Eq(1)))
    Return (found ? 1 : 0) + (same ? 1 : 0)
End Function

Local t = Spawn(Function() => Work())
Print("{t.Wait()} {score}")
```

Expected: error: this work runs on another thread, but it calls 'Work', which calls 'Eq.==', which uses the Global 'score', which only the main thread can, unless it's an Atomic or a Mutex

Actual: Builds and prints `2 2` (debug and release): the worker changed the Global score twice.

Suggested fix: In check_special_call()'s built-in Contains branch, call note_compared(element_of(function.owner), ...). In check_threads(), for a Struct or tuple that implements Equatable (or Hashable) without its own Operator == (or Hash), add edges from Equatable's == (and Hashable's Hash) to the Operator == (and Hash) of its fields, as note_compared() walks them. Add sema tests for both shapes.

Skeptic: semachk/f4.fb builds and prints '2 2' in debug and release. Each shape on its own also gets through. f4a.fb ([Eq(1)].Contains(Eq(1)) in the worker) prints '1 1'. f4b.fb (Same<T: Equatable> on a Pair Implements Equatable that holds Eq) prints '1 1'. Controls are refused with the main-thread error: f4c.fb (arr == [Eq(1)]) and f4d.fb (Same on Eq, which has its own == and Implements Equatable). Code: note_compared() (sema.cpp:10857) is called only from binary == and != (18371-18374). The built-in Contains branch (19549-19553) checks only comparable() and records no use. So the A2-4 fix leaves out these implicit == runs.

## P1-sema-checks-5 (medium): Compiler assertion crash: an early-run Const Function that converts an integer to an Enum declared later

Status: open

Where: compiler/src/lower.cpp:8190

This is a neighbor of A4-12. The fix (9f1575c) declares an Enum early only in check_enum_value(), that is, for `Extra.BONUS`. A Const Function run while types are declared, here for a Field's size, that converts an integer to an Enum declared further down never names a member. So the Enum is never declared early, its values are empty when lowering builds the function, and to_enum() reads values.front() on an empty vector. With the Enum declared above the Struct, the same program builds and prints 12. Spec 13 limits 'can't run here' to Structs and Classes whose members aren't declared yet.

```
Import std.core
Const Function Pick(n: Int): Int => ((n As Extra) As Int) + 1
Struct Grid
    Field cells: Int[Pick(2)]
End Struct
Enum Extra
    A, B, C
End Enum
Print(SizeOf<Grid>())
```

Expected: 12

Actual: fb aborts: stl_vector.h:1346: ... std::vector<long long unsigned int>::front() ... Assertion '!this->empty()' failed. (exit 3)

Suggested fix: Before an early run, declare the Enums a Const Function's body converts to or names (in precheck_early(), or wherever a type expression resolves an Enum while !m_declared). Also make to_enum() safe for an Enum with no values. Add a test with the Enum declared below the Struct.

Skeptic: semachk/f5.fb makes fb abort with stl_vector.h:1346 front() Assertion '!this->empty()' failed (exit 3), in fb run and fb check. Control f5b.fb, with the Enum declared first, prints 12. f5c.fb (Const K = Pick(2) with the Enum below, no type size) prints 3. So only the early run while types are declared crashes. Code: lower.cpp to_enum (~8170-8193) calls values.front() on the Enum's values, which are empty. In sema, declare_early() is called for an Enum only from declare_held() (SizeOf) and check_enum_value() (21247), not for an `As Enum` conversion. This is a neighbor of A4-12, and the program is valid under spec 13.

## P1-sema-checks-6 (medium): `2 As Extra` in a declaration's type is refused when the Enum is declared later

Status: open

Where: compiler/src/sema.cpp:22510

This is another neighbor of A4-12, whose suggested fix named enum_named()'s other users too. fold_to_enum() checks a known integer against the Enum's values without declare_early(). While types are declared, an Enum declared further down has no values yet, so a valid conversion in a Field's size is reported as not being one of the Enum's values. With the Enum declared first, the same program prints 12.

```
Import std.core
Struct Grid
    Field cells: Int[((2 As Extra) As Int) + 1]
End Struct
Enum Extra
    A, B, C
End Enum
Print(SizeOf<Grid>())
```

Expected: 12

Actual: error: 2 isn't one of Extra's values, and error: a fixed array's size must be an integer known at compile time

Suggested fix: In fold_to_enum() (and any other read of an Enum's values from a type), call declare_early(type) when !m_declared, as check_enum_value() now does. Add the sema test.

Skeptic: semachk/f6.fb reports '2 isn't one of Extra's values' and 'a fixed array's size must be an integer known at compile time'. Control f6b.fb, with the Enum declared first, prints 12. Code: fold_to_enum() (sema.cpp:22512-22526) reads m_analysis.types.get(type).values without declare_early() while !m_declared. check_enum_value() (21246-21248) now does that, and A4-12's suggested fix named enum_named()'s other users.

## P1-sema-checks-7 (medium): A non-generic Struct's Field of an oversized instance escapes the fixed array limit, and the program overflows its stack

Status: open

Where: compiler/src/lower.cpp:891

This is a neighbor of A4-14. check_values() now walks the Fields of an instance (info.generic set) and of tuples, but not of a non-generic Struct or Class. An instance like G<65536, 65536>, whose Int8[W, H] is 2^32 elements, is never checked when only a non-generic Struct's Field holds it. The A4-14 symptom comes back: LLVM warns about the frame size and the exe dies with a stack overflow. The same instance in a Global (`Global g: G<65536, 65536>?`) is reported: 'Int8[65536, 65536] comes out here with more elements than a fixed array can hold'.

```
Import std.core
Struct G<Const W: Int, Const H: Int>
    Field c: Int8[W, H]
End Struct
Struct Holder
    Field g: G<65536, 65536>
End Struct
Function Count(h: Holder): Int => 1
Print("hi")
If False Then Print(Count(Holder()))
```

Expected: error: Int8[65536, 65536] comes out here with more elements than a fixed array can hold, at most 2147483647

Actual: warning: stack frame size (4294967304) exceeds limit ... in function 'fb.Count(Holder)'; error: f3.exe exited with code 0xC00000FD (nothing printed)

Suggested fix: In check_values(), walk the Fields (and base) of every Struct and Class, not only of instances, or check the instance sizes of a non-generic declaration's Field types when sema resolves them. Add a MIR test with the instance inside a non-generic Struct.

Skeptic: semachk/f7.fb in debug warns 'stack frame size (4294967304) exceeds limit ... fb.Count(Holder)' and '(8589934664) ... fb.main', then 'f7.exe exited with code 0xC00000FD' with nothing printed. In release it prints 'hi'. Control f7c.fb, which reads h.g.c.Count, is refused with 'Int8[65536, 65536] comes out here with more elements than a fixed array can hold'. Code: lower.cpp check_values() (856-920) walks Fields only when info.kind == TUPLE or info.generic is set (891-896), so a non-generic Holder's Field of type G<65536, 65536> is never visited. That leaves A4-14's fix ('code that isn't generic checks its instances' sizes too') incomplete. Side note: the finding's Global control `Global g: G<65536, 65536>?` (f7b.fb) wasn't reported here. It built and printed 'hi', which is another unreported spot, not a refutation.

## P1-sema-checks-9 (medium): Tuples in an array literal still get their type from the order they're in

Status: open

Where: compiler/src/sema.cpp:22290

This is a neighbor of A4-10. Decision row 732 says tuples and empty arrays take the type the other elements meet at, 'wherever they are in it'. common_type_of() puts every tuple literal in pass 1, even one whose parts are all typed. The first tuple in source order is checked with no hint, so a tuple holding `[]` that comes before a typed tuple fails, while the same elements in the other order work. Map literals with tuple values behave the same way.

```
Import std.core
Local a: Int64[] = [1]
Local u = [(2, a), (1, [])]
Local t = [(1, []), (2, a)]
Print("{u.Count} {t.Count}")
```

Expected: 2 2

Actual: error at line 4: an empty array needs a type from where it's used, like 'Local a: Int[] = []' (line 3, the same elements reversed, compiles)

Suggested fix: Find the common type of pass-1 values before checking them for real: check the tuples quietly with no hint and meet those that type, then check each against that type. Or check pass 1 twice. Add the reversed-order tests for arrays and Maps.

Skeptic: semachk/f9.fb reports 'an empty array needs a type from where it's used' at line 4, `[(1, []), (2, a)]`, while line 3, `[(2, a), (1, [])]`, checks. Map variant: f9b.fb, {"x": (1, []), "y": (2, a)}, fails the same way, and f9c.fb, with the order reversed, prints '2 2'. Code: common_type_of() (sema.cpp:22290-22320) puts every tuple_expr_t in pass 1 and checks pass-1 values in source order. The first tuple gets a NONE hint when common is still NONE. That contradicts decision row 732 (phase 1): tuples and empty arrays take the type the others meet at 'wherever they are in it'. It's a neighbor of A4-10 and rated like it.

## P1-sema-types-1 (medium): A Static Method can still take an inherited Field's or Property's name and hide it (A1-8 fixed only half)

Status: open

Where: compiler/src/sema.cpp:3820

check_inherited_static() only flags a Static Method that clashes with an inherited instance Method with the same parameters. It never compares it with inherited Fields or Properties. An instance Method named like an inherited Field gets "'x' is already declared in Base". So does `Field x` next to `Static Method x()` in one Class. A Static Method named like an inherited Field or Property is accepted, though. Then in the derived Class's code, `x` means the Static Method, and `d.x` is "'x' is Static; use it on D". That is A1-8's hiding, just from the Method side. A `Static Method P()` next to an inherited `Property P` is accepted the same way.

```
Import std.core

Class Base
    Field x: Int32 = 1
End Class

Class D Extends Base
    Static Method x(): Int32 => 5

    Method Show()
        Print(x())
    End Method
End Class

Local d = D()
d.Show()
Local b: Base = d
Print(b.x)
```

Expected: error at line 8: 'x' is already declared in Base (first declared here at line 4), as `Method x()` gets

Actual: builds and prints 5, then 1. In D's code `x` is the Static Method, and `Print(d.x)` gives "'x' is Static; use it on D"

Suggested fix: In check_inherited_static(), a Static Method also clashes with any usable inherited member that isn't a Method (a Field, Property, or Static Field). The test becomes `member.kind != METHOD || found.kind != METHOD || (!found.modifiers.is_static && same_parameters(...))`. Add a test with an inherited Field and an inherited Property.

Skeptic: I ran sema-types/f1.fb (the finding's program). It builds and prints 5, then 1. In f1e.fb, the same Classes plus `Print(d.x)` give "'x' is Static; use it on D", so the Static Method hides the inherited Field from D's objects. In f1c.fb, `Static Method P()` next to an inherited `Property P` is accepted at the declaration, and `d.P` then fails with "'P' is Static; use it on D". For contrast, f1b.fb (instance `Method x()` next to an inherited Field x) and f1d.fb (`Field x` and `Static Method x()` in one Class) are both refused with "'x' is already declared". In check_inherited_static() (sema.cpp:3820), a Static Method clashes only with an inherited instance METHOD that has the same parameters. Its own doc comment says a Static member "can't hide what the Class inherits from the Class's own code and objects", and A1-8's title covers a Static Method taking an inherited instance member's name. The fix is half done.

## P1-sema-types-2 (medium): A type's own type parameter can take the name of a type declared further down (A1-13 depends on order)

Status: open

Where: compiler/src/sema.cpp:1435

names_type() asks top_level() which types the code sees, at the time add_type_parameters() runs. For a Function's or Method's type parameters, every type is declared by then, so `F<Foo>` is refused wherever Foo is. A Struct's, Class's, or Interface's type parameters are added while types are still being declared, so only types declared above are found. `Struct Late<Bar>` before `Struct Bar` is accepted silently, with Bar meaning the parameter inside. With `Class Later<Mode>` before `Enum Mode`, the only error is the confusing "Later<Mode> needs a New that assigns 'v', since Mode has no zero value". The sema test only declares Foo first.

```
Import std.core

Struct Late<Bar>
    Field v: Bar
End Struct

Struct Bar
    Field n: Int32
End Struct

Print(Late<String>("c").v)
```

Expected: error at line 3: 'Bar' is a type here, so a type parameter can't take its name (as when Struct Bar comes first)

Actual: builds and runs, printing c

Suggested fix: Check the type parameters of type declarations once every type is declared, for example in a pass after declaring types that runs names_type() on each one. Add a test that declares the type after the generic Struct, Class, and Interface.

Skeptic: f2.fb (`Struct Late<Bar>` before `Struct Bar`) builds and prints c. f2b.fb has the same two Structs in the other order and is refused: "'Bar' is a type here, so a type parameter can't take its name". f2e.fb (`Interface Shows<Baz>` before `Class Baz`) is accepted. f2c.fb (`Class Later<Mode>` before `Enum Mode`) gives only "Later<Mode> needs a New that assigns 'v', since Mode has no zero value". f2d.fb (`Function F<Foo>` before `Struct Foo`) is refused correctly. Spec 07:26 says a type parameter can't share a type's name. names_type() (sema.cpp:1464) uses top_level() at the moment add_type_parameters() runs, so for a type declaration the result depends on order. A1-13's fix is incomplete.

## P1-sema-types-3 (medium): A Class with its own == and an unrelated Method named Hash is made Hashable implicitly, then refused

Status: open

Where: compiler/src/sema.cpp:2598

list_object_interfaces() adds Hashable when the Class has an Operator == and has_hash() or gets_default(type, "Hash") is true. has_hash() only checks that some Method is named Hash with no parameters and isn't Static. It doesn't check that its result is Hashable's UInt64. gets_default() takes any Interface default named Hash. The Class then gets the implicit listing "at the Class's name" and fails conformance with "needs a Hash that agrees with it", though it lists nothing. Spec 07 says such a Class is Equatable but not Hashable, and the error is only for one that lists Hashable. This code compiled before phase 1. An unrelated Interface default `Method Hash(): String => ...` in a listed Interface fails the same way.

```
Import std.core

Class Doc
    Field text: String = "abc"

    Operator ==(a: Doc, b: Doc): Bool => a.text == b.text

    // A content hash, unrelated to Hashable.
    Method Hash(): String => "sha:" + text
End Class

Local d = Doc()
Print(d.Hash())
Print(d == Doc())
```

Expected: builds, with Doc Equatable but not Hashable, and prints sha:abc then True

Actual: error at 3:7: Doc has an Operator == of its own, so it needs a Hash that agrees with it (note: its == is declared here)

Suggested fix: Make has_hash() and gets_default() count only a Hash that would implement Hashable's: no parameters, not Static, and a UInt64 result. For a default, take only one from Hashable or from an Interface that redeclares Hashable's Hash. Otherwise leave the Class Equatable only. Add tests with Hash(): String written in the Class and from an Interface's default.

Skeptic: f3.fb (Class Doc with its own Operator == and `Method Hash(): String`, listing nothing) fails at 3:7: "Doc has an Operator == of its own, so it needs a Hash that agrees with it". f3b.fb gets its Hash(): String from a listed Interface's default and fails the same way. f3c.fb (the same == and no Hash) builds and prints True, and f3d.fb (Hash(): String, no ==) builds. In list_object_interfaces() (sema.cpp:2536), Hashable is added when has_hash() or gets_default(type, "Hash") is true. has_hash() (2598) checks only the name, that there are no parameters, and that it isn't Static. It never checks for a UInt64 result, and gets_default takes any non-Abstract default named Hash. Spec 07 says that error applies only to a Class that lists Hashable, and Doc lists nothing. Classes became implicitly Hashable in 2185793, so this compiled before phase 1. It's a regression.

## P1-sema-types-4 (medium): An == from an Extend of one instance makes every instance non-Hashable, even ones with no == or with their own Hash

Status: open

Where: compiler/src/sema.cpp:2501

list_object_interfaces() decides Hashable once, for the generic definition. extended_with() is true when an Extend of any one instance adds ==, and has_hash() looks only at the definition's members, not at what the instance Extend adds. So with `Extend Box<String>` giving both Operator == and Hash, Box<T> isn't Hashable at all. Box<Int32> has no == of its own but isn't Hashable, and its error gives no reason. Box<String> has both an == and a Hash of its own but isn't Hashable either, and its message says it's Hashable only with a Hash of its own, which it has. Spec 07 names "for one instance, from an Extend of it" as one way a type gets an ==, and says only a Class with such an == and no Hash isn't Hashable. Equatable already works per instance: generic Same() gives False for two Box<Int32>(1) and True for two Box<String>("a"). Row 723's exception covers only Where lines.

```
Import std.core
Import std.collections

Class Box<T>
    Field v: T
    New(v: T)
        Self.v = v
    End New
End Class

Extend Box<String>
    Operator ==(a: Box<String>, b: Box<String>): Bool => a.v == b.v
    Method Hash(): UInt64 => v.Hash()
End Extend

Local s = Set<Box<Int32>>()
Local one = Box<Int32>(1)
s.Add(one)
s.Add(Box<Int32>(1))
Print(s.Count)
Local t = Set<Box<String>>()
t.Add(Box<String>("a"))
t.Add(Box<String>("a"))
Print(t.Count)
```

Expected: 2, then 1 (Box<Int32> hashes by identity, Box<String> with its Extend's == and Hash)

Actual: error at 16:11: Box<Int32> doesn't implement Hashable, which Set<T> needs for T; error at 21:11: Box<String> doesn't implement Hashable, which Set<T> needs for T; Box<String> has an Operator == of its own, so it's Hashable only with a Hash of its own that agrees with it

Suggested fix: Decide Hashable per instance where an Extend of one instance adds == or Hash, as Equatable's == already is. That instance is Hashable when it has a Hash too, and the other instances keep identity. If that's not possible, follow row 723 (as listed, for every instance), fix the message for one that has a Hash, and say so in spec 07. Add a test.

Skeptic: f4.fb (the finding's program) fails as described. At 16:11: "Box<Int32> doesn't implement Hashable, which Set<T> needs for T". At 21:11, the same for Box<String>, "...so it's Hashable only with a Hash of its own that agrees with it", even though the Extend gives Box<String> a Hash. In f4b.fb, an Extend of Box<String> that adds only == makes Set<Box<Int32>> fail too. Without the Extend (f4c.fb), Set<Box<Int32>> works and prints 2. f4d.fb, the same Extend listing `Implements Hashable`, builds and prints 1, so a per-instance Hash does work. f4f.fb shows Equatable is per instance: generic Same() prints False for two Box<Int32>(1) and True for two Box<String>("a"). extended_with() (sema.cpp:2582) is true for every instance once any instance's Extend adds ==, and has_hash() looks only at the definition's members. Spec 14 says a Map or Set needs Hashable keys, "as every Class is unless it has an == of its own without a Hash". Box<Int32> has no == of its own, and Box<String> has both. Row 723 covers only Where lines.

## P1-sema-types-5 (medium): == on a generic Struct that holds an array of a growing instance of itself gives invalid LLVM IR (A-new-2's generic neighbor)

Status: open

Where: compiler/src/sema.cpp:10759

A-new-2 stopped comparable() from going round on `Struct Node / Field children: Node[]`. The generic version, where the field holds an array of a larger instance (Tree<T> holding Tree<T[]>[]), passes `fb check`. Then lowering's built-in == asks for Tree<Int32[]>, Tree<Int32[][]>, and so on, until a Field type past the spec 07 limits is left out as unsized, and LLVM verification fails. Spec 07 says a Field's type that grows is left out "as it's never reached", but == reaches it, and growth like this is meant to be a compile error that names it. The same program without == builds and runs. The crash is the same in --release and through `[a].Contains(a)`.

```
Import std.core

Struct Tree<T>
    Field v: T
    Field kids: Tree<T[]>[]
End Struct

Local a = Tree<Int32>(1, [])
Print(a == Tree<Int32>(1, []))
```

Expected: a compile error at the == saying it would need a larger instance each time, without end (or == refused for Tree<Int32>)

Actual: error: invalid LLVM IR: GEP into unsized type! %43 = getelementptr %"fb.Tree<Int32[][][]...[]>" ..., then "loading unsized types is not allowed" and "Invalid operand types for ICmp instruction"

Suggested fix: Have comparable(), or the == use that note_compared() walks, report an element instance that grows without end, the way check_expansion() does for calls. Or have lowering's == generation hit spec 07's instance backstop instead of reaching a left-out field. Add a sema or program test.

Skeptic: `fb check` passes f5.fb (Tree<T> with `Field kids: Tree<T[]>[]`, then `a == Tree<Int32>(1, [])`). `fb run`, in debug and in --release, fails with "error: invalid LLVM IR: GEP into unsized type! %43 = getelementptr %\"fb.Tree<Int32[][]...[]>\"" and more errors after it. f5c.fb (`[a].Contains(a)`) fails the same way. f5b.fb, the same Struct without ==, builds and prints 1. The overloaded comparable() (sema.cpp:10759) uses a `seen` list that stops only on an exact repeated type. A growing instance gets past it until a Field past the limits is left out, and lowering's built-in == then reaches that left-out field. Spec 07 says such a field is left out "as it's never reached", but == reaches it. This is the generic neighbor of A-new-2, and it still fails as an internal compiler error.

## P1-sema-checks-8 (low): SizeOf of an instance whose fixed array passes the element limit gives a size, not an error

Status: open

Where: compiler/src/sema.cpp:19129

This is a neighbor of A4-14. Decision row 735 holds a fixed array that an instance works out to 2147483647 elements. SizeOf is folded in sema, so lowering's check_values() never sees the type, and SizeOf<G<65536, 65536>>() gives 4294967296 for a type that can't exist. The written `SizeOf<Int64[65536, 65536]>()` is refused with 'a fixed array can hold at most 2147483647 elements'. SizeOf<G<65536, 65536>[2]>() gives 8589934592.

```
Import std.core
Struct G<Const W: Int, Const H: Int>
    Field c: Int8[W, H]
End Struct
Print(SizeOf<G<65536, 65536>>())
```

Expected: an error that the instance's fixed array holds more than 2147483647 elements, as for a written Int8[65536, 65536]

Actual: 4294967296

Suggested fix: In check_size_of() (or the size computation it uses), apply the same total-element limit to fixed arrays inside the measured type, instances included, and report it at the SizeOf.

Skeptic: semachk/f8.fb prints 4294967296. f8c.fb (Const S = SizeOf<G<65536, 65536>>()) prints 4294967296 too. Control f8b.fb, SizeOf<Int8[65536, 65536]>(), is refused with 'a fixed array can hold at most 2147483647 elements'. Decision row 735 (phase 1) says a fixed array an instance works out is held to 2147483647 elements and reported where the instance is used. check_size_of (sema.cpp ~19150-19170) folds c_layout's size without that limit, and lowering never sees the type. Nothing crashes: it's a value for a type that can't exist, so low.

## P1-sema-types-6 (low): A Static Property used by name before Super.New, or before a no-zero field is assigned, is refused as a use of Self

Status: open

Where: compiler/src/sema.cpp:16240

check_expr(name_expr_t)'s Property branch calls check_self_ready() for every Property used by name, Static ones too, although a Static Property's Get has no Self. A3-4's fix made the assignment path (check_target_place) skip Static Properties, but the read path wasn't changed. Under Swift's rule, a derived New now computes its own fields before Super.New, so this case is common: `part = Node("{Prefix}")` with a Static Property Prefix is refused. A Static Field by name and the qualified `D.Prefix` are accepted in the same place. The plain-Class case, before a no-zero field is assigned, fails the same way: "Self can't be used before New assigns 'part'".

```
Import std.core

Class Node
    Field name: String
    New(name: String)
        Self.name = name
    End New
End Class

Class Base
    Static Field made: Int32 = 0
    Static Property Made: Int32
        Get => made
    End Property
End Class

Class D Extends Base
    Field part: Node
    Static Property Prefix: String
        Get => "part"
    End Property

    New()
        part = Node("{Prefix} {Made}")
        Super.New()
    End New
End Class

Print(D().part.name)
```

Expected: builds and prints part 0

Actual: error at 24:23 and 24:32: Self can't be used before Super.New (under Prefix and Made)

Suggested fix: In the Property branch of check_expr(name_expr_t), call check_self_ready() only when the Property isn't Static, as check_target_place() does. Add tests before Super.New and before a no-zero field is assigned.

Skeptic: f6.fb gives "Self can't be used before Super.New" at 24:23 (Prefix) and 24:32 (Made). In f6c.fb, the same position uses a Static Field by name (`prefix`, and the inherited `made`) and the qualified `D.Prefix`, and it builds and prints "part 0 part". f6b.fb (a plain Class, `part = Node(Prefix)`) gives "Self can't be used before New assigns 'part'". f6d.fb (`Super.New(Prefix)`) is refused too. The Property branch of check_expr(name_expr_t) (sema.cpp:16241-16246) calls check_self_ready() for Static Properties as well. check_target_place() (15151) skips Static ones, and the new check_inherited_ready() lets Static fields through. That branch itself is base code, and f6b/f6d were refused before phase 1 too. But A3-1's fix created the code-before-Super.New region and decided what it may use (Static Fields yes), and this Static Property case was missed. That is why I rate it low: valid code is refused, and `D.Prefix` works around it.

## P1-sema-types-8 (low): A ReadOnly assignment in a lambda or Parallel For in New says only that Class's New can change it

Status: open

Where: compiler/src/sema.cpp:15377

A3-2's fix makes in_new_of() false inside a lambda or a Parallel For body. The error then reuses "'s' is ReadOnly; only Hub's New can change it", while the code it points at is written inside Hub's New. The message doesn't say what's actually wrong: the body runs later or on other threads, so it isn't New's own code. The sema tests pin this wording for both the lambda and the Parallel For case.

```
Import std.core

Shared Class Hub
    ReadOnly Field s: String

    New()
        Parallel For i = 0 Until 10
            s = "value number {i}"
        End For
        s = "x"
    End New
End Class

Print(Hub().s)
```

Expected: an error at 8:13 that says a Parallel For's body (or a lambda) in New can't change it, since it runs on other threads or later, and only New's own code can

Actual: error at 8:13: 's' is ReadOnly; only Hub's New can change it

Suggested fix: When m_function is the field owner's New but m_lambdas isn't empty, report the reason, for example "'s' is ReadOnly, and {lambda or a Parallel For's body} in New runs later or on other threads, so only New's own code can change it". Update the two sema test expectations.

Skeptic: f8.fb (a Shared Class whose New has a Parallel For body assigning ReadOnly s) gives "8:13: error: 's' is ReadOnly; only Hub's New can change it", pointing at code that is written inside Hub's New. The refusal is right after A3-2's fix, since in_new_of() is now false while m_lambdas isn't empty. The reason given is misleading, though: it never says that a lambda or Parallel For body runs later or on other threads (spec 06: "not later"). The message comes from sema.cpp:15377, and tests/compiler/sema_tests.cpp:2951-2972 pin the same wording for both the lambda and the Parallel For case. This is a diagnostic-quality defect that the fix introduced.

## P1-sema-types-9 (low): in_new_of() has two stacked doc comments, the old one left above the new one

Status: open

Where: compiler/src/sema.cpp:15450

31a832a added a doc comment to in_new_of() but kept the old one above it, so the function has "/// Whether the function being checked is a New of `type`." directly over "/// Whether the code is New's own, of `type`: a lambda or a Parallel For's body in it runs later, or on other threads, so it isn't." The first is stale, since the function is now false inside lambdas. This phase fixed stacked comments like it elsewhere (A2-10, A4-23). The program below runs through in_new_of(); the defect is in the source comment.

```
Import std.core

Class A
    ReadOnly Field id: Int32

    New()
        id = 1
    End New
End Class

Print(A().id)
```

Expected: sema.cpp:15450-15452 has one doc comment for in_new_of(), the new one

Actual: sema.cpp:15450 (from ded376fb) and 15451-15452 (from 31a832a) are two stacked doc comments; the program prints 1

Suggested fix: Delete line 15450, "/// Whether the function being checked is a New of `type`."

Skeptic: At sema.cpp:15450-15452, git blame shows line 15450 from ded376fb ("/// Whether the function being checked is a New of `type`.") and lines 15451-15452 from 31a832a (in this range): "/// Whether the code is New's own, of `type`: a lambda or a Parallel For's body in it runs later, or on other threads, so it isn't." These are two stacked doc comments on in_new_of(), and the first is now stale, since the function returns false inside lambdas. It's a minor comment defect left by A3-2's fix.
