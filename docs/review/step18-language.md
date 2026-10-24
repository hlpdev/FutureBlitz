# The review of M6 step 18's language part: `Sendable Func(A): R`, the `Where T: Class` constraint, and `Task<T> Throws E` written as a type (with E: Error)

24 confirmed findings: 1 high, 8 medium, 15 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S18L-class-1 (high): A Public generic using `Class` (or `Error`) as a constraint can't be declared in any imported module, std included

Status: open

Where: compiler/src/sema.cpp:1280

declare_any_class() (and declare_any_error()) add the synthetic "Class" and "Error" constraint symbols with add_symbol() and leave modifiers.visibility at its default, PRIVATE. Their span defaults to file 0, the main file. check_shown()'s show_bounds() then treats the constraint as a private type of the main file. So any Public or Package declaration in another module whose bounds name `Class` is reported: Public Class Holder<T> Where T: Class, Public Function Same<T: Class>, or Public Method Subscribe<O: Class>. That last one is exactly the API docs/stdlib.md plans for std.events in this step. Private generics in a module work, and so does anything in the main file, which is why tests/programs/class_owner_events.fb (all in one file) and the single-file sema tests pass. `E: Error` fails the same way ("'Rethrow' is Public, but it needs Error, which is private to t16.fb"). No test covers a constraint used across modules.

```
// ---- file: events/event.fb ----
Import std.core

Public Class Event<T>
    Field Handlers: (Func(T): Bool)[] = []

    Public Method Subscribe<O: Class>(owner: O, handler: Func(O, T))
        Local kept: Weak<O> = owner
        Handlers.Add(Function(value: T)
            Local o = kept
            If o == Null Then Return False
            handler(o, value)
            Return True
        End Function)
    End Method

    Public Method Emit(value: T)
        Local live: (Func(T): Bool)[] = []
        For h In Handlers
            If h(value) Then live.Add(h)
        End For
        Handlers = live
    End Method
End Class

// ---- file: t35.fb (run this one) ----
Import std.core
Import "./events/event"

Class Widget
    Field Name: String = "w"
End Class

Local clicks = Event<Int>()
Local w = Widget()
clicks.Subscribe(w, Function(x: Widget, n: Int) => Print("{x.Name} hears {n}"))
clicks.Emit(1)
```

Expected: w hears 1

Actual: events/event.fb:6:19: error: 'Subscribe' is Public, but it needs Class, which is private to t35.fb
  |
6 |     Public Method Subscribe<O: Class>(owner: O, handler: Func(O, T))
t35.fb:1:1: note: declared here
error: couldn't build t35.fb because of 1 error

Suggested fix: Declare both synthetic constraint symbols Public, for example `.modifiers = { .visibility = visibility_t::PUBLIC }` in declare_any_class() and declare_any_error(). Alternatively, have check_shown() skip types in m_analysis.constraint_only that have no declaration. Add a tests/programs case with a module exporting a Public `<O: Class>` generic and a Public `<E: Error>` one.

Skeptic: I put events/event.fb in its own module, with Public Class Event<T> and Public Method Subscribe<O: Class>, and imported it from t35.fb. Debug and --release both stop with: "events/event.fb:6:19: error: 'Subscribe' is Public, but it needs Class, which is private to t35.fb", with a note pointing at t35.fb:1:1. A module with Public Function Rethrow<E: Error>(e: E): Int Throws E, called from t16.fb, fails the same way: "'Rethrow' is Public, but it needs Error, which is private to t16.fb". The control passes: a Private Function Same<T: Class> in an imported module builds and prints True. The cause is in the code: declare_any_class() and declare_any_error() (sema.cpp 1259-1295) call add_symbol() without setting modifiers.visibility, so the visibility stays at its default and the span says file 0. check_shown_by()'s show_bounds() then runs check_shown on bound.interface and reports it as private. stdlib.md's std.events plans Subscribe(owner, handler) with the owner held weakly, which needs exactly a Public <O: Class> method in a std module, so this blocks the step's std part. Every test that uses these constraints (class_constraint.fb, class_owner_events.fb, async_throws_generic.fb, sema_tests) is a single file.

## S18L-sendable-1 (medium): An overloaded function or New won't take a lambda or a Function's name for a Sendable Func parameter

Status: open

Where: compiler/src/sema.cpp:21207

choose_overload() ranks each candidate with match(), which checks the argument with no hint (`check(argument.value, type_id_t::NONE)`). So a typed lambda or a non-generic Function's name gets its plain type, Func(A): R, and then only `converts()` is tried. A Func never converts to a Sendable Func, so every overload with a Sendable Func parameter scores NONE, and the call fails with "no overload of ... takes (Func(...))". With one function and no overloads this doesn't happen, because the argument is checked with the parameter's type as the hint. So spec 05's rule, "A lambda or a Function's name written where one is expected is one", breaks whenever the callee is overloaded. That covers overloaded News like Lazy's, Methods, and Functions, which is the shape step 18's std types (Lazy, ConcurrentMap.GetOrAdd, Select.On) are likely to take. A generic Function's name still works, because match() checks it against the parameter type. The same calls with plain Func parameters compile and run.

```
Import std.core

Shared Class Lazy<T: Sendable>
    ReadOnly Field Maker: Sendable Func(): T

    New(maker: Sendable Func(): T)
        Maker = maker
    End New

    New(maker: Sendable Func(): T, label: String)
        Maker = maker
    End New
End Class

Function Answer(): Int => 42
Function Twice(x: Int): Int => x * 2

Function Run(f: Sendable Func(Int): Int): Int => f(1)
Function Run(f: Sendable Func(Int): Int, n: Int): Int => f(n)

Local a = Lazy<Int>(Answer)
Local b = Lazy<String>(Function(): String => "x", "label")
Print(a.Maker(), b.Maker())
Print(Run(Twice))
Print(Run(Function(x: Int) => x + 1, 5))
```

Expected: 42 x
2
6

Actual: Doesn't compile: "no overload of 'Lazy<T>' takes (Func(): Int32)", "no overload of 'Lazy<T>' takes (Func(): String, String)", "no overload of 'Run' takes (Func(Int32): Int32)", and "no overload of 'Run' takes (Func(Int32): Int32, Int32)". With every 'Sendable Func' replaced by 'Func' (and 'Shared Class' by 'Class'), the same calls compile and print the expected values.

Suggested fix: In match(), handle a Sendable Func the way the Callback case just above it is handled. When `is_sendable_func(non_null(type))`, the argument isn't ByRef, and `is_code(argument.value)` is true, check the argument with `type` as the hint and rank by what comes back: EXACT if it equals `type`, CONVERTIBLE if it converts, NONE otherwise. Add sema tests for an overloaded Function and an overloaded New, each called with a typed lambda and with a Function's name.

