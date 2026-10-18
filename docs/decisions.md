# Decision Log

Every locked decision and the reason behind it. Newest decisions at the bottom of each
section.

## Project

| Decision | Why |
|---|---|
| Name **FutureBlitz**, extension `.fb` | revives an earlier dead project, this time to finish |
| **No Blitz3D source compatibility** | frees the design; old code is translated by migrator + AI instead |
| A **sibling language**, not a dialect; nothing ugly inherited from Blitz | approachable BASIC feel without the baggage |
| **No "modern vs classic" split** anywhere | it's a new language and engine; every feature is first-class |
| MIT license | broad adoption |
| Language, compiler, tooling first; engine after | the engine is built on a finished language |

## Lexical and syntax

| Decision | Why |
|---|---|
| Remove type suffixes (`%`, `#`, `$`) | pointless with real types and an IDE/LSP |
| Case-sensitive, PascalCase keywords; a miscased keyword gets a fix-it on syntax errors | simpler tooling; `if` stays a valid identifier |
| `//` and `/* */` comments | frees `'` for character literals |
| Every block ends with `End <Kind>` | consistency; no `Wend`/`Next`/`EndIf` |
| Parentheses required on every call | removes Blitz's command-call ambiguity |
| Top-level code is the only entry point; `Main` is an ordinary name | keeps Blitz's script feel; one way to start a program |
| No `Goto`/`Gosub` | |
| String escapes, interpolation, raw and multi-line strings | Blitz had none |
| Lines end at `\n`, `\r\n`, or `\r`; leading BOM ignored | same line rule as the LSP |
| Contextual keywords: `Get`, `Set`, `Step`, `Lib`, `From`, `Weak` | lets names like `Set<T>` and `.Step(dt)` exist |
| `Func` and `Callback` both reserved | both build function types; uniform type parsing |
| ASCII-only identifiers in v0 | simple lexer; Unicode can be added later without breaking code |
| `\xHH` limited to ASCII; `\u{...}` must be a scalar value | strings are always valid UTF-8 |
| Decimal literals need digits on both sides of `.` | `1..3` and `a?.b` lex unambiguously |
| Newlines end statements inside block bodies, even within brackets | block lambdas work as call arguments |
| Continuation after binary operators, `=`, compound assignment, `,`, `=>` | long expressions and signatures split naturally |
| Multi-line strings strip the closing `"""`'s indentation | unambiguous with mixed tabs and spaces |
| `#Else` added to conditional compilation | the obvious missing branch |
| A block comment counts as a space, even across lines | comments never change how code parses |
| An interpolation in a single-line string ends on the same line | the string itself is one line |
| Spaces or tabs may follow an opening `"""` | invisible trailing whitespace shouldn't be an error |
| Lines that start inside an interpolation skip the indentation check | they're code, not string text |
| Only calls, assignments, and `++`/`--` can be statements | a bare expression does nothing, so it's almost always a mistake |
| Trailing commas allowed in bracketed lists | multi-line lists are easier to edit and diff |
| Single-line `If ... Then ... Else ...`; no `ElseIf`, no block statements | one-liners stay one line; anything bigger is a block |
| A label must come right before a loop | labels only exist for `Break` and `Continue` |
| `>>`, `As?`, and `T?` are written without spaces | tells shifts from nested generics and `T?` from the ternary |
| Lambdas are always written with `Function` (`=> expr` or a block); no `() =>` | one way to write a function |
| `Func(A, B): R`; a `?` or `[]` after it belongs to the return type, so `(Func(): Int)?` needs parentheses | the return type reads like any other type annotation |
| A lambda's parameter types and return type come from the Func expected where it is, unless it writes them | `list.Filter(Function(x) => x > 0)` stays short |

## Operators

| Decision | Why |
|---|---|
| C-style operators for arithmetic, comparison, logical, bitwise | no word operators like `And`/`Or`/`Not`/`Mod` |
| `Operator +(a: T, b: T): T` in a Struct or Class: a Static function named by its operator, taking a value of its type | operators are found through their operands' types, with ordinary overloading |
| `!=` is always `!(a == b)`; `&&`, `\|\|`, `??`, `?:`, `Is`, and `As` can't be overloaded | the operators that decide control flow or types keep one meaning |
| `x op= y` uses `op`; a type's `Operator ==` is used by every `==` on it, inside Structs, tuples, `T?`, and `Contains` too | one definition of equality per type |
| An Operator has no ByRef or default parameters, no type parameters of its own, and doesn't throw | an operator reads like arithmetic, which has no hidden effects |
| Split `=` (assignment) and `==` (comparison) | BASIC's dual-use `=` is ambiguous |
| Assignment is a statement; `++`/`--` are statements | avoids `x = i++` trickery |
| Ternary `? :` | C-style |
| `Is`, `As`, `As?` remain words | C has no equivalent |

## Types and semantics

