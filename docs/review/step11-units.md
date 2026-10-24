# The review of M6 step 11's language part and units: computed Const type arguments with -, /, %, and parentheses, Operators with inferred type parameters of their own, the transparent Alias declaration, and std.math's typed units (Quantity<L, M, T> and its unit Aliases)

21 confirmed findings: 1 high, 10 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S11U-ops-alias-1 (high): Calling an Interface's generic Operator through a constraint crashes the compiler

Status: open

Where: compiler/src/lower.cpp:6503

Interface Operators may now have type parameters of their own, but when one is called on a type parameter that the Interface constrains, lowering's call_operator() goes to the implementation with implementation_arguments(Self, found), which holds only the implementing type's own arguments. The Operator's own type arguments (U) are dropped, so substitute() indexes past the end of the argument vector. Both `fb check` and `fb run` abort. It happens whether the Struct writes the Operator itself or takes the Interface's default (put `=> 7` on the Interface's Operator and remove the Struct's). Calling it directly on the Struct, `Name(4) & 5`, works. No test calls a generic Operator through a constraint.

```
Import std.core

Interface Joins
    Operator &<U>(a: Self, b: U): Int
End Interface

Struct Name Implements Joins
    Field Text: Int
    Operator &<U>(a: Name, b: U): Int => a.Text
End Struct

Function Tagged<J: Joins>(j: J): Int => j & 5

Print(Tagged(Name(4)))
```

Expected: 4

Actual: The compiler aborts: stl_vector.h:1282 ... Assertion '__n < this->size()' failed (fb::type_table_t::substitute, called from lowerer_t::call_operator at lower.cpp:6510 through the Interface branch at 6503). `fb check` exits 3 the same way.