Skeptic: I ran the program from C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18l/refute-sendable/f1.fb. It doesn't compile, and gives exactly the four errors the finding quotes: "no overload of 'Lazy<T>' takes (Func(): Int32)", "(Func(): String, String)", "no overload of 'Run' takes (Func(Int32): Int32)" and "(Func(Int32): Int32, Int32)". I checked it against two other programs. f1b.fb is the same program with plain Func and Class; it compiles and prints 42 x / 2 / 6. f1c.fb keeps Sendable Func but has no overloads; it prints 42 x / 2 / 2 / 4. The code explains it. In sema.cpp, match() (about line 21207) has a special case for a Callback parameter (`is_callback(type) && is_code(...)`, which checks with the hint) and one for a generic Function's name. Every other argument gets `check(argument.value, type_id_t::NONE)` and then `converts(natural, type)`. converts() only lets a Sendable Func become a Func (line 11461), never the other way. So every overload whose parameter is a Sendable Func scores NONE. That breaks spec 05's rule, "A lambda or a Function's name written where one is expected is one", whenever the callee is overloaded, and it's inconsistent with plain Func and Callback, which both work with overloads.

## S18L-throws-1 (medium): An Override of a generic base's Method that Throws E can't throw the instance's E

Status: open

Where: compiler/src/sema.cpp:6309

check_throws() compares an Override's Throws with original.throws as declared, without the base instance's type arguments. Base<E>'s Fail throws E, and Derived Extends Base<Oops>, so Derived's Fail should be able to throw Oops. A generic subclass hits the same check: `Class Generic<F: Error> Extends Base<F>` with `Override ... Throws F` gives "'Fail' can't throw F, since Base<E>'s doesn't". The same program with a non-generic Base that Throws Oops builds and runs.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Class Base<E: Error>
    Method Fail(error: E): Int Throws E
        Throw error
    End Method
End Class

Class Derived Extends Base<Oops>
    Override Method Fail(error: Oops): Int Throws Oops
        Print("derived")
        Throw error
    End Method
End Class

Local d = Derived()
Try
    Print(d.Fail(Oops("one")))
Catch e As Oops
    Print("caught: {e.Message}")
End Try

Local b: Base<Oops> = d
Try
    Print(b.Fail(Oops("two")))
Catch e As Oops
    Print("caught: {e.Message}")
End Try
```

Expected: derived
caught: one
derived
caught: two

Actual: t5.fb:16:21: error: 'Fail' can't throw Oops, since Base<E>'s doesn't (the build fails)

Suggested fix: In the Override loop of check_throws(), substitute the overridden Method's Throws as the overriding Class sees it (member_type(base instance, original, thrown), as is done for its parameters) before calling covers(). Add sema tests for a concrete subclass and a generic subclass.

Skeptic: Ran refute-throws/f1.fb (the finding's program) in debug: 'f1.fb:16:21: error: 'Fail' can't throw Oops, since Base<E>'s doesn't'. The generic subclass variant g1.fb (Class Generic<F: Error> Extends Base<F>, Override ... Throws F) gives 'can't throw F, since Base<E>'s doesn't'. The non-generic baseline b1.fb prints derived / caught: one / derived / caught: two. In sema.cpp's check_throws(), the Override loop (around line 6309) calls covers(original.throws, type) with the base Method's raw Throws and never maps E to the instance's argument. Spec 10 says each instance has its own E.

## S18L-throws-2 (medium): Implementing a generic Interface's Method that Throws E is rejected

Status: open

Where: compiler/src/sema.cpp:5106

check_implementation() checks found.throws against required.throws without as_implemented(), so Runner<E>'s `Throws E` never becomes Oops for `Class Job Implements Runner<Oops>`. Calling through a Runner<Oops> value already substitutes E (r.Run() needs Try for Oops), so only the conformance check is wrong. The non-generic version (Runner with Throws Oops) builds and prints the expected lines.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Interface Runner<E: Error>
    Method Run(): Int Throws E
End Interface

Class Job Implements Runner<Oops>
    Method Run(): Int Throws Oops
        Throw Oops("job failed")
    End Method
End Class

Function Go<E: Error>(runner: Runner<E>): String
    Try
        Return "{runner.Run()}"
    Catch e As E
        Local error: Error = e
        Return "failed: {error.Message}"
    End Try
End Function

Print(Go(Job()))
Local r: Runner<Oops> = Job()
Try
    Print(r.Run())
Catch e As Oops
    Print("caught: {e.Message}")
End Try
```

Expected: failed: job failed
caught: job failed

Actual: t8.fb:13:22: error: 'Run' can't throw Oops, since Runner<Oops>'s doesn't (the build fails)

Suggested fix: In check_implementation(), test each found.throws entry against required.throws mapped with as_implemented(thrown, interface, self). Add a sema test with a generic Interface whose Method Throws E.

Skeptic: f2.fb gives 'f2.fb:13:22: error: 'Run' can't throw Oops, since Runner<Oops>'s doesn't'. The non-generic baseline b2.fb prints 'failed: job failed' / 'caught: job failed'. check_implementation() (sema.cpp around line 5106) tests covers(required.throws, type) with no as_implemented(), although signature_text() and the constraint check right beside it do map through as_implemented(interface, self).

## S18L-throws-3 (medium): For ... In over a generic Class whose Iterate Throws E propagates the raw E

Status: open

Where: compiler/src/sema.cpp:14006

check_iterate_call() uses function.throws as declared, so a Source<Oops>'s Iterate throws `E`, not Oops. `For n In Try items` inside a function that Throws Oops, or inside a Try with Catch e As Oops, reports "E isn't caught". Without Try the message reads "Source<Oops>'s Iterate can throw E". check_throwing_call() got this substitution in this step, but the loop's implicit Iterate call didn't. The same program with a non-generic Source (Throws Oops) prints 6 and then caught no items.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Struct Counting Implements Iterator<Int>
    Field at: Int
    Field last: Int

    ByRef Method Next(): Int?
        If at > last Then Return Null
        at += 1
        Return at - 1
    End Method
End Struct

// Going over it can throw its E.
Class Source<E: Error>
    Field Failure: E?

    Method Iterate(): Counting Throws E
        Local failure = Failure
        If failure != Null Then Throw failure
        Return Counting(1, 3)
    End Method
End Class

Function Sum(items: Source<Oops>): Int Throws Oops
    Local total = 0
    For n In Try items
        total += n
    End For
    Return total
End Function

Local source = Source<Oops>()
Try
    Print(Sum(source))
    source.Failure = Oops("no items")
    Print(Sum(source))
Catch e As Oops
    Print("caught {e.Message}")
