# Lane 1: sema

The whole-codebase review's findings for A1, A2, A3, A4, from a review of master
at ba70e73 (2026-10-22). Only A3's were checked by a skeptic, which confirmed all 12;
reproduce every other one before fixing it. Files this lane owns: compiler/src/sema.cpp, compiler/include/fb/sema.hpp, compiler/src/types.cpp, compiler/include/fb/types.hpp, compiler/src/const_value.cpp.

65 findings: 14 high, 28 medium, 23 low. Fix the highs, then the mediums, then the lows.

Each finding's Status line is one of: open; fixed in <commit> (with its test);
dropped, with why (it doesn't reproduce, or the spec says otherwise).

## A1-1 (high): Generic Class inheritance cycles through instances aren't caught, and the compiler hangs

Status: fixed in f8db645, with tests

Where: compiler/src/sema.cpp:2989

declare_base() checks for a cycle with types.extends(extended, type) || definition_of(extended) == definition_of(type). That misses a cycle that runs through an instance whose definition's base isn't set yet. set_base() later copies the base onto the instances made earlier, so the chain becomes A -> B<Int32> -> A. Nothing is reported, and conform_instances() (sema.cpp:3570) walks base_of() forever. Every other loop over base_of() would spin the same way.

Reproduced by the reviewer:

```
File cyc5.fb:
Import std.core
Class A Extends B<Int32>
End Class
Class B<T> Extends A
End Class
Print(1)
`timeout 20 fb check cyc5.fb` prints nothing and exits 124. `Class A<T> Extends B<Int32>` with `Class B<T> Extends A<String>` hangs the same way, and so does `Class A<T> Extends B<T>` with `Class B<T> Extends A<Int32>`. With gdb attached, the stack is link_classes -> link_class -> link_class -> conform_instances (sema.cpp:3571), looping on base_of(). For comparison, `Class A<T> Extends B<T>` with `Class B<T> Extends A<T>` is reported: 'B<T> can't extend itself'.
```

Suggested fix: In declare_base(), walk the chain of `extended` with base_of() and compare definition_of(current) with definition_of(type) at each step, so any instance of the type being declared counts as a cycle. Add tests for the three hanging shapes above.

## A1-2 (high): A generic or conditional Destroy is accepted, then crashes lowering or codegen

Status: fixed in ce186d7, with tests; spec 06 in 59e2b00

Where: compiler/src/sema.cpp:3104

check_member_function() checks a Destroy's parameters, result and visibility, but not its own type parameters or its Where lines. `Method Destroy<T>()` is accepted: TypeName<T>() in it prints "T", and a `Local x: T` in it segfaults the compiler. A conditional Destroy (`Where T: Printable`) runs for every instance, including ones that don't meet the Where, and lowering then hits an assertion. The spec says Destroy takes no parameters and runs by itself on every object, so neither form can be meaningful.

Reproduced by the reviewer:

```
(1) Program:
Import std.core
Class C
    Method Destroy<T>()
        Local x: T
    End Method
End Class
Local c = C()
`fb build` dies with 'Segmentation fault' (exit 139). With `Print(TypeName<T>())` as the body instead, it builds and prints `T`.
(2) Program:
Import std.core
Struct Plain
    Field q: Int32
End Struct
Class Box<T>
    Field v: T
    New(v: T)
        Self.v = v
    End New
    Method Destroy()
        Where T: Printable
        Print("bye {v}")
    End Method
End Class
Local a = Box<Int32>(4)
Local b = Box<Plain>(Plain(7))
`fb run` stops with 'Assertion failed: false, file .../compiler/src/lower.cpp, line 1613'. With a body that doesn't use T, it prints `bye 1` for both Box<Int32> and Box<Plain>.
```

Suggested fix: In check_member_function(), when the name is Destroy, report non-empty node.type_parameters ("Destroy can't be generic; it runs by itself") and non-empty node.constraints ("Destroy can't have Where lines; every object's runs"). Add a test for each.

## A2-1 (high): common_type() drops a level of nesting for nested optionals, so code gets miscompiled and the compiler crashes

Status: fixed in 9a63ca6, with tests

Where: compiler/src/sema.cpp:10740

In common_type(), the branch that handles nullable types returns nullable_of(*inner). nullable_of() hands back a T? unchanged, so whenever the inner common type is already nullable, one level of T? goes missing. Example: Dog? and Cat?? meet at Animal? when the answer is Animal??, and Int32?? and Int64? meet at Int64? when it is Int64??. converts() refuses Cat?? -> Animal? and Int32?? -> Int64?, but the callers of common_type() (the '?' ternary, ==, '??', array and Map literals) convert both sides to the result without checking. Lowering then emits a cast between unrelated layouts: {flag, ptr} read as ptr.

Reproduced by the reviewer:

```
exp-all/A2/t6.fb: Classes Animal, Dog Extends Animal, Cat Extends Animal; `Function Pick(flag: Bool, d: Dog?, c: Cat??): Animal?? / Return flag ? d : c`; `Local inner: Cat? = Null; Pick(False, Dog(), inner)`. `--show types` prints `c: Cat?? → Animal?`, the MIR has `_5 = _2 As Animal?`, and the program segfaults (0xC0000005). With numbers, t15.fb (`flag ? a : b` with a: Int32??, b: Int64?), t35.fb (`a == b`) and t36.fb (`[a, b]`) type-check as Int64? (`a: Int32?? → Int64?`), and `fb run` itself segfaults while compiling.
```

Suggested fix: In the nullable branch, wrap with m_analysis.types.nullable(*inner) instead of nullable_of(*inner). Both sides then keep their depth: Animal?? for Dog?/Cat??, and Int64?? for Int32??/Int64?. Add tests for the ternary, ==, '??', and an array literal with optionals nested to different depths.

Skeptic: confirmed=True, medium. Reproduced in exp-all/skA2. t6.fb (Dog? ? Cat?? into Animal??): `check --show types` prints `c: Cat?? → Animal?`, and `fb run` builds it, then t6.exe exits 0xC0000005. t15.fb (ternary, Int32?? and Int64?), t35.fb (`a == b`) and t36.fb (`[a, b]`) all make fb.exe itself segfault while compiling (exit 139); --show types for t15 gives `a: Int32?? → Int64?`. The code matches the claim: common_type() (sema.cpp:10740) returns nullable_of(*inner), which returns an already-nullable inner unchanged. Dog?/Cat?? gives inner = common_type(Dog, Cat?) = Animal?, and nullable_of(Animal?) = Animal?, not Animal??. I lowered it to medium: it only happens when optionals nested to different depths meet in a ternary, ==, or literal, which is an edge-case crash or miscompile.

## A2-2 (high): The Sendable check on Mutex<T> and Channel<T> passes for a Struct declared later, so a data race is accepted

Status: fixed in 99ba25d, with tests

Where: compiler/src/sema.cpp:10241

in_place_of() and channel_of() call unsendable() when the type is resolved. For a Global's type, a Field's type, or a parameter's type, that happens while types are still being declared (m_declared is false). A Struct declared further down has no fields yet at that point, so it looks Sendable. A Mutex<S> or Channel<S> of a Struct that holds a plain Class object is then accepted. check_shared() and the Global rules treat an in-place Mutex as sendable whatever it holds, so worker threads share S's object with non-atomic refcounts. The same program with S declared first is refused. Pointers already get deferred handling for this (m_pending_pointers); these two checks don't.

Reproduced by the reviewer:

```
exp-all/A2/t32.fb: `Global shared: Mutex<S>` comes first, then `Struct S / Field t: Thing?` and `Class Thing`. Work() locks `shared`, stores a Thing in value.t, copies it to a Local, and copies that Local 2,000,000 times; two Spawns run Work(). It compiles with no error. Of 4 runs, 2 segfaulted and 2 reported `leak: 1 Thing objects were never released`, while a single-threaded control (t12c.fb) is clean. t10.fb/t12.fb do the same with `Shared Class Hub / Field m: Mutex<S>`. t11.fb, with S declared before the uses, gets 'a Mutex holds a Sendable value, which S isn't' and the matching Channel error. t9.fb shows `Function Feed(ch: Channel<S>)` with a later S getting no error.
```

Suggested fix: When !m_declared, record the element type and span of a Mutex or Channel (as m_pending_pointers does) and run the unsendable() check once types are declared, reporting there. Add a test that declares the Struct after the Mutex and Channel uses.

Skeptic: confirmed=True, high. t32.fb (`Global shared: Mutex<S>` written before `Struct S / Field t: Thing?`) builds with no error. Of 6 runs, 4 segfaulted and 2 printed `leak: 1 Thing objects were never released`. A single-threaded control (t32c.fb, same code, Work() called twice) runs clean 3 of 3. In t9.fb a later-declared S gets no error on the type annotation `Global m: Mutex<S>` (col 12) or on `Function Feed(ch: Channel<S>)`, but the constructor calls `Mutex<S>(...)` and `Channel<S>(4)` are reported. With S declared first (t11.fb), the Field, the parameter, and the tuple are all refused. in_place_of() and channel_of() (sema.cpp:10201-10250) call unsendable() as soon as the type is resolved, with no deferral while !m_declared.

## A2-3 (high): Main-thread rules don't follow an Interface Property to the Property that implements it

Status: fixed in 7028f0b, with tests

Where: compiler/src/sema.cpp:9308

note_thread_use() records a use of a Property as edges to its getter and setter. When the Property is an Interface's, those are the Interface's accessor symbols. check_threads() adds conformance edges only from the required member to the implementing member, here Property symbol to Property symbol, and nothing from the Interface's getter or setter to the implementing ones. A worker that reads a Property through an Interface value or a type parameter therefore never reaches the implementing getter, so its use of a Global, or of anything else only the main thread may use, goes unreported. A Method on the same path is caught, and so is a Class Property override, since check_inherited_property() sets accessor overrides.

Reproduced by the reviewer:

```
exp-all/A2/t17.fb: `Global counter = 0`, `Interface Valued / ReadOnly Property Value: Int`, and `Shared Class Thing Implements Valued` whose `Property Value` Get does `counter += 1`. `Function ReadIt(v: Valued): Int => v.Value` and `Spawn(Function() => Work())`, where Work calls ReadIt(Thing()), compile and run (prints 1). The same shape through `Method Get()` is refused: 'this work runs on another thread, but it calls ... 'Thing.Get', which uses the Global 'counter''. t18.fb shows the same miss through generic code: `ReadGeneric<T: Valued>(v: T) => v.Value`.
```

Suggested fix: In check_threads(), for each conformance member that is a PROPERTY, also add edges from the required Property's getter and setter to the implementing Property's getter and setter (setter_of() for an inherited Set). Alternatively, make note_thread_use() record the Property symbol too and let the conformance edges lead to the implementing Property's accessors.

Skeptic: confirmed=True, high. Minimal p1.fb: `Interface Valued / ReadOnly Property Value: Int`, `Shared Class Thing Implements Valued` whose Get does `counter += 1`, `ReadIt(v: Valued) => v.Value`, and `Spawn(Function() => ReadIt(Thing()))`. It builds and runs (prints 1). p3.fb, the generic `ReadIt<T: Valued>(v: T)`, also builds and runs. Controls are refused: p2.fb (the same shape through an Interface Method Get()) gives 'calls 'ReadIt', which calls 'Get', which calls 'Thing.Get', which uses the Global 'counter''. p4.fb (a direct Class Property, no Interface) gives '... calls 'Thing.Value', which uses the Global 'counter''. So a Global is changed from a worker thread only on the Interface Property path.

## A3-1 (high): A base Class's New can call an Override that reads the derived Class's unassigned non-null Field (segfault)

Status: fixed in e5a2900, with Swift's rule (docs/m6-plan.md, Decisions), with tests

Where: compiler/src/sema.cpp:11909

check_self_ready() only checks the fields of the Class whose New is being checked (m_required, set up in check_body() around line 12295). Once Base's New has assigned its own fields it can use Self, for example by calling a Method. That call is virtual, so a Derived override runs while Derived's New is still inside Super.New and has not assigned its own Fields that have no zero. The override reads a null object and the program crashes. Spec 06 says a Class needs a New that assigns such a Field 'on every path before reading it or using Self'. This is null unsafety in safe code.

Reproduced by the reviewer:

```
Class Node / Field name: String / End Class
Class Base / Field n: Node / New() n = Node(); n.name = "base"; Show() End New / Method Show() Print(n.name) End Method / End Class
Class Derived Extends Base / Field m: Node / New() Super.New(); m = Node() End New / Override Method Show() Print(m.name) End Method / End Class
Local d = Derived()
(exp-all/A3/t7.fb) compiles with no diagnostic. At run time it exits with 0xC0000005 (segmentation fault) inside Derived.Show, which Base.New calls.
```

Suggested fix: Close the hole at the language level. One option is Swift's two-phase rule: a Class that extends another assigns its own no-zero Fields before Super.New, so let assignments to Self's own Fields by name come before Super.New, and keep Self unusable until then. The other is to refuse virtual calls and Self escapes in a New whose Class can be extended while a derived Class has required Fields. Add a program test that would otherwise crash.

Skeptic: confirmed=True, high. I ran exp-all/V3/t7.fb, which is Base.New calling Show() after assigning n, while Derived overrides Show to read its own m. It compiles with no diagnostic and segfaults (exit 139, 0xC0000005). Spec 06:66 says a no-zero Field is assigned before 'reading it or using Self', and spec 06 also says a derived New must call Super.New first. Nothing in the Deferred work covers this.

## A3-2 (high): A lambda or Parallel For body inside New can change ReadOnly Fields; in a Shared Class this races and corrupts memory

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:14761

in_new_of() only asks whether m_function is a New of the owner. Code inside a lambda or a Parallel For body keeps m_function set to the enclosing New, so check_assignable() (14921) and check_changeable() (14683) let that code assign Self's ReadOnly Fields. A lambda made in New can therefore change a ReadOnly Field after construction, against spec 06 ('not later'). A Parallel For body in a Shared Class's New assigns the ReadOnly Field from many threads at once. A Shared Class's ReadOnly Fields are what makes it safe to share (spec 10), so this is a data race on a String field.

Reproduced by the reviewer:

```
(1) Class A / ReadOnly Field x: Int / Field f: Func(Int) / New() x = 1; f = Function(v: Int) x = v End Function End New / End Class; Local a = A(); a.f(42); Print(a.x) prints 42 (t5.fb).
(2) Shared Class Hub / ReadOnly Field s: String / New() Parallel For i = 0 Until 200000 / s = "value number {i}" / End For End New / End Class; Local h = Hub(); Print(h.s != "") (t16.fb) compiles with no error and exits with 0xC0000005 on every one of 5 runs. With a plain For it prints True. The same assignment in a Method is refused ('s' is ReadOnly).
```

Suggested fix: Make in_new_of() false while m_lambdas is non-empty, which covers lambda code and Parallel For bodies. ReadOnly assignment, ByRef passing, and AddressOf in New then work only in New's own code. Add sema tests for a lambda and for a Parallel For in New.

Skeptic: confirmed=True, high. (1) t5.fb, a lambda made in New that assigns the ReadOnly x, prints 42 after construction (it also leaks a cycle). (2) t16.fb, a Shared Class whose New runs a Parallel For assigning a ReadOnly String: 4 of 5 runs segfault, and the fifth prints True plus 'leak: 124520 Strings were never released'. In V3/v2a.fb the same assignment in a Shared Class's Method is refused (''s' is ReadOnly; only Hub's New can change it'), and a Parallel For in a non-Shared New is refused for capturing Self. This contradicts spec 06:247-248 ('not later') and spec 10:285.

