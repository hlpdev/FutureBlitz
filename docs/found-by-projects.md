# Found by projects

Every bug in FutureBlitz, fb, or std, and everything a project built on them wants changed or
added, from the day it's found: fbengine, fb-luau (the luau package), and scpcb-x; what a game finds in fbengine is at the end. A row is added
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
| Two diagnostics that disagree on one expression: with `Local room: Room?`, `room != Null && ... && (room?.Root.Showing() ?? False)` gets a warning that `?.` isn't needed and an error that `room?.Root` may be Null | scpcb-x, 2026-11-11 | scpcb-x's tests/server_test.fb | fixed, c1a3878f, in 1.3.4: a `?.` on what can't be Null gives the member's own type |
| `Null` can't be assigned straight to a Struct's `T?` Field: `asked.Jump = Null` is "Null needs a type from where it's used", though the Field's type is known | scpcb-x, 2026-11-11 | scpcb-x's sim/person.fb:531 | fixed, c1a3878f, in 1.3.4: the direct assignment worked; the error was a second one after a target refused for its own reason, and is gone |
| An error inside a lambda that's an argument to an overloaded call is said as "the call to 'SetFunction' is ambiguous", with every overload listed and nothing of the lambda's own error (here, `topic.Length` without std.text imported). Giving the lambda to a typed Local first shows the real one | scpcb-x, C7, 2026-11-13 | fbengine-notes/from-scpcb-x.md's last section | fixed, f9dab78d on dev/lambda-errors, for the next patch: overloads that all find errors in a lambda's body are ranked as if it had none, and the one called says the body's errors |

## Wanted: changes and additions

Each waits on the user's yes unless its state says otherwise.

| What | Found by | Shown by | State |
|---|---|---|---|
| A way to key by identity: a `RefHash(o)` beside `RefEquals`, or an `IdentityMap<K, V>`. A Map uses a Class's own `==` and `Hash`, and `Weak<T>` isn't Hashable | fb-luau's review, 2026-11-11 | identity_key.fb | added, c0506f68, in 1.3.1: `RefHash(o)` and std.collections' `Identity<T>` |
| A `T?` or a tuple implementing an Interface by an Extend, as `Extend T? Implements I Where T: I`. fb says "Extend adds to a Struct, Class, Enum, built-in type, or T[], not T?", for `Extend Int?` too. std.db's `Nullable(value)` is there for the same reason, and luau 0.1.1 can't let an exposed Method give "a T or nil", which scpcb-x met | fb-luau's plan, 2026-11-11; scpcb-x through luau, 2026-11-13 | extend_nullable.fb | the user's to decide; luau 0.1.2 gives `LuauValue.Nil` and `LuauValue.Of` meanwhile |
| A Func type implementing an Interface | fb-luau's plan, 2026-11-11 | | the user's to decide |
| A typed `For Method` loop's `Where R: I` picking the Methods whose R has I, or a line that says "not a `T?`". Now it's a constraint every Method the loop takes must meet, and `Where M: Func(): R` takes a Method that gives an `Int?` with R an `Int?`, so a loop for `R` and one for `R?` can't go over the same type. luau's Register can't bind a marked Method that gives a `T?` beside one that gives a `T` | fb-luau, 0.1.2, 2026-11-13 | loop_nullable_result.fb | the user's to decide |
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
| An Interface's Static default keying what implements it by identity: `Identity<T>` asks `T: Class`, which an Interface can't say of Self, `value As? Object` refused a T, and `Identity<Object>` wasn't allowed | fb-luau, review finding 2, 2026-11-12 | interface_identity.fb | fixed on dev/object-identity, for 1.3.3: `x As? Object` takes any type parameter's value, and Object meets `T: Class` |
| A generic Function giving a Bool, number, or String it holds as its `T` when T is that type: `flag As T` is "'As' can't convert Bool to T", so code that knows T by TypeName can't return the value | fb-luau, review-2, 2026-11-12 | generic_as.fb | the user's to decide |
| A Field with its type's name not hiding the type inside the Class (`Hardness: Hardness`, then `Hardness.NORMAL`) | scpcb-x, 2026-11-11 | scpcb-x's sim/refining.fb:66 | the user's to decide |
| A Class and an imported Function of one name not ambiguous where a type's meant (`Mod` beside std.math's `Mod<T>`) | scpcb-x, 2026-11-11 | scpcb-x's content/packs.fb:57 | the user's to decide |
| `Self` in `New` once what's used of it is assigned, or another way to make two objects that hold each other | scpcb-x, 2026-11-11 | scpcb-x's sim/site.fb:114 | the user's to decide |
| Assigning through `?.` (`held?.Held = True`) | scpcb-x, 2026-11-11 |  | the user's to decide |
| A `T?` Field narrowing after a null check, as a Local does | scpcb-x, 2026-11-11 |  | the user's to decide |
| `String.Slice` by characters, or one that doesn't panic off a character's boundary; `Graphemes()` with a Count and an index | scpcb-x, 2026-11-11 | scpcb-x's tests/edge_test.fb:109 | the user's to decide |
| An escape for a brace in a String beside `{{` | scpcb-x, 2026-11-11 |  | the user's to decide |
| `For`, `In`, `Is`, `As`, `Loop` as a Method's or a Field's name; a Method named `Spawn` called bare in its own Class | scpcb-x, 2026-11-11 | scpcb-x's sim/work.fb | the user's to decide |
| Two files of one name in different folders imported by a third | scpcb-x, 2026-11-11 | scpcb-x's items/items.fb | the user's to decide |
| `fb check` checking the tests too, or saying it hasn't | scpcb-x, 2026-11-11 |  | the user's to decide |
| A file nothing imports being checked, and a name from one saying where it is | scpcb-x, 2026-11-11 |  | the user's to decide |
| An `Extend`'s Methods without importing its file, or fewer imports for a test of one Class | scpcb-x, 2026-11-11 |  | the user's to decide |
| A failing test's `Print` shown without `--show-output` | scpcb-x, 2026-11-11 |  | the user's to decide |