| Decision | Why |
|---|---|
| Sized integers, floats, `Bool`, math types built in | Blitz had only Int/Float/String |
| Any variable with an initializer may omit its type; without one it needs a type; parameters and return types are always annotated; definite assignment enforced | safety without verbosity |
| Non-null by default, `T?`, flow narrowing | eliminates null crashes |
| Only lossless implicit conversions | |
| `Class` (reference) + `Struct` (value); `Class` replaces `Type` | `Type` was a confusing keyword |
| No per-type global lists | use collections / `Pool<T>` |
| Single inheritance + interfaces with default methods | |
| Visibility: `Private` (default, file), `Protected`, `Package`, `Public` | |
| Statics on classes and structs | |
| A Static Field is a Global named after its type, initialized with the Globals in source order; a Static Method has no Self and isn't virtual | statics are module data and functions in a type's namespace |
| `ReadOnly` goes before Fields only: they change only in their Class's `New`, through Self; a Global that never changes is a `Const` | `Const` already holds values made at run time, objects included |
| A ReadOnly Field can be assigned more than once in `New` | the point is that nothing changes it afterwards, which needs no flow analysis |
| A Property's Get and Set are Methods; a Class's are virtual, and `Override Property` replaces them | Properties fit the existing Method tables, and overriding works the same way |
| A Struct's Set changes the variable, like a ByRef Method; a Struct-typed Property gives a copy, so changing part of it is an error | a change that would land on a copy is always a mistake |
| `T[]` and collections are references; `T[N]` is a value; `.Clone()` copies | |
| Struct `==` field-wise; class `==` via `Equatable` or identity; `RefEquals` | |
| Overflow traps in debug, wraps in release; explicit wrapping/checked ops | |
| Strings: no `s[i]`; `.Chars` and `.Bytes` | UTF-8 correctness |
| Integer literals convert to any numeric type that holds their value; decimal literals only to `Float`/`Double` | `Vec3(0, 1, 0)` works without loosening the lossless-only rule |
| Mixed numeric operands: one side must widen losslessly to the other's type | the lossless-only rule, applied to operators |
| Overloads: an exact match beats a lossless conversion; anything else is ambiguous | predictable resolution |
| `For` declares its own loop-scoped variables, which can't be assigned inside the loop | nothing leaks out; nothing changes the count by accident |
| Functions, `Global`s, and top-level `Const`s are file-wide; top-level `Local`s belong to top-level code; no redeclaring, no local hiding a local | declare-anywhere functions like Blitz, without surprise shadowing |
| `Global`s and top-level `Const`s initialize in source order before other top-level code, using only names declared above | deterministic startup; no read before initialization |
| A struct method sees `Self` read-only; a `ByRef Method` can change it, and needs something assignable to be called on | parameters are read-only, and `Self` is a parameter; changes are visible in the declaration |
| Every struct gets a constructor whose parameters are its fields, in order, with their defaults; declaring `New` replaces it | records need no boilerplate; `Vec3(0, 1, 0)` and `Particle(life: 1.0)` both work |
| A field's default must be known at compile time, like a parameter's; struct defaults are checked in source order with the Globals | a field default is a default of the generated constructor |
| A field without a type takes its default's, so code above its Struct can't use that field | the type isn't known until the default is checked |
| A Struct is stored inline, so it can't contain itself, directly or through other Structs | it would have no size; use a class reference (M2 step 3) |
| A field, like a variable, can be passed `ByRef`; two `ByRef` arguments that overlap (`p` and `p.x`) are an error | the same rule as passing one variable twice |
| Changing a field of a `Local` reads the rest of it, so the `Local` must be assigned first | definite assignment tracks whole variables |
| A Struct or Class goes in a string through `Printable`'s `ToString` | its text is a choice the type makes |
| A Class without a `New` gets one without parameters | an object always starts from its field defaults |
| A Class's Methods can always change its fields; `ByRef Method` is only for Structs | `Self` is a reference, so there's no copy to protect |
| An object's fields change through any reference to it, even a read-only parameter or `Const`; a Class's `Self` can't be replaced | read-only covers the reference, not the object |
| Class `==` compares identity unless the Class has an Operator `==`; `RefEquals` always compares identity | identity is the equality every object has |
| Methods are virtual unless `Final`; `Override` must match the parameter types, `ByRef`, and return type exactly | one rule for what overrides what |
| Every Class's `Destroy` runs, the most derived first, and none overrides another | each Class cleans up its own fields |
| A Class's `New` runs the base's first: `Super.New(...)` as its first statement, or the base's `New` that takes no arguments; `Self` waits for it | the object is set up from the base down, like the field layout |
| A Class without a `New` that extends another takes the parameters of each of its base's News, and passes them on | an error Class is two lines, and fields with defaults don't need a New |
| An object's fields start with its base Class's, and its Class's info has a Method table after `fb_class_t` | inherited fields and overrides cost no lookups |
| `As` to a subclass, and from a `T?` to a `T`, checks at run time and panics with the Class it found; `As?` gives `Null`; unrelated Classes are a compile error | casts that can't work are caught early, and ones that fail at run time say why |
| Two Classes meet at the nearest one both extend: `True ? rect : circle` is a `Shape` | the ternary works across a hierarchy |
| `Is` doesn't narrow; `Case e As T` binds the object as a `T` | narrowing stays about `Null`; a type match names the result |
| A `T[]` is an object whose elements are in a buffer of their own, with a Class named like `Int32[]` | it grows without moving, and leak reports name arrays |
| A `T[]`'s `Count` is an `Int`; its Methods are `Add`, `Insert`, `RemoveAt`, `Clear`, `Contains`, and `Clone` | the collection vocabulary, starting small |
| A `T[]` has no zero; `[]` as a default gives each use a new array | non-null by default, without a shared mutable default |
| `T[]` is invariant: a `B[]` isn't an `A[]` | adding an `A` to a `B[]` would break it |
| An array's `==` compares its elements, when they have `==`; `RefEquals` tells `T[]`s apart | the same equality a Struct's fields have |
| A fixed array starts at zero, so a `Local` of one is ready when declared; sizes are known at compile time | declaring then filling a grid is the common case |
| An array element can't be passed `ByRef`, and a `ByRef Method` can't change a `T[]`'s element in place | the buffer moves when the array grows |
| `For ... In` holds the array and reads `Count` before each pass; its variables are read-only copies | removing during a loop can't go past the end |
| Enum values are known at compile time and can use the members above; names and values don't repeat | a value names one member, so `Select` and strings are unambiguous |
| An Enum compares with `==` and `!=` only; `@Flags` adds `\| & ^ ~` and `Has`; `As` converts to and from integers | an Enum isn't a number, but a set of flags is a set of bits |
| `As` to an Enum without `@Flags` checks that the integer is one of its values | every value of a plain Enum is a member, so `Select` can be exhaustive |
| An Enum's zero is its first member, or no flags | fields and arrays of Enums start at a real value |
| A string shows an Enum value's name; a `@Flags` one that isn't a member shows its flags | debugging output reads as source |
| `@Flags` is the one attribute in M2 | the rest come with compile-time features |
| A tuple `(A, B)` is a value like a Struct without a name; its elements are `.0`, `.1`, and so on | multiple returns and pairs need no declared Struct; numbered as in Swift and Rust |
| `Local lo, hi = t` takes a tuple apart, with a name for every element and `_` to skip one; only a Local can | each name takes its element's type, and the tuple's whole shape is checked |
| `a, b = (b, a)` makes the whole value before assigning any target, then assigns them from the left | a swap works, in the order Python uses |
| A tuple literal converts element by element to the tuple its context expects; another tuple value converts only to its own type or its `T?` | `Return (lo, hi)` works for a `(Long, Long)`, and whole tuples don't convert behind the scenes |
| Tuples compare and show in strings element by element, as `(1, a)` | they're plain values with nothing else to show, and printing pairs is common |
| A tuple can't hold a `Weak<T>` | a check can't narrow what it reads; a Weak belongs in an object's field |
| A Func can't throw; a lambda catches the errors of what it calls, and Functions that throw aren't values | a caller of a function value can't know what it throws until interfaces and generics can say |
| Inside a lambda, a captured `Local` isn't narrowed; outside, a `Local` that a lambda assigns stops narrowing once the lambda exists | any call could run the lambda between a check and a use |
| `Defer` runs one call, assignment, `++`/`--`, or one-line `If`, which can't leave the block | a way out can't jump somewhere else halfway through its cleanup |
| A Defer's expressions are evaluated when it runs, as in Swift; `Return`'s value is computed first | it cleans up what's there at the exit, and can't change the result behind the `Return` |
| A block's Defers and the releases of its variables run together, the last first | what's declared before a Defer is still alive when it runs |
| A panic doesn't run Defers | a panic ends the program, and cleanup in a broken state could hide the first failure |
| A Class, or a Struct holding one, has no zero: a `Global` of one needs a value, a Struct's generated constructor requires the field, and every `New` assigns it on every path before reading it or using `Self` | a reference that can't be `Null` must point at an object before anything reads it |
| `Null` takes the `T?` its context expects; a `T` or `T?` converts to a `U?` when the `T` converts to the `U` | `Null` alone has no type |
| Null checks narrow a Local, a Const, or a parameter that isn't `ByRef`, until it's assigned a `T?` or passed `ByRef`; a loop that changes it can't use a check from before the loop; a Global or field needs a Local copy | anything a call could change can't stay narrowed |
| `a?.b` is a `B?` on its own: a chain writes `a?.b?.c` | each `?.` is one visible check |
| `?.`, `??`, and `== Null` on something that can't be `Null` are warnings | the code works; the check is wasted |
| Two `T?` values are equal when both are `Null` or both hold equal values; a `T?` can't be interpolated without `??` | equality follows the `T`'s, and `Null` has no text |
| A `String?`, or a `T?` of a Class, is a pointer that's null for `Null`; any other `T?` is a flag and the `T`, which holds the `T`'s zero while it's `Null` | references cost nothing extra, and releasing a `Null` is always safe |
| A function with a return type returns on every path; one without can't return a value | catches missing returns |
| `Select`: case values known at compile time and of the subject's type; ranges on integers; no duplicates | clear matching and cheap code |
| Parameters are read-only; default values must be known at compile time | no surprise changes; defaults cost nothing at the call |
| `ByRef` parameters, written at the call too (`Swap(ByRef x, ByRef y)`); only assignable, already-assigned variables of the exact type; never the same one twice | changing the caller's variable is visible where it happens |
| A bare `Return` in top-level code ends the program | an early exit for scripts |
| `As` wraps between integers; float to integer truncates and errors (or panics) out of range | explicit casts do what they say; nothing silently becomes garbage |
| Bitwise operators work on integers only; `Bool As <integer>` gives `1` or `0` | no bit tricks on `Bool`, but an explicit way to count with one |
| A `Global` without an initializer starts at zero, `False`, or `""` | assignment can't be tracked across functions; Blitz did the same |
| Literals with no expected type are `Int`, or `Float` when a decimal is among them | `1 + 0.5` does what it looks like |
| A literal argument matches its default type exactly and other types it fits by conversion | keeps overload resolution two-tier |
| A `For` with a literal start counts in the end's type | `For i = 0 Until n` works for any integer `n` |
| `Until` can't see the `Repeat` body's `Local`s | the condition is outside the block |
| `While True` and friends count as never ending | a function ending in a main loop needs no dead `Return` |
| Named functions only at the top level | lambdas cover nested functions |
| Numbers in strings print in the shortest form that reads back exactly | `0.1` prints as `0.1` |
| Out-of-range shifts panic in debug and are masked in release; division by zero and out-of-range float-to-integer `As` always panic | like overflow, but never undefined behavior |
| `For` evaluates start, end, and step once, never steps past the end, and panics on a run-time `Step` of 0 | no surprise wraparound or endless loops |
| Panics exit with code 101 | tests can tell a panic from a normal exit |

