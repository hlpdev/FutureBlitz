# Found by projects

Every bug in FutureBlitz, fb, or std, and everything a project built on them wants changed or
added, from the day it's found: fbengine, fb-luau (the luau package), and scpcb-x. A row is added
when the find is reported, with the small program that shows it, and its state is kept current:
open, the user's to decide, or fixed, with the commit and then the release. What a release fixed
is in [history.md](history.md) in full; what the spec describes and the compiler doesn't do yet is
[milestones.md](milestones.md)'s Deferred work.

The programs are in fbengine-notes/luau/ unless a row says otherwise. Started 2026-11-11; finds
before it are in history.md under the release that fixed them.

## Bugs

| What | Found by | Shown by | State |
|---|---|---|---|
| `Map<K, Weak<T>>.Get` kept its object by one strong reference, as every generic place did that makes a `Weak<T>` a `V?` | fb-luau, L1, 2026-11-11 | weak_map_get_leak.fb | fixed, 64f8677f, in 1.3.1 |
| An empty Map literal for a `Map<K, V>` Local in an `Extend Map<K, V>` needed a type | fb-luau, L2, 2026-11-11 | extend_map_literal.fb | fixed, 6a5beed6, in 1.3.1 |
| Beside an overload that takes a `Func()`, a lambda that throws fit no overload | fb-luau, L3, 2026-11-11 | throws_overload.fb | fixed, 6a5beed6, in 1.3.1 |
| A For Method loop of `Func(): R` bound a Method that gives nothing | fb-luau, L4, 2026-11-11 | void_result.fb | fixed, 6a5beed6, in 1.3.1: such a Method is skipped |
| `Scheduler.Run(task)` moved the coroutines its task awaits out of order, so each level went on a tick late | fb-luau, L7, 2026-11-11 | scheduler_run_nested.fb | fixed, d3adc9b2, in 1.3.1 |
| A Class that extends one implementing an Interface got the base's Static defaults, and couldn't list the Interface itself when one gives Self | fb-luau's review, 2026-11-11 | derived_static_default.fb | fixed, 23a6498e, in 1.3.1 |
| futureblitz.dev's std reference and grammar were 1.2.3's under 1.3.0: the release's sync ran with the installed fb, not the release's | the fb lane, 2026-11-11 | source.json's "fb" line | fixed on production (f382737) and staging; the release skill's sync is to check the fb it runs |

## Wanted: changes and additions

Each waits on the user's yes unless its state says otherwise.

| What | Found by | Shown by | State |
|---|---|---|---|
| A way to key by identity: a `RefHash(o)` beside `RefEquals`, or an `IdentityMap<K, V>`. A Map uses a Class's own `==` and `Hash`, and `Weak<T>` isn't Hashable | fb-luau's review, 2026-11-11 | identity_key.fb | added, c0506f68, in 1.3.1: `RefHash(o)` and std.collections' `Identity<T>` |
| A `T?` or a tuple implementing an Interface by an Extend, as `Extend T? Implements I Where T: I` | fb-luau's plan, 2026-11-11 | | the user's to decide |
| A Func type implementing an Interface | fb-luau's plan, 2026-11-11 | | the user's to decide |
| Reflection over a type's Operators | fb-luau's plan, 2026-11-11 | | the user's to decide |
| A generic `[]=`, whose value's type is a type parameter | fb-luau's plan, 2026-11-11 | | the user's to decide |
| fb-bindgen: an option for the declarations' visibility, and `///` docs on what it writes, so `fb doc --check` passes on bindings | fb-luau, L0, 2026-11-11 | fb-luau's tools/bindgen.sh, which rewrites them | the user's to decide |
| An array type implementing an Interface that has Static members, so generic code that converts by a type's Statics takes a `T[]` | fb-luau, L2, 2026-11-11 | array_statics.fb | the user's to decide |
| An Error's Class name as the program runs, which the runtime prints for an uncaught one | fb-luau, L3, 2026-11-11 | | the user's to decide |
| `E: Error` inferred from a Func argument's Throws, as `Wrap<A, R, E>(f: Func(A): R Throws E)` | fb-luau, L3, 2026-11-11 | | the user's to decide |
| A For Field or Property loop that picks by an attribute, as a For Method loop does with `Where m Has A` | fb-luau, L4, 2026-11-11 | field_pick.fb | the user's to decide |
| `Where Self: Class` in an Interface's default, so a default can keep what implements it as an Object | fb-luau, L4, 2026-11-11 | | the user's to decide |
| A For Method loop over the marked Methods of any shape, so one a library's loops don't take can be an error where it's written | fb-luau's review, 2026-11-11 | | the user's to decide |
| `TypeName<T>()` with its module, or an id of a type | fb-luau's review, 2026-11-11 | | the user's to decide |
| std's Load taking an empty Array where an Object or a Map is wanted, as an option: a Luau table that's empty is both, and the package loads again for each one | fb-luau's review, 2026-11-11 | fb-luau's luau/records.fb | the user's to decide |
| A ParseError's path as steps, not only as text, so code that reads it needn't parse quoted keys | fb-luau's review, 2026-11-11 | fb-luau's luau/records.fb | the user's to decide |
| std.test: an `ExpectPanics`, so a panic at the caller is tested without a program of its own | fb-luau's review, 2026-11-11 | fb-luau's tests/panics | the user's to decide |
| std: setting a file's write time. fbengine's tests run `powershell` or `touch` to give a write a time of its own | fbengine's assets lane, 2026-11-11 | fbengine's packages/fbengine/tests/written.fb | the user's to decide |
| A For Method loop that visits a marked Static, ByRef, or Unsafe Method, or says it passed one over: `Register<T>` can't tell that one was marked | fb-luau's second review, 2026-11-11 | fb-luau's tools/gen_arity.py | the user's to decide |
| Whether a coroutine that's destroyed, as when its Scheduler is dropped mid-wait, should run its Defers; it runs none today | fb-luau's second review, 2026-11-11 | fb-luau's luau/threads.fb | the user's to decide |
| A wait that can be cut short: a Task or `Wait` a program can cancel, so what waits years needn't be polled each tick | fb-luau's second review, 2026-11-11 | fb-luau's luau/threads.fb | the user's to decide |
| The time of the scheduler running the code, readable by a coroutine, as `Scheduler.Current.Time`: a library that keeps its own waits can't tell what's due on a program's Scheduler, whose time is scaled and paused | fb-luau, review-2, 2026-11-12 | fb-luau's luau/threads.fb, which wakes each 50 ms instead | added, e8efe8b2, in 1.3.2: `Scheduler.Current` and `s.Time()` |
| A generic Function giving a Bool, number, or String it holds as its `T` when T is that type: `flag As T` is "'As' can't convert Bool to T", so code that knows T by TypeName can't return the value | fb-luau, review-2, 2026-11-12 | generic_as.fb | the user's to decide |