End Try
```

Expected: 6
caught no items

Actual: f3.fb:33:14: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Sum' (the build fails)

Suggested fix: In check_iterate_call(), map each of function.throws through the sequence's type (member_type(type, function, thrown)) before naming or propagating it. Add a sema test and a program test.

Skeptic: f3.fb gives 'f3.fb:33:14: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Sum''. The non-generic baseline b3.fb prints 6 / caught no items. check_iterate_call() (sema.cpp line 14006) names and propagates function.throws exactly as declared. check_throwing_call() just above it substitutes, but this function doesn't.

## S18L-throws-4 (medium): A Class that takes a generic base's New that Throws E keeps the raw E

Status: open

Where: compiler/src/sema.cpp:6150

When a Class takes its base's New, the copy's parameters get the base instance's type arguments (member_type), but `copy.throws = symbol(constructor).throws` doesn't. So Taker, which Extends Box<Oops>, throws E from Taker(...). Inside a function that Throws Oops, or a Try that catches Oops, this gives "E isn't caught". At top level it goes unreported. The same program with a non-generic Box (Throws Oops) runs correctly.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Class Box<E: Error>
    Field Name: String
    New(name: String, failure: E?) Throws E
        If failure != Null Then Throw failure
        Name = name
    End New
End Class

Class Taker Extends Box<Oops>
End Class

Function Make(fail: Bool): String Throws Oops
    Local taker = Try Taker("taker", fail ? Oops("taker failed") : Null)
    Return taker.Name
End Function

Function Safe(fail: Bool): String
    Try
        Return Taker("safe", fail ? Oops("safe failed") : Null).Name
    Catch e As Oops
        Return "caught {e.Message}"
    End Try
End Function

Print(Safe(False))
Print(Safe(True))
Try
    Print(Make(True))
Catch e As Oops
    Print("make {e.Message}")
End Try
```

Expected: safe
caught safe failed
make taker failed

Actual: t51.fb:21:23: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Make'
t51.fb:27:16: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Safe' (the build fails)

Suggested fix: Substitute the taken New's Throws the way its parameters are substituted: copy.throws[i] = member_type(base, symbol(constructor), thrown). Add a test that takes a generic base's throwing New.

Skeptic: f4.fb gives two 'E isn't caught' errors, at 21:23 (Make) and 27:16 (Safe). The non-generic baseline b4.fb prints safe / caught safe failed / make taker failed. inherit_constructors() (sema.cpp around line 6150) maps each parameter through member_type(base, ...) but then sets copy.throws = symbol(constructor).throws without that mapping.

## S18L-throws-5 (medium): An Interface default that Throws E, called on a Class that implements it, throws the raw E

Status: open

Where: compiler/src/sema.cpp:13978

check_throwing_call() substitutes with m_substitution, which has no Runner<Oops> arguments when Twice is called on a Job, a Class that implements Runner<Oops>. So `Try Job().Twice()` in a function that Throws Oops reports "E isn't caught". Calling the same default through a Runner<Oops> value works. The non-generic Runner version prints 42 twice.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Interface Runner<E: Error>
    Method Run(): Int Throws E

    Method Twice(): Int Throws E
        Return (Try Run()) * 2
    End Method
End Interface

Class Job Implements Runner<Oops>
    Method Run(): Int
        Return 21
    End Method
End Class

Function Go(): Int Throws Oops
    Return Try Job().Twice()
End Function

Function Through(runner: Runner<Oops>): Int Throws Oops
    Return Try runner.Twice()
End Function

Try
    Print(Go())
    Print(Through(Job()))
Catch e As Oops
    Print("caught {e.Message}")
End Try
```

Expected: 42
42

Actual: t52.fb:24:16: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Go' (the build fails)

Suggested fix: When the called Method is an Interface's default reached through a type that implements an instance of it, substitute its Throws with as_implemented(thrown, interface instance, receiver type), as its result type already is. Add a sema test.

Skeptic: f5.fb gives 'f5.fb:24:16: error: E isn't caught; catch it in a Try block, or add it to the Throws of 'Go'' for Try Job().Twice(). The non-generic baseline b5.fb prints 42 / 42. check_throwing_call() substitutes only through m_substitution (the type parameters and arguments that rank() and infer() give). For a default reached through a Class that implements Runner<Oops>, that substitution has no binding of Runner's E.

## S18L-throws-6 (medium): After a function's result Func(): Task<T>, the Task takes the function's Throws

Status: open

Where: compiler/src/parser.cpp:3746

m_function_result only covers the outermost type of a function's result. In `Function Make(): Func(): Task<Int> Throws Oops`, parse_function_type() parses the Func's result with the flag cleared, so `Throws Oops` becomes the inner Task's. Spec 10 and the lane's call say Throws after a function's result is the function's unless the Task is in parentheses. Before this step, that Throws was Make's, so existing code silently changes meaning. The error then tells the user to add Oops to Make's Throws, which they already wrote. Sendable Func results are affected the same way. Writing `(Func(): Task<Int>) Throws Oops` gives the expected output.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Plain(): Int
    Await NextFrame()
    Return 4
End Function

// Make gives a Func that starts a Task, and Make itself can throw Oops.
Function Make(fail: Bool): Func(): Task<Int> Throws Oops
    If fail Then Throw Oops("make failed")
    Return Function() => Plain()
End Function

Try
    Local f = Make(False)
    Print(Await f())
    Local g = Make(True)
    Print("not here")
Catch e As Oops
    Print("caught: {e.Message}")
End Try
```

Expected: 4
caught: make failed

Actual: f6.fb:16:24: error: Oops isn't caught; catch it in a Try block, or add it to the Throws of 'Make'
f6.fb:17:26: error: expected Task<Int32> Throws Oops, found Task<Int32> (the build fails; --show ast prints the result as Func(): (Task<Int> Throws Oops))

Suggested fix: Carry the function-result state into the rightmost part of the result: in parse_function_type(), set m_function_result to the caller's value before parsing the Func's own result, so a Task there leaves Throws to the function. Add parser tests for Func and Sendable Func results.

Skeptic: f6.fb gives 'Oops isn't caught; ... add it to the Throws of 'Make'' at 16:24 and 'expected Task<Int32> Throws Oops, found Task<Int32>' at 17:26. f6p.fb, with (Func(): Task<Int>) Throws Oops, prints 4 / caught: make failed. f6s.fb (Task<Int>[] Throws Oops) works too. Cause in parser.cpp: parse_type() does std::exchange(m_function_result, false), and parse_function_type() parses the Func's result with the flag cleared, so the rightmost Task takes the Throws. That contradicts spec 10 ('After a function's result, Throws is the function's'), the milestones deferred row, and the lane's own rule (unless the Task is in parentheses). It also changes the meaning of code that compiled before this step.

## S18L-throws-7 (medium): A throwing Task can't be an explicit type argument in an expression

Status: open

Where: compiler/src/parser.cpp:2899

