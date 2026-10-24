# The review of M6 lane 3's two compiler items: A-new-6 (an array, tuple, or Map literal fits an overload's parameter as it fits a lone Function's) and step 32's Deferred work row (For x In a range written there counts as a For loop does, making no Range)

3 confirmed findings: 0 high, 1 medium, 2 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## overloads-1 (medium): A literal whose element fails its check still fits every overload of its shape, so valid calls are 'ambiguous' and a typo's error is hidden

Status: fixed in 895d427 (a literal fits an overload's parameter only when nothing in it has an error as that type), with a sema test of [3000000000], a tuple and a Map holding it, and [1e300], each with an overload it fits and one it doesn't, of an untyped lambda whose body checks only as one overload's Func, and of [nope], whose own error is reported: the check as the parameter's type runs quietly on its own and fits only without an error

Where: compiler/src/sema.cpp:21519

match()'s new branch counts a literal as fitting whenever check(argument.value, type) isn't INVALID. But check_elements() and the Map check call expect_type(), and the tuple check calls converts(type, wanted). Both let an INVALID element through ('An INVALID type on either side passes'). So an element whose own check failed, with its error reported only in the quiet bag, still produces the parameter's type. 3000000000 for UInt8 and 1e300 for Float are checked as INVALID with an error, so [3000000000] 'fits' UInt8[] as well as Int64[], and the call is ambiguous. The scalar call A(3000000000) with Int64/UInt8 overloads correctly picks Int64, and each call below works when only its first overload exists (prints longs / long pair / long map / doubles). The same hole also breaks a call that worked before the change. With `Function Wide(xs: UInt8[])` and `Function Wide(xs: Int64[])`, `Print(Wide([nope]))` now says "the call to 'Wide' is ambiguous". Before the change (same sema as c4fadaf9) it said "'nope' isn't declared", which is what one Wide alone still says. This also explains why an untyped lambda whose body only type-checks for one overload's Func, like [Function(x) => x - 1] against (Func(String): String)[] and (Func(Int): Int)[], is called ambiguous: the lambda check returns its Func type despite body errors.

```
Import std.core
Import std.collections

Function A(xs: Int64[]): String => "longs"
Function A(xs: UInt8[]): String => "bytes"
Function P(pair: (Int64, String)): String => "long pair"
Function P(pair: (UInt8, String)): String => "byte pair"
Function M(m: Map<String, Int64>): String => "long map"
Function M(m: Map<String, UInt8>): String => "byte map"
Function F(xs: Float[]): String => "floats"
Function F(xs: Double[]): String => "doubles"

Print(A([3000000000]))
Print(P((3000000000, "x")))
Print(M({"a": 3000000000}))
Print(F([1e300]))
```

Expected: longs
long pair
long map
doubles

Actual: f1.fb:13:7: error: the call to 'A' is ambiguous
f1.fb:14:7: error: the call to 'P' is ambiguous
f1.fb:15:7: error: the call to 'M' is ambiguous
f1.fb:16:7: error: the call to 'F' is ambiguous
error: couldn't build f1.fb because of 4 errors

Suggested fix: In match()'s shaped-literal branch, run the check in its own quietly() and require that it reports no errors, as holds() does: `bool clean = false; const type_id_t made = quietly([&] { const type_id_t found = check(argument.value, type); clean = !m_diagnostics->has_errors(); return found; });` and return CONVERTIBLE only when `clean && made != INVALID && (made == type || converts(made, type))`. Add sema tests for [3000000000], a tuple and a Map holding one, [1e300], and Wide([nope]) reporting "'nope' isn't declared".

Skeptic: I ran the finding's f1.fb with rev-5's fb run. All four calls fail with "the call to 'A'/'P'/'M'/'F' is ambiguous". Controls in f1b.fb, with only the first overload of each, print longs / long pair / long map / doubles, and the scalar S(3000000000) with Int64/UInt8 overloads prints long. Wide([nope]) with UInt8[] and Int64[] overloads (f1c.fb) now says "the call to 'Wide' is ambiguous". One Wide alone (f1d.fb) says "'nope' isn't declared". The pre-change build, build-lane9-verify, says "'nope' isn't declared" for f1c. Its sema.cpp is byte-identical to `git show c4fadaf9:compiler/src/sema.cpp` (cmp). So the typo message is a regression. Untyped [Function(x) => x - 1] against (Func(String): String)[] and (Func(Int): Int)[] (f1e.fb) is also called ambiguous. In the code, match()'s new branch at sema.cpp:21519 only tests `made != INVALID`. check_elements() returns `array` whenever expect_type() passes, and expect_type() uses converts(info(id).type, type), which passes an INVALID element type ('An INVALID type on either side passes'). The overflowing element's error goes only to quietly()'s scratch bag. This contradicts the lane's own rule: 'an element that doesn't fit leaves that overload out'.

## overloads-2 (low): Trial checks of an untyped lambda in a literal leave reach edges, giving a false @MainThread error

Status: fixed in 41cea1b (what an overload that isn't chosen made of a call's lambdas goes, so the thread rules don't see it), with a sema test of the finding's program: choose_overload's tries run in on_trial(), which takes back what they added to m_reaches, m_main_only, and m_lambda_code, as quietly() does m_workers; only the tries that are checked again are trials, since some of quietly()'s other callers keep what they checked

Where: compiler/src/sema.cpp:22352

choose_overload() now checks an untyped lambda inside an array literal once against each overload's Func type. Every check_expr(lambda) makes a new lambda Function symbol and runs check_lambda_body(), which pushes it onto m_reaches[current code]. note_thread_use() records the calls its body resolves in the same way. quietly() puts back only the diagnostics, flow, handlers, and workers, so those edges stay. Here, the trial against Run's (Func(String): Int)[] overload resolves Touch(x) to the @MainThread Touch(String) overload. The thread rules then walk Work -> stale lambda -> Touch(String) and reject the program, even though the overload chosen (and the lambda actually lowered) calls Touch(Int). With the String overload of Run removed, the same program compiles and prints 1. Before the change this call failed differently ('x' needs a type), so the false error comes from the new trial checking.

```
Import std.core

@MainThread
Function Touch(s: String): Int => 0
Function Touch(n: Int): Int => n

Function Run(fs: (Func(String): Int)[], tag: String): Int => 0
Function Run(fs: (Func(Int): Int)[], tag: Int): Int => fs[0](tag)

Function Work(): Int => Run([Function(x) => Touch(x)], 1)

Local task = Spawn(Function() => Work())
Print(task.Wait())
```

Expected: 1

Actual: t7.fb:12:14: error: this work runs on another thread, but it calls 'Work', which calls 'Touch', which is @MainThread
   |
12 | Local task = Spawn(Function() => Work())
   |              ^^^^^
t7.fb:10:45: note: here
10 | Function Work(): Int => Run([Function(x) => Touch(x)], 1)

Suggested fix: Make quietly() put back what a trial adds to m_reaches, as it already does for m_workers. For example, save the size of m_reaches[current code] (and of any other vector it can push to) and trim them afterwards, or keep trial edges in a scratch map that quietly() drops. m_main_only and m_lambda_code need the same treatment. Add a sema test with this program.

Skeptic: I ran t7.fb, the finding's program, on rev-5. It fails: "t7.fb:12:14: error: this work runs on another thread, but it calls 'Work', which calls 'Touch', which is @MainThread", with the note at 10:45 (Touch(x)). With Run's String overload removed (t7b.fb) it compiles and prints 1. The pre-change build says "'x' needs a type" for t7.fb. A lambda passed directly, not in a literal, to the same overloads (t7c.fb) isn't trial-checked before or after the change, so this path is new. In the code, check_lambda_body (sema.cpp:22352) pushes the new lambda symbol onto m_reaches[current_code()], and note_thread_use (9765) pushes resolved calls and fills m_main_only for that stale lambda. quietly() (907-920) restores only the diagnostics, flow, handlers, and workers, so the trial's edge Work -> stale lambda -> Touch(String) survives. I rate it low: it needs an untyped lambda in a literal, overloads whose Func types make its body resolve to a different, @MainThread or Async, function, and a call from another thread. Typing x works around it.

## overloads-3 (low): Nested overloaded calls through array literals compile about 10x slower: each failing candidate re-checks the whole literal

Status: fixed in 8559f54 (a literal its own type converts to fits as it is, and is checked again only as a type it could become), with a sema test of the finding's six levels, under 15 seconds in a debug build (about 4; 37 without the fix): a literal whose own type converts to the parameter's fits without a second check, and one is checked as the parameter's type only where it could become it, an array literal as an array or an Interface, a tuple literal as a tuple of as many, and a Map literal as a Map or an Interface

Where: compiler/src/sema.cpp:21519

For every candidate whose parameter isn't the literal's own type, match() now calls check(argument.value, type). That includes parameters like Int, String, and Bool that no array, tuple, or Map literal can ever be. Each such check re-checks the whole nested literal, including the overloaded calls inside it, each of which runs its own overload resolution again. The work per nesting level therefore grows from about (1 + candidates) checks to about (1 + 2 x candidates), and the total grows exponentially with depth. The program below is a 6-deep tree built through an overloaded Of(Int/String/Bool/Json[]). It prints 6 on both builds. `fb check` took 5.0 s on a build whose sema.cpp is identical to c4fadaf9's (build-lane9-verify) and 50.4 s on the rev-5 build. At depth 5 it took 1.3 s before and 6.1 s after. With Of(values: Iterable<Json>) instead, where the literal converts, depth 5 took 1.6 s before and 10.3 s after. Before the change, a converting literal was settled with converts(natural, type) and no second check.

```
Import std.core
Import std.collections

Class Json
    Field depth: Int
    New(depth: Int)
        Self.depth = depth
    End New
End Class

Function Of(n: Int): Json => Json(0)
Function Of(s: String): Json => Json(0)
Function Of(b: Bool): Json => Json(0)
Function Of(values: Json[]): Json
    Local most = 0
    For v In values
        If v.depth > most Then most = v.depth
    End For
    Return Json(most + 1)
End Function

Print(Of([Of(5), Of([Of(4), Of([Of(3), Of([Of(2), Of([Of(1), Of([Of(0), Of(1)])])])])])]).depth)
```

Expected: 6, with `fb check` taking about as long as before the change (5.0 s with c4fadaf9's sema)

Actual: 6, but `fb check` takes 50.4 s (10x)

Suggested fix: In the shaped-literal branch, first return CONVERTIBLE when `natural != INVALID && converts(natural, type)`, without a second check. Only check the literal as `type` when its non-null form is a type the literal could become: an array (fixed or not) for an array literal, an Iterable<T> for an array literal, a tuple of the same arity for a tuple literal, or a Map instance for a Map literal. Return NONE for anything else without checking.

Skeptic: I timed `fb check` on the finding's program and shallower variants, comparing the pre-change build (build-lane9-verify, sema.cpp identical to c4fadaf9) with rev-5. Depth 4: 694 ms vs 1060 ms. Depth 5: 1205 ms vs 5538 ms. Depth 6: 4702 ms vs 49735 ms. Every check exited 0. The code shows why: for each candidate, match() now does check(argument.value, NONE) and then check(argument.value, type) on the shaped-literal path, even for Int/String/Bool parameters. There check(literal, type) acts like check(literal, NONE), so it gives the same answer as the old converts(natural, type) and only re-runs the nested overload resolution. That's a real, confirmed regression. I rate it low because the old code was already exponential in depth, and the extra cost only grows large at depth 5 or more of overloaded calls nested in literals (1.5x at depth 4).

## Refuted by the skeptics (no action)

- overloads-4: When another argument is the mismatch, a typeless literal that fits a parameter still reports its own error. The output is as described. t2.fb gives "an empty array needs a type from where it's used" and t2b.fb gives "'x' needs a type". But the pre-change build gives exactly the same errors for both programs: the old choose_overload called check_alone() up front for any INVALID non-flexible argument. So the change didn't cause this. It also fits the lane's decision and the code's comment ('A literal with no type of its own that no parameter gave one ... reports its own errors'). No overload is chosen, so no parameter gave [] a type, and the message is true. Nothing in docs/spec, stdlib.md, or the milestones row says which error wins here. This is diagnostic wording within a lane decision, not a bug the change introduced.
- overloads-5: The spec's overloading rules don't say how an array, tuple, or Map literal matches. I read docs/spec/05-functions.md's Overloading section and docs/spec/14-strings-collections.md. Spec 14:76 says 'An array literal takes the type it's used as, like Local a: Long[] = [1, 2]'. Spec 14:171 says a Map literal's K and V are the expected Map type's. Spec 05:78-83 says overloads that fit by an exact match beat ones that need a lossless conversion, and that a literal argument matches its default type exactly and any other type it fits by conversion. Together these give the new behavior: Take([1, 2, 3]) fits UInt8[] because the literal takes the type it's used as, and Wide([1, 2]) with UInt8[] and Int64[] is ambiguous like Show(5_000_000_000). The old behavior, where an array literal couldn't fit an overload's UInt8[] though it fits a lone Function's, was the one at odds with spec 14. The spec could spell this out more, but the change doesn't contradict it, so I don't count it as a bug.
- forin-1: A Const Function still rejects For x In 1 To n for calling Next, which the loop no longer calls. The error reproduces, but it is the language's settled, tested rule, not a bug. I ran three programs under scratchpad/rv-lane3c/forin1/ with build-rev5's fb.exe. const1.fb (the finding's program) fails with "a Const Function can only call Const Functions, and 'Next' isn't one" at 5:14. const2.fb (the same program with "For x = 1 To n") prints 10. const3.fb (the same Const Function looping over a Range variable) fails the same way, which is consistent. This is intended. tests/compiler/sema_tests.cpp:6067-6081 tests this exact program, with the comment "A For ... In over a range or any Iterator calls its Next, a Method, which a Const Function can't", and expects this exact error. That test comes from commits 750ec701/4ce23de6 (2026-10-22), before this change, and the change leaves it as it was. docs/history.md:114 records it as a review fix ("a Const Function's For ... In is refused at Next"), and the sema.cpp:14512 comment says the same. The spec agrees. Spec 04 still says `a To b` is a value of std.core's Range<T>, a Struct that implements Iterator<T>. Spec 13 says a Const Function calls only Const Functions, Assert, Panic, and a Struct's New. Next is a Method, so it can't be one, and spec 04:121 also says a Const Function doesn't run Range's Operator ==. The change touched lowering only: lower.cpp's lower_range_iteration/lower_counting is about how the loop counts, not what it means, and docs/spec has no change. The milestones Deferred row ("counts without making a Range, as a For loop does") is about the counting mechanics. Step 32's done-when doesn't say the two forms must be accepted in the same places. Allowing For In over a written range in a Const Function would be a language change for the user to decide, not a defect in this step.
