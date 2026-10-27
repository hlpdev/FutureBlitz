# The review of M6 step 30's compiler part for Routes<T>: a typed For Method whose Where Func binds type parameters of its own per Method, and takes Async Methods when it gives a Task

9 confirmed findings: 4 high, 3 medium, 2 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## lower-1 (high): Overloaded Methods: sema checks only the first of a name, lowering copies all, and the compiler asserts

Status: fixed in 1239a4ff (sema and lowering now share one Method list, fb::loop_methods() in sema.hpp, which lowering's reflected_methods() and sema's loop_methods() call: it drops a Method only where one found overrides it, so each overload is checked), with sema_tests' "a typed For Method checks each Method it copies, overloads and Extends' too": Put(Plain) before and after Put(Args), over a concrete type at the loop and through Routes<T> at the call

Where: compiler/src/sema.cpp:16164

sema's loop_methods() skips any Method whose name is already in `found` (std::ranges::contains(found, method.name, ...)), so of two overloads that both fit the loop's Func pattern only the first one's binding is checked against the loop's other Where lines (check_methods() for a concrete type, field_misfit() for a call). Lowering's reflected_methods() dedups only by `overrides`, so it makes a copy for every overload, including one whose binding doesn't meet the constraint. A.DefaultValue() in that copy then has no Defaultable conformance and lower.cpp:1830's assert(false) fires (an assert-free compiler would go on with no implementation). Both the loop over a concrete type and the generic Run<T>(api: T) form crash the same way. Overloads that both meet the constraint (Show(a: Args), Show(o: Other)) lower and run fine, so it's only the missing check.

```
Import std.core

Struct Args Implements Defaultable
    Field id: Int
    Static Method DefaultValue(): Args => Args(7)
End Struct

Struct Plain
    Field id: Int
End Struct

Class Api
    Method Show(a: Args): String => "show {a.id}"
    Method Show(p: Plain): String => "plain {p.id}"
End Class

For Method m: M In Api
    Where M: Func(A): String
    Where A: Defaultable
    Print("{m.Name} {TypeName<A>()} {m.Call(Api(), A.DefaultValue())}")
End For
```

Expected: error: Api's Method 'Show' has Plain for A, which doesn't implement Defaultable, as this loop needs (and the matching call-site error when the loop is in a Function Run<T>(api: T) called as Run(Api()))

Actual: Assertion failed: false, file .../compiler/src/lower.cpp, line 1830 (fb exits 3); the generic Run<T> form crashes the same way