type_arguments_follow() decides whether '<' starts type arguments by scanning for type-like tokens, and KW_THROWS isn't among them. So `Holder<Task<Int> Throws Oops>()`, `Holder<(Task<Int> Throws Oops)>()`, `TypeName<(Task<Int> Throws E)>()`, and any generic call or constructor whose type argument is a throwing Task are read as comparisons. That a written Task type can be a type argument is part of the milestone row. `Local tasks: Holder<Task<Int> Throws Oops> = Holder()` works and prints 1.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Load(n: Int): Int Throws Oops
    Await NextFrame()
    Return n
End Function

Class Holder<T>
    Field Items: T[] = []
End Class

Local tasks = Holder<Task<Int> Throws Oops>()
tasks.Items.Add(Load(3))
Print(tasks.Items.Count)
```

Expected: 1

Actual: f7.fb:18:15: error: 'Holder' is a type, not a value
f7.fb:18:22: error: 'Task' isn't declared
f7.fb:18:27: error: 'Int' is a type, not a value
f7.fb:18:32: error: expected an expression, found 'Throws' (the same with the Task in parentheses)

Suggested fix: Accept token_kind_t::KW_THROWS in type_arguments_follow()'s list of tokens that can appear in type arguments. Add parser tests for a call's and a New's type arguments with a throwing Task.

Skeptic: f7.fb gives ''Holder' is a type, not a value', ''Task' isn't declared', ''Int' is a type, not a value', and 'expected an expression, found 'Throws''. The parenthesized form f7p.fb gives the same errors, while f7l.fb (Local tasks: Holder<Task<Int> Throws Oops> = Holder()) prints 1. type_arguments_follow() (parser.cpp around line 2899) has no case for token_kind_t::KW_THROWS, so the scan returns false and the '<' parses as a comparison. Inside an expression, a throwing Task can't be written as a type argument at all.

## S18L-class-2 (low): A Weak<T> parameter never infers T, and the error suggests Read<Int>, which can't meet T: Class

Status: open

Where: compiler/src/sema.cpp:

With `Function Read<T: Class>(w: Weak<T>): T?`, a call with a Widget argument, or with a Weak<Widget> field read (a Widget?), can't infer T. The same call to `Function ReadN<T: Class>(w: T?)` infers T = Widget. A Weak<T> is assigned a T or a T? (spec 08), so inference should unify Weak<T> against T and T? the way it does for T?. Spec 07 says a call infers type arguments from the types of its arguments. The fallback message's example, `Read<Int>(...)`, names a type the constraint rejects.

```
Import std.core

Class Widget
    Field Name: String = "w"
End Class

Class Holder
    Field W: Weak<Widget>
End Class

Function ReadN<T: Class>(w: T?): T? => w
Function Read<T: Class>(w: Weak<T>): T? => w

Local x = Widget()
Local h = Holder()
h.W = x
Print(ReadN(x)?.Name ?? "gone", ReadN(h.W)?.Name ?? "gone")
Print(Read(x)?.Name ?? "gone")
```

Expected: w w
w

Actual: t25.fb:18:7: error: can't tell what T is in this call; write the type arguments, like Read<Int>(...)
error: couldn't build t25.fb because of 1 error

Suggested fix: In argument inference, treat a Weak<T> parameter like a T? one: unify its element with the argument's non-null type, or with a Weak argument's element. Pick the example type argument from the parameter's constraints, or leave the example out when one exists.

Skeptic: I ran t25.fb as given. ReadN(x) and ReadN(h.W), with a T? parameter, infer T. Read(x), with Function Read<T: Class>(w: Weak<T>), fails: "can't tell what T is in this call; write the type arguments, like Read<Int>(...)". Read(wk) with Local wk: Weak<Widget> fails the same way, and Read<Widget>(x) builds. In unify() (sema.cpp 20304), a NULLABLE argument is unwrapped to Widget. After that, Weak<T> against Widget hits `want.kind != have.kind` and returns. The WEAK case only runs when the argument's own type is a Weak, and no argument ever is one, because reading a Weak gives a T? (spec 08). So no Weak<T> parameter can ever be inferred, and that kind of parameter is new with this step. Spec 07 says a call infers type arguments from the types of the arguments when the compiler can tell them, and here it plainly can. The example in the message, report_uninferred()'s hard-coded "Int", names a type that T: Class rejects.

## S18L-class-4 (low): The class_hint for an Interface's Self suggests `<Self: Class>`, which can't be written

Status: open

Where: compiler/src/sema.cpp:8672

class_hint() adds "; a constraint like <{}: Class> says it's one" for any type parameter, including an Interface's Self. Self is not a declared type parameter and can't be given a constraint, so the hint points to code that doesn't exist. The same hint appears for Weak<Self>, RefEquals, [Weak x], and Case As on a Self.

```
Import std.core

Interface Shape
    Method Area(): Float
    Method SameKind(other: Shape): Bool => other Is Self
End Interface
```

Expected: An error without the hint, such as "'Is' tests for a Class or Interface, not Self".

Actual: t36.fb:5:53: error: 'Is' tests for a Class or Interface, not Self; a constraint like <Self: Class> says it's one

Suggested fix: In class_hint(), return an empty string when the type is an Interface's Self (self_of(...) of its owner), or for any type parameter that has no declaration a constraint could go on.

Skeptic: t36.fb (`other Is Self` in an Interface's Method) prints: "'Is' tests for a Class or Interface, not Self; a constraint like <Self: Class> says it's one". t36b.fb gives the same hint on Weak<Self> and on both RefEquals(Self, Self) arguments. That hint can't be followed: `Interface Shape<Self: Class>` fails with "expected identifier, found 'Self'", and `Interface Shape Where Self: Class` fails with "expected end of line, found 'Where'". class_hint() (sema.cpp 8672) checks only is_parameter(type), and an Interface's Self (self_of()) is a type parameter that takes no constraint, so the message points to code that can't be written.

## S18L-class-5 (low): A T? or a Weak<T> gets no hint that `Class` (or `Class & Sendable`) would fix the error, unlike the same error on `Is`

Status: open

Where: compiler/src/sema.cpp:19866

RefEquals and [Weak x] pass the nullable type to class_hint(type), and it returns nothing because a T? isn't is_parameter(). check_expr(is_expr_t) passes non_null(source), so `x Is T` with x: T? gets the hint. The two messages for the same mistake don't match. Also, the unsendable() reason for a Weak<T> of a type parameter still says "unless T is a Shared Class". It never mentions `Class & Sendable`, which is how spec 07 and spec 10 say generic code makes its Weak<T> Sendable.

```
Import std.core

Class Widget
End Class

Function B<T>(x: T?): Bool => RefEquals(x, x)
Function C<T>(x: T?): Func(): Bool => Function[Weak x]() => True
Function D<T: Class>(x: T): Bool
    Const w: Weak<T> = x
    Return Spawn(Function() => w != Null).Wait()