Suggested fix: In call_operator's Interface branch, carry the Operator's own type arguments (the ones after Self and the Interface's own) onto implementation_arguments(arguments.front(), *found), for a written implementation and for a default's copy. Add a sema/program test that calls a generic Interface Operator through `J: Joins`, with and without a default.

Skeptic: Ran the program as given (f1.fb) and the default variant (f1b.fb, `=> 7` on the Interface's Operator and no Operator on the Struct). Both abort in debug and in --release with stl_vector.h:1282 `Assertion '__n < this->size()' failed`, exit 3, and `fb check f1.fb` exits 3 as well. Calling it directly, `Print(Name(4) & 5)` (f1c.fb), prints 4. In lower.cpp, call_operator's Interface branch (around 6497-6504) calls itself again with `implementation_arguments(arguments.front(), *found)`. That passes only the implementing type's arguments, so the Operator's own U is dropped before substitute() runs over type_parameters_of(function).

## S11U-const-1 (medium): The same Const value reached through Const parameters of different integer types makes separate instances, with separate Statics

Status: open

Where: compiler/src/sema.cpp:12039

When a type argument is just the name of a Const type parameter, resolve_argument returns that parameter unchanged. It never checks or converts it to the target parameter's type. Inference does the same thing: unify keeps the actual VALUE with that value's own inner type. A known VALUE's identity includes its inner type (type_table_t::value), so Counter<5> made from an Int8 5 and Counter<5> made from an Int 5 are two different instances with the same name. A Static on that type silently splits in two. In declarations it shows up as a contradictory type error: with Box8<Const M: Int8> and Make<Const N: Int>(): Box8<N>, `Local a: Box8<5> = Make<5>()` gives "expected t31.Box8<5>, found t31.Box8<5>". Inference across types splits Statics too: Of<Const N: Int>(s: Small<N>) with Small's S: Int8 prints 1 1 2 2 where 1 2 3 4 is right. Also, a Const K: Int as an argument for UInt8 is refused ("A takes UInt8, not Int32"), but a Const parameter N: Int is accepted unchecked. Computed arguments are not affected: Bump<N + 0>() prints the right 1 2 3 4 5. This code wasn't changed in the range, but it is the Const argument check this step now relies on for any value of a parameter's type.

```
Import std.core

Struct Counter<Const M: Int>
    Static Field Made: Int = 0
    Method Get(): Int => M
End Struct

Function Bump<Const M: Int>(): Int
    Counter<M>.Made += 1
    Return Counter<M>.Made
End Function

Function Via<Const N: Int8>(): Int => Bump<N>()

Print(Bump<5>(), Bump<5>(), Via<5>(), Via<5>(), Bump<5>())
```

Expected: 1 2 3 4 5

Actual: 1 2 1 2 3 (debug and release)

Suggested fix: Make a known VALUE's identity its number alone. One way: every VALUE gets an Int64 inner type, and check_arguments' fits() checks the parameter's type in each instance, as it already does. The other way: convert to the target parameter's inner type wherever an argument is passed (resolve_argument's name path), inferred (unify), or substituted. In sema, also check that a named Const parameter's type converts to the target's, as a Const's is checked. Add the program above and the Box8 type-error case as tests.

Skeptic: Ran f1.fb (the finding's program) in debug and release: it prints '1 2 1 2 3', not '1 2 3 4 5', so Counter<5> reached through Via's Int8 N gets its own Static. Box8 case (Box8<Const M: Int8>, Make<Const N: Int>(): Box8<N>, Local a: Box8<5> = Make<5>()) gives 'error: expected f1b.Box8<5>, found f1b.Box8<5>'. In the code, resolve_argument (sema.cpp ~12035) returns the enclosing Const parameter `other` unchanged for a bare name, without converting it to the target's inner type. type_table_t::value (types.cpp ~532) interns by inner type plus number, so value(Int8,5) and value(Int32,5) are different ids and make different instances. Spec 07 says each value makes an instance. One part of the finding doesn't hold up: an Int N passed to a UInt8 parameter is caught when the instance is made ('comes out as 300 here, which doesn't fit in UInt8').

## S11U-const-2 (medium): A size computed as Int[N - 1] and Int[M] with M = N - 1 are different types, so 'expected Int32[N - 1], found Int32[N - 1]'

Status: open

Where: compiler/src/sema.cpp:12212

resolve_fixed_array computes a size with computed_value(expression, INT64), so its VALUE has an Int64 inner type. A computed Const argument like Box<N - 1> gets the parameter's type (Int32). value_expression interns computed values by inner type, so the two N - 1 values are different type ids. A Field Int[M] of Box<N - 1> therefore never equals a written Int[N - 1], and neither does a parameter Int[N] of Count<N - 1>. Valid generic code is refused with a message that names the same type twice. Count<N - 1>(t) with t: Int[N - 1] fails the same way. This step adds -, / and %, and sizes like N - 1 are the case it is for.

```
Import std.core

Struct Box<Const M: Int>
    Field Items: Int[M]
End Struct

Function F<Const N: Int>(): Int
    Local b = Box<N - 1>()
    Local t: Int[N - 1] = b.Items
    Return t.Count
End Function

Print(F<5>())
```

Expected: 4

Actual: t61.fb:9:27: error: expected Int32[N - 1], found Int32[N - 1]

Suggested fix: Give computed sizes and computed arguments one representation: VALUE identity independent of inner type, or both computed in Int64, with the parameter's type checked per instance by fits(). The same change fixes S11U-const-1. Add F<5> above and a Count<N - 1>(t) case as tests.

Skeptic: Ran f2.fb: 'f2.fb:9:27: error: expected Int32[N - 1], found Int32[N - 1]'. resolve_fixed_array calls computed_value(expression, type_id_t::INT64), so a size's VALUE has an Int64 inner type. Box<N - 1>'s argument is built with the parameter's inner type (Int32). value_expression (types.cpp ~550) only reuses an entry when inner, op, and parameters all match, so the two N - 1 values are different type ids. Valid generic code with the step's own N - 1 sizes is refused.

## S11U-const-3 (medium): Inferring a Const parameter from a fixed array whose size is a Const parameter or computed takes the size as 0

Status: open

Where: compiler/src/sema.cpp:20263

unify's FIXED_ARRAY case always unifies with m_types->value(inner, have.sizes[i]). When the actual array's size is a Const type parameter (Int[N]) or a computed value (Int[N - 1]), sizes[i] is 0 and the real size is in have.parameters[i]. So inside generic code, Count(a) with a: Int[N] infers Count's N = 0, and Trimmed(a) gets the return type Int32[-1] ("expected Int32[N - 1], found Int32[-1]"). Every generic function that infers a size can't be called from generic code with a Const-sized array. In non-generic code it gives a meaningless message: Count(Trim(a)) with a: Int[1], where Trim returns Int[N - 1], says "expected Int32[0], found Int32[0]" instead of the size error lowering would give ("Int32[0] comes out here, but a fixed array's sizes are from 1 to 2147483647"). The plain Int[N] case is older, but the step's computed sizes go through the same path.

```
Import std.core

Function Count<Const N: Int>(a: Int[N]): Int
    Return N
End Function

Function Trimmed<Const N: Int>(a: Int[N]): Int[N - 1]
    Local t: Int[N - 1]
    Return t
End Function

Function Wrap<Const N: Int>(a: Int[N]): Int
    Return Count(a) + Trimmed(a).Count
End Function

Local a: Int[5]
Print(Wrap(a))
```

Expected: 9

Actual: f3.fb:13:18: error: expected Int32[0], found Int32[N]
f3.fb:13:31: error: expected Int32[0], found Int32[N]

Suggested fix: In unify's FIXED_ARRAY case, when have.parameters[i] != INVALID, unify the dimension with that parameter or computed VALUE, and use value(inner, have.sizes[i]) only for a plain size. Don't infer from a dimension that is an out-of-range known VALUE: let the size error be reported. Test both programs (generic Wrap, and the non-generic Count(Trim(a)) with a: Int[1]).

Skeptic: Ran f3.fb: 'f3.fb:13:18: error: expected Int32[0], found Int32[N]' and the same at 13:31. A non-generic variant (a: Int[1]; Count(Trim(a)) with Trim returning Int[N - 1]) gives 'expected Int32[0], found Int32[0]'. unify's FIXED_ARRAY case (sema.cpp ~20257) always unifies with m_types->value(inner, have.sizes[i]). For an array whose size is a Const parameter or a computed value, sizes[i] is 0 and the real size is in have.parameters[i]. This contradicts spec 07's 'A call infers it from a fixed array's size'.

## S11U-const-4 (medium): An instance whose size is below 1, held by a non-generic Struct's or Class's Field or by a Global, is made with no error

Status: open

Where: compiler/src/lower.cpp:940

Before this change sema refused Grid<0>. Now 0 and below are allowed, and only lowering checks a size, in check_values. check_values only goes into Fields for tuples and generic instances (info.generic != INVALID), and a Global's type isn't checked where it's declared. So Level() and Room() each make a Grid<0> or Grid<-4> as a Field's default, `Global g: Grid<0>` makes one too, and the program builds and runs. Spec 07 says a size outside 1 to 2147483647 "is an error where the instance is made". Touching the value later (level.Map) does report it, at that expression. The same gap lets a Holder Field of Pair<2000000000> with Int8[N * 2] (4000000000 elements) build.

```
Import std.core

Struct Grid<Const W: Int>
    Field Cells: Float[W]
    Field Name: String = "grid"
End Struct

Struct Level
    Field Map: Grid<0>
End Struct

Class Room
    Field Map: Grid<-4>
End Class

Global g: Grid<0>
Local level = Level()
Local room = Room()
Print("made all three")
```

Expected: an error such as "Float32[0] comes out here, but a fixed array's sizes are from 1 to 2147483647" at the Field Map: Grid<0> (or at Level()), and likewise for Room and the Global

Actual:    Compiling f4.fb
    Finished debug build
     Running f4.exe
made all three

Suggested fix: Have check_values go into the Fields (and base) of every Struct and Class, not only instances, keeping the m_checked_values guard, and call it on each Global's type where Globals are lowered, with that declaration's span. Add these three cases to mir_tests.

Skeptic: Ran f4.fb in debug and release: it builds and prints 'made all three' with Level's Field Map: Grid<0>, Room's Field Map: Grid<-4>, and Global g: Grid<0>. In lower.cpp, check_values only goes into Fields when info.kind == TUPLE or info.generic != INVALID, so a non-generic Struct or Class never gets its Fields' instances checked. Before this change sema refused Grid<0>; now only lowering checks, and it misses these. Spec 07 says a size below 1 'is an error where the instance is made'.

## S11U-ops-alias-3 (medium): A generic Operator == is used for a direct == but not inside tuples, Structs, arrays, or Equatable

Status: open

Where: compiler/src/lower.cpp:6612

Operator == may now have type parameters of its own, like `Operator ==<U>(a: Box<T>, b: Box<U>)`. A direct `a == b` and a T? comparison call it. But equal_operator() and compares_two() only accept an == whose two parameter types are exactly the type, so a tuple's or Struct's field-wise ==, T[].Contains, and array == all fall back to the built-in comparison. Spec 06 says a type with an Operator == uses it for every == on its values. For a Class, `Function Same<E: Equatable>` also compares references, while the Hash check still treats the type as having an Operator == of its own. For a Struct, the type just stops being Equatable or Hashable, with no reason given. Debug and release behave the same.

```
Import std.core

Struct Box<T>
    Field Item: Int
    Field Tag: Int
    Operator ==<U>(a: Box<T>, b: Box<U>): Bool => a.Item == b.Item
End Struct

Struct Holder
    Field B: Box<Int>
End Struct

Local a = Box<Int>(4, 1)
Local b = Box<Int>(4, 2)
Print(a == b, (a, 1) == (b, 1), Holder(a) == Holder(b), [a].Contains(b), [a] == [b])
```

Expected: True True True True True (as with a non-generic Operator ==), or a compile error that rejects type parameters on Operator ==

Actual: True False False False False

Suggested fix: Simplest: in check_operator, report "an Operator == can't have type parameters of its own", since every == on the type uses it. Otherwise, make equal_operator()/compares_two() accept a generic == whose operands unify with (Self, Self), pass its own type arguments, and let it implement Equatable. Add a test either way.

Skeptic: f3.fb (generic `Operator ==<U>(a: Box<T>, b: Box<U>)`) prints `True False False False False` in debug and release. f3b.fb, the same program with a non-generic `Operator ==(a: Box<T>, b: Box<T>)`, prints `True True True True True`. Spec 06 says a type with an Operator == uses it for every == on its values: a Struct's or tuple's field-wise ==, a T[]'s Contains, and T? values. In lower.cpp, compares_two (6612) needs both parameter types to equal the type exactly, so Box<U> never matches and equal_operator() falls back to the built-in comparison.

## S11U-ops-alias-4 (medium): An Enum's values can't be named through an Alias

Status: open

Where: compiler/src/sema.cpp:21489

enum_named() only accepts a TYPE symbol, so `Hue.Green` with `Alias Hue = Color` falls through to type_named(), which looks the name up as a Static member and reports that Color has none. Spec 02 says an alias is the type it names everywhere. Using Hue as a type works.

```
Import std.core

Enum Color
    Red
    Green
End Enum

Alias Hue = Color

Local c: Color = Color.Green
Print(c)
Local h: Hue = Hue.Green
Print(h)
```

Expected: Green
Green

Actual: al2.fb:12:20: error: Color has no member named 'Green'

Suggested fix: In enum_named(), resolve an ALIAS symbol with alias_type() and accept it when that type is an Enum. Add a sema test.

Skeptic: f4.fb: `Local h: Hue = Hue.Green` with `Alias Hue = Color` gives "Color has no member named 'Green'" at 12:20, while `Local h: Hue` used as a type is accepted. Spec 02 (added in this step) says 'An alias is the type it names, everywhere', so naming an Enum's value through its alias should work.

## S11U-ops-alias-5 (medium): Extend can't name an Alias: Extend Meters says 'Meters' isn't a type

Status: open

Where: compiler/src/sema.cpp:2815

The Extend target's resolution requires a TYPE symbol (sema.cpp:2815), so `Extend Meters` and `Extend IntBox` (with `Alias IntBox = Box<Int>`) report "isn't a type", and what the Extend adds is missing. Spec 02 says an alias is the type it names everywhere, and Extend of one instance (`Extend Box<Int>`) is allowed, so `Extend Meters` should be `Extend Quantity<1, 0, 0>`. Adding a Method to a unit is a natural use of std.math's Aliases.

```
Import std.core

Struct Box<T>
    Field Item: T
End Struct

Alias IntBox = Box<Int>

Extend IntBox
    Method Twice(): Int => Item * 2
End Extend

Print(Box<Int>(4).Twice())
```

Expected: 8 (what Extend Box<Int> prints)

Actual: al8.fb:9:8: error: 'IntBox' isn't a type
al8.fb:13:19: error: Box<Int32> has no member named 'Twice'

Suggested fix: When the Extend's name is an ALIAS, resolve it with alias_type() and treat it as an Extend of that type or instance (an error if it names an array, tuple, or other type an Extend can't take). Add a sema test with Extend Meters.

Skeptic: f5.fb: `Extend IntBox` with `Alias IntBox = Box<Int>` gives "'IntBox' isn't a type" (9:8), and then 'Box<Int32> has no member named Twice'. f5b.fb, the same Extend written as `Extend Box<Int>`, prints 8. This contradicts spec 02's 'An alias is the type it names, everywhere'.

## S11U-ops-alias-6 (medium): A Class can't extend an Alias of a Class

Status: open

Where: compiler/src/sema.cpp:3277

The Extends resolution only accepts a TYPE symbol that is a Class, so `Class Dog Extends Base` with `Alias Base = Animal` reports "'Base' isn't a Class" and leaves Dog with no base. An Alias of an Interface after Implements, and as a constraint, works.

```
Import std.core

Class Animal
    Method Sound(): String => "..."
End Class

Alias Base = Animal

Class Dog Extends Base
    Override Method Sound(): String => "woof"
End Class

Local a: Animal = Dog()
Print(a.Sound())
```

Expected: woof

Actual: al6.fb:9:19: error: 'Base' isn't a Class
al6.fb:10:21: error: 'Sound' has Override, but Dog extends no Class
al6.fb:13:19: error: expected Animal, found Dog

Suggested fix: In the Extends resolution, accept an ALIAS whose alias_type() is a Class, or an instance of a generic one, and extend that instance. Add a sema test.

Skeptic: f6.fb: `Class Dog Extends Base` with `Alias Base = Animal` gives "'Base' isn't a Class" (9:19), then "'Sound' has Override, but Dog extends no Class" and 'expected Animal, found Dog'. This contradicts spec 02's 'An alias is the type it names, everywhere'.

## S11U-ops-alias-7 (medium): An Operator inherited from a generic Class isn't found for a Class that extends one of its instances

Status: open

Where: compiler/src/sema.cpp:18376

resolve_operator() unifies each candidate's parameter types (Base<T>, Base<U>) directly with the operand's type. For `Derived Extends Base<Int>`, Base<T> doesn't unify with Derived, so T, and the Operator's own U, stay unknown, and the candidate is dropped. Spec 06 says a + b calls the Operator that a's or b's type defines or inherits from a Class it extends. The new generic Operators hit this (`Derived(1) & Base<String>(2)`), and so does an Operator with only its type's type parameter (`Derived(1) + Derived(2)`). A non-generic Base works.

```
Import std.core

Class Base<T>
    Field Item: Int
    New(item: Int)
        Item = item
    End New
    Operator &<U>(a: Base<T>, b: Base<U>): Int => a.Item * 10 + b.Item
    Operator +(a: Base<T>, b: Base<T>): Int => a.Item + b.Item
End Class

Class Derived Extends Base<Int>
    New(item: Int)
        Super.New(item)
    End New
End Class

Print(Base<Int>(1) & Base<String>(2), Base<Int>(1) + Base<Int>(2))
Print(Derived(1) & Base<String>(2))
Print(Derived(1) + Derived(2))
```

Expected: 12 3
12
3

Actual: inh3.fb:19:7: error: no Operator & takes (Derived, Base<String>)
inh3.fb:20:7: error: no Operator + takes (Derived, Derived)

Suggested fix: Before unifying a candidate from a Class the operand extends, swap the operand's type for its base instance whose definition owns the Operator (walk types().base, as a Method call on Derived does). Then the type's arguments come from the Extends line and only the Operator's own are inferred. Add sema tests for both lines.

Skeptic: f7.fb gives 'no Operator & takes (Derived, Base<String>)' and 'no Operator + takes (Derived, Derived)', while the Base<Int> line compiles. f7b.fb, with only the non-generic-own `Operator +(a: Base<T>, b: Base<T>)`, fails the same way for Derived(1) + Derived(2). f7c.fb, a non-generic Base, prints 3. f7d.fb, the same values held in `Base<Int>` locals, prints 3. Spec 06 says a + b calls the Operator that a's or b's type defines 'or inherits from a Class it extends'. The plain + case may be older than this step, but the step's generic Operators run into it too.

## S11U-ops-alias-8 (medium): Generic unit code fails because computed exponents aren't simplified: L - 0 isn't L, L + L isn't L * 2

Status: open

Where: compiler/src/types.cpp:557

In generic code, a generic Operator's result type is the computed value as built (Quantity<L - L2, ...> with L2 = 0 gives `L - 0`), and value types are compared by their structure. So `Function PerSecond<Const L: Int>(a: Quantity<L, 0, 0>, t: Seconds): Quantity<L, 0, -1> => a / t` doesn't compile, though L - 0 is L in every instance. It compiles only if the return type is written as Quantity<L - 0, 0, -1>. Likewise `a * a` gives L + L, which doesn't match L * 2. Any generic Function over Quantity that mixes in a unit with a 0 exponent hits this.

```
Import std.core
Import std.math

Function PerSecond<Const L: Int>(a: Quantity<L, 0, 0>, t: Seconds): Quantity<L, 0, -1>
    Return a / t
End Function

Function Squared<Const L: Int>(a: Quantity<L, 0, 0>): Quantity<L * 2, 0, 0>
    Return a * a
End Function

Print(PerSecond(Meters(9), Seconds(3)), Squared(Meters(3)))
```

Expected: 3 m/s 9 m^2

Actual: f6.fb:5:12: error: expected Quantity<L, 0, -1>, found Quantity<L - 0, 0, -1>
f6.fb:9:12: error: expected Quantity<L * 2, 0, 0>, found Quantity<L + L, 0, 0>

Suggested fix: In type_table_t::value_expression, fold the identities (x + 0, 0 + x, x - 0, x * 1, 1 * x, x / 1, x * 0 to 0). Better, keep +, -, and * by known values in a canonical linear form (constant plus coefficient times each parameter), so equal expressions get the same type. Add sema tests with a mixed-unit generic Function.

Skeptic: f8.fb gives 'expected Quantity<L, 0, -1>, found Quantity<L - 0, 0, -1>' and 'expected Quantity<L * 2, 0, 0>, found Quantity<L + L, 0, 0>'. A more natural generic Function also fails (f8b.fb): `Speed<Const L, Const M, Const T>(a: Quantity<L, M, T>, t: Seconds): Quantity<L, M, T - 1> => a / t` gives 'expected Quantity<L, M, T - 1>, found Quantity<L - 0, M - 0, T - 1>'. Generic code over units that divides by, or multiplies with, any concrete unit can't be written without spelling out `L - 0`. In types.cpp, value_expression (557+) folds only when both sides are known values and otherwise builds a structural expression, with no folding of identities. This step allowed 0 as an exponent and added the generic Operators, and those two together make x - 0 and x + 0 come up all the time. Commutativity (f8c.fb: Int[M + N] vs Int[N + M]) is older than this step; the 0 and 1 identities are the core of the bug.

## S11U-const-5 (low): A constant inside a computed Const argument is checked against the parameter's type, not worked out in an Int64, and gets a second misleading error

Status: open

Where: compiler/src/sema.cpp:12143

computed_value checks each leaf that doesn't name a Const parameter with check(id, type), where type is the target parameter's type. In Q8<N - 200> (A: Int8) the literal 200 has to fit Int8, and in U8<300 - N> the 300 has to fit UInt8, even though spec 07 says each instance works the value out in an Int64 and only the result has to fit. F<100> gives -100 and G<100> gives 200, both of which fit. Then expect_type fails, and the function also reports "a value computed from Const type parameters uses integers known at compile time, with +, -, *, /, and %" at the same span. That second error is wrong, since 200 is such an integer. U8<N + -1> is refused the same way.

```
Import std.core

Struct Q8<Const A: Int8>
    Method Get(): Int => A
End Struct

Struct U8<Const A: UInt8>
    Method Get(): Int => A
End Struct

Function F<Const N: Int>(): Int => Q8<N - 200>().Get()
Function G<Const N: Int>(): Int => U8<300 - N>().Get()

Print(F<100>(), G<100>())
```

Expected: -100 200

Actual: t55.fb:11:43: error: 200 doesn't fit in Int8
t55.fb:11:43: error: a value computed from Const type parameters uses integers known at compile time, with +, -, *, /, and %, like N * 2
t55.fb:12:39: error: 300 doesn't fit in UInt8
t55.fb:12:39: error: a value computed from Const type parameters uses integers known at compile time, with +, -, *, /, and %, like N * 2

Suggested fix: Check constant leaves of a computed value as Int64 (check(id, INT64), then value(type, n)), and leave the parameter's type to the instance's fits() check. Return INVALID without the second error when the leaf's own check already reported one. Add both calls as tests.

Skeptic: Ran f5.fb: four errors: '200 doesn't fit in Int8' plus 'a value computed from Const type parameters uses integers known at compile time, with +, -, *, /, and %' at 11:43, and the same pair for 300 in UInt8 at 12:39. computed_value (sema.cpp ~12142) checks each constant leaf with check(id, type), where type is the target parameter's inner type. When expect_type fails, it falls through to the generic error. Spec 07 says each instance works the value out in an Int64 and only the result has to fit, and -100 and 200 do fit. Low, since it needs narrow Const parameter types with out-of-range constant leaves.

## S11U-const-6 (low): The growth check refuses recursion whose argument shrinks or stays bounded, like Bits<N / 2> or Cycle<(N + 1) % 3>, saying it would need a larger instance each time

Status: open

Where: compiler/src/sema.cpp:9950

check_expansion marks any edge whose argument isn't the parameter itself as growing (.grows = argument != from), then reports a cycle as "would need a larger instance each time, without end". That was true when the only operators were + and *. With / and %, which this step adds, it isn't. Bits<8> needs Bits<4>, <2>, <1>, <0>, and then Bits<0 / 2> is Bits<0> again, so five instances. Cycle<(N + 1) % 3> needs three. Lowering would make them and stop, and its 50-deep chain limit already catches recursion that really grows. For Down<N - 1> the error is right, but its wording ("a larger instance") is wrong.

```
Import std.core

Function Bits<Const N: Int>(): Int
    If N == 0
        Return 0
    End If
    Return 1 + Bits<N / 2>()
End Function

Print(Bits<8>())
```

Expected: 4

Actual: t38.fb:7:16: error: 'Bits' gets N / 2 for N here, which leads back to this code, so it would need a larger instance each time, without end

Suggested fix: Don't treat an edge as growing when its argument is a computed Const value that uses / or % (or any Const VALUE), and leave those to lowering's max_chain check. Or, if that recursion is meant to be an error, say so in spec 07, and word the message for Const values as 'a different instance each time' rather than 'larger'. Add Bits and Cycle as tests.

Skeptic: Ran f6.fb: "'Bits' gets N / 2 for N here, which leads back to this code, so it would need a larger instance each time, without end". Cycle<(N + 1) % 3> is refused the same way, and so is Down<N - 1>, also called 'larger'. check_expansion sets .grows = argument != from (sema.cpp ~9950), so any computed Const argument counts as growing. Bits<8> needs only the instances 8, 4, 2, 1, and 0, so the message's claim is false. Spec 07 bases this error on code that 'would need instances without end'. Low: it's a conservative compile-time refusal, not wrong code, but the rule and its wording no longer fit / and %. At least the message needs fixing, or the spec needs to say so.

## S11U-const-7 (low): A type argument that starts with '(' on a continued line doesn't parse as a value

Status: open

Where: compiler/src/parser.cpp:3003

parenthesized_value_follows scans raw tokens from m_pos and returns false at the first NEWLINE. Inside '<...>' the parser steps over newlines lazily, so after `Q2<A,` and a line break, m_pos is on the NEWLINE before '('. The check says it's not a value, and the argument is parsed as a tuple type, which fails at '-'. A break inside the parentheses, `(A -` newline `1)`, fails the same way. The same argument without a leading '(' (`A - 1` on the continued line) parses fine, and so does the single-line form.

```
Import std.core

Struct Q2<Const A: Int, Const B: Int>
    Method Get(): Int => A * 10 + B
End Struct

Function Make<Const A: Int>(): Int
    Local q: Q2<A,
                (A - 1) * 2> = Q2<A, (A - 1) * 2>()
    Return q.Get()
End Function

Print(Make<3>())
```

Expected: 34

Actual: f7.fb:9:20: error: expected ')', found '-'
f7.fb:9:17: note: opened here

Suggested fix: Start the scan at skip_newlines(m_pos) and step with index = skip_newlines(index + 1), as type_arguments_follow does. Inside the parentheses a NEWLINE never ends the value, so drop the NEWLINE return, or keep it only at bracket depth 0. Add a parser test with the argument on its own line.

Skeptic: Ran f7.fb: 'f7.fb:9:20: error: expected ')', found '-'' with 'opened here' at 9:17. parenthesized_value_follows (parser.cpp ~3003) starts at m_pos, which is the NEWLINE after 'Q2<A,', and returns false on NEWLINE. type_arguments_follow uses skip_newlines instead. Part of the finding doesn't hold up: a break inside the parentheses, '(A -' then a newline then '1) * 2>', parses and prints 34 (f7b.fb). Only the leading-'(' on a continued line fails. Low, since writing it on one line works around it.

## S11U-const-8 (low): A size below 1 in a function's parameter type is reported at line 1, column 1 (the Import line)

Status: open

Where: compiler/src/lower.cpp:1906

begin_function resets m_span, and the parameter types are then made concrete. check_values reports with m_span.value_or(span_t{}), so the error points at offset 0 of the file, here 'Import std.core'. A return type's instance is reported at the Return, and a Local's at the Local, so only parameters get the empty span. Before this step, a size below 1 in a parameter type was rarely reachable. Now Grid<0> or N - 1 makes it common.

```
Import std.core

Struct Grid<Const W: Int>
    Field Cells: Float[W]
End Struct

Print("start")

Function Size(g: Grid<0>?): Int
    Return 1
End Function

Print(Size(Null))
```

Expected: the error at line 9, on g: Grid<0>?

Actual: f8.fb:1:1: error: Float32[0] comes out here, but a fixed array's sizes are from 1 to 2147483647
  |
1 | Import std.core
  | ^

Suggested fix: Set m_span to each parameter's declaration span (or the function's) while its type is made concrete, and test the span.

Skeptic: Ran f8.fb: 'f8.fb:1:1: error: Float32[0] comes out here, but a fixed array's sizes are from 1 to 2147483647', pointing at the 'Import std.core' line. begin_function (lower.cpp ~1906) calls m_span.reset(), and check_values reports at m_span.value_or(span_t{}), so a parameter type's instance gets offset 0. The error is right; only its location is wrong.

## S11U-const-9 (low): A UInt64 Const parameter refuses values above 9223372036854775807, though spec 07 says it takes any value of its type; the error is untested

Status: open

Where: compiler/src/sema.cpp:12191

value_of refuses an unsigned value above Int64's maximum ("M takes a value up to 9223372036854775807") because VALUE stores an Int64. Spec 07 says a Const type argument is "any value of its type", and the lane's call was 'any value of its parameter's type'. Real UInt64 constants above that limit exist, such as the 64-bit FNV offset basis 14695981039346656037. No test covers this error.

```
Import std.core

Struct Mask<Const M: UInt64>
    Method Get(): UInt64 => M
End Struct

Print(Mask<9223372036854775807>().Get())
Print(Mask<18446744073709551615>().Get())
```

Expected: 9223372036854775807
18446744073709551615

Actual: t30.fb:8:12: error: M takes a value up to 9223372036854775807, not 18446744073709551615

Suggested fix: Either support the full UInt64 range (keep the bit pattern and compare and fold as unsigned for an unsigned inner type), or say in spec 07 that a UInt64 Const argument goes up to 9223372036854775807. Either way, add a sema test for the error.

Skeptic: Ran f9.fb: 'f9.fb:8:12: error: M takes a value up to 9223372036854775807, not 18446744073709551615'. value_of (sema.cpp ~12191) refuses an unsigned value above Int64's maximum because a VALUE holds an Int64. Spec 07 says a Const type parameter's argument is 'any value of its type' and says nothing about this limit. grep finds no test with 'takes a value up to' in tests/. Low: rare, and the error message is clear.

## S11U-ops-alias-10 (low): Two doc comments are now on the wrong functions

Status: open

Where: compiler/src/sema.cpp:20729

alias_constructors() was put between usable_constructors()'s doc comment and its body, so sema.cpp:20729-20733 runs both comments together above alias_constructors, and usable_constructors (20760) has none. parser.cpp has the same problem: parse_alias was put under parse_enum's comment (713-716), so parse_alias carries the Enum text and parse_enum (728) has no comment.

```
Import std.core
Import std.math

Print(Meters(5))
```

Expected: 5 m. Each comment sits directly above the function it describes.

Actual: 5 m. sema.cpp:20729-20733 and parser.cpp:713-716 each run two functions' comments together above the new function, and usable_constructors and parse_enum have none.

Suggested fix: Move usable_constructors' three lines down to sit above it at 20760, and parse_enum's three lines down to sit above parse_enum.

Skeptic: Read the code. sema.cpp 20729-20733 puts usable_constructors' three comment lines ('Of a type's News, the ones code at span can use...') directly above alias_constructors' own two lines, and usable_constructors (about line 20760) has no comment. In parser.cpp 713-716, parse_enum's comment ('@Flags Enum ...', 'Enum Name: Type ...') sits above parse_alias's line, and parse_enum (728) has none.

## S11U-ops-alias-9 (low): Operator <<Const B: Int>(...) lexes as the shift Operator <<, with a cascade of errors

Status: open

Where: compiler/src/parser.cpp:1889

A generic Operator < written without a space reads its '<' and the type parameter list's '<' as one '<<' token, so the declaration parses as Operator << and gives six unrelated errors. `Operator < <Const B: Int>(...)` works, as do `><`, `<=<`, `>=<`, `>><`, and `<<<`. This spelling first became possible with this step's generic Operators.

```
Import std.core

Struct Q<Const A: Int>
    Field V: Double
    Operator <<Const B: Int>(x: Q<A>, y: Q<B>): Bool => x.V < y.V
End Struct

Print(Q<1>(1) < Q<2>(2))
```

Expected: True (as with `Operator < <Const B: Int>`)

Actual: lt2.fb:5:14: error: Operator << takes two parameters, then 'an Operator gives a value, so it needs a return type', 'expected '(', found 'Const'', 'expected 'End Operator'', and '<' can't combine Q<1> and Q<2>

Suggested fix: In parse_operator_name, when the token is '<<' and the next token isn't '(', take it as '<' followed by the type parameter list's '<'. Or report "put a space between < and its type parameters". Add a parser test.

Skeptic: `fb check f9.fb` reports 6 errors: 'Operator << takes two parameters', 'needs a return type', 'needs a parameter of that type', "expected '(', found 'Const'", "expected 'End Operator'", and "'<' can't combine Q<1> and Q<2>". f9b.fb, written with a space (`Operator < <Const B: Int>`), prints True. Spec 06 writes an Operator's type parameters touching its symbol (`Operator *<Const L2: Int>`). parse_operator_name (parser.cpp ~1887) already splits a touching '>>' and '[]=' but has no case for '<<' followed by a type parameter list, so the touching form misparses only for '<'.

## S11U-units-1 (low): Abs of a Quantity keeps -0's sign, unlike std.math's Abs

Status: open

Where: std/math/units.fb:85

units.fb's Abs is `q.Value < 0 ? -q.Value : q.Value`. -0 < 0 is False, so -0 comes back as -0. std.math's scalar Abs says it gives "0 for -0" (scalar.fb:10) and does that, and the vectors' Abs follow it. So Abs(Meters(-0.0)) prints "-0 m", and anything that divides by its Value gets -inf instead of inf.

```
Import std.core
Import std.math

Local negZero: Double = -0.0
Print(Abs(negZero), Abs(Meters(negZero)), 1 / Abs(Meters(negZero)).Value)
```

Expected: 0 0 m inf

Actual: 0 -0 m -inf (the same in debug and release)

Suggested fix: Use the scalar Abs, which handles -0 and NaN as std documents: `Return Quantity<L, M, T>(Abs(q.Value))`. I checked this on a stand-in struct: it prints 0 for -0 and 3 for -3. Add an Abs(Meters(-0.0)) line to math_units.fb.

Skeptic: I ran units-verify/u1.fb, which prints Abs(negZero), Abs(Meters(negZero)), and 1 / Abs(Meters(negZero)).Value. Debug and release both print "0 -0 m -inf". The scalar Abs gives 0 but the Quantity Abs gives -0 m. The cause is std/math/units.fb:85, `q.Value < 0 ? -q.Value : q.Value`: -0 < 0 is False, so -0 comes back unchanged. std/math/scalar.fb:10-15 promises "0 for -0" and uses `value <= T.Zero() ? T.Zero() - value : value`, and vector.fb:285-287 calls the scalar Abs for each component. So the Quantity Abs breaks the family's documented behavior. NaN behaves the same in both Abs functions. Calling the scalar Abs, `Quantity<L, M, T>(Abs(q.Value))`, fixes it. I rated it low rather than medium because only -0 is affected.

## S11U-units-2 (low): ToString overflows negating an exponent of Int32's lowest value: debug panics, release prints a wrong unit

Status: open

Where: std/math/units.fb:29

ToString negates a negative exponent in Int32 with `Power(part.0, -part.1)`. Quantity<-2147483648, 0, 0> is a valid instance: a Const argument can be any value of its type. For it, the negation overflows. A debug build panics inside std. A release build wraps and prints "1/m^-2147483648", which reads as m to the plus 2^31. So the results differ between debug and release, and neither is right.

```
Import std.core
Import std.math

Print(Quantity<-2147483648, 0, 0>(1))
```

Expected: 1 1/m^2147483648

Actual: debug: panic: integer overflow in '-' at std/math/units.fb:29:53 (exit code 101); release: 1 1/m^-2147483648

Suggested fix: Negate in an Int64 before formatting. Either give Power an Int64 exponent and pass `-(part.1 As Int64)`, or build the text from `-(exponent As Int64)`. On a stand-in, "{unit}^{-(exponent As Int64)}" with -2147483648 prints m^2147483648. Add the line to math_units.fb.

Skeptic: I ran units-verify/u2.fb, which is Print(Quantity<-2147483648, 0, 0>(1)). Debug: "panic: integer overflow in '-' at std/math/units.fb:29:53" and exit code 101. Release: "1 1/m^-2147483648" and exit code 0. The lane says a Const argument can be any value of its type, so this instance is valid. Its ToString either panics inside std or prints the wrong unit. The cause is the Int32 negation `Power(part.0, -part.1)` at units.fb:29, which passes Power's `exponent: Int` (line 77). Negating in an Int64 fixes it.

## S11U-units-4 (low): math_units.fb doesn't test Defaultable or ToString's 1/(a b) form

Status: open

Where: tests/programs/math_units.fb:32

stdlib.md says a Quantity is Defaultable, and units.fb has DefaultValue. No test calls it, neither math_units.fb nor sema_tests. ToString's branch with nothing above and several units below (`above == ""` together with `belowCount > 1`) is also never reached: the tests print "1/s" (one unit below) and "kg/(m s^2)" (with a numerator), never "1/(m s)". Both work now, as the program shows, but nothing would catch a regression.

```
Import std.core
Import std.math

Function Made<T: Defaultable>(): T => T.DefaultValue()

Print(Made<Meters>(), Newtons.DefaultValue(), Quantity<-1, 0, -1>(2))
```

Expected: 0 m 0 kg m/s^2 2 1/(m s), with lines like these in math_units.fb and its .out

Actual: It prints 0 m 0 kg m/s^2 2 1/(m s), but math_units.fb has no DefaultValue or Defaultable call and no Quantity with only negative exponents across two or more units.

Suggested fix: Add to math_units.fb a generic `T.DefaultValue()` call on a unit, such as Made<Meters>() through a `Function Made<T: Defaultable>(): T`, and a print of a Quantity like Quantity<-1, 0, -1>(2). Then update math_units.out.

Skeptic: I ran units-verify/u4.fb. Debug and release both print "0 m 0 kg m/s^2 2 1/(m s)", so both features work today. Then I grepped the tests. tests/programs/math_units.fb and its .out never call DefaultValue or use Defaultable. The step's sema_tests units case (sema_tests.cpp:7835-7847) covers only /, +, *, and <. No test reaches ToString's form with nothing above the line and two or more units below it. math_units.out has "1/s", with one unit below, and "kg/(m s^2)", which has units above. stdlib.md says a Quantity is Defaultable, so that claim is untested. Each ToString line does run on its own, so the 1/(a b) gap is the smaller half of this finding.

## Refuted by the skeptics (no action)

- S11U-const-10: docs/milestones.md still lists computed Const type arguments with -, /, and % as Deferred work for M6 step 11. The row is still at milestones.md line 258, but docs/m6-plan.md's Roles say the coordinator 'alone edits CLAUDE.md, docs/milestones.md, docs/decisions.md, docs/history.md'. Landing step 3 is where the coordinator updates milestones.md. The lane isn't allowed to remove the row, so a stale row before landing is the process working as planned, not a defect in this change. f10.fb prints '0 0 -5' as expected.
- S11U-const-11: docs/decisions.md still says a Const type parameter is a positive integer. decisions.md line 390 does still say 'positive integer'. But m6-plan.md says that for language calls 'the lane decides ... says so in its report, and the coordinator records it in decisions.md', and that only the coordinator edits decisions.md, at landing (step 3). Recording the lane's 'any value of its type' call is the coordinator's job at landing, not a defect in this change. f11.fb prints '0 -5'.
- S11U-const-12: No tests for the Int64-edge folding and the new parenthesized names of computed values. No wrong behavior: f12.fb prints '0 -9223372036854775808', as the reviewer also found by hand. worked_out (types.cpp) handles min / -1 (nullopt), x % -1 (0), and subtract overflow (__builtin_sub_overflow) correctly. Some of it is already tested: mir_tests check divide by 0, % by 0, and a multiply overflow ('passes what an Int64 holds'). One case checks a grouped name, 'Show<10 % (N - 3)>(): ... 10 % 0'. A coverage gap with correct code doesn't contradict the spec or the milestones row, so it is refuted under the confirm-only-on-wrong-behavior rule.
- S11U-ops-alias-2: std.math's Seconds Alias hides the built-in Seconds(n), so Await Seconds(2) breaks once std.math is imported. Reproduced: with Import std.math, `Await Seconds(0.01)` gives 'Await waits for a Task, not Quantity<0, 0, 1>'. Without it (f2b.fb) it prints gate opens / top / gate closes. With `Import std.math As M` (f2d.fb) the built-in works and M.Meters(2) prints '2 m'. This is what the docs say should happen. Spec 11 line 22-23 says a built-in Function is hidden by a type or variable of its name, imported ones included, and decisions.md row 458 agrees. Milestones row 11's done-when names a unit called Seconds ('Meters / Seconds is MetersPerSecond, and adding Meters to Seconds is a compile error'), and stdlib.md listed Seconds as a unit both before and after this step. Row 612 is only about how Durations are spelled. So nothing here contradicts spec, stdlib.md, or the milestones row. It is still a real design conflict worth raising with the user: any file that imports std.math loses spec 10's `Await Seconds(n)` unless it uses an `As` or From import. The second symptom isn't wrong either: without std.math, 'Seconds' names the built-in Function, so "'Seconds' isn't a type" is accurate.
- S11U-units-3: A Double can't be divided by a Quantity, so 1 / period (a Hertz) doesn't compile. I ran units-verify/u3.fb. The reported error is real in debug and release: "u3.fb:5:26: error: no Operator / takes (Int32, Quantity<0, 0, 1>)". But the docs don't promise this operation. stdlib.md's Units bullet says "`*` and `/` any two ... and a Double scales", and units.fb:5-6 says "a Double scales one". 1 / period isn't scaling: it gives different units. The milestones row only asks that Meters / Seconds be MetersPerSecond and that Meters + Seconds be an error. The rest of std doesn't do this either. `grep "Operator /" std/math/*.fb` finds no scalar / value Operator in vector.fb, int_vector.fb, matrix.fb, or fixed.fb: each takes (value, scale) only, while `*` takes both orders. "50 1/s" in the ToString comment is just how a value prints; Hertz(50) gives it. Inverses can still be written: Quantity<0, 0, 0>(1) / period goes through the generic Operator /. This is a feature request, not a bug.
