# The review of M6 partial type-argument inference (the user's call, 2026-10-26): a call writes a generic Function's or Method's first type arguments and the compiler infers the rest

4 confirmed findings: 0 high, 3 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## infer-1 (medium): When a call's type arguments can't be inferred, errors inside its lambda arguments are hidden

Status: fixed in 822e9454 (a call that can't infer its type arguments checks each lambda as its parameter's Func, with what was told in it, and a parameter of a type still untold isn't told it needs a type, while the body's errors are reported), with a sema case of [1, 2].Map(Function(x) => Doubel(x)), Apply(1, Function(x: Int) => Missing(x)), and Apply<Int>(1, Function(x) => Missing(x)), each giving the call's error and the lambda's. A call whose arguments don't fit the parameters, like one too few, now reports that instead of what it leaves untold ("missing an argument for parameter 'f'"), since its lambdas have no Func to be checked as

Where: compiler/src/sema.cpp:20529

The step changed check_expr's failure path so that every lambda argument is checked inside quietly(...) after report_uninferred. This drops the old cascade error "'x' needs a type", but it also drops all of the lambda body's real errors. A typo in a lambda passed to Map, Filter, or a user's generic function now shows only "can't tell what U is in this call", which points at the call instead of the mistake. The lambda's parameter types are often fully known (T comes from the array or from what's written), and U can't be inferred only because the body has an error. The new sema test (the Task Throws case) asserts that the lambda isn't reported, and no test checks that a lambda body's own errors still appear. The old compiler (build-rev2's fb) reported "'Doubel' isn't declared" for this program. It also happens without written type arguments, e.g. Apply(1, Function(x: Int) => Missing(x)).

```
Import std.core
Import std.collections

Function Double(x: Int): Int => x * 2

Local doubled = [1, 2, 3].Map(Function(x) => Doubel(x))
Print(doubled.Count)
```