## Memory

| Decision | Why |
|---|---|
| **Refcounting** (not tracing GC) | deterministic, no frame hitches |
| `Weak<T>`; debug cycle detector | break cycles |
| `Destroy()` runs when refcount hits 0 | deterministic cleanup |
| `Destroy` is `Method Destroy()`, without parameters or a result; it runs before the fields are released, can't be called, and panics if it keeps a reference to its object | the object is about to be freed, so nothing may hold on to it |
| An object starts with a header: its counts and its Class's info (name, size, destroy, release, visit, count); the leak report names Classes | leaks are easy to find, and the runtime needs no type tables |
| A `Weak<T>` is assigned a `T`; reading it gives a `T?` that's `Null` once the object is gone | `parent?.name` works with the null operators already in the language |
| Reading a `Weak<T>` makes a strong reference, which a check can't narrow | the object can't go away while it's used, but it can between reads |
| An object's header has a 32-bit refcount and a 32-bit weak count; memory waits for the last Weak, the object doesn't | Weak needs no side table, and the header stays 16 bytes |
| `fb_class_t` has separate destroy, release, and visit functions | a `New` that threw skips `Destroy()`, and the cycle detector walks references |
| Debug builds track every object and report each leaked cycle by its fields; release builds only count leaks | finding a cycle is the hard part of fixing a leak, and release builds shouldn't pay for it |
| Closures capture by reference; C++-style capture lists `[Weak Self]`; compiler warns on strong `Self` capture stored in a field | prevent refcount cycles |
| A function value is a reference to a closure: an object of a Class made for its lambda, named like "Function at main.fb:3:12", with a field for each capture and its code in the Method table | closures are counted and freed like any object, and leak reports name them and the cycles through them |
| A captured `Local` lives in a `Cell<T>` object for its whole life; what can't change (parameters, loop variables, Consts, Self) is copied | sharing a variable needs somewhere both sides can reach, and copies cost nothing; each `For` pass gets its own variable |
| A Function used by name becomes a new closure object each time, whose code calls it | one way to call every function value; a static object for closures without captures can come later |
| Pointers only in `Unsafe` | normal code never needs them |