End Function
```

Expected: The RefEquals and [Weak x] errors end with "; a constraint like <T: Class> says it's one", as `x Is T` with x: T? does. D's error mentions that a constraint `<T: Class & Sendable>` makes it Sendable.

Actual: t38.fb:6:41: error: RefEquals compares Class references, not T?
t38.fb:6:44: error: RefEquals compares Class references, not T?
t38.fb:7:53: error: [Weak x] needs a reference to an object, not T?
t38.fb:10:18: error: 'w' is Weak<T>, which another thread can't have: a Weak<T> stays on the thread that made it, unless T is a Shared Class

Suggested fix: Pass non_null(type) to class_hint() in check_special_call() (RefEquals) and weak_captures(). In unsendable()'s Weak branch, when element_of(type) is a type parameter, word the reason as "unless T is a Shared Class, or a type parameter that's Class & Sendable".

Skeptic: In t38.fb, `x Is T` with x: T? ends with "; a constraint like <T: Class> says it's one". RefEquals(x, x) gives only "RefEquals compares Class references, not T?", twice, and [Weak x] gives only "[Weak x] needs a reference to an object, not T?". All three checks test validity on non_null(type) (sema.cpp 19865, 22047). But RefEquals (19868) and [Weak x] (22050) pass the nullable `type` to class_hint(), which returns nothing for a T?, while Is (22947) and Case As (14990) pass non_null. So the same mistake gets different messages, and the hint is lost where it was meant to apply. A Weak<T> captured by Spawn with T: Class gives "... unless T is a Shared Class", while the T: Class & Sendable version builds and prints True. That wording fits spec 07's framing ("the T is a Shared Class"), so that part is only a nicety. The hint mismatch is the confirmed part.

## S18L-sendable-2 (low): Spawn given a nullable Sendable Func says it needs a Sendable Func

Status: open

Where: compiler/src/sema.cpp:19046

check_spawn() checks the work against is_sendable_func(func) without taking the T? off. A (Sendable Func(): Int)? value fails that test and gets the generic error: "Spawn takes a lambda, a Function's name, or a Sendable Func, since a function value it was given would stay on this thread too". The value is a Sendable Func. The real problem is that it may be Null, and the usual T? mismatch message ("..., which may be Null; check it first, or give a value for Null with '??'") would say so. Thread.Start has the same wording.

```
Import std.core

Function Get(): (Sendable Func(): Int)? => Function() => 1

Local t = Spawn(Get())
Print(t.Wait())
```

Expected: An error that says the work may be Null, such as: expected Sendable Func(): Int32, found (Sendable Func(): Int32)?, which may be Null; check it first, or give a value for Null with '??'

Actual: f.fb:5:17: error: Spawn takes a lambda, a Function's name, or a Sendable Func, since a function value it was given would stay on this thread too

Suggested fix: In check_spawn(), when func is nullable and non_null(func) is a Sendable Func, report the may-be-Null mismatch, for example through report_mismatch(work, non_null(func)), before the generic error. Add a sema test for it.

Skeptic: I ran f2.fb. It prints "f2.fb:5:17: error: Spawn takes a lambda, a Function's name, or a Sendable Func, since a function value it was given would stay on this thread too". f2b.fb shows Thread.Start(Get()) gives the same wording. In the same file, `Local x: Sendable Func(): Int = Get()` gives the usual "expected Sendable Func(): Int32, found (Sendable Func(): Int32)?, which may be Null; check it first, or give a value for Null with '??'". In check_spawn() (sema.cpp:19044), the test `!made_here && !is_sendable_func(func)` runs on func without taking the T? off it. For this value, the message's stated reason ("would stay on this thread") is false: it's a Sendable Func, and the real problem is that it may be Null. This is a misleading diagnostic, not wrong codegen, so it's low.

## S18L-sendable-3 (low): No cycle warning when a Sendable lambda that captures Self is stored in Self's Mutex field

Status: open

Where: compiler/src/sema.cpp:15398

Spec 05 says "The compiler warns when a closure stored in a field captures Self strongly". warn_self_cycle() only fires when the assignment target is a field of Self. Until this step a Mutex couldn't hold a function value, since no Func was Sendable. A Shared Class can't keep a Self-capturing Sendable Func in a ReadOnly Field either: New rejects using Self before the field is assigned. So the only way such a class can store one in its own field is through a Mutex: `Lock Work As work` then `work = Function() => Hits`. That assignment never warns, and the object leaks. The same code with a plain Class and a plain Func field does warn.

```
Import std.core

Shared Class Node
    ReadOnly Field Hits: Int
    Field Work: Mutex<(Sendable Func(): Int)?>

    New(hits: Int)
        Hits = hits
    End New

    Method Arm()
        Lock Work As work
            work = Function() => Hits
        End Lock
    End Method
End Class

Local node = Node(3)
node.Arm()
Print("armed")
```

Expected: A compile-time warning at the lambda: "this lambda captures Self, so keeping it in a field of Self makes a cycle that's never freed; capture it with [Weak Self]"

Actual: No warning. The program prints "armed", then at exit: "leak: 1 Node object was never released", "leak: 1 Function at h1.fb:13:20 object was never released", "leak: a cycle of 2 objects: Node.Work -> Function at h1.fb:13:20.Self -> the first Node"

Suggested fix: Make warn_self_cycle() also fire when the target is a Lock's variable whose Mutex is a field of Self, or treat an assignment to such a Lock variable as storing into that field. Add a sema test for it.

Skeptic: I ran f3.fb (the finding's program). Debug prints no warning, then "armed", then the leak report with "a cycle of 2 objects: Node.Work -> Function at f3.fb:13:20.Self -> the first Node". Release prints no warning either and still leaks. warn_self_cycle() (sema.cpp:15398) returns early unless `target.field` is set. A Lock's variable is a local target, so storing into it never warns, even though it writes the value of Self's Mutex field. For comparison, f3c.fb's `Self.G = Function() => Hits + 3` does warn. Spec 05 says "The compiler warns when a closure stored in a field captures Self strongly", and spec 05/10 now say a Mutex can hold a Sendable Func. In a Shared Class, a Mutex field is the practical place to keep one, so the rule never fires for Shared Classes. A caveat: the warning was already narrow before this step. f3c.fb's `Box.F = ...` (a field of an object Self holds) and f3b.fb's `Arr.Add(...)` into a field array don't warn either. Even so, the Lock variable is the field's own contents, and this step is what made that path reachable, so I count it as a low finding.

## S18L-sendable-4 (low): Spec 01's table of contextual keywords leaves out Sendable

Status: open

Where: docs/spec/01-lexical.md:165

The parser makes 'Sendable' a keyword only right before Func (parser.cpp: an IDENTIFIER whose text is "Sendable" with KW_FUNC after it), and it's an ordinary name everywhere else. Spec 01's "Contextual" section lists every word that acts this way (Get/Set, Step, Lib, From, Weak), but it doesn't list Sendable. Spec 05 never says Sendable is contextual either.

```
Import std.core