Suggested fix: In loop_methods(), dedup the way lowering does: drop a base Method only when a Method already found overrides it (compare against the symbol's overrides, or keep symbols in bound_method_t), not by name, so every overload lowering copies gets its binding checked. Add a sema test with two overloads where the second's binding misses the constraint, for both the concrete loop and the call-site check.

Skeptic: Ran the finding's program (rv-formethod/v3/l1.fb) in debug: 'Assertion failed: false, file .../rev-crypto/compiler/src/lower.cpp, line 1830', fb exits 3. sema.cpp's loop_methods() (around 16164) skips a Method when std::ranges::contains(found, method.name, &bound_method_t::name), so the second Show overload (Plain for A) never gets its binding checked against Where A: Defaultable. Lowering still makes a copy for it. A compiler crash on a well-formed program that sema should reject.

## sema-1 (high): A type parameter's Method constraint is taken as met by any of its own For Method loops, whatever their Func pattern, so the compiler asserts in lowering

Status: fixed in 1239a4ff (field_misfit()'s check of a type parameter counts another Method bound as met only through same_method_bound(): a loop of the same Func type, with its own type parameters lined up, constraining the same one with the same Interface), with the same sema case's Outer<U>: its Func(A) loop says nothing for Inner's Func(B, Int) loop, nor its Func(A, B) loop's Where A for Second's Where B, so both calls are errors, while Routes(api), whose loop is Outer's own, isn't

Where: compiler/src/sema.cpp:10260

field_misfit()'s is_parameter branch treats a METHOD bound as known when any bound in m_bounds has the same parameter, members == METHOD and the same Interface. It never checks that the other bound comes from the same Func pattern and the same position in it. That is fine for For Field, where every loop covers all fields. A For Method loop covers only the Methods that match its pattern, though. So Outer<U>'s loop over Func(A): String with Where A: Defaultable lets Outer call Inner<U>, whose loop is over Func(B, Int): String with Where B: Defaultable, and nothing checks Inner's Methods. Outer(Api()) then makes a copy with B = Plain, which has no DefaultValue, and lowering hits assert(false). The same happens with one pattern when the Where lines constrain different positions, for example Func(A, B) with Where A in one loop and Where B in the other.

```
Import std.core

Struct Args Implements Defaultable
    Field id: Int
    Static Method DefaultValue(): Args => Args(1)
End Struct

Struct Plain
    Field id: Int
End Struct

Class Api
    Method One(args: Args): String => "one {args.id}"
    Method Two(args: Plain, n: Int): String => "two {args.id}"
End Class

Function Inner<T>(api: T)
    For Method m: M In T
        Where M: Func(B, Int): String
        Where B: Defaultable
        Print(m.Call(api, B.DefaultValue(), 2))
    End For
End Function

Function Outer<U>(api: U)
    For Method m: M In U
        Where M: Func(A): String
        Where A: Defaultable
        Print(m.Call(api, A.DefaultValue()))
    End For
    Inner(api)
End Function

Outer(Api())
```

Expected: An error at Inner(api): 'Inner' needs what each Method of U has for B to implement Defaultable, but nothing says that of U's Methods. Calling Inner(Api()) directly does give the matching error, about Two having Plain for B.

Actual:    Compiling t1.fb
Assertion failed: false, file .../compiler/src/lower.cpp, line 1830 (the compiler exits with code 3)

Suggested fix: For a METHOD bound, count another bound as known only when it constrains the same position of an equal pattern. Compare the two loops' method_pattern_t signatures with the fresh parameters lined up (m_method_patterns has both), and compare the Interfaces after mapping one loop's fresh parameters onto the other's. Add a sema test with two loops whose patterns differ.

Skeptic: Ran the finding's program as rv-formethod/v-sema/s1.fb with fb run (debug): it printed 'Compiling s1.fb' and then 'Assertion failed: false, file .../compiler/src/lower.cpp, line 1830', exit 3. field_misfit()'s is_parameter branch (sema.cpp ~10260) counts any bound with the same parameter, members and interface as known. It never compares the two loops' method patterns or the positions they constrain, so Inner's Func(B, Int) loop over U is taken as met by Outer's Func(A) loop.

## sema-2 (high): Sema's loop_methods() keeps only the first of a set of overloaded Methods, while lowering copies them all, so an overload that breaks the Where lines goes unchecked and the compiler asserts

Status: fixed in 1239a4ff (as lower-1, the shared list), with the same sema case's two overload orders

Where: compiler/src/sema.cpp:16164

loop_methods() skips a Method whose name is already in `found` (std::ranges::contains(found, method.name, ...)). The aim is to drop a base Method that a derived one overrides, but it also drops a second overload with the same name. Lowering's reflected_methods() makes a copy for each overload (t10 prints both 'Put Int32' and 'Put String'). check_methods() and field_misfit() check only the first Put. With Put(a: Args) declared before Put(a: Plain), the loop passes sema. Lowering then makes the Put(Plain) copy, where A.DefaultValue() doesn't exist, and asserts. This happens over a concrete type and over a type parameter.

```
Import std.core

Struct Args Implements Defaultable
    Field id: Int
    Static Method DefaultValue(): Args => Args(1)
End Struct

Struct Plain
    Field id: Int
End Struct

Class Api
    Method Put(a: Args): String => "args {a.id}"
    Method Put(a: Plain): String => "plain {a.id}"
End Class

Function Routes<T>(api: T)
    For Method m: M In T
        Where M: Func(A): String
        Where A: Defaultable
        Print("{m.Name} {TypeName<A>()} {m.Call(api, A.DefaultValue())}")
    End For
End Function

Routes(Api())
```

Expected: t.fb:24:1: error: 'Routes' needs what each Method of Api has for A to implement Defaultable, but its Method 'Put' has Plain for A, which doesn't. The same loop written directly over Api should give "Api's Method 'Put' has Plain for A, which doesn't implement Defaultable, as this loop needs".

Actual:    Compiling t11b.fb
Assertion failed: false, file .../compiler/src/lower.cpp, line 1830 (exit code 3). The loop written directly over Api (t11.fb) asserts the same way.

Suggested fix: Drop only the Methods that a derived Method overrides, keyed by name and Func type or by the override link, not by name alone, so that sema's list matches lowering's reflected_methods(). Better, have sema and lowering share one Method list. Add a test with two overloads in each order.

Skeptic: s2.fb (over a type parameter) and s2b.fb (the loop written directly over Api) both stop with 'Assertion failed: false ... lower.cpp, line 1830', exit 3. loop_methods() skips a Method when std::ranges::contains(found, method.name, &bound_method_t::name) holds, so it drops the second Put overload. Lowering makes a copy for it, and that copy has Plain for A, which has no DefaultValue.

## sema-3 (high): Sema's loop_methods() leaves out what an Extend of one instance, like Extend Box<Int>, adds, which lowering copies, so its Methods go unchecked and the compiler asserts

Status: fixed in 1239a4ff (sema and lowering now share one Method list, fb::loop_methods() in sema.hpp, which lowering's reflected_methods() and sema's loop_methods() call, which adds analysis_t::instance_members as lowering's member_chain() did), with the same sema case's Routes(Box<Int>()), whose Extend Box<Int> adds Two(Plain), and Routes(Box<String>()), which has no Two and is fine

Where: compiler/src/sema.cpp:16154

loop_methods() reads only m_analysis.symbols[declared].members. Lowering's member_chain() also adds m_analysis.instance_members for the instance, which spec 13 says the loops go over. A Method from `Extend Box<Int>` is therefore among the loop's copies (t14a prints 'One Args' and 'Two Plain'), but check_methods() and field_misfit() never see it. When it binds A to a type that doesn't meet the loop's Where lines, sema accepts the loop and lowering asserts.

```
Import std.core

Struct Args Implements Defaultable
    Field id: Int
    Static Method DefaultValue(): Args => Args(1)
End Struct

Struct Plain
    Field id: Int
End Struct

Class Box<T>
    Method One(a: Args): String => "args {a.id}"
End Class

Extend Box<Int>
    Method Two(a: Plain): String => "plain {a.id}"
End Extend

Function Routes<T>(api: T)
    For Method m: M In T
        Where M: Func(A): String
        Where A: Defaultable
        Print("{m.Name} {m.Call(api, A.DefaultValue())}")
    End For
End Function

Routes(Box<Int>())
```

Expected: error: 'Routes' needs what each Method of Box<Int> has for A to implement Defaultable, but its Method 'Two' has Plain for A, which doesn't

Actual:    Compiling t14b.fb
Assertion failed: false, file .../compiler/src/lower.cpp, line 1830 (exit code 3). The loop written directly over Box<Int> asserts too.

Suggested fix: Include the instance's instance_members in loop_methods(), the same way lowering's member_chain() does, ideally through one shared helper, and add a test for it.

Skeptic: s3a.fb, which has no Where on A, prints 'One Args' and 'Two Plain', so lowering does go over Extend Box<Int>'s Two. s3.fb, with Where A: Defaultable over Box<Int>, stops with 'Assertion failed: false ... lower.cpp, line 1830', exit 3. loop_methods() reads only m_analysis.symbols[declared].members and never adds the instance's own members.

## lower-2 (medium): In a Function, a nested typed For Method treats an outer loop's type parameter as a new one of its own

Status: fixed in 1239a4ff (collect_field_bounds() enters each loop's own type parameters, a For Method's or a For Field's, before going into its body, as checking does, through collect_loop_bounds(); and check_stmt() now runs method_signature() itself, reusing the pre-pass's type parameters by name, so a loop in a function reports its own mistakes too), with tests/programs/reflection_bound_methods.fb's nested loops at the top and in Function Nested, which print the same two pairs

Where: compiler/src/sema.cpp:10452

collect_method_bounds() runs method_signature() in a pre-pass over a function's body and caches the result in m_loop_methods, which check_for_member then uses. At that point the outer loop's own type parameters aren't in the type scope, so add_new_names() sees the inner loop's `A` as not a type and makes it a new type parameter. The spec says a name that is a type stays one. At top level the nested loop is checked in scope, and A is the outer loop's A. So the same nested loop gives different copies at top level and inside any Function, generic or not. Lowering then goes over every Method of the inner Func's shape, and the inner body's A is the inner Method's type, not the outer's.

```
Import std.core

Class Api
    Method SI(n: Int): String => "si"
    Method SS(s: String): String => "ss"
    Method II(n: Int): Int => 1
    Method IS(s: String): Int => 2
End Class

For Method m: M In Api
    Where M: Func(A): String
    For Method k: K In Api
        Where K: Func(A): Int
        Print("top {m.Name} with {k.Name}: {TypeName<A>()}")
    End For
End For

Function Plain(api: Api)
    For Method m: M In Api
        Where M: Func(A): String
        For Method k: K In Api
            Where K: Func(A): Int
            Print("fn {m.Name} with {k.Name}: {TypeName<A>()}")
        End For
    End For
End Function

Plain(Api())
```

Expected: top SI with II: Int32
top SS with IS: String
fn SI with II: Int32
fn SS with IS: String

Actual: top SI with II: Int32
top SS with IS: String
fn SI with II: Int32
fn SI with IS: String
fn SS with II: Int32
fn SS with IS: String
(the same in --release; a generic Run<T>(api: T) gives the same wrong four lines)

Suggested fix: Don't cache a pattern made out of scope. Either have collect_method_bounds() enter the enclosing loops' own type parameters before calling method_signature(), or skip caching for nested loops so check_for_member computes it in scope. Then re-key m_method_patterns to match. Add a program or sema test of a nested typed For Method inside a Function that reuses the outer loop's A.

Skeptic: Ran the finding's program (v3/l2.fb). In debug and --release the top-level loops print 'top SI with II: Int32' and 'top SS with IS: String', but the same nested loops inside Function Plain print all four pairs: 'fn SI with II: Int32', 'fn SI with IS: String', 'fn SS with II: Int32', 'fn SS with IS: String'. So inside a Function the inner A is a new type parameter, not the outer one. The same code means different things depending on where it is, which goes against the spec rule that a name that is already a type stays one.

## lower-3 (medium): A loop's own type parameter's Where lines aren't in force when its Func type is resolved, so Weak<A>, maps, and the like can't be written

Status: fixed in 1239a4ff (method_signature() puts the loop's other Where lines, resolved quietly, in m_bounds while it resolves the Func type), with the sema case's and the program's Func(Weak<A>): String under Where A: Class, which prints "Watch Node"

Where: compiler/src/sema.cpp:16059

method_signature() enters the new type parameters into scope bare and resolves the Func type before the loop's other Where lines are applied. So any part of the pattern whose type needs a constraint on A fails, such as Weak<A> (needs Class) or ImmutableMap<A, B> (needs Hashable & Sendable). That happens even when the loop has exactly the Where line the error suggests. The spec says A is 'constrained by the loop's other Where lines', and a For Field's or a function's type parameters can be used this way under their constraints. A Routes<T>-style pattern that takes a map or a Weak can't be written.

```
Import std.core

Class Node
    Field v: Int
End Class

Class Api
    Method Watch(w: Weak<Node>): String => "watch"
End Class

For Method m: M In Api
    Where M: Func(Weak<A>): String
    Where A: Class
    Print("{m.Name} {TypeName<A>()}")
End For
```

Expected: Watch Node

Actual: t15.fb:12:19: error: Weak<T> refers to an object of a Class, not A; a constraint like <A: Class> says it's one
(likewise 'A doesn't implement Hashable, which ImmutableMap<K, V> needs for K' for Func(ImmutableMap<A, B>): String with Where A: Hashable & Sendable and Where B: Sendable)

Suggested fix: In method_signature() (and the pre-pass), resolve the fresh_constraints() Where lines into m_bounds for the fresh parameters before resolving the Func type, as a generic function's own constraints are in force when its parameter types are resolved. Then pop them. Add a test with Func(Weak<A>) under Where A: Class.

Skeptic: Ran v3/l3.fb (Where M: Func(Weak<A>): String plus Where A: Class): 'l3.fb:12:19: error: Weak<T> refers to an object of a Class, not A; a constraint like <A: Class> says it's one'. The constraint it suggests is already there. Control program v3/l3b.fb, Function Look<A: Class>(w: Weak<A>), compiles and prints 'Node'. method_signature() (sema.cpp ~16058) calls enter_type_scope(fresh) and resolve_type() with no bounds from the loop's other Where lines. That contradicts the spec, docs and lane notes, which say A is 'constrained by the loop's other Where lines'.

## sema-4 (medium): Sema checks Extend Methods that the loop's file can't see, so it reports an error about a Method the loop doesn't go over

Status: fixed in 1239a4ff (sema and lowering now share one Method list, fb::loop_methods() in sema.hpp, which lowering's reflected_methods() and sema's loop_methods() call, which leaves out what an Extend the loop's file doesn't see adds, by the loop's file, which method_pattern_t keeps, over a type parameter too), with tests/programs/reflection_bound_modules.fb and its folder, the finding's layout, which prints "plain 9" and "One args 1"

Where: compiler/src/sema.cpp:16154

loop_methods() doesn't apply the sees_extension() filter that lowering's member_chain() applies. Spec 13 says the loops go over what an Extend adds 'only where the loop's file sees the Extend's module'. Here an Extend in mod/ext.fb, which only mod/user.fb imports, adds Two(a: Plain) to Api. The main file's loop doesn't go over Two: without the Where line it prints only 'One Args'. Adding `Where A: Defaultable` still makes sema report Two, so a correct program fails to build.

```
// mod/api.fb
Import std.core

Public Struct Args Implements Defaultable
    Public Field id: Int
    Public Static Method DefaultValue(): Args => Args(1)
End Struct

Public Struct Plain
    Public Field id: Int
End Struct

Public Class Api
    Public Method One(a: Args): String => "args {a.id}"
End Class

// mod/ext.fb
Import "./api"

Extend Api
    Public Method Two(a: Plain): String => "plain {a.id}"
End Extend

// mod/user.fb
Import "./api"
Import "./ext"

Public Function UseTwo(api: Api): String => api.Two(Plain(9))

// main.fb
Import std.core
Import "./mod/api"
Import "./mod/user"

Local api = Api()
Print(UseTwo(api))
For Method m: M In Api
    Where M: Func(A): String
    Where A: Defaultable
    Print("{m.Name} {m.Call(api, A.DefaultValue())}")
End For
```

Expected: plain 9
One args 1

Actual: main.fb:7:20: error: Api's Method 'Two' has Plain for A, which doesn't implement Defaultable, as this loop needs
error: couldn't build main.fb because of 1 error

Suggested fix: Filter loop_methods() by whether the loop's file sees each Extend's module, as lowering does (sees_extension()), preferably by sharing one Method-list helper with lowering. Over a type parameter, use the loop's file. Add a module test.

Skeptic: I built the module layout (mod/api.fb, mod/ext.fb, mod/user.fb, main.fb). main2.fb, the same program without the Where A line, prints 'plain 9' then 'One', so the loop doesn't go over Two. main.fb with 'Where A: Defaultable' fails with 'main.fb:7:20: error: Api's Method 'Two' has Plain for A, which doesn't implement Defaultable, as this loop needs'. loop_methods() has no sees_extension() filter, so sema rejects a correct program, against spec 13's rule that a loop sees an Extend only where its file sees the Extend's module.

## sema-5 (low): Using the loop's own M in its Func type, as in `Where M: Func(M): String`, silently makes M a fresh parameter that hides the Func type

Status: fixed in 1239a4ff (add_new_names() reports the loop's own M: "M is the Methods' Func type, so it can't be one of the types in it", and the loop goes on as an untyped one, without a second error), in the sema case

Where: compiler/src/sema.cpp:16057

method_signature() calls add_new_names() before the loop's M is in scope, so the M inside Func(M) becomes a new type parameter of the loop's own. The loop then goes over every one-argument Method that gives a String, binding M to that argument's type. In the body, TypeName<M>() gives Int32 instead of the Func type, which it gives in every other typed loop (t7: 'One Func(Int32): String'). Nothing reports that the Func type names itself.

```
Import std.core

Class Api
    Method One(a: Int): String => "one {a}"
    Method Two(s: String): Int => 2
End Class

For Method m: M In Api
    Where M: Func(M): String
    Print("{m.Name} {TypeName<M>()}")
End For
```

Expected: An error: M is the Methods' Func type, so it can't be one of the types in that Func type.

Actual: One Int32

Suggested fix: In add_new_names() (or method_signature()), report a name equal to node.type_name instead of making it a fresh parameter, and add a sema test.

Skeptic: s5.fb prints 'One Int32' and reports no error. method_signature() calls add_new_names() before M is in scope, so the M in Func(M) becomes a new type parameter that hides the loop's M. TypeName<M>() then gives the argument's type, not the Func type. The lane's rule is that a name that is a type stays one, and M is the loop's own type name, so this should be an error, not a quiet new parameter.

## sema-6 (low): A second Where line on M in a typed For Method is silently ignored

Status: fixed in 1239a4ff (method_signature() reports each Where line on M after the first: "a For Method loop says which Methods it goes over with one Where line on M; this is another"), in the sema case

Where: compiler/src/sema.cpp:16124

method_signature() uses only the first Where line on M, and fresh_constraints() drops every line on M. Any later `Where M: ...` line is never resolved or reported, so a loop that seems to ask for two Func types goes over the first one's Methods only. The behaviour isn't new, but this change rewrote how the loop's Where lines are handled, and now reports every other stray line ('B' isn't a type parameter here).

```
Import std.core

Class Api
    Method One(a: Int): String => "one"
    Method Two(a: Int): Int => 2
End Class

For Method m: M In Api
    Where M: Func(A): String
    Where M: Func(A): Int
    Print(m.Name)
End For
```

Expected: An error on the second line, for example "a For Method loop says which Methods it goes over with one Where line on M".

Actual: One

Suggested fix: In method_signature(), report each Where line on node.type_name after the first, and add the case to the sema test.

Skeptic: s6.fb, with 'Where M: Func(A): String' and then 'Where M: Func(A): Int', prints only 'One' and reports nothing. method_signature() uses find_if for the first line on M, and fresh_constraints() drops every line on M, so the second line is never resolved or reported. Any other stray Where line does get reported.