## fbengine

What a game found in the engine, kept here beside FutureBlitz's so there's one list; the game's own list is fbengine-notes/from-scpcb-x.md.

| What | Found by | Shown by | State |
|---|---|---|---|
| A `Sprite2D`'s last row and last column of pixels aren't always drawn, by where on the screen it is | scpcb-x, 2026-11-11 | the note scpcbx-sprite2d-last-row.md, with a program | a bug, open, with the engine's rendering lane |
| A model loaded again by hot reload has its file's own materials, so a room whose look the game set is drawn nearly black until it's made again; a hook as an asset's reloaded would let a game give it its look again | scpcb-x, 2026-11-11 | scpcb-x's rooms/look.fb | a bug, open, with the engine's rendering lane |
| `Canvas` has no stored-values blending, as `Camera.SetStoredValues(True)` gives 3D, so what a HUD fades or adds is brighter than the original game's | scpcb-x, 2026-11-11 | the note scpcbx-canvas-stored-values.md | a gap, open, with the engine's rendering lane |
| Building a scene with `Gpu.SetDisabled(True)`: inert textures and materials, so one path makes a room for a window and for a server | scpcb-x, 2026-11-11 | scpcb-x's main.fb:48 | wanted |
| Hiding what's drawn of a tree of entities and leaving it solid | scpcb-x, 2026-11-11 | | wanted |
| A resting contact: a body that didn't move touching what it rests on, so "is it on a floor" isn't true every other tick | scpcb-x, 2026-11-11 | scpcb-x's sim/person.fb | wanted |
| The slide's `MaxSlope` as Blitz's, which slides up whatever it touched | scpcb-x, 2026-11-11 | scpcb-x's sim/person.fb:514 | wanted, minor |
| A canvas and a font in one unit: a canvas's places are pixels and a font's size the window's units | scpcb-x, 2026-11-11 | scpcb-x's app/ui.fb:44 | wanted |
| Mounting over a path forgetting what was loaded from under it | scpcb-x, 2026-11-11 | scpcb-x's content/packs.fb:363 | wanted, minor |
| One mount's own files, to list or hash a base pack with a mod over it | scpcb-x, 2026-11-11 | scpcb-x's content/packs.fb:160 | wanted |
| What's in view with no `Camera`, for a server | scpcb-x, 2026-11-11 | scpcb-x's sim/person.fb:1006 | wanted, minor |