## A3-3 (high): A lambda in New can capture Self before New assigns its no-zero Fields, and calling it reads null (segfault)

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:15949

check_lambda_body() (around 20853) swaps m_required out, so inside a lambda neither check_read() nor check_self_ready() sees the Fields that New hasn't assigned yet. use_self() and capture_self_for() also capture Self without asking whether it's ready. A lambda made in New can use a Field by name, or Self, before the Field is assigned, and New can call it at once (or hand it to code that does). The existing tests (sema_tests.cpp:1460-1486) state the rule that 'nothing can read it, or use Self' until the Field is assigned. The same gap affects a Spawn lambda, a [Weak Self] lambda, a lambda passed in Super.New's arguments that uses a Field by name, and a Shared Class's Parallel For body in New.

Reproduced by the reviewer:

```
Class Node / Field name: String / End Class
Class B / Field m: Node / New() / Local f = Function() / Print(m.name) / End Function / f() / m = Node() / End New / End Class
Local b = B()
(t9.fb) compiles with no diagnostic and exits with 0xC0000005.
```

Suggested fix: When a lambda (or Parallel For body) in a New captures Self, through use_self(), check that Self is ready in the enclosing flow, as check_self_ready() does. For example, record in lambda_context_t whether m_before_super was set or a required Field was unassigned when the lambda began, and report 'Self can't be used before New assigns m' at the capture. Add a test.

Skeptic: confirmed=True, high. V3/t9.fb has a lambda in New that reads m.name. It is called before m = Node(), compiles with no diagnostic, and segfaults (exit 139). The same read or Self use written directly in New is refused: the sema_tests.cpp:1460-1486 cases, and V3/v4b.fb's 'Self can't be used before New assigns 'm''.

## A3-4 (high): Assigning a Property in New before New assigns its no-zero Fields isn't checked; the Set reads null (segfault)

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:14440

A Property's Set is a Method that may read any Field. check_target_place() resolves a Property assigned by name through lookup_value() and never calls check_self_ready(). The member path for 'Self.P = v' skips it too, because the check is guarded by '!target' (around 20611). Reading a Property is checked (check_variable at 15527) and so is calling a Method, but a plain '=' to a Property is not. New can therefore run a Set while a Field it reads is still null.

Reproduced by the reviewer:

```
Class Node / Field name: String / End Class
Class A / Field m: Node / Property P: Int / Get => 1 / Set(value) / Print(m.name) / End Set / End Property / New() / P = 5 / m = Node() / End New / End Class
Local a = A()
(t29.fb) compiles and exits with 0xC0000005. The variant with 'Self.P = 5' (class B in t29.fb) also compiles and crashes.
```

Suggested fix: In check_target_place(), when the target is a non-Static PROPERTY used by name, or 'Self.P', and the code is a New, call check_self_ready(). Add sema tests for 'P = v' and 'Self.P = v' before a required Field is assigned.

Skeptic: confirmed=True, high. V3/v4.fb, 'P = 5' before 'm = Node()' with a Set that reads m.name, compiles and segfaults (exit 139). The t29.fb variant with 'Self.P = 5' (class B) also segfaults. Reading the Property ('Local x = P') or calling a Method in the same place is refused with 'Self can't be used before New assigns 'm'' (v4b.fb), so only Property assignment slips through.

## A3-5 (high): Reflection's f.Set changes ReadOnly Fields anywhere, and racing on a Shared Class's ReadOnly Field corrupts the heap

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:13902

