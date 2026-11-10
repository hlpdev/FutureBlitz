# Function values that throw: a design

Asked for by SCP:CB X (2026-11-10): a lambda that throws, held in a function value whose type
has Throws, and called with Try. It's more than one step, so this says what it is, what it
costs, and the order to build it in. The user said yes to steps 1 to 3 (2026-11-10), with the
picks below; steps 4 to 6 are Deferred work.

Built so far: step 1, the type's rules, in sema: the type as a value's, a call through one
needing Try, and the conversions with their errors (rules 1, 2, and 5 below). A build that
calls one, or makes one of a Func that doesn't throw, says its code isn't built yet. "Today",
below, is how it was before step 1.

## Today

- A Func value can't throw (decisions.md, "A Func can't throw"). A lambda catches what it calls,
  a Function that throws isn't a value, and `Func(A): R Throws E` is an error anywhere but a
  `For Method` loop's Func type, where it only picks Methods (spec 13).
- The type already exists: `type_table_t::function(parameters, result, sendable, throws)` makes
  and names `Func(A): R Throws E`, and reflection's `m.Call` already calls a throwing Method
  with Try.
- A Function that throws takes one more parameter, last and ByRef, an `Error?` it fills, and its
  caller tests it after the call. A closure's code has no such parameter, and a call through a
  Func value doesn't pass one.

So the type and the check of "this call can throw" are there. What's missing is values of the
type, and a way to call them.

## The rules

1. **The type.** `Func(A): R Throws E, F` is a type a value can have, anywhere a Func type goes:
   a Local, a parameter, a Field, an array's elements. Its Throws lists Classes as a Function's
   does. A Throws after a Func type's result is still that Func type's, as it is in a header
   today, so a Function that returns a throwing Func and itself throws writes the inner one in
   parentheses.
2. **Calling one** can throw what its type lists, so it needs `Try`, `Try?`, or a Catch, exactly
   as a call of a Function with that Throws does: `Local n = Try parse(text)`.
3. **A lambda** throws when the Func type expected of it does: its body may then let out the
   errors that type lists, with Try, and anything else is caught or an error, as in a Function.
   With no Func type expected, a lambda says so itself, `Function(text: String): Int Throws
   ParseError => Try Int.Parse(text)`. A lambda with neither still can't throw, so nothing that
   compiles today changes meaning.
4. **A Function's name** is a value of its own type, Throws and all: `Local f: Func(String): Int
   Throws ParseError = Parse`. A Method's still isn't.
5. **Converting.** A Func that throws less converts to one that throws more: a Func that doesn't
   throw is a `Func(A): R Throws E` for any E, and `Throws ParseError` is a `Throws Error`. The
   other way is an error, which says the errors that would be lost. Parameter and result types
   match exactly, as they do between Func types today.
6. **What can't throw stays so.** A Callback, which C calls. The work of `Parallel For`. A
   `Sendable` Func may throw. Spawn is the next section's.
7. **Generics.** A type parameter can stand for the errors, with the constraint it has today:
   `Function Each<T, E: Error>(items: T[], work: Func(T) Throws E) Throws E`. A lambda that
   doesn't throw needs E to be nothing, which no type is. See the open question.

## The call's shape

Two ways to pass the error out of a closure's code:

- **A.** Every closure's code takes the `Error?` parameter, used or not. One shape, so
  converting a Func that doesn't throw to one that does costs nothing. But every call through a
  Func value pays for a parameter and a test it mostly doesn't need, and every prebuilt library
  has to be rebuilt, since its closures change shape.
- **B.** Only a Func type that throws has the parameter. A call through a plain Func is as it is
  now. Converting a Func that doesn't throw to one that does wraps it in a small closure that
  takes the parameter and ignores it: one allocation at the conversion, which a lambda written
  where a throwing Func is expected never pays, since it's made with the parameter.

**My pick: B.** It leaves every existing program's code as it is, keeps prebuilt libraries
valid, and puts the cost where the feature is used. Lowering already makes small adapters of
this kind for an Interface's table (its table thunks), though not yet for a function value, so
the wrapper is new code in step 2.

Converting between two throwing Func types (`Throws ParseError` to `Throws Error`) needs no
wrapper: the parameter is an `Error?` either way.

## Spawn and Tasks

`Spawn(work)` with work that throws E gives a `Task<T> Throws E`, as an Async Function that
throws does, and Await or Wait re-throws it. That type and its re-throw exist. It's a step of
its own after the core, since Spawn's work runs on the pool, whose jobs don't carry an error
out today.

## std

Nothing in std has to change for the core. Afterwards, the callbacks worth a throwing form are
few and are decided one at a time: `Each`, `Map`, `Filter` over arrays and sequences, a
`Lazy<T>`'s maker, a `Retry`. Each is either an overload beside the one there is or, with the
open question settled, the same Function with an E.

## Steps

| Step | What | Size |
|---|---|---|
| 1 | The type as a value's, its conversions and their errors; a call through one needs Try. Sema only, with a lowering that reports "not built" | small |
| 2 | Lowering: a throwing closure's code takes the error parameter, a call through the value passes and tests it, and the wrapper for a Func that doesn't throw. Lambdas where a throwing Func is expected, and ones that write Throws | the main step |
| 3 | A Function's name as a throwing value; throwing Func values in Fields, arrays, Statics, and generic instances; interface files, `fb doc`, the debug form, hover | medium |
| 4 | An E type parameter on a Func parameter, and the open question below | medium |
| 5 | Spawn of work that throws | medium |
| 6 | std's throwing forms, each as it's wanted | small each |

Steps 1 to 3 give SCP:CB X what it asked for. Each lands with tests of its own: programs that
throw through a value, in a debug and a release build, with no leak of the error or of what the
closure holds; sema's errors for each conversion that loses errors; and a prebuilt library that
takes a throwing Func across its edge.

## Open questions, with my picks

1. **A lambda that doesn't throw, where `Func(T) Throws E` with a generic E is expected.** E has
   to be something. Pick: a Function with such an E can be called with a Func that doesn't
   throw, and then E is no error at all, so the call needs no Try, as though the Function's own
   Throws E weren't there. This is what makes one `Each` serve both, and it's the part of the
   design most worth a second look, since "no error" isn't a type anywhere else.
2. **Does a lambda's Throws ever come from its body alone?** Pick: no. With no Func type
   expected and no Throws written, a lambda that lets an error out is the error it is today,
   whose message now says to write the Throws. Inferring it would make `Local f = Function() =>
   Try Load()` change type when Load's Throws changes, far from where f is used.
3. **A Method's name as a value.** It isn't one today, throwing or not, and this doesn't change
   that.
4. **Reflection's `For Method` loop** keeps its meaning: its Func type is still a pattern that
   picks Methods. Only the error "a Func value can't throw" goes away elsewhere.

## What it doesn't change

A Func without Throws, every program that compiles today, the shape of any existing closure or
call, and prebuilt libraries. The decision "A Func can't throw" becomes "a Func throws only what
its type says", and its reason, that a caller couldn't know what a function value throws, is
what the type now answers.