## Generics

| Decision | Why |
|---|---|
| `<T>` syntax | |
| **Both** inline constraints and `Where`; formatter enforces which | inline for simple cases, `Where` for everything else (conditional methods/conformance, relations) |
| Monomorphization | native speed |
| Invariant generics | avoids soundness bugs |
| Const generics | `T[W, H]`, fixed-size math |
| C#-style `<` disambiguation | |
| A generic body is checked once with opaque type parameters, then lowered once per set of type arguments | errors point at the generic code, and each instance is ordinary code |
| Type arguments are inferred from the arguments' types, then literals, then lambdas, then the type expected | each step uses what the earlier ones learned, so `Map(xs, Function(x) => ...)` needs nothing written |
| A non-generic overload beats a generic one that fits as well | the specific function is the one meant |
| Generic Methods aren't in the Method table, so they can't be Abstract or Override | each set of type arguments is a different function, which a table can't hold ahead of time |
| `T?` with a nullable `T` is that type, not a `T??` | one kind of Null |
| A lambda's Class, and `Cell<T>`, are generic in generic code | a closure's captures have the instance's types |
| No generic base Classes, and no Static members in generic types, yet; see the Deferred work in [milestones.md](milestones.md) | each needs a design of its own |
| Constraints are checked at each call, `New`, and type written with type arguments; a constrained `T` has its Interfaces' Methods, Properties, Statics through `T.Zero()`, and Operators | errors point at the use that doesn't meet them, and generic code reads like the code it stands for |
| A conditional Method, whose Where line constrains its type's type parameter, isn't in the Method table | only some instances have it, and a generic Class can't be extended yet |

## Interfaces

| Decision | Why |
|---|---|
| An Interface's Method that's only its first line is required, and one with a body is a default, which a type without its own gets as a Method | a default becomes a Method like any other, which a Class extending it can override |
| An Interface is a type only when it needs no Statics or Operators and doesn't use Self in its members' types; the rest are only constraints | only those can be called on an object whose Class isn't known |
| An Interface's value is a Class's object, and a call goes through a table its Class has for the Interface, which the runtime finds in the Class's list | the value is the object's own reference, so converting it costs nothing and `Is` and `As` work as for Classes; a cache can make the search faster later |
| A Struct implements Interfaces only for generic code, which never boxes it | a Struct stays a value, with no hidden objects |
| A Class extending one inherits its Interfaces, except those that are only constraints | their Self would be the base Class, not the one extending it |
| A required `ByRef Method` is implemented by a Struct's ByRef Method or a Class's Method; a Property by a Property, with a Set if the Interface's has one | a Class's Methods can always change its fields |
| In generic code, an Interface's member calls the type argument's own: directly for a Struct, through the Method table for a Class | generic code costs what the code it stands for would |
| An Interface's Methods can't throw or have type parameters yet, its Properties have no defaults, and its type parameters have no constraints | each needs a design of its own |
| `==` on an Interface's values is an error; `RefEquals` compares the objects | their Classes' == could differ, so neither one is the answer |
| `Abstract Property`, `Abstract ReadOnly Property`, and `Static Property` work like Abstract and Static Methods | a Property's Get and Set are Methods |
| The built-in Interfaces are FutureBlitz code in std/core/interfaces.fb, which the compiler finds by name | the language's own interfaces read like any other |
| Built-in types implement the built-in Interfaces that fit them, and tuples, `T?`s, and arrays by their elements; a Struct or Class implements what it lists | generic code works on the built-in types without anything written, and a user type says what it is |
| A Struct or Class that lists `Equatable` or `Hashable` without its own `==` or `Hash` gets the built-in one: its fields', or its object's | the common case needs nothing written; the built-in `Hash` only goes with the built-in `==`, so equal values always hash the same |
| `Comparable` needs only `<`, and gives `>`, `<=`, and `>=` as defaults | one comparison to write, which the others can't contradict |
| `Hash` gives a `UInt64`; the runtime mixes and combines hashes | combining them by hand needs wrapping arithmetic, which isn't in the language yet |
| A built-in type has the built-in Interfaces' Methods, like `5.ToString()` | what generic code can call on a `T` works on the type itself too |
| An array's `==`, `Hash`, and text are functions lowering makes for each array type | the loop over elements is written once per type |