check_member_call() lets 'f.Set(ByRef value, x)' through for every field of a For Field loop, and lowering makes a copy for ReadOnly Fields too. So any code can change a ReadOnly Field after New, against spec 06 and decisions.md line 80 ('they change only in their Class's New'). A Shared Class's ReadOnly Fields are what make it Sendable (spec 10). decisions.md line 670 builds the immutable collections on Shared Class nodes with ReadOnly Fields. A generic copy helper running on several threads therefore races on one String Field. The Deferred work row about Set covers only Properties and Static Fields, not ReadOnly Fields.

Reproduced by the reviewer:

```
Shared Class Hub / ReadOnly Field s: String / New(t: String) s = t End New / End Class
Function CopyInto<T>(ByRef dst: T, src: T) / For Field f: F In T / f.Set(ByRef dst, f.Get(src)) / End For / End Function
Const hub = Hub("start")
Parallel For i = 0 Until 200000 / Local target = hub / CopyInto(ByRef target, Hub("value {i}")) / End For
(t21.fb) Three runs: exit 0xC0000374 (heap corruption), exit 0xC0000005, and once 'True' plus 'leak: 3633 Strings were never released'. In a single thread, t20.fb sets a Class's and a Struct's ReadOnly Field to 0 through f.Set.
```

Suggested fix: Don't give a ReadOnly Field a Set outside its Class's New. One option is a per-copy error in lowering when f.Set lands on a ReadOnly Field. Another is to leave ReadOnly Fields out of the copies that call Set and say so in spec 13. Add a test.

Skeptic: confirmed=True, high. t20.fb prints 0 and 0: f.Set zeroes a Class's and a Struct's ReadOnly Field from outside New. Of 7 runs of t21.fb (a Parallel For running CopyInto on a Shared Hub's ReadOnly String), 5 segfaulted and 2 printed True with 'leak: 3474/3621 Strings were never released'. The Deferred row ('Setting a reflected Property or Static Field...') and spec 13:173-175 cover only Properties and Static Fields. Spec 13:162 gives f.Set to every Field, with no ReadOnly exception, which contradicts spec 06:247.

## A4-1 (high): `a?.M()` on an Async Method gives a Task of the wrong layout, which isn't nullable: wrong results and segfaults

Status: fixed in 2386f2e, with tests: a Task<T>?, which needed lower.cpp's Async call to make its Task from the T?'s T (one line); spec 10 in 59e2b00

Where: compiler/src/sema.cpp:18131

check_call() already wraps a null-safe call's result as nullable_of(T). Then check_expr(call_expr_t) wraps that in task(type, ...), so `w?.Go()` for `Async Method Go(): Int` gets the type Task<Int32?>, not a nullable Task. Lowering builds a Task<Int32?> and passes it to Worker.Go(), which writes into it as a Task<Int32>. When w is Null, the Task variable is a null pointer typed as a non-nullable Task. The same goes for an Async Method that gives nothing: check_call() keeps NONE as it is, so the result is a plain `Task`.

Reproduced by the reviewer:

```
Program: `Class Worker` with `Field n: Int = 7` and `Async Method Go(): Int` / `Return n`; `Function Pick(make: Bool): Worker?`; `Local t2 = Pick(True)?.Go()`; `Print("got2 {t2.Wait() ?? -1}")`. It prints `got2 0`, not 7. With `Pick(False)`, `t.Wait()` segfaults with 0xC0000005. `Async Method Run()` with `Local t = w?.Run()` and `t.Wait()` on a Null w also segfaults. In `fb check --show mir`, `_3 = new Task<Int32?>` is passed to `call virtual Worker.Go()`, whose own task local is `Task<Int32>`.
```

Suggested fix: For an Async callee, build the Task from the callee's own result type: task(substituted(symbol.type), throws, error). Make a null-safe call of it give that Task's T? (Task<T>?), and have lowering branch as it does for other `?.` calls. Another option is to refuse `?.` on Async Methods. Add a program test either way.

Skeptic: confirmed=True, high. Reproduced in exp-all/skeptic-a4/a1.fb: Pick(True)?.Go() then t2.Wait() ?? -1 prints 'got2 0', not 7. a1b.fb (Pick(False)) segfaults with 0xC0000005. a1c.fb (Async Method Run() with no result, w = Null, w?.Run().Wait()) also segfaults. `check --show mir` shows `_3 = new Task<Int32?>` and `class Task<Int32?>`, while Worker.Go's own `_1 task: Task<Int32>`.

## A4-2 (high): fb::implements() leaves out Defaultable and Cloneable, so lowering disagrees with sema and crashes

Status: fixed in 5669678, with tests

Where: compiler/src/sema.cpp:21829

conformance_query_t::builtin() says it works 'As sema's builtin_implements()', but it only knows Equatable, Hashable, Comparable, Numeric, and Printable. Sema's builtin_implements() (line 8635) also makes arrays Cloneable and makes numbers, String, Bool, and Enums Defaultable. Lowering asks fb::implements() which Interfaces an instance has, for its Interface tables and for `Is`. A conditional Extend `Where T: Defaultable` or `Where T: Cloneable` therefore type-checks, but at run time the instance has no table.

Reproduced by the reviewer:

```
Program: `Interface Shown` / `Method Label(): String`; `Class Thing`; `Class Bin<T> Extends Thing` with `Method Label(): String => "bin"`; `Extend Bin<T> Implements Shown` / `Where T: Defaultable` / `End Extend`; `Function Show(s: Shown)` / `Print(s.Label())`. `Local thing: Thing = Bin<Int>()` followed by `Print("{thing Is Shown}")` prints `False`, though sema accepts Bin<Int> as a Shown. `Show(Bin<Int>())` segfaults (0xC0000005). The same happens with `Jar<T>` and `Where T: Cloneable` for `Jar<Int[]>`.
```

Suggested fix: Give conformance_query_t::builtin() the same Cloneable rule (any T[]) and Defaultable rule (numbers, String, Bool, Enums) that sema's builtin_implements() has. Better, have both use one shared helper so they can't drift apart again. Add a conditional-Extend program test for each.

Skeptic: confirmed=True, high. a2c.fb (Bin<Int> with `Extend Bin<T> Implements Shown / Where T: Defaultable`) prints False for `thing Is Shown`. Show(Bin<Int>()) segfaults (0xC0000005). The Cloneable version with Jar<Int[]> does the same (False, then a segfault). With Where T: Equatable it prints True and 'bin'. In the code, conformance_query_t::builtin() (sema.cpp ~21829) returns false for anything but equatable, hashable, comparable, numeric, and printable. builtin_implements() (8628) handles cloneable (is_array) and defaultable (numbers, String, Bool, Enums).

## A4-3 (high): Compiler crash: a Const Function called with a known argument of the wrong type

Status: fixed in 2386f2e, with tests

Where: compiler/src/sema.cpp:18405

check_call() reports the argument mismatch, but check_expr(call_expr_t) still calls run_at_compile_time(). That function only checks that every argument has a value, then passes values whose variant doesn't match the parameter's type (a double, string, or bool for an Int) to the evaluator. The evaluator's std::get then throws, and the compiler dies.

Reproduced by the reviewer:

```
`Const Function Twice(a: Int): Int` / `Return a * 2`; then, even inside an ordinary Function, `Local x = Twice(1.5)`. `fb check` prints `error: fb failed unexpectedly: std::get: wrong index for variant`. It does the same for `Twice("no")` and `Twice(True)`. With a plain `Function Twice`, the correct error is reported: `1.5 doesn't fit in Int32`.
```

Suggested fix: In run_at_compile_time(), run only when every argument fits: skip when any argument's info().converted isn't the parameter's substituted type, or when the error count rose while the call was checked (check_arguments' expect_type failures). A simple version: record in check_arguments whether all arguments were accepted. Add a sema unit test.

Skeptic: confirmed=True, medium. `Const Function Twice(a: Int): Int`, called as `Twice(1.5)`, `Twice("no")`, or `Twice(True)` inside a Function: `fb check` prints 'error: fb failed unexpectedly: std::get: wrong index for variant' (exit 1) for each. The crash is real. I lowered it to medium to match A4-5, A4-6, and A4-7: these are compiler crashes on invalid code, not miscompiles.

## A4-4 (high): An Async lambda is accepted as Spawn or Thread.Start work, so a coroutine runs and awaits on a worker thread

Status: fixed in 3c4f2f9, with tests; spec 10 in 59e2b00

Where: compiler/src/sema.cpp:17936

check_spawn() refuses an Async Function's name (it isn't a function value) and work that calls an Async Function. It doesn't look at whether the work lambda itself is Async (lambda_expr_t::is_async). Spec 10 says Async code never runs on another thread and 'work on another thread can't call one'. As accepted, the worker runs the coroutine. Its Await calls fb_await() from the worker, which adds to current_scheduler() (the main scheduler) without synchronization while the main thread is ticking it inside Wait(): a data race. Thread.Start leaks the Task.

Reproduced by the reviewer:

```
`Local t = Spawn(Async Function()` / `Print("before")` / `Await Spawn(Function() => 1)` / `Print("after")` / `End Function)` / `t.Wait().Wait()`. It compiles and runs, and 'before' prints on a pool worker. `Local th = Thread.Start(Async Function()` / `Await Spawn(Function() => 1)` / `End Function)` / `th.Join()` compiles and ends with `leak: 1 Task objects were never released`. `Spawn(Async Function() => OnlyMain())` is refused only because of the @MainThread call inside.
```

Suggested fix: In check_spawn(), when the work is a lambda_expr_t with is_async (or its closure's function has modifiers.is_async), report that the work runs on another thread and Async code runs on the main thread, as the Async Function case does. Add tests for both Spawn and Thread.Start.

Skeptic: confirmed=True, high. a4.fb, Spawn(Async Function() ... Await Spawn(...) ...) then t.Wait().Wait(), compiles and prints 'before'/'after'. The Async lambda's body runs on a pool worker. a4b.fb, Thread.Start(Async Function() ...).Join(), compiles and ends with 'leak: 1 Task objects were never released'. Calling an Async Function from the work (a4c.fb) is refused, which shows only the Async lambda case is missed. The runtime's fb_await() calls current_scheduler()->add() with no lock and no thread check. current_scheduler() is a plain static, the main scheduler, so a worker changes m_waiting while the main thread ticks it: a data race. This breaks spec 10's 'Their code never runs on another thread, so work on another thread can't call one'.

## A1-3 (medium): Overriding a Property of a generic base instance is refused: 'must be T'

Status: fixed in 328d8df, with tests

Where: compiler/src/sema.cpp:5452

check_inherited_property() compares `member.type != original.type`. original.type is the type as the generic base declares it (T), not as the instance has it. So `Class D Extends Base<Int32>` can't override `Abstract Property Value: T` with `Override Property Value: Int32`. check_inherited() does the same check for Methods correctly, with member_type(base, original). The spec says what a Class inherits from an instance has its type arguments in it, and its overrides take those types. The same raw type also goes into the message (name_of(original.type, ...)).

Reproduced by the reviewer:

```
Program:
Import std.core
Abstract Class Base<T>
    Abstract Property Value: T
End Class
Class D Extends Base<Int32>
    Field v: Int32 = 3
    Override Property Value: Int32
        Get => v
        Set(value)
            v = value
        End Set
    End Property
End Class
Local b: Base<Int32> = D()
b.Value = 7
Print(b.Value)
`fb run` reports "error: 'Value' must be T, like Base<T>'s". A non-Abstract `Property Value: T` in `Class Plain<T>`, overridden by `Class E Extends Plain<String>`, gets "'Value' must be T, like Plain<T>'s". The Method version (`Abstract Method Value(): T`, overridden returning Int32) builds and prints 3.
```

Suggested fix: Compare member.type with member_type(base, original), and show that type in the message, as check_inherited() does. Add a test that overrides an Abstract and a plain Property of a generic base instance.

## A1-4 (medium): An Extend of one instance can redeclare the generic type's members without an error, and calls then dispatch inconsistently

Status: fixed in 13b3a53, with tests

Where: compiler/src/sema.cpp:2432

declare_extension() adds members with add_member(extension_members_of(target), member). For an instance like Box<String>, extension_members_of() returns analysis.instance_members[instance], which holds only what instance Extends added. The generic type's own Fields, Methods and Properties are never compared. So an Extend of Box<String> can declare `Method Same()` or `Property v` that Box<T> already has. For the same Extend on Box<T> or a plain Struct, add_member reports "'Own' is already declared with the same parameter types". The spec (06, Extend) says what an Extend adds can't have a member the type has already. Instead the Extend's member is silently hidden (hide_in_place), or, for a Method whose parameters mention T, it wins in a direct call while generic code still runs the generic type's.

Reproduced by the reviewer:

```
Program:
Import std.core
Struct Box<T>
    Field v: T
    Method Take(x: T): String => "generic take"
    Method Same(): String => "generic same"
End Struct
Extend Box<String>
    Method Take(x: String): String => "string take"
    Method Same(): String => "string same"
End Extend
Function Through<T>(b: Box<T>, x: T): String
    Return b.Take(x)
End Function
Local b = Box<String>("a")
Print(b.Take("b"))
Print(b.Same())
Print(Through(b, "c"))
This builds and prints `string take`, `generic same`, `generic take`: no error, one dead Method, and two different Take()s for the same object. Adding `Property v: Int32` (named like the Field) and `Property Size` (like Box<T>'s Size) in an `Extend Box<Int32>` also builds, and prints the Field's 5 and Box<T>'s 1. `Extend Box<Int32> Implements Named` with a `Method Hello()` that Named's default already gave Box<T> builds, and b.Hello() runs the default.
```

Suggested fix: When declaring an Extend of one instance, compare each member against the generic type's own members, with the instance's types (member_type(instance, ...)), as add_member() does: a Field or Property of the name, or a Method or constructor with the same parameters, is "already declared", labeled at the generic type's member. Add tests for a Method, a Property and a Field name.

## A1-5 (medium): An Interface default can clash with the type's own Field or Static Method and nothing is reported

Status: fixed in b5e98fe, with tests

Where: compiler/src/sema.cpp:4856

take_default() pushes the default's copy straight into extension_members_of(type), bypassing add_member(). check_default_inherited() only compares it with what a Class inherits from its base. So a type that lists an Interface in its own declaration gets the default even when it already has a Field named like it, or a Static Method with its parameters. The spec says such a default is a Method of the type's own, checked like a Method written there, and a Method written there would be "'v' is already declared". The inherited case is reported ("D takes I's default 'v', but 'v' is already declared in Base"); the type's own case isn't. The default then can't be called directly, while calls through the Interface or a type parameter run it.

Reproduced by the reviewer:

```
(1) Program:
Import std.core
Interface I
    Method v(): Int32 => 1
    Method Call(): Int32 => v()
End Interface
Struct S Implements I
    Field v: Int32
End Struct
Function G<T: I>(x: T): Int32
    Return x.v()
End Function
Local s = S(3)
Print(s.v)
Print(G(s))
Print(s.Call())
This builds and prints 3, 1, 1. `s.v()` is "'v' is a field, not a method". A `Class C Implements I` with `Field v` behaves the same.
(2) A Struct with `Static Method Make(): Int32 => 2` that implements an Interface with a default `Method Make(): Int32 => 1` gets no error at the declaration, but `S.Make()` is then "the call to 'Make' is ambiguous", with the Static Method and the default both listed.
```

Suggested fix: In take_default(), for a copy that isn't `extended` (the declaration lists the Interface), check the type's own members of the name as add_member() would. Report a Field or Property, or a Static Method with the same parameters, like check_default_inherited()'s "takes I's default 'v', but 'v' is already declared". Do the same for a usable inherited Static Method in check_default_inherited(). Add tests.

## A1-6 (medium): @MainThread on a Property, Interface or Enum is accepted silently and does nothing

Status: fixed in 1877ac3, with tests: refused on a Property, Interface, or Enum

Where: compiler/src/sema.cpp:3261

The spec (13) says @MainThread goes before a Function or Method. check_field_modifiers() and check_variable_modifiers() refuse it elsewhere. But check_property_modifiers() doesn't refuse it on a Property, and declare_property() builds the Get's and Set's modifiers without is_main_thread, so it's dropped: a worker can read a @MainThread Property and nothing is reported. declare_interface() (line 1286) and declare_enum() (line 1626) don't check is_main_thread either, so `@MainThread Interface` and `@MainThread Enum` compile with no effect. (@MainThread on a New is enforced, which is fine.)

Reproduced by the reviewer:

```
Program:
Import std.core
Shared Class Engine
    @MainThread Property Frame: Int32
        Get => 1
    End Property
End Class
@MainThread Interface Drawn
    Method Draw(): Int32
End Interface
@MainThread Enum Mode
    A, B
End Enum
Function Work(e: Engine): Int32
    Return e.Frame
End Function
Const e = Engine()
Local t = Spawn(Function() => Work(e))
Print(t.Wait())
`fb run` builds and prints 1, with no error about the worker reading a @MainThread Property, and no error for the Interface or the Enum. A @MainThread Method called the same way is reported.
```

Suggested fix: Either refuse @MainThread on a Property in check_property_modifiers() ("@MainThread only goes before a Function or Method"), or copy is_main_thread to the accessors in declare_property() and document it. Refuse it in declare_interface() and declare_enum() with the message declare_type() uses. Add tests.

## A1-7 (medium): Override on a Method or Property of a Class with no base isn't reported

Status: fixed in c8f9e06, with tests

Where: compiler/src/sema.cpp:3522

The "'M' has Override, but A has no Method like it" check is only in check_inherited() and check_inherited_property(). link_class() calls those only when the Class has a base, and check_modifiers() returns for a plain Override. So in a Class that extends nothing, `Override Method` and `Override Property` compile silently, though there is nothing to override.

Reproduced by the reviewer:

```
Program:
Import std.core
Class C
    Override Method M(): Int32 => 1
    Override Property P: Int32
        Get => 2
    End Property
End Class
Class D Extends C
    Override Method M(): Int32 => 3
End Class
Local c: C = D()
Print(c.M())
Print(c.P)
This builds and prints 3 and 2, with no error at C's two Overrides. With a base that lacks M, the same line is reported, as sema_tests.cpp:1559 checks.
```

Suggested fix: In link_class(), when base_of(type) is INVALID, report each member with modifiers.is_override that isn't Static: "'M' has Override, but C extends no Class". Add a test.

## A1-8 (medium): A Static Field or Method can take an inherited instance member's name, hiding it from the Class's objects

Status: fixed in c8f9e06, with tests

Where: compiler/src/sema.cpp:5094

check_inherited() returns at once for a Static member, so a Static Field or Static Method in a derived Class is never compared with what it inherits. The spec says a Class can't declare a field with an inherited member's name. The reverse order is reported: an instance Field named like an inherited Static Field gets "'x' is already declared in Base". But `Static Field x` beside an inherited `Field x` is accepted, and `d.x` then fails with "'x' is Static; use it on D". A Static Method with an inherited instance Method's name and parameters also hides it: D's code calling Talk() runs the Static one, while b.Talk() through Base runs the inherited one.

Reproduced by the reviewer:

```
Program:
Import std.core
Class Base
    Field x: Int32 = 1
    Method Speak(): String => "base"
End Class
Class D Extends Base
    Static Field x: Int32 = 5
    Static Method Speak(): String => "static"
End Class
Local d = D()
Print(d.x)
Print(d.Speak())
The only errors are at the uses: "'x' is Static; use it on D" and "'Speak' is Static; call it on D". Nothing is reported at the declarations. With Base `Static Field x` and D `Field x`, the declaration is reported: "'x' is already declared in Base".
```

Suggested fix: In check_inherited(), compare a Static member too. Report a Static Field or Property named like a member the Class's code can use from its base, and a Static Method with an inherited Method's name and parameters, as "already declared in Base". Add tests for both orders.

## A1-9 (medium): An Extend of a built-in type or Enum can't list an Interface that extends a built-in Interface

Status: fixed in 6ac26af, with tests, among them lane 5's `Interface Real Extends Numeric` on Float32, in tests/programs/extend_refined_builtins

Where: compiler/src/sema.cpp:2442

An Extend may list an Interface that isn't built in ("an Extend can give it others"). conform() then walks with_extended(interface) and makes add_conformance() find members for every Interface the listed one extends, built-in ones included. Int32, String and Enums come with Comparable, Printable and Hashable through builtin_implements(), and their operators and ToString have no symbols, so implementation_of() reports them missing. Listing `Ranked Extends Comparable` on Int32 fails with "Int32 implements Comparable, so it needs an Operator '<'". This blocks the usual pattern of an Interface that refines Comparable or Printable and is given to numbers or Enums.

Reproduced by the reviewer:

```
Program:
Import std.core
Interface Ranked Extends Comparable
    Method Rank(): Int32
End Interface
Extend Int32 Implements Ranked
    Method Rank(): Int32 => Self
End Extend
Interface Shown Extends Printable, Hashable
    Method Tag(): String => "t"
End Interface
Extend String Implements Shown
End Extend
Enum Mode
    A, B
End Enum
Extend Mode Implements Shown
End Extend
Function Best<T: Ranked>(a: T, b: T): T
    Return a < b ? b : a
End Function
Print(Best(3, 7))
`fb run` reports "Int32 implements Comparable, so it needs an Operator '<'" at line 5, and "String implements Printable, so it needs a Method 'ToString'" and "Mode implements Printable, so it needs a Method 'ToString'" at the two Extends.
```

Suggested fix: In conform(), for a target that isn't a Struct or Class (or for any target), skip each Interface from with_extended() that is_builtin_interface() and that builtin_implements(type, it) already gives the type, as the error at line 2751 assumes. Keep the listed Interface's own conformance. Add a test with Int32, String and an Enum.

## A1-10 (medium): A call is ambiguous between an unconditional Extend's default and the conditional default of an Interface refining it

Status: fixed in 4e25bfe, with tests

Where: compiler/src/sema.cpp:16461

Spec 07 says a type that lists Sized for every instance implements it with Sized's default, a conditional `Doubled Extends Sized` gives what a call through Doubled runs, and a call on an instance that has both reaches the one every instance has. That works when the type's declaration lists Sized. When an unconditional Extend lists Sized, a direct call on an instance that meets the Where is "ambiguous". The cause is hide_in_place()'s `hides`: a near member hides a conditional default only when `!near.extended`, and Sized's copy from an Extend is `extended`. The calls through Sized, Doubled and type parameters still give the right answers.

Reproduced by the reviewer:

```
Program:
Import std.core
Interface Sized
    Method Size(): Int32 => 1
End Interface
Interface Doubled Extends Sized
    Method Size(): Int32 => 10
End Interface
Class Box<T>
    Field v: T?
End Class
Extend Box<T> Implements Sized
End Extend
Extend Box<T> Implements Doubled
    Where T: Numeric
End Extend
Local a = Box<Int32>()
Print(a.Size())
This reports "the call to 'Size' is ambiguous", pointing at both Extends. With `Class Box<T> Implements Sized` and no unconditional Extend, the same program prints 1, and D(a) and a Doubled value print 10, as the spec describes.
```

Suggested fix: In hide_in_place(), let a member every instance has (unconditional, extended or not) hide a conditional_default() of the same name and types at the same depth, as it does for the type's own members. Add a test with Sized listed by an Extend.

## A1-11 (medium): A ByRef default that an array type gets stays ByRef

Status: fixed in fdc5342, with tests

Where: compiler/src/sema.cpp:4810

take_default() keeps `by_ref = method.by_ref && !is_class(type)`, so a ByRef default given to an array type through an Extend is a ByRef Method. The spec (06, Extend) says an array's Methods can always change it, so they aren't ByRef, as for a Class, and extension_member_fits() refuses a written ByRef Method on an array for that reason. Calling the default on a temporary array is then refused.

Reproduced by the reviewer:

```
Program:
Import std.core
Interface Grows
    ByRef Method Grow()
        Print("grow")
    End Method
End Interface
Extend Int32[] Implements Grows
End Extend
Function Make(): Int32[]
    Return [1, 2]
End Function
Make().Grow()
`fb run` reports "'Grow' is a ByRef Method, so it needs a variable to change, not a value" at `Make().Grow()`.
```

Suggested fix: Use `method.by_ref && !is_class(type) && !is_array(type)` (and the same for the Self parameter). Add a test.

## A1-12 (medium): body_t and swap_body() don't swap m_locks, so a Const checked early inside a Lock gets a false Await error

Status: fixed in 9fdadca, with tests

Where: compiler/src/sema.cpp:740

initialize_early() swaps in a fresh body_t to check a Const's initializer when something needs it before its turn. body_t holds the scopes, loops, handlers, lambdas and m_unsafe, but not m_locks. If the need comes from code inside a Lock block (in a Global's lambda), the Const's top-level initializer is checked with m_locks > 0. An Await there is then reported as being in a Lock, though there's none. Whether this happens depends only on declaration order.

Reproduced by the reviewer:

```
Program:
Import std.core
Async Function Get(): Int32
    Return 5
End Function
Global m = Mutex<Int32>(0)
Global f = Function(): Int32
    Lock m As v
        Return v + k
    End Lock
End Function
Const k = Await Get()
Print(f())
This reports "Await can't wait in a Lock block, which would hold the Mutex meanwhile" at line 11, `Const k = Await Get()`. With the Const moved above `Global f`, the same program builds and prints 5.
```

Suggested fix: Add `std::size_t locks = 0;` to body_t and swap m_locks in swap_body(). Check what else a body owns but swap_body() leaves out (m_place_use, m_element_use). Add a test with the order above.

## A1-13 (medium): A type parameter can share a declared type's name, against the spec

Status: fixed in 2ea3568, with tests; Extend T[]'s T too, and spec 06 says so

Where: compiler/src/sema.cpp:1343

Spec 07 says a type parameter can't share a name with a type, or with a type parameter it would hide. add_type_parameters()'s comment says the same ("can't share a name with another type or type parameter it could hide"). But it only refuses built-in types and type parameters in scope. `Function F<Foo>(x: Foo)` beside `Struct Foo` is accepted, and Foo means the parameter inside. The same shadowing makes `Extend T[]` (extension_target(), line 2449) mean every array even when the program declares `Struct T`, so T's own array type can't be extended.

Reproduced by the reviewer:

```
Program:
Import std.core
Struct Foo
    Field n: Int32
End Struct
Function F<Foo>(x: Foo): Foo
    Return x
End Function
Struct Box<Foo>
    Field v: Foo
End Struct
Print(F("a"))
Print(Box<String>("b").v)
This builds and prints `a` and `b` with no error. Also: with `Struct T` (Field n) and `Extend T[]` whose Method reads `item.n`, it reports "T has no members without a constraint, like <T: I>".
```

Suggested fix: In add_type_parameters(), also report a name that a type the declaration's code sees has (a top-level TYPE symbol, through top_level() or type_symbols() quietly). Either refuse `Extend T[]` beside a type named T or document the exception. Add tests.

## A2-4 (medium): Main-thread rules miss the ToString and Operator == that strings and element-wise == run implicitly

Status: fixed in 7028f0b, with tests

Where: compiler/src/sema.cpp:9184

Only uses that reach note_use() count for check_threads(). check_expr(string_expr_t) (line 15261) checks that a part can be shown but records nothing about the ToString it will run. Likewise, == on arrays, tuples, and other values that hold a Struct runs that Struct's Operator == without recording a use. A worker can therefore run a type's ToString or Operator == that uses a Global only the main thread may touch. A direct `Eq(1) == Eq(1)` is caught.

Reproduced by the reviewer:

```
exp-all/A2/t20b.fb: `Global counter = 0`. Struct Eq has an `Operator ==` that does `counter += 1`, and Struct Shown Implements Printable has a ToString that does `counter += 1`. Work2 is `Local arr = [Eq(1)]; Return arr == [Eq(2)] ? 1 : 0` and Work3 is `Local s = "{(Shown(1), 2)}"`. Both are Spawned, compile without error, and print '0 0 6 2': counter was changed twice from worker threads. t19.fb shows the same for `"{Shown(1)}"`. In t20.fb the direct `Eq(1) == Eq(1)` in Work is refused ('calls 'Eq.==', which uses the Global 'counter'').
```

Suggested fix: Record the code a value's text and == run as reached from the current code: for each shown part (string interpolation and `Printable...` arguments), the ToString of every Printable type it is made of, and for == on arrays, tuples, Structs, and T?s, the Operator == (and the Hash where one is used) of each element type that has its own. One way is a helper that walks the type the way runs_code() does and pushes those symbols into m_reaches[current_code()].

Skeptic: confirmed=True, high. p5.fb: a worker does `"{Shown(1)}"`, where Shown's ToString does `counter += 1`. It builds and prints '1 1'. p8.fb: `Print(Shown(1))` in a worker builds and runs. p6.fb: a worker does `[Eq(1)] == [Eq(2)]`, where Eq's Operator == does `counter += 1`. It builds and prints '0 1', so counter was changed on the worker. Control p7.fb, the explicit `Shown(1).ToString()`, is refused ('calls 'Shown.ToString', which uses the Global 'counter''), as is the direct `Eq(1) == Eq(1)` in A2/t20.fb. I raised it to high: the main-thread rules accept code that changes a Global from a worker, the same thread-safety hole as A2-3.

## A2-5 (medium): check_struct_cycles() misses a generic Struct that holds an instance of itself, and the compiler crashes

Status: fixed by lane 3 in f4b47ef (a generic struct that holds an instance of itself inline can't contain itself either), with sema tests for Wrap<Int> and Wrap<T[]>

Where: compiler/src/sema.cpp:5874

contains() looks only for the exact type being checked, the generic definition. A generic Struct whose field holds one of its own instances inline, like `Field w: Wrap<Int>` or `Field w: Wrap<T[]>` inside Wrap<T>, is never reported, although its size is infinite. With Wrap<Int>, type_table_t::has_zero() then recurses without end from check_field_zeros() (line 5909), and `fb check` dies of a stack overflow with no output. With Wrap<T[]>, `fb check` reports nothing and `fb run` segfaults in codegen.

Reproduced by the reviewer:

```
exp-all/A2/t34.fb: `Struct Wrap<T> / Field v: T / Field w: Wrap<Int>` plus `Function Use(w: Wrap<String>): String => w.v`. `fb check` exits with 0xC00000FD (stack overflow) and prints nothing. In gdb the stack is type_table_t::has_zero(type=257) recursing through its fields. exp-all/A2/t28.fb: `Struct Wrap<T> / Field v: T / Field w: Wrap<T[]>` gives 'Finished check' with no error, then `fb run` segfaults while compiling.
```

Suggested fix: In contains(), when the target is a generic Struct, treat reaching any instance of its definition inline (definition_of(type) == definition_of(target)), through fields, tuples, fixed arrays, and T?s, as the Struct containing itself. Report it with the existing 'can't contain itself' error and set the field INVALID before anything asks for a size or a zero. Add tests for both shapes.

Skeptic: confirmed=True, medium. t34.fb (`Struct Wrap<T> / Field v: T / Field w: Wrap<Int>`): `fb check` prints nothing and exits 0xC00000FD (stack overflow), as checked from PowerShell. t28.fb (`Field w: Wrap<T[]>`): `fb check` says 'Finished check' with no error, and `fb run` segfaults while compiling (exit 139). contains() (sema.cpp:5876) only matches `type == target`, the generic definition, so a field holding an instance of the definition, Wrap<Int> or Wrap<T[]>, is never seen as the Struct containing itself. Nothing in Deferred work covers it.

## A3-6 (medium): A fixed array's element count overflows before the limit check, so Int[4, 2^62] becomes Int32[4, 0]

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:11538

resolve_fixed_array() computes 'count *= size' in std::uint64_t and only then compares the result with max_count. Each size can be up to Int64's maximum, so the product wraps. The wrapped count passes the check, and static_cast<std::uint32_t>(*size) then truncates the dimension. The declared type silently becomes a different, smaller one instead of the error 'a fixed array can hold at most 2147483647 elements'.

Reproduced by the reviewer:

```
Local a: Int[4, 4611686018427387904] / Print(a.Count) / a[1, 0] = 5
(t15.fb) compiles. --show types prints 'a: Int32[4, 0]', the program prints 0, then panics 'index 0 is out of range for 0 elements'. Int[4294967296] alone is correctly refused.
```

Suggested fix: Check before multiplying: if (*size > max_count || count > max_count / *size) report the limit error. Add a test with two dimensions whose product passes 2^64.

Skeptic: confirmed=True, medium. V3/v6.fb: 'Local a: Int[4, 4611686018427387904]' compiles. --show types prints 'a: Int32[4, 0]', the program prints 0, then 'panic: index 0 is out of range for 0 elements'. In sema.cpp:11538, 'count *= static_cast<std::uint64_t>(*size)' wraps to 0 before the 'count > max_count' check at 11539, and 11544 then truncates the size to uint32.

## A3-7 (medium): T.Has(A) and T.Attribute(A) are refused when the type has an instance member named Has, though spec and comment say only a Static one hides them

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:13731

type_attribute() drops the reflection reading whenever members_of(type, "Has") is non-empty, whatever the members are. Spec 13 says 'T.Has(A) and T.Attribute(A) read the attributes on a type's own declaration ... unless it has a Static member by that name', and the function's own comment says the same. An ordinary instance Method Has therefore turns 'Bag.Has(Tag)' into a Static member access that fails.

Reproduced by the reviewer:

```
Attribute Tag() / @Tag / Class Bag / Method Has(n: Int): Bool => n > 0 / End Class / Print(Bag.Has(Tag))
(t22.fb) gives errors: "'Has' belongs to each Bag, so it's used on one, not on Bag" and "'Tag' is a type, not a value".
```

Suggested fix: Fall back to the member only when one of the found members is Static: !std::ranges::none_of(members, is_static). Add a test with an instance Has.

Skeptic: confirmed=True, medium. V3/v7.fb, a Class with an instance 'Method Has(n: Int)', gives "'Has' belongs to each Bag, so it's used on one, not on Bag" and "'Tag' is a type, not a value" for Bag.Has(Tag), while Box.Has(Tag) checks. type_attribute() at sema.cpp:13730 drops reflection when members_of(...) is non-empty at all. Its own comment (13714) and spec 13:189-190 say only a Static member hides it.

## A3-8 (medium): For Method over a type with an Async Method produces invalid LLVM IR

Status: fixed in 178a50c, lane 2's batch 1 (A6-5 and A5-5), which leaves out Async Methods and ones with a ByRef parameter, with its test tests/programs/reflection_more; lane 1's copy of the fix and its test were folded into it

Where: compiler/src/lower.cpp:3689

reflected_methods() leaves out static, Unsafe, ByRef, generic, throwing, Destroy and indexer Methods, but not Async ones. Sema's check_member_call() (sema.cpp 13881) types m.Call(value) from the Func signature alone. An Async Method that takes and gives nothing matches Func(): None and gets a copy whose call doesn't pass the Task that an Async Method's code takes after Self. LLVM verification then fails. Spec 13's list of what For Method goes over doesn't mention Async Methods, or ByRef ones, which are also skipped.

Reproduced by the reviewer:

```
Class Door / Field name: String = "door" / Async Method Open() / Print("opening {name}") / Await NextFrame() / End Method / Method Close() / Print("closed {name}") / End Method / End Class
Local d = Door() / For Method m In Door / m.Call(d) / End For
(t31.fb) gives 'error: invalid LLVM IR: Incorrect number of arguments passed to called function! %19 = call ptr %18(ptr %14)'.
```

Suggested fix: Skip Async Methods in reflected_methods() as ByRef ones are skipped, and say in spec 13 that ByRef and Async Methods aren't gone over. Alternatively, have m.Call make the Task and give it. Add a program test.

Skeptic: confirmed=True, medium. V3/v8.fb (Door with 'Async Method Open()' and 'Method Close()', then 'For Method m In Door / m.Call(d)') fails with 'error: invalid LLVM IR: Incorrect number of arguments passed to called function! %10 = call ptr %9(ptr %5)'. That is an internal failure, not a diagnostic. Spec 13:165-167 doesn't say Async Methods are left out.

## A4-5 (medium): Compiler assertion crash: Scheduler's Run called without an argument

Status: fixed by lane 3 in b782ba1 (a scheduler's run without its task is one error, not a crash), with a sema test

Where: compiler/src/sema.cpp:18733

check_special_call() reads node.arguments.front() for a Scheduler's Run without checking that there is an argument. bind() has already reported the missing argument, but check_call() goes on to check_special_call() anyway.

Reproduced by the reviewer:

```
`Local s = Scheduler()` then `s.Run()`. `fb check` aborts: `stl_vector.h:1358: ... front() const ... Assertion '!this->empty()' failed.` (exit 3). In a build without assertions, this is undefined behavior.
```

Suggested fix: Guard on `!node.arguments.empty()` (or on info(id).arguments having a value) before reading the Task argument in check_special_call().

Skeptic: confirmed=True, medium. `Local s = Scheduler()` / `s.Run()`: `fb check` aborts with stl_vector.h:1358 front() const ... Assertion '!this->empty()' failed (exit 3).

## A4-6 (medium): Compiler crash: a known integer's Wrapping or Checked Method folded with a mistyped known argument

Status: fixed by lane 3 in a1b154b (an integer method's known argument of another type isn't folded), with sema tests

Where: compiler/src/sema.cpp:16762

fold_integer_call() takes the argument's const value as it is, without checking that it has the integer type the 'other' parameter wants. check_argument has already reported the mismatch, but folding still runs, and fold_integer_method()/fold_binary() call std::get<uint64_t>/<int64_t> on a double or a string.

Reproduced by the reviewer:

```
`Const a: Int = 5` then `Print(a.WrappingAdd(1.5))`. `fb check` prints `error: fb failed unexpectedly: std::get: wrong index for variant`. `a.CheckedAdd("x")` crashes the same way. `a.WrappingAdd(b)` with b an Int64 Const reports `expected Int32, found Int64` and doesn't crash.
```

Suggested fix: In fold_integer_call(), fold only when the argument's value type is the method owner's integer type, for example `other->type == method.owner` after conversion, or skip when the argument has errors.

Skeptic: confirmed=True, medium. `Const a: Int = 5` / `Print(a.WrappingAdd(1.5))` and `Print(a.CheckedAdd("x"))` both give 'error: fb failed unexpectedly: std::get: wrong index for variant' at `fb check`.

## A4-7 (medium): Compiler crash: pointer arithmetic on a known Null Ptr is folded as integer arithmetic

Status: fixed by lane 3 in 6ceb2c2 (pointer arithmetic on a known null ptr isn't folded), with a sema test of all three shapes

Where: compiler/src/sema.cpp:17136

check_operation() returns the Ptr type from check_pointer_arithmetic(). check_expr(binary_expr_t) then folds every binary operation whose two operands have known values. A known Null Ptr (nullptr data) plus a known integer reaches fold_binary(), which goes to fold_unsigned() and calls std::get<uint64_t> on nullptr.

Reproduced by the reviewer:

```
`Const p: Ptr<Int32> = Null` then `Unsafe` / `Local q = p + 1` / `End Unsafe`. Also `Unsafe` / `Local d = (Null As Ptr<Int32>) - (Null As Ptr<Int32>)`. Both give `error: fb failed unexpectedly: std::get: wrong index for variant` at `fb check`.
```

Suggested fix: In check_expr(binary_expr_t), don't fold when either operand's type is a Ptr, or when check_pointer_arithmetic() handled the operation. Pointer values aren't compile-time numbers.

Skeptic: confirmed=True, medium. Three cases crash `fb check` with 'std::get: wrong index for variant': `Const p: Ptr<Int32> = Null` with `Local q = p + 1` in Unsafe, `(Null As Ptr<Int32>) - (Null As Ptr<Int32>)`, and the valid `(Null As Ptr<Int32>) + 1`.

## A4-8 (medium): fb::implements() says Weak<Shared Class> isn't Sendable while sema says it is: crash through a conditional Extend

Status: fixed by lane 3 in 21b60cf (a weak of a shared class is sendable for lowering's interface tables too), with tests/programs/sendable_weak.fb: conformance_query_t::sendable() has a WEAK case, and spec 10 says a Weak<T> of a Shared Class is Sendable

Where: compiler/src/sema.cpp:21829

conformance_query_t::sendable() has no WEAK case, so it falls to the default and answers false. Sema's unsendable() (line 8424) makes a Weak<T> Sendable when T is a Shared Class. A conditional `Where T: Sendable` Extend then type-checks for Bin<Weak<Node>>, but lowering builds no Interface table for it. Spec 10 line 300 also says plainly that Weak<T>s aren't Sendable, so the spec and sema disagree too.

Reproduced by the reviewer:

```
`Shared Class Node` / `ReadOnly Field n: Int = 3`; `Class Bin<T> Extends Thing` with `Method Label(): String => "bin"`; `Extend Bin<T> Implements Shown` / `Where T: Sendable`. `Local thing: Thing = Bin<Weak<Node>>()` followed by `Print("{thing Is Shown}")` prints `False`, while Bin<Int> prints `True`. `Show(Bin<Weak<Node>>())` segfaults (0xC0000005).
```

Suggested fix: Add `case type_kind_t::WEAK: return m_types.get(info.inner).shared;` to conformance_query_t::sendable(), and state the Shared Class exception in docs/spec/10-concurrency.md (Weak references to a Shared Class are atomic). Alternatively, make sema refuse it. Add a test either way.

Skeptic: confirmed=True, medium. a8b.fb with `Extend Bin<T> Implements Shown / Where T: Sendable` prints True for a Bin<Int> and False for a Bin<Weak<Node>> (Node a Shared Class), though sema accepts both. Show(Bin<Weak<Node>>()) segfaults (0xC0000005). conformance_query_t::sendable() has no WEAK case, so a Weak falls to the default and gives false. Sema's unsendable() makes Weak<T> Sendable when T is shared. Spec 10 line 300 says Weak<T>s aren't Sendable, with no Shared Class exception.

## A4-9 (medium): A generic overload and one that isn't generic, both needing a conversion, are called ambiguous

Status: fixed by lane 3 in 7ce596a (a function that isn't generic beats a generic one that fits as well with a conversion too), with sema tests

Where: compiler/src/sema.cpp:19859

Spec 07 says: 'When a generic overload and one that isn't generic fit a call equally well, the one that isn't generic is chosen.' choose_overload() applies that tie-breaker only when several candidates match exactly. When the best fit for both is CONVERTIBLE, it reports the call as ambiguous.

Reproduced by the reviewer:

```
`Function F(x: Int?): String` / `Return "plain"` and `Function F<T>(x: T?): String` / `Return "generic"`; `Local n: Int = 5`; `Print(F(n))`. The result is `error: the call to 'F' is ambiguous`, with both labeled 'could be this'. The spec says 'plain' should be chosen.
```

Suggested fix: Apply the same 'plain beats generic' filter to the convertible tier (and to the unpacked tie-break) before reporting ambiguity. Add a test with T? parameters.

Skeptic: confirmed=True, medium. With F(x: Int?) and F<T>(x: T?), `F(n)` for an Int n gives 'error: the call to 'F' is ambiguous' with both candidates noted. With exact matches, F(x: Int) and F<T>(x: T), the plain one is chosen ('plain'). Spec 07 line 66 says the non-generic one wins when both fit equally well.

## A4-10 (medium): Array and Map literals refuse Null before the first literal, and `[]` beside typed arrays

Status: fixed by lane 3 in 3d06e65 (null, tuples, and empty arrays and maps in a literal take the type the others meet at), with sema tests: common_type_of() takes typed values, then literals, tuples, and empty arrays and Maps with the common type as their hint, then Null

Where: compiler/src/sema.cpp:21414

common_type_of() puts Null in the same second pass as numeric literals, in source order. So a Null that comes before any literal is checked with no hint and fails, while the same values in another order work. It also passes the common type as a hint only to numeric types and to Null, so `[]` (or a tuple holding one) beside an Int[] gets no type, though spec 14 says literals take the elements' common type.

Reproduced by the reviewer:

```
`Local a = [1, Null]` compiles, as Int32?[]. `Local a = [Null, 1]` reports `Null needs a type from where it's used`. `Local b = {"x": Null, "y": 2}` fails the same way. With `Local a: Int[] = [1]`, `Local x = [a, []]` reports `an empty array needs a type from where it's used`.
```

Suggested fix: Use three passes: typed values, then literals, then Null last. For later values, pass `common` as the hint whenever it's known, not only for numeric types, so `[]` and tuple literals take it too.

Skeptic: confirmed=True, medium. `[1, Null]` checks (Int32?[]), but `[Null, 1]` reports 'Null needs a type from where it's used'. `{"y": 2, "x": Null}` checks, but `{"x": Null, "y": 2}` fails the same way. `[a, []]` and `[[], a]` with a: Int[] report 'an empty array needs a type from where it's used'. The Null case depends on order, which is plainly wrong. The `[]` case is weaker, since spec 14 says '[] needs a type from where it's used', but its 'literals taking it' supports the claim.

## A4-11 (medium): A Const Function call that uses a parameter's default doesn't run at compile time where Consts and types are declared

Status: fixed by lane 3 in 7857407 (a const function run where consts and types are declared checks the defaults it uses first), with sema tests: check_defaults() runs once per function, on demand from run_at_compile_time()

Where: compiler/src/sema.cpp:18407

run_at_compile_time() uses symbol(parameter).value for a parameter the call leaves out. That value is set only by check_defaults(), which runs after top-level Consts and declaration types are checked. While they're checked, the default is missing, so the run is skipped without a word. A top-level Const then silently becomes a run-time Const, and a type size using it is refused.

Reproduced by the reviewer:

```
`Const Function Add(a: Int, b: Int = 10): Int` / `Return a + b`; then `Const A = Add(1)`. `fb check --show mir` shows `global A: Int32 = 0` and `_0 = call Add(Int32, Int32)(1, 10)` in main, not a folded 11. Adding `Local arr: Int[A]` reports `a fixed array's size must be an integer known at compile time`. A Struct `Field cells: Int[Add(1)]` reports the same. `Const A = Add(1, 10)` works.
```

Suggested fix: Check a function's parameter defaults on demand when an early run needs them, as check_const_body() is done on demand, or run check_defaults() for Const Functions before Globals and Consts are initialized.

Skeptic: confirmed=True, medium. `Const A = Add(1)`, with b defaulting to 10, gives `global A: Int32 = 0` and `_0 = call Add(Int32, Int32)(1, 10)` in main. `Local arr: Int[A]` then reports 'a fixed array's size must be an integer known at compile time'. `Const A = Add(1, 10)` with Int[A] works and prints 11. Int[Add(1)] inside a Function body works too.

## A4-12 (medium): An Enum declared later has 'no member' while declaration types and early Const runs are checked

Status: fixed by lane 3 in 9f1575c (an enum declared later has its values where declarations' types use them), with sema tests: check_enum_value() declares the Enum early while types are declared

Where: compiler/src/sema.cpp:20410

check_enum_value() reads the Enum's values from the type table without declare_early(type). SizeOf's declare_held() does declare Enums early. Before the Enum's own declaration is processed, its values are empty. So an Enum value used in a Field's size, or in a Const Function that a size runs, gets a wrong error. Spec 13 limits the 'can't run here' case to Structs and Classes whose members aren't declared yet.

Reproduced by the reviewer:

```
`Struct Grid` / `Field cells: Int[Extra.BONUS As Int]` / `End Struct` with `Enum Extra` / `BONUS = 2` declared below it. The result is `error: Extra has no member named 'BONUS'`, and `Field more: Int[SIZE]` with `Const SIZE = (Extra.BONUS As Int) * 3` reports that the size must be known at compile time. A `Const Function Size(n: Int): Int` returning `n + (Extra.BONUS As Int)`, used as `Int[Size(4)]`, reports `this Const Function can't run here, since its code needs types that aren't declared yet`. Moving the Enum above makes all of them work.
```

Suggested fix: In check_enum_value() (and enum_named() users), call declare_early(type) when `!m_declared`, as declare_held() does for SizeOf.

Skeptic: confirmed=True, medium. With the Enum declared below the Struct, `Field cells: Int[Extra.BONUS As Int]` gives 'Extra has no member named 'BONUS''. `Int[SIZE]` with `Const SIZE = (Extra.BONUS As Int) * 3` gives 'must be an integer known at compile time'. Int[Size(4)] gives 'this Const Function can't run here, since its code needs types that aren't declared yet'. With the Enum moved above, it works and prints 2. Spec 13 limits that error to code that reaches a Struct or Class whose members aren't declared yet, not an Enum.

## A4-13 (medium): A generic Async Function infers its type argument from the expected Task<T> as if it returned the Task

Status: fixed by lane 3 in 687c7ed (a generic async function takes its type arguments from the task expected's element), with sema tests: told_arguments() unifies an Async Function's Task of its result with the type expected

Where: compiler/src/sema.cpp:18942

told_arguments() unifies the callee's declared result type with the hint. For an Async Function, the call gives Task<result>, so T is unified with the whole Task<Int64>. The call then gives Task<Task<Int64>>, and the bounds check fails on Task<Int64>.

Reproduced by the reviewer:

```
`Async Function Zero<T: Numeric>(): T` / `Await NextFrame()` / `Return T.Zero()`; `Local t: Task<Int64> = Zero()`. Errors: `Task<Int64> doesn't implement Numeric, which 'Zero' needs for T` and `expected Task<Int64>, found Task<Task<Int64>>`. The non-Async `Function Zero<T: Numeric>(): T` with `Local t: Int64 = Zero()` compiles and prints 0.
```

Suggested fix: In told_arguments(), when the function is Async and the hint is a TASK, unify the result with element_of(hint) instead of the hint itself.

Skeptic: confirmed=True, medium. `Async Function Zero<T: Numeric>(): T` with `Local t: Task<Int64> = Zero()` gives 'Task<Int64> doesn't implement Numeric, which 'Zero' needs for T' and 'expected Task<Int64>, found Task<Task<Int64>>'. With Zero<Int64>() it compiles and prints 0.

## A4-14 (medium): A fixed array sized by Const type arguments can exceed the element limit; the program stack-overflows

Status: fixed by lane 3 in 1f022d7 (a fixed array an instance works out holds no more elements than a written one, and code that isn't generic checks its instances' sizes too), with MIR tests: lowering's check_values() checks every type lowering uses, and an instance's Fields and base, and reports a fixed array whose sizes make more than 2147483647 elements

Where: compiler/src/types.cpp:717

A written fixed array is limited to 2147483647 elements in all ('a fixed array can hold at most 2147483647 elements'). substitute() and the instance checks only bound each dimension's value to 1..max_count. An instance like G<65536, 65536> gets an Int8[65536, 65536] of 2^32 elements, which nothing reports. check_count()'s make_number(element_count, INT32) then gives no value as well.

Reproduced by the reviewer:

```
`Struct G<Const W: Int, Const H: Int>` / `Field c: Int8[W, H]`; `Function Cells<Const W: Int, Const H: Int>(g: G<W, H>): Int` / `Return g.c.Count`; `Local g = G<65536, 65536>()`; `Print(Cells(g))`. `fb check` passes. `fb run` warns 'stack frame size (12884901960) exceeds limit' and the exe exits with 0xC00000FD.
```

Suggested fix: In substitute()'s FIXED_ARRAY case, keep the dimensions unresolved when the product of the sizes passes max_count (as for one dimension), so lowering or sema reports it. Or check the total element count where instances' fields are checked, with the same message as for a written array.

Skeptic: confirmed=True, medium. `Struct G<Const W: Int, Const H: Int>` / `Field c: Int8[W, H]` with `G<65536, 65536>()` passes `fb check`. `fb run` warns 'stack frame size (12884901960) exceeds limit' and the exe exits with 0xC00000FD. A written `Local c: Int8[65536, 65536]` is refused: 'a fixed array can hold at most 2147483647 elements'.

## A4-15 (medium): Spawn and Thread.Start ignore type arguments written on them

Status: fixed by lane 3 in b6aea2d (spawn and thread.start take no type arguments), with sema tests, and spec 10 says so

Where: compiler/src/sema.cpp:17914

check_spawn() never looks at the name's or member's type_arguments. Every other built-in maker refuses wrong ones: Scheduler, Channel, Atomic and Mutex (more than one), AddressOf, and SizeOf/TypeName (a count).

Reproduced by the reviewer:

```
`Local t = Spawn<String>(Function() => 5)` / `Print(t.Wait())` compiles and prints 5, as a Task<Int32>. `Thread.Start<Int, Bool>(Function() => Print("hi"))` compiles and runs too.
```

Suggested fix: In check_spawn(), report written type arguments, e.g. 'Spawn takes no type arguments; a Task<T> expected says what the work gives'. Check both name_expr_t::type_arguments and member_expr_t::type_arguments.

Skeptic: confirmed=True, medium. `Spawn<String>(Function() => 5)` compiles, and Wait() prints 5. `Thread.Start<Int, Bool>(Function() => Print("hi")).Join()` compiles and prints hi. `Scheduler<Int>()` is refused ('a Scheduler is made without arguments').

## A4-16 (medium): `Async Function() => Await x` is refused when a Func(): Task is expected, but accepted without one

Status: fixed by lane 3 in 878d7a6 (an async lambda that returns nothing can await after its =>), with sema tests: is_call() takes an Await as a call

Where: compiler/src/sema.cpp:20929

With a Func(): Task hint, the Async lambda's result is NONE, and check_lambda_value() then requires the `=>` value to be a call. An await_expr_t isn't one, so it's refused. Without a hint, the same lambda takes the Await's NONE type and compiles.

Reproduced by the reviewer:

```
`Local g = Async Function() => Await NextFrame()` compiles. `Local f: Func(): Task = Async Function() => Await NextFrame()` reports `this lambda returns nothing, so its '=>' needs a call, like '=> Print(x)'`.
```

Suggested fix: Let is_call() (or check_lambda_value()) accept an await_expr_t, at least in an Async lambda, as it accepts a call.

Skeptic: confirmed=True, medium. `Local g = Async Function() => Await NextFrame()` compiles and runs. `Local f: Func(): Task = Async Function() => Await NextFrame()` reports 'this lambda returns nothing, so its '=>' needs a call, like '=> Print(x)''. With a Func(): Task<Int> hint, `=> Await Spawn(...)` works, which shows only the NONE case is broken.

## A1-14 (low): Static and ReadOnly before a Struct or Class are accepted silently

Status: fixed in 57f5d25, with tests

Where: compiler/src/sema.cpp:1194

declare_type() checks Abstract, Final, Shared, Override, @Inline, @MainThread, @PanicsAtCaller and generic @CLayout, but not is_static or is_read_only. declare_interface() refuses both on an Interface, and the parser refuses them on an Enum, an Extend and an Attribute. But `Static Class C`, `Static Struct C`, `ReadOnly Class C` and `ReadOnly Struct C` compile with no error, and the modifier means nothing.

Reproduced by the reviewer:

```
For each of `Static Class C`, `Static Struct C`, `ReadOnly Class C` and `ReadOnly Struct C`, a file with `Import std.core`, the declaration with one `Field x: Int32`, its End line and `Print(1)` passes `fb check` with no diagnostics. `Static Interface C` gets "an Interface doesn't take modifiers like Abstract or Final", and `Static Enum C` gets "'Enum' doesn't take modifiers like Static or Final".
```

Suggested fix: In declare_type(), report modifiers.is_static ("Static only goes before a Field or Method of a Struct or Class") and modifiers.is_read_only ("ReadOnly only goes before a Field"). Add tests.

## A1-15 (low): Extend of Task, Channel, Scheduler or Thread says "'Task' isn't a type"

Status: fixed in 274a48f, with tests

Where: compiler/src/sema.cpp:2484

extension_target() has a dedicated message for Atomic, Mutex and Ptr ("can't be extended; it has only its own operations"). Every other built-in kind that isn't a named symbol falls through to "'X' isn't a type", though `Local t: Task<Int32>`, `Channel<Int32>` and `Scheduler` are types. Weak<C> gets the generic "Extend adds to a Struct, ..." message.

Reproduced by the reviewer:

```
For each of `Extend Task<Int32>`, `Extend Channel<Int32>`, `Extend Scheduler` and `Extend Thread`, an Extend holding `Method Q(): Int32 => 1` reports "'Task' isn't a type" (or 'Channel', 'Scheduler', 'Thread'). Meanwhile `Local t: Task<Int32> = Spawn(Function() => 5)`, `Local c: Channel<Int32> = Channel<Int32>(2)` and `Local s: Scheduler = Scheduler()` build and run.
```

Suggested fix: Extend the Atomic/Mutex/Ptr branch to cover Task, Thread, Channel, Scheduler and Weak, with the same "can't be extended; it has only its own operations" wording.

## A1-16 (low): ReadOnly on a Const says "a Local that never changes is a Const"

Status: fixed in 57f5d25, with tests

Where: compiler/src/sema.cpp:3190

check_variable_modifiers() builds the ReadOnly message with `node.kind == GLOBAL ? "Global" : "Local"`, so a Const, at the top level or in a block, is called a Local.

Reproduced by the reviewer:

```
`ReadOnly Const X = 1` at the top level reports "error: a Local that never changes is a Const". `ReadOnly Const Y = 2` inside a Function reports the same.
```

Suggested fix: For a CONSTANT, say it's a Const already, for example "a Const never changes, so it takes no ReadOnly".

## A1-17 (low): extension_member_fits()'s comment lists rules the code no longer has

Status: fixed in 4d3c67a (a comment)

Where: compiler/src/sema.cpp:2670

The comment says a Static member goes "only to a type that isn't generic, and a Property not to T[] yet". The code only refuses Statics on arrays, and both `Static Method` in `Extend Box<T>` and `Property` in `Extend T[]` are accepted and work, as CLAUDE.md's M6 notes say (an Extend's Property on arrays can be set).

Reproduced by the reviewer:

```
`Extend Box<T>` with `Static Method Make(v: T): Box<T> => Box<T>(v)`, and `Extend T[]` with `Property Second: T` and `Get => Self[1]`, build. `Box<Int32>.Make(3).v` prints 3, and `[1, 2, 3].Second` prints 2.
```

Suggested fix: Reword the comment to what the function checks: an Operator only to a Struct or Class, a ByRef Method only to a value, no Statics on an array, and no Destroy.

## A1-18 (low): Misplaced and stale comments on members and in sema.hpp

Status: fixed in 4d3c67a (comments)

Where: compiler/src/sema.cpp:716

(1) Line 716 is a stray line, "Each Const Function's MIR, lowered the first time a call runs it.", sitting above m_member_types's own comment. m_const_modules (line 731), which it describes, has none of its own. (2) m_declared_types (line 440) is documented as "Each Struct's symbol, by its type", but it holds Structs, Classes, Interfaces and Enums. (3) sema.hpp's symbol_t::members (lines 130-132) says "fields in order, then its Methods and constructors". It also holds Properties, Static Fields, Interface members, what Extends of all instances add, and Interface default copies (take_default() pushes them there). conformance_t::type says "The Struct or Class", but built-in types, Enums, arrays and instances have conformances too.

Reproduced by the reviewer:

```
Read sema.cpp:716-718, sema.cpp:440-441, sema.hpp:130-132 and sema.hpp:179-180 against declare_type()/declare_interface()/declare_enum() (which all fill m_declared_types), declare_members()/declare_extension()/take_default() (which fill members), and link_extensions() (which conforms built-in types).
```

Suggested fix: Move the line to m_const_modules, and correct the m_declared_types, symbol_t::members and conformance_t::type comments.

## A2-6 (low): parameters_in() skips Channel, Task, Ptr, Callback, Atomic and Mutex, so check_expansion() misses polymorphic recursion through them

Status: fixed by lane 3 in ad7a933 (a type parameter inside a channel, task, ptr, atomic, mutex, or callback counts toward instances without end), with a sema test

Where: compiler/src/sema.cpp:9377

parameters_in() looks inside NULLABLE, ARRAY, FIXED_ARRAY, WEAK, FUNCTION, tuples, and instance arguments only. A generic function that calls itself with Channel<T> (or Task<T>, Ptr<T>, and so on) gets no growing edge, so check_expansion(), which says it 'Reports each', stays quiet. Only lowering's depth backstop catches it, with a different, less helpful message that doesn't say which type parameter grows.

Reproduced by the reviewer:

```
exp-all/A2/t24.fb: `Function Deep<T: Sendable>(x: T, n: Int): Int` whose recursive step is `Return 1 + Deep(Channel<T>(1) stored in ch, n - 1)` gives only lowering's "'Deep' needs a larger instance each time it runs here, without end". The T[] version (t25.fb) gets sema's "'Deep' gets T[] for T here, which leads back to this code...".
```

Suggested fix: Add TASK, CHANNEL, PTR, ATOMIC, and MUTEX to the `holds` kinds (inner), and CALLBACK with its parameters and inner, matching mentions() and declared_parts().

Skeptic: confirmed=True, low. t24.fb (Deep<T: Sendable> recursing with Channel<T>) gets only lowering's backstop: "'Deep' needs a larger instance each time it runs here, without end". t25.fb (the T[] version) gets sema's "'Deep' gets T[] for T here, which leads back to this code...". parameters_in() (sema.cpp:9359-9402) descends `inner` only for NULLABLE, ARRAY, FIXED_ARRAY, WEAK, and FUNCTION. CHANNEL, ATOMIC, MUTEX, and PTR are built with make(kind, inner, ...) in types.cpp, so their element is never visited. The program is still refused; only the diagnostic is worse.

## A2-7 (low): comparable()'s comment says == doesn't work on arrays, but the code and the spec say it does

Status: fixed in e2d38b0 (a comment)

Where: compiler/src/sema.cpp:10310

The doc comment reads "Whether == works on a type: not on arrays, or a Struct, tuple, or T? holding one." The body returns comparable(element_of(type)) for arrays ('An array compares its elements'), and spec 03-operators.md and 14-strings-collections.md say == on arrays compares their elements.

Reproduced by the reviewer:

```
Plain reading: line 10310 against lines 10317-10320 and docs/spec/03-operators.md:33.
```

Suggested fix: Reword it, for example: "Whether == works on a type: not on Interfaces, Weak<T>s, Funcs, or CStrings, or a Struct, tuple, array, or T? holding one."

Skeptic: confirmed=True, low. sema.cpp:10310 says "Whether == works on a type: not on arrays, or a Struct, tuple, or T? holding one." Lines 10317-10320 return comparable(element_of(type)) for arrays ('An array compares its elements'), and spec 03-operators.md says "`==` on arrays compares their elements." The comment also leaves out what the code really refuses: Weak, Func, Interface, and CString.

## A2-8 (low): std_name()'s doc comment is fused onto std_constraint(), and std_name() has none

Status: fixed by lane 3 in 21b5634 (std_name's doc comment is above std_name, not std_constraint)

Where: compiler/src/sema.cpp:8054

Lines 8054-8056 describe std_name() ("`declared`, a type std declares that isn't generic, as an error at `span` that also shows `beside` names it..."). They sit directly above std_constraint()'s own comment (8057-8059), which makes one confusing two-paragraph comment, while std_name() at line 8102 is left without one.

Reproduced by the reviewer:

```
Plain reading of lines 8054-8105.
```

Suggested fix: Move lines 8054-8056 to just above std_name() at line 8102.

Skeptic: confirmed=True, low. sema.cpp:8054-8056 describe std_name() ("`declared`, a type std declares that isn't generic, as an error at `span` that also shows `beside` names it..."). They sit directly above std_constraint()'s own three-line comment (8057-8059), and std_name() at 8102 has no comment.

## A2-9 (low): note_use()'s doc comment sits above current_code()

Status: fixed by lane 3 in bbb7878 (note_use's doc comment is above note_use, not current_code)

Where: compiler/src/sema.cpp:9154

"A warning at a use of something @Deprecated, or of a New of a @Deprecated type. Its own code, and its type's, can use it." is stacked on top of current_code()'s comment. note_use() at line 9689 has no comment.

Reproduced by the reviewer:

```
Plain reading of lines 9154-9160 and 9689.
```

Suggested fix: Move the two @Deprecated lines above note_use(). That doc could also say it records the use for the main-thread rules (note_thread_use()).

Skeptic: confirmed=True, low. sema.cpp:9154-9155 ("A warning at a use of something @Deprecated, or of a New of a @Deprecated type...") is stacked on current_code()'s comment at 9156-9157. note_use() at line 9689 has no doc comment.

## A2-10 (low): declare_global_item() has two stacked doc comments, and the first is stale

Status: fixed by lane 3 in 834fd2c (declare_global_item has one doc comment, which says it declares const functions too)

Where: compiler/src/sema.cpp:6876

Lines 6876-6877 ("A Global or top-level Const. Without an annotation, its type comes from the initializer later, in initialize_globals().") are followed by a second comment (6878-6879) saying the type comes next from resolve_global_type(). The function also declares Const Functions early, which neither comment mentions.

Reproduced by the reviewer:

```
Plain reading of lines 6876-6888.
```

Suggested fix: Keep one comment, for example: "A top-level Global or Const, or a Const Function, before anything else, so that types can use it. A variable's written type comes next, from resolve_global_type(), and an initializer gives the rest, in initialize_globals()."

Skeptic: confirmed=True, low. sema.cpp:6876-6879 has two stacked doc comments on declare_global_item(): "A Global or top-level Const. Without an annotation, its type comes from the initializer later, in initialize_globals()." and "A top-level Global or Const, before anything else... Its type comes next, from resolve_global_type()." Neither mentions that the function also declares Const Functions, which its first branch (6881-6887) does.

## A2-11 (low): check_async_function()'s doc comment is broken across lines

Status: fixed by lane 3 in 9221565 (check_async_function's doc comment is wrapped as one sentence)

Where: compiler/src/sema.cpp:6751

The comment is wrapped wrongly: "/// An Async Function or Method runs as a coroutine on the main thread, which goes on after the" / "/// call" / "/// that starts it returns, so its parameters can't be ByRef." A lone "call" line is left behind from a bad rewrap.

Reproduced by the reviewer:

```
Plain reading of lines 6751-6753.
```

Suggested fix: Rewrap to two lines: "An Async Function or Method runs as a coroutine on the main thread, which goes on after the call that starts it returns, so its parameters can't be ByRef."

Skeptic: confirmed=True, low. sema.cpp:6751-6753 reads "/// An Async Function or Method runs as a coroutine on the main thread, which goes on after the" / "/// call" / "/// that starts it returns, so its parameters can't be ByRef." A lone "call" line is left from a bad rewrap.

## A3-9 (low): For ... In over a nullable Channel reports the wrong error

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:13445

The nullable branch of the For ... In check only handles arrays and iterables (is_any_array or iteration_of of the non-null type), not Channels. A Channel<T>? therefore gets the generic message, which lists only arrays, Iterators, and Iterate() and calls the Channel? un-iterable. It should get report_maybe_null(), as an Int[]? does.

Reproduced by the reviewer:

```
Function Go(ch: Channel<Int>?, a: Int[]?) / For x In ch / End For / For y In a / End For / End Function
(t19.fb) reports "'For ... In' goes over arrays, Iterators, and values whose Iterate() gives an Iterator, not Channel<Int32>?" for ch, but "'a' may be Null; check that it isn't, or use '?.'" for a.
```

Suggested fix: Add is_channel(non_null(sequence)) to the nullable condition, and mention Channels in the generic message.

Skeptic: confirmed=True, low. V3/v9.fb: 'For x In ch' with ch: Channel<Int>? reports "'For ... In' goes over arrays, Iterators, and values whose Iterate() gives an Iterator, not Channel<Int32>?", while the Int[]? case reports "'a' may be Null...". At sema.cpp:13443-13447 the non-null branch accepts is_channel, but the nullable branch tests only is_any_array or iteration_of.

## A3-10 (low): A Return in a Try whose Finally assigns a required Field is wrongly reported in New

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:14284

check_stmt(return_stmt_t) calls check_required(span, "before this Return") against the flow at the Return. A Return inside a Try body runs the Finally before New actually returns, and spec 09 says what the Finally assigns is assigned. So New is refused although the Field is always assigned when it returns.

Reproduced by the reviewer:

```
Class A / Field m: Node / New() / Try / Try Risky() / Return / Catch e As E1 / Print("x") / Finally / m = Node() / End Try / End New / End Class
(t23.fb) gives "New must assign 'm' before this Return, since Node has no zero value".
```

Suggested fix: While a Finally encloses the Return in New, defer that check to the Finally's end: check the Fields the Finally assigns as assigned, for example by recording pending Returns per Try and checking them in after_finally().

Skeptic: confirmed=True, low. V3/v10.fb, a Return inside a Try body whose Finally does 'm = Node()', gives "New must assign 'm' before this Return, since Node has no zero value", although spec 09:65 says Finally runs through Return, so m is always assigned when New returns. The same happens with 'Defer m = Node()' before a Return. The rule is too strict, so safe code is refused.

## A3-11 (low): For f = 0 To 1.5 Step 0.5 is refused with '1.5 doesn't fit in Int32'

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:13382

The counting For loop takes the end's type only when the start is a literal and the end isn't (is_flexible(start) && !is_flexible(end)). When both are literals, the variable is Int, so a decimal end or Step is reported as not fitting Int32. Spec 04 says 'A literal start takes the end's type instead' without that condition, and spec 02 types a decimal literal as Float. A For loop counts Floats, so this natural loop is refused with a confusing message.

Reproduced by the reviewer:

```
For f = 0 To 1.5 Step 0.5 / Print(f) / End For
(t26.fb) gives errors: '1.5 doesn't fit in Int32' and '0.5 doesn't fit in Int32'.
```

Suggested fix: When start, end, and step are all literals, type them together, using default_type() over all three, so any decimal makes the loop a Float. Or state in spec 04 that a literal start keeps Int beside a literal end.

Skeptic: confirmed=True, low. V3/v11.fb, 'For f = 0 To 1.5 Step 0.5', gives '1.5 doesn't fit in Int32' and '0.5 doesn't fit in Int32'. With '0.0' as the start, or a non-literal end of 1.5, it counts 0, 0.5, 1, 1.5. The code at sema.cpp:13380-13382 deliberately requires '!is_flexible(end)', while spec 04:46 says only 'A literal start takes the end's type instead', with no such condition.

## A3-12 (low): A throwing call in a Defer or Finally inside a Try block says it 'needs Try, Try?, or a Try block'

Status: fixed in 31a832a and 5c29316, with tests

Where: compiler/src/sema.cpp:13040

untried() treats a SEALED handler (a Defer, Finally, lambda, or Parallel For body) like the function's own. A call that throws inside a Defer or Finally that is itself inside a Try block is told it needs a Try block, though one encloses it. Adding 'Try' then gives a second error, that a Defer/Finally can't throw. check_iterate_call() (13066) has the same wording problem. The useful message is that a Defer or Finally can't throw, so the call needs Try?.

Reproduced by the reviewer:

```
Function G() Throws E1 / Try / Defer F() / Try F() / Catch e As E1 / End Try / Try / Try F() / Finally / F() / End Try / End Function
(t34.fb) gives "'F' can throw E1, so calling it needs Try, Try?, or a Try block" at both 'Defer F()' and the 'F()' in the Finally.
```

Suggested fix: When m_handlers.back() is SEALED, report "{handler.what} can't throw, so calling 'F' needs Try?, or a Try block inside it" instead. Do the same in check_iterate_call().

Skeptic: confirmed=True, low. V3/v12.fb gives "'F' can throw E1, so calling it needs Try, Try?, or a Try block" at 'Defer F()' inside a Try block and at 'F()' in a Finally. Following the advice with 'Try F()' (v12b.fb) then gives 'a Defer can't throw, so E1 has to be caught in it' and 'a Finally can't throw...'. untried() (sema.cpp:13025) treats SEALED like FUNCTION, and check_iterate_call() (13066) does the same. The same misleading advice also appears outside any Try block (H in v12b.fb).

## A4-17 (low): Spawn ignores a nullable Task<T> hint

Status: fixed by lane 3 in 29ee47f (spawn takes what its work gives from a nullable task expected too), with a sema test

Where: compiler/src/sema.cpp:17928

check_spawn() tests `m_analysis.types.get(hint).kind == TASK` without non_null(hint). The lambda, tuple, array, and Map checks all take non_null(hint) first.

Reproduced by the reviewer:

```
`Local a: Task<Int64> = Spawn(Function() => 5)` works. `Local t: Task<Int64>? = Spawn(Function() => 5)` reports `expected Task<Int64>?, found Task<Int32>`.
```

Suggested fix: Take `const type_id_t task = non_null(hint)` and use it for the TASK test and for element_of().

Skeptic: confirmed=True, low. `Local a: Task<Int64> = Spawn(Function() => 5)` works. `Local t: Task<Int64>? = Spawn(Function() => 5)` reports 'expected Task<Int64>?, found Task<Int32>'. A nullable Func hint for a plain lambda, `(Func(): Int64)? = Function() => 5`, is accepted.

## A4-18 (low): A known UInt64 index past Int64's range isn't checked at compile time, and the panic shows it as negative

Status: fixed by lane 3 in 6b2ef09 (a fixed array's known uint64 index or bound past int64's range is checked at compile time) and 35311df (an index's panic shows a uint64 past int64's range as it is), with sema tests and tests/programs/panic_index_unsigned.fb and panic_fixed_index_unsigned.fb: the runtime has fb_unsigned_index_out_of_range()

Where: compiler/src/sema.cpp:21357

check_index() (and check_bound()) use integer_value(), which gives nullopt for a UInt64 above INT64_MAX. A fixed array's known index then goes unchecked, though spec 14 says such an index 'known at compile time is checked then'. At run time, the panic message prints the index as signed.

Reproduced by the reviewer:

```
`Local a: Int[3]` then `Print(a[18446744073709551615 As UInt64])`. It compiles, and running it panics with `index -1 is out of range for 3 elements`.
```

Suggested fix: In check_index() and check_bound(), when the value is a uint64_t above INT64_MAX, report it as out of range. In lowering/runtime, print an unsigned index as unsigned.

Skeptic: confirmed=True, low. `Local a: Int[3]` / `Print(a[18446744073709551615 As UInt64])` compiles, though `a[5]` is refused at compile time ('index 5 is out of range for Int32[3]'). Running it panics with 'index -1 is out of range for 3 elements'. Spec 14 line 91 says a fixed array's known index is checked at compile time.

## A4-19 (low): An overload error shows '<invalid>' for a literal too big for Int

Status: fixed by lane 3 in 73e9e79 (an overload error shows a literal too big for its type, or a generic function's name, as written), with sema tests

Where: compiler/src/sema.cpp:19940

choose_overload() keeps INVALID in `types` for a literal that doesn't fit its default type, which is allowed so that it may still fit a parameter. When nothing fits, the 'no overload' message names the type with name_among(INVALID), which prints the internal '<invalid>'.

Reproduced by the reviewer:

```
`Function F(a: String)` and `Function F(a: Bool)`, then `Print(F(5_000_000_000))`. The error is `no overload of 'F' takes (<invalid>)`.
```

Suggested fix: For an INVALID flexible argument, show its text (text(argument.value)) or say it doesn't fit in Int32, as the Null case shows 'Null'.

Skeptic: confirmed=True, low. With F(a: String) and F(a: Bool), `F(5_000_000_000)` reports 'no overload of 'F' takes (<invalid>)'.

## A4-20 (low): Calling a Property says it's a field

Status: fixed by lane 3 in 103080f (calling a property says it's a property, and calling a static field says it's a field), with sema tests; a Static Field called said "only functions can be called" before

Where: compiler/src/sema.cpp:19453

report_not_callable() handles FIELD and PROPERTY together with the message "'{}' is a field, not a method".

Reproduced by the reviewer:

```
`Class Box` with `Property Size: Int` / `Get` / `Return 3`; then `Print(Box().Size())`. The error is `'Size' is a field, not a method`.
```

Suggested fix: Say "is a Property, not a method" for a PROPERTY symbol.

Skeptic: confirmed=True, low. `Class Box` with a `Property Size: Int` Get: `Box().Size()` reports ''Size' is a field, not a method'.

## A4-21 (low): A name repeated in a Weak capture list isn't reported

Status: fixed by lane 3 in 660e719 (a name repeated in a capture list is an error), with sema tests

Where: compiler/src/sema.cpp:20882

check_lambda_body() pushes each weak capture's symbol straight into the scope, so `[Weak a, Weak a]` makes two captures and two symbols of one name with no error. A parameter with the name is reported against the first, which shows the duplicate check is skipped only here.

Reproduced by the reviewer:

```
`Local a = Node()` then `Local f = Function[Weak a, Weak a]() => a?.n ?? -1`. It compiles quietly and makes two Weak fields in the closure.
```

Suggested fix: In weak_captures(), report a name already in the list ('a is already in this capture list'), or add the symbols through enter_scope() so the usual 'already declared' check runs.

Skeptic: confirmed=True, low. `Function[Weak a, Weak a]() => a?.n ?? -1` compiles without a word and prints 4. The MIR closure class has two Weak<Node> captures (_3, _4) and two parameters named a.

## A4-22 (low): An Async lambda's errors underline 'Async Fu'

Status: fixed by lane 3 in fb0e8a8 (an async lambda's errors point at its function keyword), with a sema test: lambda_expr_t keeps its Function keyword's span

Where: compiler/src/sema.cpp:20751

The lambda's symbol span is span_of(id).begin plus 8, meant to cover 'Function'. For an Async lambda, the expression starts at 'Async', so the caret covers 'Async Fu'. The comment says 'The keyword is where errors about the whole lambda point.'

Reproduced by the reviewer:

```
`Local f = Async Function(): Int` / `If c Then Return 1` / `End Function` gives `this lambda doesn't return a value on every path` with `^^^^^^^^` under 'Async Fu'.
```

Suggested fix: Start the span after 'Async ' for an Async lambda (or cover 'Async Function', 14 characters).

Skeptic: confirmed=True, low. An `Async Function(): Int` lambda missing a return gives 'this lambda doesn't return a value on every path' with ^^^^^^^^ (8 carets) under 'Async Fu' at column 11.

## A4-23 (low): Doc comments that describe the wrong function or are stacked twice

Status: fixed by lane 3 in 428a01b (sema's and types.hpp's misplaced doc comments are above what they describe, and entry_t's lists every kind it keeps), lookup_callee() took the stray callee comment

Where: compiler/src/sema.cpp:16837

Several doc comments are misplaced. At 16837-16840, lookup_member()'s description ('The members named after a '.' on `object`...') sits above hides_qualifier(), and lookup_member() at 16865 has none. At 18852-18854, a stray comment ('What a callee names: ...') sits above infer()'s own. At 19702-19705, usable_constructors() has two overlapping doc comments. At 21005-21006, the index_expr_t comment ('"a[i]", or "grid[x, y]"...') is fused into the Await check's comment, and check_expr(index_expr_t) at 21051 has none. In types.hpp line 149, '/// One entry in a type table.' sits above value_op_t, not type_t. At types.hpp line 387, entry_t's comment lists five cached kinds, but it holds eight (task, channel, and pointer too).

Reproduced by the reviewer:

```
Reading compiler/src/sema.cpp lines 16837-16865, 18852-18859, 19702-19708, and 21005-21051, and compiler/include/fb/types.hpp lines 149-158 and 385-397.
```

Suggested fix: Move each comment to the function or type it describes, merge the duplicate, and list all the cached kinds in entry_t's comment.

Skeptic: confirmed=True, low. Every misplaced comment is as described. sema.cpp 16837-16840: lookup_member's comment is stacked on hides_qualifier's, and lookup_member at ~16865 has none. 18852-18854: 'What a callee names' sits above infer()'s own comment. 19702-19705: usable_constructors() has two doc comments. 21005-21009: the index_expr_t comment is fused into the Await one, and check_expr(index_expr_t) at ~21051 has none. types.hpp 149: '/// One entry in a type table.' sits above value_op_t's comment. 387: entry_t's comment lists 5 kinds, but it also caches task, channel, and pointer.

## A4-24 (low): The spec says AddressOf takes a parameter; the compiler takes only a ByRef one

Status: fixed by lane 3 in c164de1 (spec 08: addressof takes a byref parameter, as the compiler does), and a sema test of AddressOf on a parameter and a ByRef one

Where: docs/spec/08-memory.md:111

Spec 08 says '`AddressOf(x)` takes a Local, parameter, Global, field, ...', and its list of exclusions doesn't name read-only parameters. check_address_of() refuses an ordinary parameter through check_changeable()/check_assignable(), as its comment says, and accepts a ByRef one.

Reproduced by the reviewer:

```
`Unsafe Function Peek(x: Int, ByRef y: Int): Int` / `Local p = AddressOf(x)` reports `'x' is a parameter and can't be changed; copy it into a Local, or make it ByRef`. `AddressOf(y)` works.
```

Suggested fix: Change the spec to 'a ByRef parameter', or add read-only parameters to the 'not ...' list after 'it takes only what code could change there'.

Skeptic: confirmed=True, low. `AddressOf(x)` for an ordinary parameter reports ''x' is a parameter and can't be changed; copy it into a Local, or make it ByRef'. AddressOf(y) for a ByRef y works. Spec 08 lists 'parameter' without qualification. Its exclusion list after 'takes only what code could change there' doesn't name ordinary parameters. The general rule arguably covers them, so this is a misleading doc, low.

## A-new-1 (language change): Every Class is Equatable and Hashable without listing them

Status: fixed in 2185793, with tests

From the user, through the coordinator (2026-10-23). Equatable: a Class's own Operator == if it has one (written, inherited, from an Interface's default or an Extend), else identity. Hashable: identity, unless the Class has an Operator == of its own without a Hash of its own; that Class is Equatable but not Hashable, so a Map key or Set of it is an error that says why, and listing Hashable on it stays the existing error. Structs don't change, Interfaces' values still have no ==, generic Classes and their instances get the same, a T? of a Class is Equatable and Hashable as a T? of a listed one is, and Comparable is unchanged. Specs 06, 07, and 14 change to say so, and tests/programs/equatable_objects.fb no longer lists Equatable.

## A-new-2 (medium): == on a Struct that holds an array of itself overflows the compiler's stack

Status: fixed in e2d38b0, with tests

Where: compiler/src/sema.cpp, comparable()

Found while testing A2-4. comparable() asks each of a Struct's fields whether == works on it, and an array's element, with nothing to stop it going round, so `Struct Node / Field children: Node[]` with `Node([]) == Node([])` recurses until `fb check` dies with 0xC00000FD and prints nothing. The same Struct without the == checks and runs. comparable()'s doc comment also says == doesn't work on arrays, which is A2-7.

## A4-new-1 (medium): A generic Function's name passed to an overloaded call or constructor gets no type arguments

Status: fixed by lane 3 in 629b610 (a generic function's name passed to an overload, or a constructor, takes its type arguments from the parameter), with sema tests; and 2dbcb51 lets "f = Less<Int>", which the error suggests, end a value

Where: compiler/src/sema.cpp, choose_overload()

Found by lane 5: `PriorityQueue<Box<Int>>(Less)` with a generic `Less` fails with "'Less' is generic, so it needs a Func type to say what T is", though passing it to a Function works and spec 07 says the Func expected gives its type arguments. Any overloaded call fails the same way, since choose_overload() checks each argument without a hint first and gave up on one with no type of its own. Lane 3 reproduced it with an overloaded Function and an overloaded New; one New alone worked. choose_overload() now lets a generic Function's name through, and match() checks it against each overload's parameter.

## A4-new-2 (medium): An Int literal that fits one overload exactly and another by conversion is ambiguous

Status: not a bug, as spec 05 and decisions.md (rows 90 and 157) have it: resolution has two tiers, and a call that two overloads fit only with conversions is ambiguous. Lane 3 pinned the rule in 28b9ed8 (a test of overloads' two tiers) with sema tests; making the exact match per argument win, as C# and C++ do, would change those rows

Where: compiler/src/sema.cpp, choose_overload() and rank()

Found by lane 5: `noise.Fbm(x, y, 3)` between `Fbm(x: Double, y: Double, octaves: Int = 5, ...)` and `Fbm(x: Double, y: Double, z: Double, octaves: Int = 5, ...)` was ambiguous. Lane 3 found that with Double x and y the first is an exact match and wins; lane 5's x and y came from a tuple of decimal literals, which are Float32s, so both overloads needed a conversion. Lane 5's std/math/noise.fb now takes the octaves, lacunarity, and gain as a Noise's Fields, which avoids the clash.

## A-new-3 (low): The Ctrl+C CLI test fails for good once any run leaves an interrupted-* folder in the temp folder

Status: fixed in bfe0081, with its own check: a stale folder planted there no longer fails it

Where: tests/cli/cli_tests.cpp, "Ctrl+C during fb run ends the program, and fb cleans up and gives its exit code"

Found while verifying lane 1's third batch. left_behind() looks for any interrupted-* entry in the temp folder (%TEMP%/futureblitz), which every run on the machine shares. One run whose fb was stopped before cleaning up left an empty interrupted-77d925f5 there, and after that every run failed, on master and on commits that had passed it, until the folder was removed. The test now counts only folders that weren't there before its run.