Local Sendable = 5
Local f: Sendable Func(): Int = Function() => 1
Print(Sendable + f())
```

Expected: Spec 01's contextual-keyword table has a row for `Sendable`, a keyword only directly before `Func` (Sendable Func(A): R), and a name elsewhere. The program prints 6.

Actual: The program prints 6, so Sendable is contextual, but spec 01's table has no row for it.

Suggested fix: Add a row to spec 01's Contextual table: | `Sendable` | directly before `Func` (`Sendable Func(A): R`); elsewhere it names the Interface |.

Skeptic: I ran f4.fb. It prints 6, so `Local Sendable = 5` and `Sendable Func(): Int` work together in one program. parser.cpp:3574-3576 makes "Sendable" a keyword only when an IDENTIFIER with that text comes right before KW_FUNC; its comment says "'Sendable' is a contextual keyword before Func, and a name everywhere else". Spec 01's Contextual table (docs/spec/01-lexical.md, about lines 155-165) is introduced as the words that are "Ordinary identifiers everywhere except in one position, where they act as keywords". It lists Get/Set, Step, Lib, From and Weak ("elsewhere it names the type Weak<T>"), which is the same shape as Sendable, but has no row for Sendable. The 8ff414c..4c600a9 diff changes specs 05, 07, 08, 09 and 10, not 01. It's a doc gap, so low.

## S18L-throws-10 (low): E is inferred from a Task's Throws by position, so the order of its Classes matters

Status: open

Where: compiler/src/sema.cpp:20384

unify() pairs want.throws with have.throws index by index. For a parameter `Task<Int> Throws Oops, E`, a Task that throws `Oops, Bad` infers E = Bad, but one that throws `Bad, Oops` infers E = Oops and then fails to convert. Those two Tasks throw the same errors, and conversion compares them as sets through covers(). `Take<Bad>(Two())` works and prints "E two".

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Class Bad Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Two(): Int Throws Bad, Oops
    Await NextFrame()
    Throw Bad("two")
End Function

Async Function Owt(): Int Throws Oops, Bad
    Await NextFrame()
    Throw Bad("owt")
End Function

Function Take<E: Error>(task: Task<Int> Throws Oops, E): String
    Try
        Return "{task.Wait()}"
    Catch e As Oops
        Return "oops {e.Message}"
    Catch e As E
        Local error: Error = e
        Return "E {error.Message}"
    End Try
End Function

Print(Take(Owt()))
Print(Take(Two()))
```

Expected: E owt
E two

Actual: t33.fb:37:12: error: expected Task<Int32> Throws Oops, Oops, found Task<Int32> Throws Bad, Oops

Suggested fix: Match the Throws lists as sets: first set aside the have-side Classes that a concrete want-side entry covers, then unify the want-side type parameters with what's left (or skip inference when the counts differ).

Skeptic: f10.fb: Take(Owt()) is accepted, and Take(Two()) gives 'expected Task<Int32> Throws Oops, Oops, found Task<Int32> Throws Bad, Oops'. Both Tasks throw the same set {Oops, Bad}. With explicit Take<Bad>(Two()) (f10e.fb), it prints 'E owt' / 'E two' in debug and in release. unify() (sema.cpp around line 20384) pairs want.throws with have.throws by index, so the order of the list decides what E is inferred as.

## S18L-throws-11 (low): Type names of a throwing Task in a tuple, Func parameters, or type arguments read alike

Status: open

Where: compiler/src/types.cpp:966

A throwing Task's name is parenthesized only before a suffix (suffixed()). In a tuple, a Func's parameters, or type arguments, the ',' after it reads as part of its Throws. So ((Task<Int> Throws A, B), Int) and ((Task<Int> Throws A), B, Int) are both named (Task<Int32> Throws A, B, Int32), and diagnostics say "expected X, found X" with identical text. MIR also shows two classes named Holder<(Task<Int32> Throws A, B, Int32)> for the two instances.

```
Import std.core

Class A Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Class B Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function One(): Int Throws A
    Await NextFrame()
    Return 2
End Function

// A tuple of a Task that throws A, a B, and an Int, then a tuple of a Task that throws A and B,
// and an Int.
Local y: ((Task<Int> Throws A), B, Int) = (One(), B("b"), 4)
Local z: ((Task<Int> Throws A, B), Int) = y
Local f: Func((Task<Int> Throws A), B): Int = Function(t: (Task<Int> Throws A), b: B) => 0
Local g: Func(Task<Int> Throws A, B): Int = f
```

Expected: two errors whose expected and found types read differently, e.g. expected ((Task<Int32> Throws A, B), Int32), found ((Task<Int32> Throws A), B, Int32)

Actual: f11.fb:23:43: error: expected (Task<Int32> Throws f11.A, f11.B, Int32), found (Task<Int32> Throws f11.A, f11.B, Int32)
f11.fb:25:45: error: expected Func(Task<Int32> Throws f11.A, f11.B): Int32, found Func(Task<Int32> Throws f11.A, f11.B): Int32

Suggested fix: Parenthesize a throwing Task's name wherever a ',' can follow it: tuple elements, Func and Callback parameters, generic instances' type arguments, and a Task's own Throws list. Do it in both the names that tuple(), function(), and instance() build and in qualified_name()'s list().

Skeptic: f11.fb gives 'expected (Task<Int32> Throws f11.A, f11.B, Int32), found (Task<Int32> Throws f11.A, f11.B, Int32)' and 'expected Func(Task<Int32> Throws f11.A, f11.B): Int32, found Func(Task<Int32> Throws f11.A, f11.B): Int32'. Expected and found read the same. f11m.fb also shows it at run time: TypeName<T>() prints '(Task<Int32> Throws A, B, Int32)' for both ((Task<Int> Throws A), B, Int) and ((Task<Int> Throws A, B), Int), in debug and release. Spec 10 says the Task needs parentheses before a ',' that goes on to a tuple element or type argument, but names don't add them.

## S18L-throws-12 (low): The can't-be-Null hint suggests 'Task<Int32> Throws Oops?' instead of '(Task<Int32> Throws Oops)?'

Status: open

Where: compiler/src/sema.cpp:16332

The Null hint parenthesizes only FUNCTION types. For a throwing Task it appends '?' to the bare name, which spells a Task that throws Oops?. Writing that gives its own error telling the user to use (Task<T> Throws Oops)?. The lane's call says type names print the Task in parentheses where a suffix could follow.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Load(): Int Throws Oops
    Await NextFrame()
    Return 1
End Function