## Extend

| Decision | Why |
|---|---|
| `Extend` adds Methods, Static Methods, Properties, Operators, and Interfaces to a type, as if its declaration had them; not Fields, a New, or a Destroy | a type's data and its making stay in one place |
| A generic type is extended with its own type parameters' names, `Extend Box<T>`, for every instance; every array is `Extend T[]` | the names mean what they mean in the type, and one Extend fits every instance |
| What an Extend adds isn't in the Method table: nothing overrides it, and it overrides nothing | an Extend in another file can't change what a Class's objects run |
| A built-in type, Enum, or array gets the Interfaces an Extend lists, but not the built-in ones | those come with it by the rules for built-in types |
| An Extend's Where makes what it adds, and what it lists, conditional on its type parameters | `List<T>` is Printable exactly when its elements are |
| A Class that implements an Interface only through an Extend's Where isn't a value of it yet | each instance's object would need a table of its own, which codegen doesn't make yet |

## Sequences

| Decision | Why |
|---|---|
| Interfaces can be generic, like `Iterator<T>`, and a type implements one instance of each | its members' types are always known from the type alone |
| A call infers a constraint's type arguments from what the type argument implements | `Collect(numbers)` needs none written for `Where S: Iterator<E>` |
| `For ... In` goes over an `Iterator<T>`, whose `ByRef Method Next(): T?` gives each element, then Null | an Iterator is usually a Struct, which a loop steps without making an object; like Swift's and Rust's `next` |
| A value whose `Iterate()` gives an Iterator is gone over by that Iterator, with no Interface for it | an Interface would have to name the Iterator's type, which a Struct's would need; like C#'s `foreach` |
| Over an Iterator, `For a, b In` takes each pair apart; only arrays give an index | `For key, value In map` reads the way it's meant |
| `s.Chars` and `s.Bytes` are new arrays | they index and loop like any array; Iterators that don't copy can come with `std.text` |
| A slice is a new `T[]` with copies of the elements, like C#'s ranges on arrays | a view would see the elements move when the array grows or shrinks |
| `nums: Int...` packs the rest of a call's arguments into a new array, and one array argument is passed as it is | like C#'s `params`, both kinds of call read naturally |

## Errors

| Decision | Why |
|---|---|
| Checked `Throws` + `Try`/`Catch`/`Finally` + `Try`/`Try?` prefixes | explicit, no surprise exceptions |
| `Panic` for bugs | |
| Errors lowered as return values | zero cost when nothing throws |
| `Error` is a Class with a `Message`, in std/core/error.fb; every error's Class extends it | one root for Catch and Throws, and a message every error has |
| `Throws` lists Classes, and each error a call can throw is caught or in the function's `Throws` | the compiler knows what can go wrong where |
| A call that throws needs `Try`, `Try?`, or a Try block around it | every place that can throw is marked |
| `Try` and `Try?` bind like prefix operators | `Try? Parse(s) ?? 0` reads the way it's meant |
| `Try?` catches every error and gives a `T?` | treating a failure as no value takes one word |
| A Catch that an earlier one covers, or that nothing in the body throws, is an error | a Catch that can't run is a mistake |
| A Catch and the Finally start from what was true before the Try | the error can come from any part of the body |
| `Finally` runs on every way out of its Try, and can't throw or leave itself; neither can a Defer's statement | cleanup can't replace the error, or jump away in the middle of leaving |
| An `Override` can't throw more than the Method it overrides | a call through the base handles every error |
| A `New` can throw; its object is released without its `Destroy()`, and a throwing base `New` is called `Try Super.New(...)` | opening or parsing in `New` is normal, and `Destroy()` could read fields `New` didn't set |
| `Destroy` can't throw | it runs at whatever release is last, with no caller to give the error to |
| Top-level code can let errors out: its Defers and Finallys run, then "uncaught Class: Message" and exit code 1 | scripts don't need a Try around everything, and still clean up |
| `Panic(message)` and `Assert(condition, message)` are built-ins; code after a Panic doesn't run, and an Assert narrows | a bug stops the program where it's found |
| Assert is checked in release builds too | code after it relies on its condition, so skipping the check would turn a clear panic into a crash |
| A function that throws takes its caller's `Error?` as a hidden last parameter; every Method in a slot does when the first one throws | errors are return values, and overrides stay callable through the Method table |
| `Task.Wait()`/`Await` **re-throw** the task's error | like C# |

## Concurrency

