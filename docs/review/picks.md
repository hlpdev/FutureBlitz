# The review of M6 step 30's For Method picks (the user's A1 and B1): `Where m Has Get Or Post ...` picks Methods by attribute at compile time, and a typed loop takes throwing Methods when its Func type throws

10 confirmed findings: 1 high, 5 medium, 4 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## sema-1 (high): A picked loop's constraint vouches for an unpicked loop over the same type parameter, and the compiler hits an assertion

Status: fixed in b015b07e (same_method_bound() also needs the two loops' picks to be the same), with sema_tests' "a picked or throwing For Method vouches only for its own Methods, and only its own Func type throws": Routes<T>'s Get-picked loop no longer vouches for All(api), which is an error

Where: compiler/src/sema.cpp:10339

same_method_bound() treats another For Method loop over the same type parameter as proof of a Method constraint when the Func type, the type parameter's position and the Interface all match. It never compares the loops' picks. A loop with `Where m Has Get` only shows that the Get Methods' A is Defaultable, but it is accepted as proof for a loop that has no picks and goes over every Method. Here `All(api)` inside Routes<T> passes the check. Routes(Api()) checks only the picked Methods, so Log is never looked at. Lowering then instantiates All<Api>, copies the loop body for Log with A = Socket, and looks up Socket's DefaultValue, which hits `assert(false)` at lower.cpp:1832. Calling All(Api()) directly reports the error properly. This only happens with picks, so it is new in this change.

```
Import std.core

Attribute Get

Struct Args Implements Defaultable
    Field Id: Int
    Static Method DefaultValue(): Args => Args(1)
End Struct

Struct Socket
    Field Id: Int
End Struct

Class Api
    @Get Method Find(a: Args): String => "find {a.Id}"
    Method Log(s: Socket): String => "log {s.Id}"
End Class

Function All<T>(api: T)
    For Method m: M In T
        Where M: Func(A): String
        Where A: Defaultable
        Print(m.Call(api, A.DefaultValue()))
    End For
End Function

Function Routes<T>(api: T)
    For Method m: M In T
        Where M: Func(A): String
        Where A: Defaultable
        Where m Has Get
        Print(m.Call(api, A.DefaultValue()))
    End For
    All(api)
End Function

Routes(Api())
```

Expected: A compile error at `All(api)` or at `Routes(Api())`, like the one a direct All(Api()) gives: "'All' needs what each Method of Api has for A to implement Defaultable, but its Method 'Log' has Socket for A, which doesn't".

Actual: The compiler aborts in both debug and release builds: "Assertion failed: false, file .../compiler/src/lower.cpp, line 1832", exit code 3.

Suggested fix: In same_method_bound(), accept `other` only when its loop picks no more narrowly than `bound`'s loop. The simplest rule is that the two patterns' picks are equal. A looser rule is that `known->second.picks` is empty, or that each of its lines is also one of the wanted loop's lines. Add a sema test that has a picked loop in a function calling an unpicked one over the same T.

Skeptic: Ran the finding's program as rv-picks/sema/s1.fb. Debug and --release both stop with "Assertion failed: false, file .../rev-crypto/compiler/src/lower.cpp, line 1832" and no error is reported. In sema.cpp:10339, same_method_bound() compares only the signature, the type parameter's position in `fresh`, and the Interface. It never looks at the patterns' picks. So Routes' `Where m Has Get` loop, which checked only Find, is taken as proof for All's unpicked loop, which also goes over Log(Socket). Spec 13 says another loop vouches only when it is of the same Func type with the same line on the same type parameter. A picked loop covers fewer Methods, so it can't vouch for an unpicked one, and the compiler crashes instead of giving an error.

## parse-1 (medium): A Func type with no result can't take a Throws, so throwing Methods that give nothing can't be gone over

Status: fixed in b015b07e (parse_function_type() takes a Throws after a Func type without a result too, as Func(A) Throws E), with parser_tests' "For Field and For Method go over a type's members"'s Func(Int) Throws E and tests/programs/reflection_picked_methods.fb's Drop, which prints "dropped 1" and "Drop threw: no -1"; spec 13 writes the form

Where: compiler/src/parser.cpp:4036

parse_function_type() only accepts a Throws when the Func type has a result (`if (result && !function_result && accept(KW_THROWS))`). A typed loop over Methods that give nothing works (`Where M: Func(Int)`), and the untyped loop only takes Methods that throw nothing, so a Method like `Method Drop(id: Int) Throws Missing` can't be gone over by any For Method loop. `Func(Int) Throws E` is unambiguous outside a function's result, which already has its own guard, but it's a parse error.

```
Import std.core

Class Missing Extends Error
End Class

Class Api
    Method Drop(id: Int) Throws Missing
        If id < 0 Then Throw Missing("no {id}")
        Print("dropped {id}")
    End Method
End Class

For Method m: M In Api
    Where M: Func(Int) Throws E
    Where E: Error
    Try
        m.Call(Api(), 1)
        m.Call(Api(), -1)
    Catch e As Error
        Print("{m.Name} threw: {e.Message}")
    End Try
End For
```

Expected: dropped 1
Drop threw: no -1

Actual: noresult.fb:14:24: error: expected end of line, found 'Throws'
noresult.fb:15:11: error: 'E' isn't a type parameter here
noresult.fb:19:16: error: nothing in the Try block throws Error

Suggested fix: Drop the `result &&` from the condition, so a Func type without a result takes a Throws too (still not when function_result), and add a parser test and a program case for a throwing Method that gives nothing; spec 13 can then say `Func(A) Throws E` as well.

Skeptic: Ran rv-picks/parse/p1.fb (the finding's program) in debug: 'p1.fb:14:24: error: expected end of line, found 'Throws'', then 'E' isn't a type parameter here, and 'nothing in the Try block throws Error'. parser.cpp:4036 is `if (result && !function_result && accept(KW_THROWS))`, so `Func(Int) Throws E` can't parse. Spec 13 says a typed loop takes Methods that throw when its Func type throws, and `Func(A)` is the Func type of a Method that gives nothing, while the untyped loop takes only Methods that throw nothing. So no For Method loop can go over `Method Drop(id: Int) Throws Missing`. Nothing makes `Func(A) Throws E` ambiguous outside a function's result, which has its own guard.

## parse-3 (medium): In a loop's `Func(A): Func(): R Throws E`, the Throws goes to the result's Func, so a Method declared with the same text is silently skipped

Status: fixed in b015b07e (a Func type's result is parsed as a function's is, so a Throws after it is the outer Func type's, and a result that throws goes in parentheses), with parser_tests' "For Field and For Method go over a type's members"'s Func(Int): Func(): Int Throws E and Func(Int): (Func(): Int Throws E), which the dump now tells apart, and the program's Make, which the loop goes over. The one parser case that read Func(): Task<Int> Throws A as a Func value giving a throwing Task ("a Task's type takes a Throws, unless it's a function's result") now expects the Func's Throws, and spec 10 says such a Task goes in parentheses

Where: compiler/src/parser.cpp:4029

A function header gives a Throws after its result to the function (m_function_result), but parse_function_type() sets m_function_result = function_result (false in a Where line) for its own result, so in a loop's Func type a result Func takes the Throws. `Method Make(id: Int): Func(): Int Throws Oops` throws from Make, but `Where M: Func(Int): Func(): Int Throws E` means Methods giving a throwing Func value. Sema doesn't reject that inner throwing Func (parse-4), so the loop compiles and goes over nothing. The dump ('Func(Int): Func(): Int Throws E') doesn't show which Func throws.

```
Import std.core

Class Oops Extends Error
End Class

Class Api
    Method Make(id: Int): Func(): Int Throws Oops
        If id < 0 Then Throw Oops("bad {id}")
        Return Function() => id
    End Method
End Class

Print("start")
For Method m: M In Api
    Where M: Func(Int): Func(): Int Throws E
    Where E: Error
    Print(m.Name)
End For
Print("end")
```

Expected: start
Make
end   (the Throws is the loop Func's, as in Make's own header), or else a compile error that a Func value can't throw

Actual: start
end

Suggested fix: Parse a Func type's result as a function's result is parsed (parse_type with m_function_result set for the outermost result type), so a Throws after it belongs to the outer Func, as in a Method's header, and a result Func that throws needs parentheses; add a parser test of `Func(Int): Func(): Int Throws E`.

Skeptic: Ran p3.fb in debug: it printed 'start' then 'end' and compiled with no diagnostic. Make, declared `Method Make(id: Int): Func(): Int Throws Oops` (the Throws is Make's, per spec 10's 'Function Make(): Func(): Task<Int> Throws FileError can throw itself'), was silently skipped by `Where M: Func(Int): Func(): Int Throws E`. parse_function_type sets m_function_result = function_result (false in a Where line) for its result, so the inner result Func takes the Throws. Spec 13 writes the loop's form as `Func(A): R Throws E`, and it puts the Async Task form in parentheses, which implies the header rule. Either way the result is wrong: the loop should match Make or report the throwing Func value, not go over nothing in silence. This partly overlaps parse-4, which is why the error is missing.

## parse-4 (medium): A throwing Func nested in a loop's Func type is accepted, and the error lands on the loop's own Func type instead

Status: fixed in b015b07e (each Func type takes the loop's pattern flag before it resolves its parts, so a Func type inside the loop's own sees it cleared), with sema_tests' "a picked or throwing For Method vouches only for its own Methods, and only its own Func type throws": a throwing Func in the parameters, beside the loop's own throwing Func type, and in the result, each reported at the inner Func

Where: compiler/src/sema.cpp:12880

Sema allows a throwing Func type once per Where line through `std::exchange(m_method_pattern, false)`, but it resolves the parameters and result first, so a nested Func value type that throws consumes the flag. Then a nested throwing Func (a Func value, which spec 13 says can't throw) is accepted silently, and when the loop's own Func type throws too, 'a Func value can't throw' is reported on the outer, allowed Func type. E.g. `Where M: Func(Func(): Int Throws E): String` compiles and the loop silently matches nothing.

```
Import std.core

Class Oops Extends Error
End Class

Class Api
    Method Run(f: Func(): Int): String Throws Oops
        Return "ran {f()}"
    End Method
End Class

For Method m: M In Api
    Where M: Func(Func(): Int Throws E): String Throws E
    Where E: Error
    Print(m.Name)
End For
```

Expected: error: a Func value can't throw ... pointing at the inner `Func(): Int Throws E` (the parameter's type)

Actual: both.fb:13:14: error: a Func value can't throw; only a For Method loop's Func type says that its Methods do, pointing at the whole outer `Func(Func(): Int Throws E): String Throws E`. Without the outer Throws (`Where M: Func(Func(): Int Throws E): String`) it compiles with no error and prints nothing.

Suggested fix: Take m_method_pattern before resolving the function type's parts (e.g. `const bool pattern = std::exchange(m_method_pattern, false);` at the top of the function_type_t branch) and test that saved value for this Func's own throws, so nested Func types see it false; add sema tests for a nested throwing Func in a parameter and in the result.

Skeptic: Ran p4.fb: 'p4.fb:13:14: error: a Func value can't throw; only a For Method loop's Func type says that its Methods do', and its caret spans the whole outer `Func(Func(): Int Throws E): String Throws E`, which is allowed. Ran p4b.fb (no outer Throws): it compiled and printed 'start' then 'end', so the nested throwing Func value was accepted in silence. In sema.cpp around 12866-12884, the parameters and result are resolved through part() before `std::exchange(m_method_pattern, false)`, so a nested Func consumes the flag that belongs to the loop's own Func type. That contradicts spec 13's 'a Func value can't'.

## parse-5 (medium): A Callback type's Throws is now parsed and silently dropped

Status: fixed in b015b07e (the parser reports a Throws after a Callback's result: "a Callback can't throw, since C code takes no errors"), with parser_tests' "For Field and For Method go over a type's members"

Where: compiler/src/sema.cpp:12873

parse_function_type() now takes a Throws after any Func or Callback type's result, but sema returns callback_of() for a Callback before the 'a Func value can't throw' check, so `Callback(Int): Int Throws Oops` compiles and the Throws is ignored. On 9c156eb2 this was a parse error. A C function pointer can't throw.

```
Import std.core

Class Oops Extends Error
End Class

Function Twice(x: Int): Int
    Return x * 2
End Function

Global hook: Callback(Int): Int Throws Oops = Twice
Unsafe
    Print(hook(21))
End Unsafe
```

Expected: A compile error that a Callback can't throw (as a Func value can't)

Actual: Compiles and prints 42; the Throws Oops is ignored.

Suggested fix: In the parser, don't take a Throws after a Callback's result (or report it there), or in sema check function->throws before the callback branch and report it; add a parser or sema test.

Skeptic: Ran p5.fb: it compiled and printed 42, with `Callback(Int): Int Throws Oops` accepted and its Throws ignored. parse_function_type takes a Throws for Callback types too, since only `result && !function_result` guards it. In sema.cpp, `if (function->callback) return callback_of(...)` comes before the 'a Func value can't throw' check, so the Throws is dropped with no diagnostic. A C function pointer can't throw, and spec 13 lets only a For Method loop's Func type throw.

## sema-2 (medium): A non-throwing Override of a throwing Method keeps the base Method's slot in a throwing loop, so picks use the base Method's attributes

Status: fixed in b015b07e (loop_methods() lets an Override that doesn't fit the loop's Func type take its base's place too, by leaving it out, so a base is never kept for an Override), with tests/programs/reflection_picked_methods.fb's Base and Derived: "plain Find: 10" only

Where: compiler/src/sema.cpp:26480

loop_methods() replaces an overridden entry only when the Override itself matches the loop's Func type. An Override can now differ from its base in what it throws: a base `Find(n): Int Throws Missing` with an `Override Method Find(n): Int` that throws nothing. In a throwing loop over Derived, Base.Find matches and Derived.Find doesn't, so Base.Find's entry stays, and it is picked and tested with Base.Find's attributes. This breaks spec 13's "an Override goes by its own attributes". Without picks, the same Method is in both a throwing loop and a non-throwing loop over Derived, though spec 13 says the loop without a Throws goes over the Methods that don't throw, and only a throwing Func type takes the ones that do. Before this change, throwing Methods were never matched, so this could not happen.

```
Import std.core

Attribute Get
Class Missing Extends Error
End Class

Class Base
    @Get Method Find(n: Int): Int Throws Missing
        If n < 0 Then Throw Missing("neg")
        Return n
    End Method
End Class

Class Derived Extends Base
    // No Get here, so a loop that picks Get leaves it out.
    Override Method Find(n: Int): Int
        Return n * 2
    End Method
End Class

For Method m: M In Derived
    Where M: Func(Int): Int Throws E
    Where E: Error
    Where m Has Get
    Print("throwing picked {m.Name}: {Try? m.Call(Derived(), 4) ?? -1}")
End For
For Method m: M In Derived
    Where M: Func(Int): Int Throws E
    Where E: Error
    Print("throwing {m.Name}")
End For
For Method m: M In Derived
    Where M: Func(Int): Int
    Print("plain {m.Name}: {m.Call(Derived(), 5)}")
End For
```

Expected: Derived's Find has no Get and doesn't throw, so it appears only in the plain loop:
plain Find: 10

Actual: Debug and release print the same:
throwing picked Find: 8
throwing Find
plain Find: 10

Suggested fix: In loop_methods(), when a Method overrides an entry already in `methods` but doesn't match the signature, remove that entry instead of keeping it. Check `method.overrides` against `methods` before the `if (!bound) continue;`. Then an Override always takes its base's place, and it is picked by its own attributes and matched by what it throws. Add sema or MIR cases with a non-throwing Override of a throwing Method, with and without a pick.

Skeptic: Ran rv-picks/sema/s2.fb. Debug and release both print "throwing picked Find: 8", "throwing Find", "plain Find: 10". Derived.Find has no @Get, yet the Get-picked throwing loop takes it: 8 is Derived's n*2, reached through Base.Find's slot. In loop_methods() (sema.cpp around 26472), the `if (!bound) continue;` runs before the override replacement. Derived.Find doesn't match the throwing Func type, so it never replaces Base.Find's entry. The pick then reads Base.Find's attributes. This contradicts spec 13: "an Override goes by its own attributes", each Method sits "in the place of what it overrides", and "Without a Throws, the loop goes over Methods that don't throw" while the throwing loop also lists this non-throwing Method.

## parse-2 (low): A Func type's Throws inside a Func type's parameters swallows the next parameter as a thrown type

Status: fixed in b015b07e (parse_function_type() takes parse_type()'s `parameter` and ends a Throws at a ',' there, as a Task's does), with parser_tests' "For Field and For Method go over a type's members"'s Func(Func(): Int Throws E, Int): String, whose dump shows the two parameters, and the sema case's error at the inner Func. A Func type with an error no longer says what m.Call takes, so the misleading count error is gone too

Where: compiler/src/parser.cpp:4037

parse_function_type() calls parse_task_throws(false), so a ',' always goes on to another thrown type. Inside a Func type's parameter list (where a Task's Throws uses `one` from m_list_parameter so it ends at the ','), `Func(Func(): Int Throws E, Int): String` parses as a one-parameter Func whose inner Func throws E and Int. The dump prints the same text either way, so the parser test can't tell. Nothing reports it either (see parse-4): the loop just takes the wrong arity.

```
Import std.core

Class Api
    Method Two(f: Func(): Int, n: Int): String => "two"
End Class

For Method m: M In Api
    Where M: Func(Func(): Int Throws E, Int): String
    Where E: Error
    Print(m.Call(Api(), Function() => 1, 2))
End For
```

Expected: An error that a Func value (the parameter's Func type) can't throw, with the loop's Func type read as taking two parameters.

Actual: funcparam4.fb:10:11: error: 'm.Call' takes 2 arguments  (i.e. the loop's Func type took one parameter; Int became a second thrown type)

Suggested fix: Pass the `parameter` flag parse_type() takes from m_list_parameter into parse_function_type() and call parse_task_throws(parameter), as parse_named_type() does for a Task; add a parser test whose dump distinguishes the two readings (e.g. check the parameter count or an error).

Skeptic: Ran p2.fb (the finding's program): 'p2.fb:10:11: error: 'm.Call' takes 2 arguments', so the loop's Func type was read with one parameter and `Int` became a second thrown type of the inner Func. parser.cpp:4037 calls parse_task_throws(false), unlike parse_named_type's Task Throws, which gets `parameter` from m_list_parameter. Spec 10 says that in a Func type's parameters a ',' always goes on to the next parameter. The misparse can only affect programs that are already invalid, since a nested Func value can't throw. Its harm is a misleading error in place of 'a Func value can't throw' (which parse-4 also hides), so low.

## parse-6 (low): Spec 01's table of contextual words doesn't list Has and Or

Status: fixed in b015b07e (spec 01's Contextual table has a row for Has and Or)

Where: docs/spec/01-lexical.md:163

Spec 13 now says `Has` and `Or` are words only in a For Method's Where line on its variable, and the parser treats them that way, but spec 01's Contextual table, which lists every word that acts as a keyword in one position (Get/Set, Step, Lib, From, Weak, Sendable), wasn't updated.

```
Import std.core

Attribute Get

Class Api
    @Get
    Method A()
        Print("A")
    End Method
End Class

For Method m In Api
    Where m Has Get Or Get
    m.Call(Api())
End For
Local Has = 1
Local Or = 2
Print(Has + Or)
```

Expected: A
3, with spec 01's Contextual table listing `Has` and `Or` (after the loop's variable in a For Method's Where line)

Actual: Prints A and 3 as expected, but docs/spec/01-lexical.md's Contextual table has no row for Has or Or.

Suggested fix: Add a row to spec 01's Contextual table: `Has`, `Or` | in a For Method loop's Where line on its variable (`Where m Has Get Or Post`).

Skeptic: Ran p6.fb: it printed 'A' then '3', so Has and Or are keywords only after the loop variable in a Where line and are identifiers elsewhere. docs/spec/01-lexical.md's Contextual table, introduced as 'Ordinary identifiers everywhere except in one position, where they act as keywords', lists Get/Set, Step, Lib, From, Weak, and Sendable, but not Has or Or, while spec 13 now says 'Has and Or are words only there'. This is a docs inconsistency, so low.

## sema-3 (low): A throwing Method that gives nothing can't be in any For Method loop: `Func(A) Throws E` doesn't parse

Status: fixed in b015b07e (as parse-1), with parser_tests' "For Field and For Method go over a type's members" and tests/programs/reflection_picked_methods.fb's Drop

Where: compiler/src/parser.cpp:4036

The parser accepts a Throws after a Func type only when the type has a result (`if (result && !function_result && accept(KW_THROWS))`). The type table already names and prints a throwing Func type with no result (function() and qualified_name() both handle a NONE result with throws), but `Where M: Func(Int) Throws E` is a parse error. So a Method like `Method Remove(id: Int) Throws Missing`, for example a Delete route, can't be gone over by a typed loop. The untyped loop doesn't take throwing Methods, and a non-throwing typed loop doesn't either. The B1 decision is that a typed loop takes throwing Methods when its Func type throws, and nothing excludes Methods that give nothing.

```
Import std.core

Class Missing Extends Error
End Class

Class Api
    Method Remove(id: Int) Throws Missing
        If id < 0 Then Throw Missing("no {id}")
        Print("removed {id}")
    End Method
End Class

For Method m: M In Api
    Where M: Func(Int) Throws E
    Where E: Error
    Try
        m.Call(Api(), 1)
        m.Call(Api(), -1)
    Catch e As Error
        Print("{m.Name} threw")
    End Try
End For
```

Expected: removed 1
Remove threw

Actual: t12.fb:14:24: error: expected end of line, found 'Throws'
t12.fb:15:11: error: 'E' isn't a type parameter here
t12.fb:19:16: error: nothing in the Try block throws Error

Suggested fix: Accept a Throws after a Func type that has no result too: drop the `result &&` from the condition, keeping `!function_result`. Add the no-result form to the parser, sema and program tests, and to spec 13's bullet.

Skeptic: Ran rv-picks/sema/s3.fb. Debug and release both give "14:24: error: expected end of line, found 'Throws'" for `Where M: Func(Int) Throws E`, then cascade errors. parser.cpp:4036 accepts Throws only when the type has a result: `if (result && !function_result && accept(KW_THROWS))`. Spec 05 makes `Func(A)` a valid no-result Func type. Spec 13 says a typed loop takes throwing Methods when its Func type throws, and the untyped loop and non-throwing typed loops exclude throwing Methods. So a Method that gives nothing and throws can't be in any For Method loop. That is a real gap in B1, but routes give a Response, so it is low impact.

## sema-4 (low): The 'Func value can't throw' check uses one flag for the whole pattern, so a nested throwing Func is accepted and the outer one gets the error

Status: fixed in b015b07e (as parse-4: the flag is taken before a Func type's parts are resolved), with sema_tests' "a picked or throwing For Method vouches only for its own Methods, and only its own Func type throws"'s Func(Int[], Func(Int): Int Throws E): Int, reported at the inner Func

Where: compiler/src/sema.cpp:12880

method_signature() sets m_method_pattern for the whole resolve_type() of the loop's Func type. The first throwing Func type resolved clears the flag with std::exchange. Parameters are resolved before the outer type's own Throws is checked, so a throwing Func type nested in a parameter uses up the flag. In `Func(Int[], Func(Int): Int Throws E): Int`, the inner throwing Func value type is accepted with no error, though spec 13 says a Func value can't throw. In `Func(Func(Int): Int Throws E): Int Throws E`, the inner one is accepted and the error is reported against the outer loop Func type, which is allowed.

```
Import std.core

Class Missing Extends Error
End Class

Class Api
    Method Twice(n: Int): Int Throws Missing
        Return n * 2
    End Method
End Class

For Method m: M In Api
    Where M: Func(Int[], Func(Int): Int Throws E): Int
    Where E: Error
    Print(m.Name)
End For
Print("built")
```

Expected: error at 13:26 (the inner `Func(Int): Int Throws E`): a Func value can't throw; only a For Method loop's Func type says that its Methods do

Actual: It builds and prints "built". With `Where M: Func(Func(Int): Int Throws E): Int Throws E`, the error is reported at 13:14 and spans the whole outer loop Func type instead of the inner one.

Suggested fix: Let only the top-level Func type of the loop's pattern throw. For example, method_signature() could resolve the pattern's Func type itself, or record its type_expr_id_t and compare it with the type being resolved, instead of using a flag that any nested Func can take. Add sema cases for a throwing Func type nested in a parameter.

Skeptic: Ran rv-picks/sema/s4.fb (`Where M: Func(Int[], Func(Int): Int Throws E): Int`). Debug and release both build and print "built" with no error, though the inner `Func(Int): Int Throws E` is a Func value type, and spec 13 says "Only a For Method loop's Func type throws: a Func value can't." With s4b.fb (`Func(Func(Int): Int Throws E): Int Throws E`), the error is reported at 13:14 and spans the whole outer, allowed Func type. The cause is at sema.cpp:12880: `std::exchange(m_method_pattern, false)` is one flag for the whole pattern, and the first throwing Func resolved, here a nested parameter, uses it up.

## Found by lane 6 (lane6-step30.md's "For lane 2's review"), both fixed in b015b07e

- A loop's `Where E: Error` on what its Methods throw, which always holds, was a constraint each
  caller had to vouch for, so `Outer<T>()` calling `Inner<T>()` failed. collect_method_bounds()
  leaves out an Error constraint on a type parameter in the Func type's Throws, or its Task's
  (thrown_in()). Tested by the sema case's Inner<T>() from Routes<T>, and tests/programs/reflection_picked_methods.fb's
  Forward<Store>() calling Throwing<T>(), which prints "throws: Drop".
- The message named std.core's Error constraint with the caller's module, like
  "forward.Error". name_among() names what only a constraint names, Error or Class, as it's
  written. Tested by the sema case's Params<T>(): "... to implement Error, but ...".