Local t: Task<Int> Throws Oops = Null
Local f: Func(): Int = Null
```

Expected: error: Task<Int32> Throws Oops can't be Null; (Task<Int32> Throws Oops)? can

Actual: t29.fb:14:34: error: Task<Int32> Throws Oops can't be Null; Task<Int32> Throws Oops? can
t29.fb:15:24: error: Func(): Int32 can't be Null; (Func(): Int32)? can

Suggested fix: Build the hint with m_analysis.types.suffixed(hint) + "?" (or the name of nullable_of(hint)) instead of special-casing FUNCTION. Add a sema test.

Skeptic: f12.fb gives 'Task<Int32> Throws Oops can't be Null; Task<Int32> Throws Oops? can', while the Func case gives '(Func(): Int32)? can'. The suggested spelling means a Task that throws Oops?, which check_task_throws() itself rejects with the hint (Task<T> Throws Oops)?. The lane's rule that names print in parentheses where a suffix follows isn't applied here.

## S18L-throws-13 (low): A bad class in a Task's Throws is reported more than once

Status: open

Where: compiler/src/sema.cpp:12014

written_task() keeps a non-error in the Task's throws, so the conversion after it fails too ("expected Task<Int32> Throws String, found ..."). A function's Throws, by contrast, drops what it reported. The pending check is also queued from inside quietly(): constraint_interface() resolves a constraint once quietly and once for real, so a Task type written in a Where line gets the same Throws error twice.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Load(): Int Throws Oops
    Await NextFrame()
    Return 1
End Function

Local t: Task<Int> Throws String = Load()

Function F<T>(x: T)
    Where T: Task<Int> Throws String
End Function
```

Expected: f13.fb:14:27: error: Throws lists Classes that extend Error, not String
f13.fb:17:14: error: Task<Int32> Throws String isn't an Interface
f13.fb:17:31: error: Throws lists Classes that extend Error, not String

Actual: f13.fb:14:27: error: Throws lists Classes that extend Error, not String
f13.fb:14:36: error: expected Task<Int32> Throws String, found Task<Int32> Throws Oops
f13.fb:17:14: error: Task<Int32> Throws String isn't an Interface
f13.fb:17:31: error: Throws lists Classes that extend Error, not String
f13.fb:17:31: error: Throws lists Classes that extend Error, not String

Suggested fix: Don't queue pending Throws checks while quiet, or take back the ones a quiet try queued. Make a Task whose Throws holds a non-error convert from anything, or resolve it to INVALID once that's reported, so nothing follows from it.

Skeptic: f13.fb prints 'f13.fb:17:31: error: Throws lists Classes that extend Error, not String' twice, the same diagnostic for the same span. It also gives the follow-on error 'f13.fb:14:36: error: expected Task<Int32> Throws String, found Task<Int32> Throws Oops' after the bad Throws at 14:27 was already reported. The output matches the finding's 'actual' exactly.

## S18L-throws-14 (low): mentions() and parameters_in() skip a Task's Throws, so an unknown E leaks into a call

Status: open

Where: compiler/src/sema.cpp:10325

For TASK, both helpers look only at info.inner, so `Task<Int> Throws E` doesn't count as mentioning E. A lambda's untyped parameter then takes `Task<Int> Throws E` while E is still being inferred, and E is "inferred" as itself. The call reports a constraint failure instead of "can't tell what E is" and "'t' needs a type", which the equivalent T[] case gives. check_expansion() likewise misses growth through a Throws (Deep calling Deep(Later(Wrapped<T>(value)))), which only lowering's backstop then reports.

```
Import std.core

// Nothing says what E is, so the call can't tell it, as with RunArray's T.
Function Run<E: Error>(f: Func(Task<Int> Throws E): Int): Int
    Return 1
End Function

Function RunArray<T>(f: Func(T[]): Int): Int
    Return 1
End Function

Print(RunArray(Function(t) => 0))
Print(Run(Function(t) => 0))
```

Expected: for Run, as for RunArray: can't tell what E is in this call ... and 't' needs a type, since nothing here says what this lambda takes

Actual: f14.fb:12:7: error: can't tell what T is in this call; write the type arguments, like RunArray<Int>(...)
f14.fb:12:25: error: 't' needs a type, since nothing here says what this lambda takes
f14.fb:13:7: error: E isn't a Class that extends Error, which 'Run' needs for E

Suggested fix: In mentions(), return any(info.throws) || mentions(info.inner, ...) for TASK. In parameters_in(), walk info.throws for TASK. Add a sema test for each.

Skeptic: f14.fb: RunArray gives 'can't tell what T is' plus ''t' needs a type'. Run gives only 'E isn't a Class that extends Error, which 'Run' needs for E', so E was taken as itself. mentions() (sema.cpp around line 10325) returns mentions(info.inner) for TASK and ignores info.throws, and parameters_in() walks only inner for TASK. Expansion check too: f14e.fb (Deep<T> calling Deep(n - 1, Later<T>()), where the type argument grows only through a Throws Wrapped<T>) gets only the generic backstop 'Deep' needs a larger instance each time it runs here. The array version f14f.fb gets sema's specific 'gets Wrapped<T>[] for T here, which leads back to this code'. Both are still rejected, so only the diagnostics are wrong.

## S18L-throws-15 (low): The can't-tell hint suggests Int for a type parameter that needs E: Error

Status: open

Where: compiler/src/sema.cpp:20513

The example in "can't tell what E is in this call" always fills an unknown type parameter with Int. For E: Error that example (`Attempt<Int>(...)`) is itself an error: "Int32 isn't a Class that extends Error".

```
Import std.core

Async Function Plain(): Int
    Await NextFrame()
    Return 3
End Function

Function Attempt<E: Error>(task: Task<Int> Throws E): String
    Try
        Return "{task.Wait()}"
    Catch failed As E
        Return "failed"
    End Try
End Function

Print(Attempt(Plain()))
```

Expected: error: can't tell what E is in this call; write the type arguments, like Attempt<Error>(...)

Actual: f15.fb:16:7: error: can't tell what E is in this call; write the type arguments, like Attempt<Int>(...)

Suggested fix: When the parameter's bounds include any_error, use "Error" in the example (and a Class name for any_class).

Skeptic: f15.fb gives 'can't tell what E is in this call; write the type arguments, like Attempt<Int>(...)'. The hint code (sema.cpp around line 20513) always writes "Int" (or "1" for a Const parameter) for an unknown parameter. For E: Error that suggestion is itself an error, since Int32 isn't a Class that extends Error.

## S18L-throws-16 (low): Some of the step's new Throws diagnostics have no tests

Status: open

Where: tests/compiler/sema_tests.cpp:9178

None of these new diagnostics is checked by any test under tests/:
- "only a Task's type takes a Throws, and 'Task' here is a type of the code's"
- "'X' is a type parameter, so it takes no type arguments or Throws"
- throws_as() in Catch coverage (Catch c As Error before Catch d As E)
- check_catch_reached()'s rule for a type parameter
- the Await/Wait message for a Task type that was written out

They work, as the program shows.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Class Task<T>
    Field Value: T?
End Class

Local t: Task<Int> Throws Oops = Task<Int>()

Function F<Task>(x: Task Throws Oops)
End Function