| Decision | Why |
|---|---|
| Three tools for three jobs: tasks/`Parallel For`, async/await, coroutines | covers all game concurrency needs |
| Data races are compile errors (Sendable, `Shared Class`) | safety; keeps refcounts non-atomic by default |
| Engine APIs `@MainThread`; mutable globals main-thread-only unless `Atomic`/`Mutex` | |
| v1 ships tasks, async, channels, Mutex, Atomic, `Thread.Start`; defers lock-free collections, actors, GPU compute | |
| Strings are `Sendable`, so their refcounts are atomic from M2; class objects stay non-atomic | immutable text is the data most often shared between threads |
| The pool starts its workers when it's first used | a program that never uses it has no threads to start or stop |
| A worker that waits for a job runs only the jobs it started itself meanwhile, newest first, and a Channel wait runs none (changed in M4 step 8) | a job it ran for someone else could wait on a Channel for what this thread would do after it, which would never come |
| Each worker's queue is a deque behind a lock; it takes its newest job, and others steal its oldest | simple and correct first; a lock-free deque can replace it without changing the runtime's API |
| A parallel range hands out a few chunks per thread from a shared counter, and the calling thread takes chunks too | a thread that finishes early takes more, and a small range needs no other thread |
| main waits for the pool's jobs before releasing the Globals | a job may still use a Global, like an Atomic |
| A panic or an uncaught error ends the program with `_Exit`, after flushing, and a second thread's panic waits for the end | exiting normally would destroy what other threads still use, like the pool |
| `Sendable` is an Interface in std/core/interfaces.fb, which the compiler decides from a type's parts | generic code constrains on it like any Interface, and no type has to say it |
| A Struct is Sendable only when its fields all are, so one holding a Class reference isn't (settled from open-questions.md) | a copy of the Struct shares the object, whose refcount isn't atomic |
| A type can list Sendable, or an Interface that extends it, and the compiler checks it | a promise a type makes, which a later field can't break quietly |
| Sendable is only a constraint, but an Interface that extends it can be a type | a value of Sendable alone could do nothing; one of an Interface extending it has its Methods |
| `Atomic<T>` and `Mutex<T>` are built-in types, ATOMIC and MUTEX in the type table, that stay where they're made: a Global, a Static Field, or a Class's Field, or passed ByRef | every thread has to share the one; a copy would be a variable of its own, and a Struct is copied |
| An Atomic holds an integer or a Bool, and its operations are sequentially consistent | the simplest order to reason about; LLVM's atomics do it directly |
| Add, Subtract, and Exchange give what the Atomic held before | like fetch_add, which hands out tickets |
| A Lock block's variable is the Mutex's value in place, like a ByRef parameter | the body changes the shared value, and a lambda can't keep it past the Lock |
| A Mutex lets go on every way out of its Lock, as a Defer runs | an error or a Return can't leave it held |
| Locking a Mutex the thread holds already panics, which the runtime knows from each thread's number | it would wait forever, which is worse to find than a panic |
| The runtime's Mutex is a word that's 0, 1, or 2 (held with waiters), waiting with C++20's atomic wait | its zero is a Mutex no thread holds, so Globals and objects need no setup |
| A Shared Class's fb_class_t says it's shared, and the runtime's retain and release check it | an Interface's value can hold either kind of object, so only the object knows; every retain already goes through the runtime |
| A Shared Class extends only a Shared Class, and only a Shared Class extends it | a variable of one never holds an object that one thread owns |
| A Shared Class's object holds a Weak reference to itself while it lives, and counts far above any real count while its Destroy() runs | another thread's last Weak reference can't free it early, and a Weak read can't bring it back while it goes |
| Work on another thread captures only what can't change, holding a Sendable value: a Const, a parameter, a loop variable, or Self (settled from open-questions.md) | a Local is shared by reference, which another thread would change at the same time |
| Spawn and Thread.Start take a lambda or a Function's name, of no arguments | the closure moves to the other thread; one the program kept would be on two threads |
| A Task or Thread stays on the thread that started it, for now | only that thread takes its result, which may be an object of the work's own |
| A Task's object is of a Class the compiler makes for each Task type, shared, whose Method table's first entry runs the closure and keeps the result | the runtime runs any Task the same way, without knowing T |
| The program waits for every Task and Thread before releasing the Globals | they may still use an Atomic or Mutex Global |
| Thread.Start's work runs on a thread of its own that fb_pool_finish joins, as a job the pool counts | one way to wait for both, and a waiting thread still helps with the pool's jobs |
| The main-thread rules follow what a worker's code can reach, through calls, lambdas, overrides, Interface implementations, and a New's Destroy(), once every body is checked | a Func value or object a worker uses is one its own code made, so that's all it can run |
| A Global or Static Field is the main thread's unless it's an Atomic or a Mutex, and so is a top-level Const that isn't Sendable | the main thread could change one while another thread reads it |
| A Shared Class's Destroy() is checked as work on another thread | whichever thread lets go of the object last runs it |
| The error names the shortest way from the work to the use, with the use labeled | the use can be far from the Spawn that reaches it |
| A Parallel For's body is a closure whose code loops over a chunk of the indexes, which the runtime finds first in its Class's Method table | one call per chunk, not per index, and the closure's captures carry what the body uses |
| A Parallel For's body copies what it uses from outside, even a Local, which it can't change | passes run at once, so a shared variable would be a race; the copies need no cell |
| An array from outside a Parallel For's body is used only through its elements and Count, and changed only at the pass's own index | each pass owns its element, and the array object's refcount is never touched off the calling thread |
| Parallel For counts up by 1, with no Step | the runtime's ranges are of consecutive indexes |
| A Channel is a built-in, shared object of the runtime's, with a ring of element bytes that compiled code copies in and out with their references | like an array, the runtime needn't know T, and each Channel type's Class releases what's left |
| A Channel's Receive gives a T?, Null once it's closed and empty, and TryReceive sets a ByRef variable | one value says both whether it got one and what, and For x In ch stops at the Null |
| A Channel waits on the pool's condition, whose generation every send, receive, and close changes | one way to sleep and wake for jobs and Channels alike |
| An Async Function is an LLVM coroutine (switched resume), whose frame holds its allocas across an Await | LLVM does the frame and the splitting; lowering only marks where it suspends |
| An Async Function's call makes its Task, which the coroutine takes as its first parameter and finishes, and it keeps references to its parameters while it runs | it goes on after its caller has let go of them |
| The runtime's scheduler resumes a coroutine once its awaited Task is done, on the main thread, when top-level code ends and in every Wait() there | the coroutines' code stays on the main thread, and a Wait there can't block what it waits for |
| A Wait or Await on any Task works the same way: a job's done flag, which an Async Function's Task has without the pool running it | Spawns and Async Functions give the same Task |
| Only a Function can be Async for now, without Throws or ByRef parameters, and Await can't be in a Lock | a Method needs its Self kept, Throws needs a Task that holds the error, and a Lock would stay held across the wait |
| `Task`, `Spawn`, `Channel`, `Mutex`, `Atomic`, `Scheduler`, and `Thread` are built in, as `T[]` is, and backed by the runtime | std/ files can't call the runtime until FFI (M5) |
| NextFrame, Seconds, and RealSeconds give a Task that the scheduler running the code finishes at a tick | Await, Wait, and IsDone work on them as on any Task |
| A coroutine waits on the scheduler that ran its Await, and a Scheduler's Run moves it there, with the coroutines and timers it waits for | an Async call starts on the scheduler running the code, before Run has its Task |
| The main scheduler's time is the real clock's, and a Scheduler's is the sum of its Ticks | pausing one is not ticking it, and scaling it is ticking it by less |
| `MainThread()` gives a Task that's done | coroutine code only runs on the main thread, so there's nowhere to hop from |
| A coroutine that starts waiting during a tick goes on at the next one | a loop that Awaits a done Task can't keep a tick from ending |
| A Scheduler that goes destroys the coroutines waiting on it, which let go of what they hold without running their Defers | a Global Scheduler goes at the end with coroutines mid-wait, which would leak; a release shouldn't run user code |
| An Async Function's errors go in its Task, whose type lists them, "Task<T> Throws E", and Wait and Await re-throw them each time (M4 step 11) | the call returns before the errors happen, and the type says what waiting for it can throw |
| An Async lambda's code makes its Task and calls a coroutine of its own | a call through a function value works the same whatever the lambda is |
| An Override of an Async Method is Async, and an Interface's Methods can't be Async yet | a call through the base, or an Interface, has to make the Task the same way |
| A member loop in a lambda in generic code gets the fields of each instance's type arguments | each instance of the code already lowers its own copy of the lambda's code |