Expected: An error at the lambda body: "'Doubel' isn't declared" (with or without the call's "can't tell what U is" error).

Actual: Only one error: "g3.fb:6:17: error: can't tell what U is in this call; write the type arguments, like [1, 2, 3].Map<Int>(...)". The undeclared 'Doubel' is never reported.

Suggested fix: In the failure path, check each lambda against its parameter's Func type with the known type arguments substituted, setting m_inferring to the unknown ones as infer_from_arguments does. Report its errors and suppress only the "needs a type" error for a parameter whose type mentions an unknown type parameter. Alternatively, quiet only the lambdas whose parameter types mention an unknown type parameter. Add a sema test showing that a body error such as an undeclared name is still reported.

Skeptic: build-rev2's fb is the old compiler (built from 3c2621e8; it rejects Two<Int64>(1, 2.5) with "takes 2 type arguments, not 1"), so I used the reviewer's build of 59a4a08e at rv-infer/sema/build/cli/fb.exe as the new one. With the finding's program, the new fb gives only "i1.fb:6:17: can't tell what U is in this call". The old fb also reports "'Doubel' isn't declared" at 6:46. Apply(1, Function(x: Int) => Missing(x)) is the same: the new fb gives only the U error, and the old one also gives "'Missing' isn't declared", even though x has a written type. The cause is the diff at sema.cpp ~20529. The failure path now checks every lambda argument inside quietly(...), and that hides the lambda body's real errors along with the 'x needs a type' cascade. This is a regression in diagnostics.

## infer-2 (medium): An overload with fewer type parameters than the call wrote still ranks as fitting, so a valid call is ambiguous or rejected

Status: fixed in 822e9454 (rank() gives NONE to an overload with fewer type parameters of its own than the call wrote, and when none takes as many, the error is "no overload of 'G' takes 3 type arguments"), with a sema case of G<Int64, String>(x, "s") and (1, "s") choosing G<A, B>, and of three written for both G and H, and tests/programs/partial_type_arguments.fb printing which overload runs

Where: compiler/src/sema.cpp:22782

In rank(), infer() goes through known_arguments(), which calls resolve_arguments(written, parameters, span, true). When more type arguments are written than the candidate has, the count error is raised quietly and an empty vector comes back. The written ones are then dropped and inferred from the arguments instead, so the candidate ranks EXACT or CONVERTIBLE as if nothing had been written. With partial inference, overload sets whose members differ in type-parameter count become a normal case. The overload that can't take two type arguments then ties with the one that can (ambiguous), or wins and is reported afterwards ("takes 1 type argument, not 2"). This predates the step (the old build behaves the same), but it is this step's interaction between written arguments and overload resolution, and "what's written is taken as given" doesn't hold here.

```
Import std.core

Function G<A>(a: A, b: String): String => "one {TypeName<A>()}"
Function G<A, B>(a: A, b: B): String => "two {TypeName<A>()} {TypeName<B>()}"

Local x: Int64 = 1
Print(G<Int64, String>(x, "s"))
```

Expected: two Int64 String

Actual: error: the call to 'G' is ambiguous (could be either G). With a literal, Print(G<Int64, String>(1, "s")) gives "error: 'G<Int64, String>' takes 1 type argument, not 2" instead.

Suggested fix: Make rank() return match_t::NONE for a candidate that has more written type arguments than it has (own) type parameters. For example, have infer()/known_arguments report a count mismatch as nullopt instead of leaving every argument unknown. Add a sema test with overloads that differ in type-parameter count.

Skeptic: With G<A>(a: A, b: String) and G<A, B>(a: A, b: B) and Local x: Int64, G<Int64, String>(x, "s") gives "the call to 'G' is ambiguous" (could be either G). With a literal first argument it gives "'G<Int64, String>' takes 1 type argument, not 2". Only G<A, B> can take two written type arguments, so the valid call is rejected, against spec 07's "What's written is taken as given". The old fb behaves the same, so the bug predates this step. resolve_arguments(..., leading=true) still treats written.size() > count as a count error, but in ranking that error is raised quietly and the candidate is then ranked by inference alone.

## infer-3 (medium): Type arguments written on a non-generic Function or Method are silently accepted

Status: fixed in 822e9454 (check_written_arguments() reports "isn't generic" for type arguments written on a Function or Method that isn't generic, and rank() gives it NONE, so a generic overload is chosen), with a sema case of Plain<String, Bool>(1), Db().Plain<String>(1), and the function value Plain<String>, and H<Int64>(1, "s") running the generic H in the sema case and the program

Where: compiler/src/sema.cpp:21503

told_arguments() returns {} as soon as type_parameters_of(function) is empty, before known_arguments() and so before resolve_arguments(), which would report "'...' isn't generic". check_written_arguments only covers constructors. So H<String, Bool>(1) on a non-generic H compiles and runs, and the written arguments are ignored. This contradicts "what's written is taken as given", and it also lets a non-generic overload win ranking against a generic one for a call that wrote type arguments (b1: H<Int>(1, "s") with a plain H(Int, String) and a generic H<A, B> runs the plain one). This predates the step (the old build accepts it too), but it falls under the written-arguments checks this step reworked.

```
Import std.core

Class C
    Method M(a: Int): String => "plain {a}"
End Class

Function H(a: Int): String => "plain {a}"

Print(C().M<String>(1))
Print(H<String, Bool>(1))
```

Expected: Compile errors, like "'C().M<String>' isn't generic" and "'H<String, Bool>' isn't generic".

Actual: Compiles and prints:
plain 1
plain 1

Suggested fix: When type_parameters_of(function) is empty and the callee wrote type arguments, report "isn't generic", e.g. in check_written_arguments for non-constructors. Make rank() treat that candidate as NONE so a generic overload is chosen. Add sema tests for both.

Skeptic: C().M<String>(1) and H<String, Bool>(1) on non-generic M and H compile and print "plain 1" twice, in debug and in --release. With a plain H(Int, String) and a generic H<A, B>, H<Int64>(1, "s") runs the plain one (prints "plain"), so the written Int64 is dropped. The function-value path does reject this: Local f: Func(Int): String = Plain<String> gives "'Plain<String>' isn't generic". So calls and function values disagree. The cause is told_arguments (sema.cpp:21501): it returns {} when type_parameters_of is empty, before resolve_arguments can report "isn't generic". This predates the step but contradicts "what's written is taken as given".

## infer-4 (low): The function-value error's example repeats the type arguments the code already wrote

Status: fixed in 822e9454 (the function value's example writes every type argument: what's written as written, what the Func told, and one that fits for each of the rest, or none when nothing every program has fits), with the sema case's Query<Int> now expecting "like Query<Int, Int>"

Where: compiler/src/sema.cpp:17936

In check_function_value, when the first type arguments are written and the Func expected can't give the rest, the error correctly names the missing parameter (A). But its example is still the fixed text "like {name}<Int>", which here is exactly what the user wrote (Query<Int>), so it gives no help. The call path's example (report_uninferred) writes what was written plus a filler for each missing one, e.g. Db().Query<Int, Int>(...). The new sema test asserts the unhelpful text (sema_tests.cpp:4112).

```
Import std.core

Function Query<T, A>(sql: String, a: A): T[] => []

Local g: Func(Int): Int = Query<Int>
```

Expected: An example that writes every type argument, e.g. "'Query' is generic, so it needs a Func type to say what A is, or type arguments, like Query<Int, Int>".

Actual: error: 'Query' is generic, so it needs a Func type to say what A is, or type arguments, like Query<Int>

Suggested fix: Build the example from the written type arguments' text followed by Int (or '1' for a Const parameter) for each one not told, as report_uninferred does, and update the sema test's expected text.

Skeptic: Local g: Func(Int): Int = Query<Int> with Query<T, A> gives "'Query' is generic, so it needs a Func type to say what A is, or type arguments, like Query<Int>". The example repeats exactly what was written. The diff at sema.cpp ~17936 kept the fixed text "like {}<Int>". Spec 07, as this step changed it, says the error asks for the type arguments "with what it wrote and what it told" (e.g. Query<Player, Int>), and report_uninferred does build that example for calls. The function-value message doesn't.

## Refuted by the skeptics (no action)

- infer-5: An overload that has every type argument written ties with one that infers the rest, and spec 07 doesn't say which wins. The ambiguity reproduces: G<Float>(1, 2) with G<A>(a: A, b: Int) and G<A, B>(a: A, b: B) gives "the call to 'G' is ambiguous". But both overloads do fit under spec 07: G<A> takes Float as given, and G<A, B> takes Float and infers B as Int. The spec's only tie-break is generic against non-generic overloads, so reporting ambiguity is consistent with it. This is a possible spec addition, not a contradiction of the spec, stdlib.md, or milestones, so I'm refuting it.
- infer-6: No milestones.md row or decisions.md entries record partial type-argument inference. It's true that milestones.md, stdlib.md, decisions.md, and history.md don't mention partial type-argument inference at 59a4a08e or on master, and the program prints "Int64 Float32" as intended. But in this project the coordinator adds the decisions, history, and milestones entries when a stack lands; git log of decisions.md shows only "... in the notes" landing commits. coordinator-notes.md records the user's 2026-10-26 decision ("user chose partial type-argument inference (trailing ones inferred) -> lane 2"). The lane's spec 07 text matches it. No behavior is wrong and no code contradicts the docs, so this is a landing-time docs task, not a defect in this change.