Function G<E: Error>(e: E): Int
    Try
        Throw e
    Catch c As Error
        Return 1
    Catch d As E
        Return 2
    End Try
End Function
```

Expected: each of these errors checked by a sema test

Actual: f16.fb:13:27: error: only a Task's type takes a Throws, and 'Task' here is a type of the code's
f16.fb:15:12: error: 'Task' is a type here, so a type parameter can't take its name
f16.fb:15:21: error: 'Task' is a type parameter, so it takes no type arguments or Throws
f16.fb:23:16: error: this Catch is already covered by an earlier Catch (none of these is in tests/)

Suggested fix: Add sema tests for each of these diagnostics, plus a Catch As E reached only by a concrete Class (no "nothing in the Try block throws").

Skeptic: f16.fb shows each diagnostic working. Grepping tests/ finds no test of the new text 'takes no type arguments or Throws' (sema.cpp line 11656) or 'is a type of the code's' (line 11732). The only 'already covered by an earlier Catch' test (sema_tests.cpp line 2669) is the earlier concrete FileError case. Nothing tests throws_as() for Catch As Error before Catch As E, or check_catch_reached()'s is_parameter rule for a Catch As E reached only by a concrete Class. The step's two new Task Throws TEST_CASEs (sema_tests diff) cover neither.

## S18L-throws-8 (low): A parameter after a Task-typed one is read as a thrown Class in a lambda or a Func type

Status: open

Where: compiler/src/parser.cpp:3700

parse_task_throws() stops at ',' only when `name:` or ByRef follows. A lambda's untyped parameter (`Function(t: Task<Int> Throws Oops, n)`) and a Func type's next parameter type (`Func(Task<Int> Throws Oops, Int)`) are parameters too, but both become thrown types. Spec 10 and the lane's call say the Throws list goes on past a ',' unless a parameter follows. Those cases are ambiguous for the parser, but neither the spec nor the diagnostic says to use parentheses there; the '?' and '[]' cases get that hint. With `(Task<Int> Throws Oops)` in both places, the program prints 7.

```
Import std.core

Class Oops Extends Error
    New(message: String)
        Super.New(message)
    End New
End Class

Async Function Load(n: Int): Int Throws Oops
    Await NextFrame()
    Return n
End Function

Function Apply(f: Func(Task<Int> Throws Oops, Int): Int): Int
    Return f(Load(3), 4)
End Function

Print(Apply(Function(t: Task<Int> Throws Oops, n) => ((Try? t.Wait()) ?? 0) + n))
```

Expected: 7 (or, if parentheses are required there, a diagnostic saying the Task's type goes in parentheses before the ',')

Actual: f8.fb:14:47: error: Throws lists Classes that extend Error, not Int32
f8.fb:15:12: error: Func(Task<Int32> Throws Oops, Int32): Int32 takes 1 argument, not 2
f8.fb:18:48: error: 'n' isn't a type
f8.fb:18:56: warning: 't.Wait()' doesn't throw, so it doesn't need Try?
f8.fb:18:79: error: 'n' isn't declared

Suggested fix: Say in spec 10 that a Func type's parameters and a lambda's untyped parameters after a throwing Task need it in parentheses. Give the parentheses hint when a Task's Throws lists a non-error, as check_task_throws() does for '?' and '[]'. Or end the list at ',' inside a Func type's or a lambda's parameter list.

Skeptic: f8.fb gives 'Throws lists Classes that extend Error, not Int32' (14:47), 'takes 1 argument, not 2', and ''n' isn't a type' / ''n' isn't declared' for the lambda's untyped parameter n. parse_task_throws() ends the list only before 'name:' or ByRef. Spec 10 and the lane's rule say the list goes on past ',' unless a parameter follows, and the lambda's n is a parameter. No hint about parentheses is given, unlike the '?' and '[]' cases in check_task_throws().

## S18L-throws-9 (low): A Task's Throws doesn't report a type parameter that's covered or repeated

Status: open

Where: compiler/src/sema.cpp:6210

check_task_throws() skips type parameters entirely, so `Task<Int> Throws E, Error` and `Task<Int> Throws E, E` aren't reported, while a function's `Throws E, Error` reports "E is already covered by this Throws". Spec 10 says a Task's Throws "lists Classes as a function's does", and the comment says none is covered by another.

```
Import std.core

Function F<E: Error>(error: E): Int Throws E, Error
    Throw error
End Function

Function G<E: Error>(task: Task<Int> Throws E, Error): Int
    Return 0
End Function

Function H<E: Error>(task: Task<Int> Throws E, E): Int
    Return 0
End Function
```

Expected: three errors: E is already covered by this Throws (in F, G, and H)

Actual: f9.fb:3:44: error: E is already covered by this Throws (only F's; G and H pass)

Suggested fix: Keep the bounds where the Task's type was written (or check it there), and run covered_in() for type parameters too, skipping only the is_error() check that was already done.

Skeptic: f9.fb reports only F's 'E is already covered by this Throws' (3:44). G's Task<Int> Throws E, Error and H's Task<Int> Throws E, E pass silently. check_task_throws() (sema.cpp line 6210) does 'if (is_parameter(type)) continue;' before covered_in(), so a type parameter's coverage is never checked. Spec 10 says a Task's Throws 'lists Classes as a function's does', and spec 09 says none is covered by another.

## Refuted by the skeptics (no action)

- S18L-sendable-5: No test covers Thread.Start taking a Sendable Func value. I ran f5.fb in debug and release. Both print "g ran", 7, "done". f5b.fb (Thread.Start given a Sendable Func(Int): Int) gives the right error: "the work Thread.Start runs takes no arguments; a lambda can pass them". The gap is real: grep finds no Thread.Start( in tests/ without a lambda. But the behavior is correct. Spawn and Thread.Start share the same check_spawn() branch, which differs only in the `what` wording and the THREAD result type, and tests/compiler/sema_tests.cpp:8452-8455 plus tests/programs/sendable_func.fb:59/77 test that branch through Spawn. The milestones row's done-when doesn't name this case. Nothing I ran behaved wrongly, and no code contradicts the spec, stdlib.md or the milestones row. So this is a nice-to-have test, not a bug, and I refute it as a finding.
- S18L-class-3: `x As T` and `x As? T` reject a T: Class, though `x Is T` and `Case t As T` take one. The behavior is as the finding says: `n As T` gives "'As' can't convert Node to T" and `n As? T` gives "'As?' converts objects between Classes and Interfaces, not Node to T". The same downcast written as Select n / Case t As T builds and prints b. But spec 07's Class section lists exactly what takes a T under the constraint: Weak<T>, [Weak x], RefEquals, `x Is C`, and `Case c As C`. As and As? aren't on that list, and the lane decision lists the same set. The compiler matches the spec, and the finding itself calls this a gap, not a contradiction. The As? message is accurate: it names Node and T, with no false claim. This would be a spec extension to raise, not a bug in this step.