## M4 build order

| Decision | Why |
|---|---|
| Eleven steps: threads in the runtime, `Sendable`, `Atomic` and `Mutex`, `Shared Class`, `Spawn` and `Task`, the main-thread rules, `Parallel For`, `Channel`, `Async` and `Await`, coroutines and schedulers, then loose ends | each step's checks are in place before the code that relies on them; see milestones.md |

## Compile time

| Decision | Why |
|---|---|
| `#If` is handled by the lexer, which skips the lines of a branch that isn't taken without lexing them | the spec says inactive code needn't parse, and the parser never sees it |
| The symbols are DEBUG or RELEASE, and the platform's; an #If that tests another gets a warning | a misspelled symbol would quietly be False |
| An attribute goes before a declaration, on its line or the one above; the ones for later milestones are reported as not supported yet | attributes read like modifiers, and long ones fit on a line of their own |
| `@Inline` is LLVM's alwaysinline, and debug builds run the pass that inlines it | the attribute asks for inlining whatever the build |
| `@Deprecated` warns at each use outside the declaration and its type | the deprecated code can still use itself while callers move off it |
| A top-level Const known at compile time can be used anywhere, even above its line; one that something needs early is worked out then | sizes in types need them, and the order of known values shouldn't matter |
| A Const Function runs on MIR: sema lowers it and the interpreter in compiler/src/evaluate.cpp runs it, with its own stack of calls | lowering already says what every construct means, so the interpreter only needs MIR; deep recursion doesn't use the compiler's stack |
| A Const Function takes and gives what a Const holds, calls only Const Functions, and uses no Globals or objects | that's what the compiler can hold and run without the runtime |
| A Const Function that fails while it runs is a compile error at the call, with where it failed; it stops after 10 million steps or 10,000 calls deep | the spec makes overflow and division by zero at compile time errors, and a loop that never ends can't hang the compiler |
| In a declaration's type, a size can't come from a Const Function yet | declarations' types are worked out before Const Functions' bodies are checked |
| A Const type parameter is a positive integer, written as a literal, a Const, or another Const type parameter; calls infer it from fixed arrays' sizes | it's for sizes and counts, which are known when types are made |
| `Attribute Name(params)` declares an attribute, a Struct of its parameters, whose values are known at compile time; `f.Attribute(Name)` gives a `Name?` | reflection reads attributes as ordinary values, and their parameters work like a call's, with defaults and names |
| Reflection is `For Field f: F In T` and `For Method m In T`, which lowering unrolls into a copy of the body for each member | it's code generation without a second language: the body is ordinary code, checked once |
| A For Field's body is checked once, with `F` a type parameter that its Where lines constrain; over a function's own `T`, they're constraints on T's fields that each call checks | errors show up where the loop is written or where it's called, never inside a copy |
| For Method goes over Methods that take nothing and give nothing, which `m.Call(value)` calls | they can all be called the same way; Methods with parameters need a way to pass them, which comes later |

## Collections

| Decision | Why |
|---|---|
| The collections are FutureBlitz code in std/collections/, and `T[]`'s new Methods are an `Extend T[]` | they exercise the language, and the compiler only adds the `{key: value}` literal |
| A Map keeps its entries in an array, with each bucket starting a chain of them; removing one moves the last entry into its place | entries without empty slots need no placeholder values, and iterating is in the order added until something is removed |
| A Set is a Map whose values are True | one hash table to get right |
| A Map literal makes a Map, then calls its `Add` for each entry | the literal means what the code it stands for means; a repeated key keeps its last value |
| Stack, Queue, and Deque have their own words (`Push`, `Enqueue`, `AddFirst`), and `Add` too | each reads as what it is, and every collection still has `Add` |
| Taking a value from an empty Stack, Queue, Deque, or PriorityQueue panics | `Count` says whether there is one, and a `T?` would make every use unwrap |
| A Queue drops its taken values once they're half its array, and a Deque is two arrays that take half of each other when one runs out | both are amortized constant time without placeholder slots |
| A Pool's Handles carry their slot's generation | a Handle to a removed value never finds the one that reuses its slot |
| Sema's symbols are a `std::deque` | a reference to one stays valid while checking adds more, like a lambda's parameters in a call's argument |

## Loose ends (M3 step 13)

| Decision | Why |
|---|---|
| Explicit arithmetic is an integer's Methods, `a.WrappingAdd(b)` and `a.CheckedAdd(b)`, which gives a `T?`; MIR tests for overflow with a flag on `binary_t` | reads like the operation it replaces, and LLVM's `with.overflow` intrinsics do the test |
| What implements an Interface's throwing Method gets the error variable, whether it throws or not | every Method in a table is called the same way, without an adapter in between |
| An Interface's generic Methods are called only on type parameters | each set of type arguments makes a Method of its own, which a table can't hold |
| `For x In Try items` passes on a throwing Iterate's errors | Try already means "let this go on"; the loop has no call to put it before |
| An Extend of one instance or one array type has the instance's types | `Extend Box<Int>` should let `Value` be an Int, not a T |
| A Class that implements an Interface through an Extend's Where gets the table for each instance that meets it, which lowering asks sema's implements() about | lowering makes instances analysis never saw |
| A generic base is an instance in `type_t::base`, and walks over a Class's bases go through each one's definition | inherited members keep the instance's type arguments |
| A generic type's Statics are each instance's globals, made as lowering uses them, and start with what's known at compile time | the instances are only all known once lowering is done |
| Const Functions are declared with the Globals, and while types are declared, one runs only when its code reaches no type whose members are still being declared | a size in a Field's type can come from one, without order mattering |
| A computed Const type argument uses only `+` and `*` | the result is positive whenever the parameters are |
| Reflected Properties and Static Fields have no Set | a Property without a Set or a ReadOnly Static Field can't be told apart for a type parameter |
| A For Method's Where line picks the Methods of a Func type | so `m.Call(value, ...)` can be checked once |
| Each copy of a loop over members has its own closure Classes | a lambda there can use the copy's member |

## Modules, FFI, compile-time

| Decision | Why |
|---|---|
| File = module; `Import`; private by default; no textual include | replaces `Include` |
| Until modules (M5), a program is its main file plus every file in std/, which share one namespace; library files only declare things | `std.collections` can be written in FutureBlitz before `Import` exists |
| A program gets only the library code it uses: a library function once something calls it, and a library Class once the program makes one or tests for it | std/ can grow without every program's MIR and executable growing with it |
| The compiler reads std/ from its source tree (`build_config::std_directory`) for now | there's no install layout yet |
| `project.toml` packages with versioned deps and `[native]` libs; static linking default | single-binary games |
| In-source `Extern "C"` blocks + `@CLayout` + `Callback` + bindgen | replaces `.decls` |
| `Const Function`s evaluated at compile time | |
| `Const` is a read-only value, computed at compile time when possible | one keyword for constants and read-only bindings |
| Everything computable at compile time is computed at compile time; overflow or division by zero there is a compile error | free speed, and mistakes show up early |
| Compile-time-only reflection via attributes | small, fast binaries |

## Engine, renderer, assets

| Decision | Why |
|---|---|
| Modern object API with properties | discoverable, type-safe |
| One renderer; Blitz look via `Shading.Vertex` etc. | no classic/modern split |
| Own Vulkan backend from the start | |
| SDL3, miniaudio, Jolt, fastgltf, stb_image, KTX | |
| Windows, Linux, macOS first | |
| Standardized asset system (VFS, cache, async, typed refs, cooking, glTF) | Blitz's asset handling is a mess |

## Tooling and process

| Decision | Why |
|---|---|
| C++23, CMake, FetchContent, LLVM + lld | |
| LLVM through its **C API**, linked dynamically (`LLVM-C.dll`, `libLLVM`) | stable ABI; works with the MinGW toolchain and the official Windows installer |
| A small **MIR** between the AST and LLVM IR, from M1 | strings already need scope-based releases; lowering is testable without LLVM |
| M1 links through the C++ compiler that built the runtime, with `lld`, statically | it finds the C runtime; a self-contained toolchain comes later |
| End-to-end tests are `.fb` programs with their expected output | checks the whole pipeline, including the runtime |
| The CLI is `fb`: `fb build`, `fb run`, and `fb check` in M1 | short to type, like `go` and `zig` |
| `--show tokens,ast,types,mir,llvm` prints any stage | one flag on every command; see what the compiler did without a debugger |
| `fb build hello.fb` writes the executable next to the source; `-o` moves it | no project files needed for a single file |
| Color with `--color auto\|always\|never`; `auto` follows the terminal and `NO_COLOR` | readable output that still pipes cleanly |
| Exit codes: 0 success, 1 compile errors, 2 bad usage; `fb run` passes the program's code through | scripts and CI can tell what went wrong |
| One driver runs the pipeline for the CLI, the tests, and later the LSP | the stages are wired together in one place |
| Monorepo; target names = project names (`compiler`, `runtime`, `cli`, `lsp`) | |
| Own LSP and eventually own IDE | |
| Hunter types all real code; Claude writes it in chat | stay sharp while using agentic tools |
| Claude writes the tests directly | tests check Hunter's typing; writing them isn't where the learning is |
| Claude does project setup and minor review fixes directly | setup and nits aren't where the learning is |
| Catch2 (via FetchContent) for unit tests | standard, header-friendly, works with ctest and CLion |
| Temporary built-in `Print(String)` until `std.core` exists (M5) | M1 needs output before there are modules |
| M1 type checking covers `Bool`, integers, floats, and `String`, with `As` between numbers | the smallest set that compiles real programs; the rest arrives in M2 |
| M2 is built in 11 steps, each designed and then built in one go; see [milestones.md](milestones.md) | plumbing first, and every step ends with leak-free programs |
| M3 is built in 12 steps the same way, from several-file programs to `std.collections` in FutureBlitz | the library needs files before modules, and each feature needs the ones before it |
| M2 leaves interfaces, `Extend`, operator overloading, tuples, lambdas, slices, variadics, and `.Chars`/`.Bytes` to M3, and visibility to M5 | they need generics or modules; M2 stays about data |
