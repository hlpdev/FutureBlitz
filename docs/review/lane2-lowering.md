# Lane 2: lowering and codegen

The whole-codebase review's findings for A5, A6, A7, from a review of master
at ba70e73 (2026-10-22). Only A3's were checked by a skeptic, which confirmed all 12;
reproduce every other one before fixing it. Files this lane owns: compiler/src/lower.cpp, mir.cpp, include/fb/mir.hpp, evaluate.cpp, codegen.cpp, c_abi.cpp, include/fb/c_abi.hpp, link.cpp.

31 findings: 10 high, 9 medium, 12 low, and A7-new-1 (medium), which lane 4 found. Fix the highs, then the mediums, then the lows.

Each finding's Status line is one of: open; fixed in <commit> (with its test);
dropped, with why (it doesn't reproduce, or the spec says otherwise).

## A5-1 (high): A plain ++/-- never finishes its statement, so its temporaries leak or get released on paths that never set them

Status: fixed in 080d511 (++ and -- let go of their temporaries where they end), with tests/programs/increment_temporaries.fb

Where: compiler/src/lower.cpp:3113

lower_stmt(increment_stmt_t) calls finish_statement() on its index_set and property paths. Its ordinary path (lines 3111-3117) emits the binary_t assign and returns without calling finish_statement(). Any owned temporary made while lowering the target, like a call's or a Property's object in `Make().N++` or `w.Current.Score++`, stays in m_temporaries. The next statement's finish_statement() releases it in whatever block comes next. That can be a join block reached by paths where the local was never assigned, which releases garbage. When the ++ is the last statement before the end of the function or a loop's back edge, the temporary leaks. A `Defer x.y++` hits the same path.

Reproduced by the reviewer:

```
exp-all/A5/inc3.fb: Class World has a Property Current that gives a Player, and `If hit` / `w.Current.Score++` / `End If` / `Print(...)` sits in Tick(w, hit). Calling Tick(w, True) then Tick(w, False) exits with 0xC0000374 (heap corruption). exp-all/A5/inc2.fb's MIR for Run4 shows `_1 = call Make()()` / `_1.N = _1.N + 1` in bb1, with `release _1` in the join block bb3 that bb2 (flag False) also reaches. The program segfaults. exp-all/A5/inc.fb (`Make().Values[0]++`, `Make().N++`, `GetArr()[1]--` at the end of functions) prints 'leak: 2 Bag objects were never released' and 'leak: 3 Int32[] objects were never released'.
```

Suggested fix: Call finish_statement() after the emit in the ordinary ++/-- path, as the other assignment paths do. Add a program test with ++ through a Property's or call's object inside an If and inside a loop.

Skeptic: confirmed=True, high. skA5/inc_b.fb (`If flag` / `Make().N++` / `End If` / `Print("x")`, called with False first) segfaults (0xC0000005). Its MIR has `_1 = call Make()()` and `_1.N = _1.N + 1` in bb1, and `release _1` in the join bb3, which bb2 also reaches with _1 never set. skA5/inc_a.fb (`Make().N++` as a function's last statement) prints 'leak: 1 Bag objects were never released'. lower.cpp's increment_stmt_t lowering ends with emit(assign_t{...binary_t...}) and has no finish_statement().

## A5-2 (high): For ... In over a fixed array doesn't hold it: a temporary's array is read after it's freed

Status: fixed in 52b9667 (for in holds a fixed array as it does a t[]), with tests/programs/for_in_fixed.fb

Where: compiler/src/lower.cpp:3451

For a T[], the loop copies the array reference into a held local. For a fixed array, `sequence = as_place(value)` reads the elements through the original place on every pass. Only a whole temporary is kept (take_temporary). When the fixed array is a field of a temporary, like `Make().Grid` or `MakeRow().Cells`, finish_statement() at line 3461 releases the temporary object or Struct before the first pass. Each pass then reads and retains elements from freed memory. The spec (04-control-flow.md) says For ... In "evaluates the array once and holds it for the loop". The same code also lets the loop see a fixed array variable that the body reassigns.

Reproduced by the reviewer:

```
exp-all/A5/fixed2.fb: `For x In Make().Grid`, where Make() returns a Holder whose Grid: String[3] holds run-time strings, with a body that allocates 60 Holders and strings. It prints 'one 1', 'junk 16', 'junk 16' instead of 'one 1', 'two 2', 'three 3'. The MIR shows `_0 = call Make()()` / `release _0` / then `retain _0.Grid[_1]` in the loop. exp-all/A5/fixed3.fb: the MIR for `For x In MakeRow().Cells` releases the Row before reading `_1.Cells[_2]`. In the same file, `For x In grid` with `grid = ["c {3}", "d {4}"]` in the body prints 'a 1', 'd 4', not 'a 1', 'b 2'.
```

Suggested fix: Hold a fixed array like a T[]: copy the value into a local that the loop's scope owns and releases, taking a whole temporary over as now. Then read the elements from that local.

Skeptic: confirmed=True, high. In for_in_stmt_t lowering (lower.cpp ~3449), only is_array() types get a held local. A fixed-array field of a temporary isn't held. skA5/fix_c.fb (`For x In Make().Grid`, Grid: String[3]) prints 'one 1', 'junk 14', 'junk 14'. Its MIR is `_1 = call Make()()` / `release _1` / then `retain _1.Grid[_2]` in the loop, and skA5/fix_d.fb's Struct version releases `_1` the same way before the loop. skA5/fix_b.fb: reassigning a String[2] or Int[2] variable in the body changes the remaining passes ('a 1','d 4' and '1','20'), while an Int[] gives '1','2'. That contradicts 04-control-flow.md:42, which says the loop "evaluates the array once and holds it".

## A5-3 (high): A Select's type match in generic code tests and uses the unsubstituted type, so the compiler crashes

Status: fixed in 3323788 (a select's type match in generic code tests its own instance), with a case in tests/programs/generic_bases.fb

Where: compiler/src/lower.cpp:3982

In lower_stmt(select_stmt_t), `const type_id_t match = m_analysis.symbol(*variable).type;` isn't passed through concrete(), unlike the downcast's type at line 4016. In an instance of generic code, `Case b As Box<T>` emits `Is Box<T>` and use_class(Box<T>) puts the generic definition in the module as a Class. Codegen then segfaults.

Reproduced by the reviewer:

```
exp-all/A5/sel2.fb: Class Box<T> Extends Animal, and `Function Peek<T>(a: Animal, fallback: T): T` does `Select a` / `Case b As Box<T>` / `Return b.V`, called as Peek(Box<Int>(5), 0). `fb check` passes, and `fb check --show mir` shows `class Box<T> Extends Animal` and `_3 = _2 Is Box<T>`. `fb build` and `fb run` crash (Segmentation fault, exit 139).
```

Suggested fix: Use `concrete(m_analysis.symbol(*variable).type)` for `match`, as the Catch lowering and the downcast do. Add a program test with a generic type match.

Skeptic: confirmed=True, high. skA5/sel_a.fb (`Select a` / `Case b As Box<T>` inside Peek<T>) passes `fb check`, but `fb run` segfaults the compiler (exit 139). The MIR shows `class Box<T> Extends Animal` and `_3 = _2 Is Box<T>`. lower.cpp ~3982 takes `match = m_analysis.symbol(*variable).type` without concrete(), while the downcast a few lines below uses concrete(). The If/Is/As version (sel_b.fb) prints 5 correctly.

## A5-4 (high): Break/Continue keep a reference into m_loops across release_scopes(), so a Finally with a loop loses the jump

Status: fixed in 58c0bd7 (break and continue find their loop again after a finally's loops), with a case in tests/programs/errors.fb

Where: compiler/src/lower.cpp:4075

lower_jump() takes `loop_t& loop = *find_if(m_loops, ...)`, then calls release_scopes(loop.scope_depth), and only then calls leave(loop.breaks or loop.continues). release_scopes() lowers each Finally block on the way out. A Finally that holds a loop pushes onto m_loops (lower_loop_body / for-in / iteration), which can reallocate the vector. The reference then dangles, and leave() writes the jump block into freed memory. The real loop never patches that block, so it keeps its default `unreachable` terminator. The result is undefined behavior in the compiler and wrong control flow in the program.

Reproduced by the reviewer:

```
exp-all/A5/brk.fb: `For i = 1 To 5` / `Try` / `Print("body {i}")` / `If i == 2 Then Break` / `Finally` / `For j = 1 To 2 ... End For` / `End Try` / `End For`. The MIR's break path, bb3..bb8, ends in `bb8: unreachable` instead of a goto past the loop. At run time the Break doesn't leave the loop: after 'body 2' it prints the Finally twice, then 'body 3', 'body 4', 'body 5', and 'after'.
```

Suggested fix: Read the loop's index (or scope_depth and stmt) first, run release_scopes(), then look the loop up again (or index m_loops) before calling leave(). Add a program test with Break and Continue through a Finally that holds a loop.

Skeptic: confirmed=True, high. lower_jump() (lower.cpp ~4073) holds `loop_t& loop` into m_loops across release_scopes(), and the Finally's For pushes onto m_loops. skA5/brk_a.fb's MIR has `unreachable` on the break path. At run time the Break doesn't leave the loop: after 'body 2' the Finally prints twice, then the loop goes on to 'body 5' and 'after'. skA5/brk_b.fb (Continue through a Finally holding a While) prints 'finally 2' twice and 'body 2', which a Continue at i == 2 must skip.

## A5-7 (high): A ByRef parameter, or a Struct's ByRef Self, passed on by value isn't pinned, though the callee can free it

Status: fixed in 5a38a39 (what a byref parameter holds is pinned and copied like a global's): through_reference() counts a ByRef root, and store() copies from one; with cases in tests/programs/borrowed.fb

Where: compiler/src/lower.cpp:2647

through_reference() returns false for any place rooted in a local with no step through an object, array, or Ptr. pinned() therefore never copies an argument read from a ByRef parameter's local (or from a ByRef Method's Self), as if it were the function's own variable. Such a local aliases the caller's storage, like a Global, which the callee can change. The callee then borrows a String or object whose last reference goes away while it runs. The call that passed the Global ByRef can't pin it either: pinned_place() only pins objects, arrays, and Ptrs on the way in.

Reproduced by the reviewer:

```
exp-all/A5/byref.fb: `Global g = Build("first", 1)` (a run-time String), `Function Outer(ByRef s: String)` / `Use(s)`, and `Function Use(t: String)` / `Change()` / `Print(t)`, where Change() sets g and allocates 50 strings. `Outer(ByRef g)` prints 'junk 7' instead of 'first 1'. The MIR of Outer is `call Use(String)(_0)` with no copy or retain. exp-all/A5/byref2.fb: a Struct's `ByRef Method Show()` / `Use(Name)` on `Global h`, where Use's call replaces h, prints 'junk 29' instead of 'first 1'.
```

Suggested fix: Count a root local that is ByRef (current().locals[i].by_ref, which includes a Struct's ByRef Self) as reached through a reference in through_reference(). pinned() then copies and retains what's read from it for the call. Check store() for the same assumption.

Skeptic: confirmed=True, high. through_reference() (lower.cpp ~2646) returns false for a place rooted in any local with no object, array, or Ptr step, a ByRef parameter's local included. skA5/byref_a.fb: `Use(g)` prints 'first 1' (pinned), but `Outer(ByRef g)` / `Use(s)` prints 'junk 34'. skA5/byref_b.fb with Class objects prints 'destroyed first' before 'using first', so the object is used after its Destroy. Outer's MIR is just `call Use(Thing)(_0)`. skA5/byref_c.fb (a Struct's ByRef Method Show() calling Use(Name) on a Global) prints 'junk 20' instead of 'first 1'.

## A6-1 (high): Compile-time '~' on a @Flags Enum gives 0

Status: fixed in ff43b91 (a const function's ~ on a flags enum flips its bits), with a case in tests/programs/compile_time.fb

Where: compiler/src/evaluate.cpp:305

compute() folds a unary_t with fold_unary(op, scalar(operand)) without as_integer(), unlike compute_binary(). For BIT_NOT on a @Flags Enum value, wrap(~bits, enumType) uses bit_width(enum) == 0, so unsigned_max(0) masks everything away and the result is 0, with uint64 data even when the Enum's integer is signed. The spec (06-user-types.md) says @Flags values combine with ~, and the same code gives the right answer at run time, so a Const Function's value silently differs from the program's.

Reproduced by the reviewer:

```
exp-all/a6/flags.fb and flags2.fb: `@Flags Enum Layer World = 1, Player = 2, Enemy = 4`; `Const Function Without(all: Layer, l: Layer): Layer => all & ~l`; `Const C = Without(World|Player|Enemy, Player)` prints `0`, while the same body as a plain Function prints `World | Enemy`. `Const Function Neg(a: Layer): Layer => ~a` makes `Const N = Neg(Layer.World)`, and `--show mir` shows `global N: Layer = 0` (it should be -2).
```

Suggested fix: In compute(), fold the unary on as_integer(operand) and give the result the Enum's type back, as compute_binary() does for bits of a @Flags Enum. Add a test for a Const Function using ~ on a @Flags Enum.

## A6-2 (high): A cancelled Await in a Defer or Finally releases locals a second time

Status: fixed in 9f428a7 (a cancelled await in a defer or finally lets go of only what's still held), with tests/programs/scheduler_cancel_cleanups.fb

Where: compiler/src/lower.cpp:6589

When an Await sits in a deferred statement or a Finally that runs on the way out (Return, Break, Continue, or the end of a block), release_scopes() has already released the locals of the inner scopes, and of later declarations in the same scope. Those scopes are still in m_scopes, though. lower_await()'s cancel block walks every scope in m_scopes and releases every local again. When the Scheduler is dropped while the coroutine waits there, already-freed objects are released again. That is a use after free: their refcount wraps from 0, and a Weak reference then brings back a destroyed object.

Reproduced by the reviewer:

```
exp-all/a6/deferc3.fb: `Async Function F(): String` with `Defer Print("deferred {Await Never()}")`, then `Local light = Light("lamp")`, `watch = light` (a Global Weak<Light>), then `Return "a"`. A Scheduler runs F, ticks it, and is dropped. Output: `lamp let go`, then `before drop, watch is gone`, then `after drop, watch is lamp`. The destroyed object is read through the Weak after the double release. finc.fb shows the same with `Try ... Local light ... Return 1 ... Finally Print("{Await Never()}")`. In the MIR for deferc.fb, cancel block bb2 has `release _2` after bb0 already did `release _2`.
```

Suggested fix: Have the cancel path release only the cleanups that haven't run yet on the current way out. For example, keep a record of how far release_scope()/release_scopes() have got (scope index and cleanup position) while lowering a deferred statement or Finally, and have lower_await() skip the locals before that point. Add a program test that drops a Scheduler while a Defer or Finally awaits after locals were released.

## A6-4 (high): `Try Await` / `Try? Await` of a Task that gives nothing crashes the compiler

Status: fixed in fa05eff (try and try? await a task that gives nothing), with cases in tests/programs/async_throws.fb

Where: compiler/src/lower.cpp:6639

lower_try()'s operand lambda sends only call_expr_t and try_expr_t through lower_effect(). An await_expr_t goes to lower_value(), which calls lower_expr(await_expr_t), which does expect(lower_await(id)). For a Task with no result, lower_await() returns nullopt, so expect() fails an assertion. A release compiler would hit undefined behavior instead. `Try Await X()` is how the spec has an Await's error go on.

Reproduced by the reviewer:

```
exp-all/a6/ta1.fb: `Async Function Work(fail: Bool) Throws Oops` (it gives nothing). Inside an Async Function, `Try` / `Try Await Work(False)` / `Catch e`. Compiling stops with `Assertion failed: value.has_value(), file .../compiler/src/lower.cpp, line 35`. ta2.fb has `Try? Await Work(True)` at the top level and fails the same way. An Async lambda `=> Try Await Work(False)` takes the same path.
```

Suggested fix: In lower_try()'s operand lambda, send await_expr_t through lower_effect() too, as calls and Trys already go, so a resultless Await gives nullopt. Add program tests for `Try Await` and `Try? Await` of a Task with no result, at the top level and in an Async Function.

## A6-5 (high): For Method over a type with an Async Method emits invalid LLVM IR

Status: fixed in 178a50c (for method leaves out async methods and ones with a byref parameter): Async Methods are left out, and spec 13 says so; with a case in tests/programs/reflection_more.fb

Where: compiler/src/lower.cpp:7248

reflected_methods() doesn't skip Async Methods, and method_type() gives an Async Method its declared result, like Func() or Func(): Int32, rather than the Task it gives. So `For Method m In T` picks an Async Method that gives nothing, and `Where M: Func(): Int32` picks `Async Method Load(): Int32`. lower_reflection()'s Call then emits a plain call_t to the coroutine without ASYNC_BEGIN or its Task argument, and codegen produces invalid IR. The spec says For Method goes over Methods that give nothing, and an Async Method gives a Task.

Reproduced by the reviewer:

```
exp-all/a6/reflasync3.fb: `Class Door` has `Async Method Open()` and `Method Knock()`. `Function CallAll<T>(value: T)` does `For Method m In T / m.Call(value)`, called as `CallAll(Door())`. Build output: `error: invalid LLVM IR: Incorrect number of arguments passed to called function! %20 = call ptr %19(ptr %15)`. reflasync2.fb with `Where M: Func(): Int32` and an `Async Method Load(): Int32` fails the same way. With `Where M: Func(): Task<Int32>`, nothing matches.
```

Suggested fix: Either leave Async Methods out in reflected_methods() (and say so in 13-compile-time.md), or give them their Task type in method_type() and have lower_reflection()'s Call make the Task (ASYNC_BEGIN, the Task after the object), as call_on() does. Add a test with a Class that has an Async Method.

## A7-1 (high): Globals are released in declaration order and left dangling, so a Destroy that reads an earlier Global uses freed memory

Status: fixed in 6bc4fb7 (globals are released last to first, each reference zeroed before it goes): a use after the release panics where the Global has a ready flag, and spec 02 says so; with tests/programs/globals_released.fb and panic_global_released.fb

Where: compiler/src/codegen.cpp:1986

generate_main() releases every Global in m_mir.globals index order (the order they initialize in). It never sets a released Global back to its zero and never resets its ready flag. If releasing a later Global runs a Destroy() that reads an earlier Global, the Destroy sees an object or String that is already freed. Reading the Global and retaining what it holds (Local kept = first) then releasing it touches freed memory, which corrupts the heap. This is safe code with no Unsafe. Static Fields and generic instances' Statics are Globals too, so they have the same problem. The spec never states a release order, but decisions.md line 338 already says that running code after the Globals go would reach released objects, and this is that case through Destroy.

Reproduced by the reviewer:

```
g3.fb:
Import std.core
Class Holder
    Field Name: String
    New(name: String)
        Name = name
    End New
    Method Destroy()
        Print("holder destroyed")
    End Method
End Class
Global first = Holder("first holder")
Class Watcher
    Method Destroy()
        Local kept = first
        Print("watcher keeps " + kept.Name)
    End Method
End Class
Global second = Watcher()
Print("main done")

`fb run g3.fb` gives "error: g3.exe exited with code 0xC0000374" (STATUS_HEAP_CORRUPTION), and "main done" is never printed. The --release build crashes too. With only `Print("watcher sees " + first.Name)` in the Destroy (g2.fb), the output is "holder destroyed" and then "watcher sees first holder", so Holder is freed before Watcher's Destroy reads it (a silent use-after-free). A String Global read from a Destroy (g4.fb) is also read after it is freed.
```

Suggested fix: In generate_main, release the Globals in reverse initialization order, so a Global's Destroy can still use the Globals initialized before it, as C++ static destruction does. After releasing each Global, store its zero_constant() into it, and in debug builds clear its fb.ready flag, so a later read gets a checked panic instead of a dangling reference. Add tests/programs for a Destroy that reads an earlier Global (Class and String) in both build modes.

Skeptic: confirmed=True, high. Reproduced with the reviewer's program, saved as C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/exp-all/skeptic-a7/g3.fb. `fb run g3.fb` fails with "g3.exe exited with code 0xC0000374" (heap corruption). The release build's g3.exe also exits with 0xC0000374 when run from PowerShell. g2.fb, whose Destroy reads `first.Name` directly, prints "main done", then "holder destroyed", then "watcher sees first holder": Holder is destroyed before Watcher's Destroy reads it. In codegen.cpp, generate_main() calls for_each_reference(m_globals[i], ..., false) for i = 0..size-1 in forward order. It never stores a zero back into a released Global and never resets fb.ready. No row in the Deferred work and no decision sets a release order. decisions.md:338 talks about code reaching released objects only for coroutines. The g4.fb String variant printed correctly this time, but it reads after the free too, so its luck says nothing. This is memory unsafety in safe code, so high stands.

## A5-5 (medium): For Method picks Async Methods and Methods with ByRef parameters, whose m.Call makes invalid IR

Status: fixed in 178a50c (for method leaves out async methods and ones with a byref parameter), with A6-5's case in tests/programs/reflection_more.fb

Where: compiler/src/lower.cpp:3689

reflected_methods() filters on static, Unsafe, ByRef Self, generic, Throws, Destroy and []/[]=, and compares method_type() with the signature. It doesn't leave out Async Methods: their code takes a Task after Self, and they give a Task, not nothing. method_type() also drops ByRef, so a Method taking `ByRef x: Int` matches `Where M: Func(Int)`. m.Call then passes a plain value where the code takes a pointer. The spec says For Method goes over Methods that 'take nothing, give nothing'.

Reproduced by the reviewer:

```
exp-all/A5/refl.fb: a Class with `Method Plain()` and `Async Method Later()`, and `For Method m In T` / `m.Call(value)`. `fb run` fails with 'error: invalid LLVM IR: Incorrect number of arguments passed to called function! %25 = call ptr %24(ptr %20)'. exp-all/A5/refl2.fb: `Method Changes(ByRef x: Int)` with `For Method m: M In T` / `Where M: Func(Int)` / `m.Call(value, 5)` fails with 'invalid LLVM IR: Call parameter type does not match function signature! i32 5 ... call void %24(ptr %20, i32 5)'.
```

Suggested fix: In reflected_methods(), skip `method.modifiers.is_async` and Methods with any ByRef parameter, or make method_type() reflect them so they never match. Add tests for both cases.

Skeptic: confirmed=True, medium. skA5/refl_a.fb (a Class with `Method Plain()` and `Async Method Later()`, `For Method m In T` / `m.Call(value)`) fails with 'error: invalid LLVM IR: Incorrect number of arguments passed to called function! %25 = call ptr %24(ptr %20)'. skA5/refl_b.fb (`Method Changes(ByRef x: Int)` with `Where M: Func(Int)` / `m.Call(value, 5)`) fails with 'invalid LLVM IR: Call parameter type does not match function signature! i32 5'. 13-compile-time.md:165 limits For Method to Methods that "give nothing", which leaves an Async Method out.

## A5-6 (medium): Lock evaluates the Mutex's object expression twice

Status: fixed in d6d271c (lock reaches its mutex once and holds the object it's in), with a case in tests/programs/atomic_mutex.fb

Where: compiler/src/lower.cpp:3246

lower_stmt(lock_stmt_t) first lowers the whole Mutex place with lower_place(node.mutex), which evaluates member->object. For a Field, it then calls lower_value(member->object) again to hold the object. Any call or side effect in the object expression runs twice, and the Mutex locked is in the second result, not the first.

Reproduced by the reviewer:

```
exp-all/A5/lock2.fb: `Lock Next().Count As c` / `c += 1` / `End Lock`, where Next() prints, counts calls, and adds a new Store to a list. Output: 'Next called', 'Next called', 'calls 2, stores 2', 'first 0', 'last 1'. The MIR shows `_1 = call Next()()` / `_2 = call Next()()` / `_3 = _2` / `release _1` / `lock _3.Count`.
```

Suggested fix: Lower the object once: when the Mutex is a Field, lower member->object with lower_value(), hold it, and build the place from the holder and the Field's index, without calling lower_place(node.mutex) first. Or reuse the first evaluation through m_reused.

Skeptic: confirmed=True, medium. lower_stmt(lock_stmt_t) calls lower_place(node.mutex) and then lower_value(member->object) for a Field. skA5/lock_a.fb (`Lock Next().Count As c`, where Next() counts calls and prints) prints 'Next called' twice and 'calls 2'.

## A6-3 (medium): A cancelled Await in a Defer or Finally leaks the error being thrown

Status: fixed in ea76821 (a cancelled await in a defer or finally lets go of the error on its way out), with cases in tests/programs/scheduler_cancel_cleanups.fb

Where: compiler/src/lower.cpp:6584

raise() holds the error that's leaving in its own `error` local while release_scopes() runs the Defers and Finallys. That local is neither a temporary nor a scope cleanup. When one of those statements Awaits and the Scheduler is dropped, lower_await()'s cancel block never releases the in-flight error, so the Error object and its Strings leak.

Reproduced by the reviewer:

```
exp-all/a6/raisecancel.fb has two cases. `Async Function F() Throws Boom` does `Try / Throw Boom("bad") / Finally / Print("finally {Await Never()}")`. `Async Function G() Throws Boom` does `Defer Print("deferred {Await Never()}")` then `Throw Boom("worse")`. Both run on a Scheduler that is ticked and dropped. The debug build reports `leak: 2 Strings were never released` and `leak: 2 Boom objects were never released`.
```

Suggested fix: Track the raised error local while raise() lowers the cleanups, for example by pushing it as a pending cleanup or temporary for the duration, so the cancel block releases it. Add a leak test for both the Finally case and the Defer case.

## A6-6 (medium): `+=`/`-=` on a Ptr Property emits invalid LLVM IR

Status: fixed in a9223f1 (+= and -= on a ptr property step by its t's size), with a case in tests/programs/pointers_more.fb

Where: compiler/src/lower.cpp:4292

combine(), which set_property()'s op= and ++ paths use, special-cases only Strings and otherwise emits a binary_t. A plain variable's op= on a Ptr emits a pointer_t, which steps by T's size (lower_stmt(assign_stmt_t)). For a Property of type Ptr<T>, `obj.P += n` becomes an integer add on a pointer, which codegen can't build. The spec says `+=` and `-=` step a Ptr.

Reproduced by the reviewer:

```
exp-all/a6/ptrprop.fb: `Class Cursor` has `Property Here: Ptr<Int32>` with a Get and a Set. In an `Unsafe` block: `c.at = buf`, then `c.Here += 1`. Build output: `error: invalid LLVM IR: Both operands to ICmp instruction are not of the same type! %44 = icmp ne ptr %43, i32 1`. The MIR shows `_9 = _8 + 1` instead of `Ptr _8 + 1`.
```

Suggested fix: In combine(), emit a pointer_t when type_of(left) is a Ptr (op ADD or SUBTRACT), as the variable path does. Add a program test that steps a Ptr Property with += and -=.

## A6-7 (medium): Property op= and ++ call the Set on another object when the Get changes the receiver

Status: fixed in 1e8e8c1 (a property's op= and ++ run its get and set on one object), with a case in tests/programs/property_kinds.fb

Where: compiler/src/lower.cpp:4266

set_property() settles the receiver only when `changes` is true. Its callers pass may_change(node.value) for op=, and false for ++, which counts only the right side. For op= and ++, the Property's Get runs between the receiver and the Set, and the spec (03-operators.md) lists a Property's Get among what could change a variable, with the target going before its value. When the Get reassigns the variable that holds the receiver, the Set runs on the new object, with a value computed from the old one. lower_index_set() settles everything when it reads, so the two paths disagree.

Reproduced by the reviewer:

```
exp-all/a6/getswap.fb: `Holder.Health`'s Get does `current = other` (Globals `first` with hp 1, `other` with hp 50, and `current = first`). `current.Health++` prints `set other to 2` and leaves first.hp = 1, other.hp = 2. Expected: first.hp = 2 and other untouched. `current.Health += 10` likewise sets other to 11.
```

Suggested fix: Settle the receiver whenever the Get runs (op= and ++), for example by passing `node.op.has_value() || may_change(node.value)` from lower_stmt(assign_stmt_t), and `true` from increment_stmt_t. Add a program test where a Get changes the receiver.

## A6-8 (medium): Const Functions run DebugAssert in release builds

Status: fixed in d19254f (a const function runs as the build does, so release builds leave its debugassert out): evaluate() takes the build mode, which sema's analyze() gets from the driver, and spec 13 says so; with tests/compiler/evaluate_tests.cpp and tests/programs/release_const_debug_assert.fb

Where: compiler/src/evaluate.cpp:549

run_runtime() always answers DEBUG_BUILD with True, so a Const Function's DebugAssert runs at compile time in release builds too. 09-errors.md says release builds leave DebugAssert out, condition and message too. The spec only makes overflow, division by zero, and casts the same in both builds. So a release build refuses `Const X = F(0)`, while the same F, called with a value only known at run time, goes past the DebugAssert in that build. #If DEBUG, by contrast, follows the build mode.

Reproduced by the reviewer:

```
exp-all/a6/cdebug.fb: `Const Function Half(n: Int32): Int32` with `DebugAssert(n > 0, "n must be positive")`. `fb build --release` fails with `'Half' can't run at compile time: it panics: n must be positive`. With `Local zero = 0 / Local H = Half(zero)`, the release program prints 0 and runs fine.
```

Suggested fix: Pass the build mode into evaluate() and answer DEBUG_BUILD with it, or document the debug-only rule in 13-compile-time.md. Add a test either way.

## A6-9 (medium): The steps, depth, and size limit errors don't point into the Const Function

Status: fixed in 9a7c3d1 (a const function's limits point at where it got to, and the depth limit has a test): calls and runtime calls carry their span, and the evaluator points a limit at the last one or operation it got to, as spec 13 now says; with tests/compiler/evaluate_tests.cpp

Where: compiler/src/evaluate.cpp:139

13-compile-time.md says more than 10 million steps, calls more than 10,000 deep, or variables holding more than 256 MiB is "a compile error at the call, pointing at where in the Const Function it happened". The failures for these three (run() line 139, start_call() line 212, too_much() line 117, and zero_of()) carry no span, so the error shows only the call, with no note. Panics, overflows, and indexes do get a note.

Reproduced by the reviewer:

```
exp-all/a6/climits.fb: `Const A = Spin(100000000)` gives only `climits.fb:16:11: error: 'Spin' can't run at compile time: it runs more than 10000000 steps`. `Const B = Deep(20000)` gives only `...: its calls go more than 10000 deep`. cheld.fb `Const B = Big(29)` gives only `it holds more than 256 MiB`. None has a `note: here`.
```

Suggested fix: Give these failures the span of the statement or call being run. MIR statements carry no span, so use the nearest span the frame has, such as the call's, a step's, or a binary_t's. Otherwise, reword the spec to say these point only at the call.

## A6-10 (medium): No test for the 10,000-deep call limit

Status: fixed in 9a7c3d1 (a const function's limits point at where it got to, and the depth limit has a test), with the depth tests in tests/compiler/evaluate_tests.cpp

Where: compiler/src/evaluate.cpp:211

13-compile-time.md promises that calls more than 10,000 deep in a Const Function are a compile error. sema_tests.cpp tests the step limit, the 256 MiB limit, and panics, but nothing in tests/ covers the depth limit.

Reproduced by the reviewer:

```
grep for "go more than" or 10000 in tests/ finds only the steps test (sema_tests.cpp:5239). Running exp-all/a6/climits.fb shows the limit works (`its calls go more than 10000 deep`), but no test would catch a change to it.
```

Suggested fix: Add a sema_tests.cpp case with a recursive Const Function past 10,000 calls, checking the error text.

## A7-2 (medium): T[].Count (and Capacity) silently wrap once an array has more than 2^31-1 elements

Status: fixed in f8cffbc (an array holds at most as many elements as an int counts): runtime/src/array.cpp's reserve_one() stops the capacity at 2147483647 and panics past it, as spec 14 now says; with a test in tests/runtime/runtime_tests.cpp and tests/programs/release_panic_array_full.fb

Where: compiler/src/codegen.cpp:3431

array_call() lowers ARRAY_COUNT and ARRAY_CAPACITY as LLVMBuildTrunc of the runtime's Int64 count/capacity to i32. Nothing bounds the count: fb_array_push and reserve_one() grow without limit, and fb_string_chars/fb_string_bytes fill arrays the same way. An array can therefore hold more elements than Count (an Int, spec 14) can represent, and Count then returns a wrapped negative number. For ... In loops, index checks written against Count, and std's collections all read that wrong Count. Neither build mode panics.

Reproduced by the reviewer:

```
big.fb:
Import std.core
Struct Nothing
End Struct
Local a: Nothing[] = []
Local n: Int64 = 0
While n < 2147483649
    a.Add(Nothing())
    n += 1
End While
Print(a.Count)

`fb build --release big.fb` and running it prints -2147483647 (after about 6 s; the elements take no memory). The code path is ARRAY_COUNT, `LLVMBuildTrunc(builder(), count_of(array), m_i32, "")`; ARRAY_CAPACITY at line 3464 does the same.
```

Suggested fix: Keep arrays within Int's range. fb_array_push, fb_array_insert, fb_array_new, fb_array_reserve and the Chars/Bytes fills should panic ("an array holds at most 2147483647 elements") before the count passes INT32_MAX. Alternatively, codegen can check before truncating. Add a test with an array of an empty Struct that reaches the limit.

Skeptic: confirmed=True, medium. Reproduced with `fb build --release big.fb` (the reviewer's program, plus Print(a.Capacity)). It runs in 7.7 s and prints -2147483647 for Count and 0 for Capacity: a capacity of 2^32 truncates to 0. codegen.cpp's array_call() lowers ARRAY_COUNT as `LLVMBuildTrunc(builder(), count_of(array), m_i32, "")` and ARRAY_CAPACITY the same way. runtime/src/array.cpp's fb_array_push (`array->count++`) and insert have no upper bound. Spec 14:80 and decisions.md:116 say Count is an Int. No Deferred work row covers this. With real elements it needs more than 2 GiB of memory, so it's an edge case and medium is right.

## A5-8 (low): Two doc comments sit on the wrong declarations

Status: fixed in bdde024 (lowering's comments say what its code does now)

Where: compiler/src/lower.cpp:112

Lines 112-113: cleanup_t's doc ('What leaving a scope does: releases a variable, or runs a Defer's statement or a Finally.') is merged into unlock_t's, above unlock_t. cleanup_t has no doc, and the doc doesn't mention the unlock it also holds. Lines 1788-1790: finish_function()'s doc ('Ends the block that runs off the end of the function, if code reaches there.') is above finish_async(), merged with that function's doc. finish_function() has none.

Reproduced by the reviewer:

```
Reading lower.cpp lines 112-127 and 1788-1815.
```

Suggested fix: Move each first sentence to the declaration it describes (cleanup_t, finish_function), and mention a Lock's unlock in cleanup_t's doc.

Skeptic: confirmed=True, low. lower.cpp:112-113 puts cleanup_t's sentence ('What leaving a scope does: ...') above unlock_t, and cleanup_t at line 118 has no doc. Lines 1788-1790 put 'Ends the block that runs off the end of the function...' above finish_async(), and finish_function() at ~1801 has no doc.

## A5-9 (low): Stale comments describe refcounting as Strings-only and name the wrong contents

Status: fixed in bdde024 (lowering's comments say what its code does now)

Where: compiler/src/lower.cpp:2846

Several comments still say only Strings are counted, though objects, arrays, Weak references and Structs holding them are counted too. Line 184-187 (class doc: 'Each variable holding a String, or a Struct with Strings in it'), 2633 ('An owned String in it is released'), 2846 ('Releases the Strings made for the statement'), 2907 ('whose Strings are released'), 3946 ('`owned` when it returns a new String', yet WEAK_LOAD passes owned=true for an object), and 4071 ('releasing their Strings'). Line 428 says m_declared_types holds 'Each Struct's symbol', but it holds every declared type's (Classes, Interfaces, Enums too). Line 2921 says 'Statements past M1 never pass analysis', which hasn't been true since M2.

Reproduced by the reviewer:

```
Reading lower.cpp at the listed lines.
```

Suggested fix: Reword them to say what they do now: references, objects, all declared types, and the catch-all lowering for statements that can't reach lowering.

Skeptic: confirmed=True, low. The stale comments are all there. Line 184-186 says 'Each variable holding a String, or a Struct with Strings in it'. Line 2633 says 'An owned String', 2846 'Releases the Strings made for the statement', 2907 'whose Strings are released', and 3946 '`owned` when it returns a new String', though WEAK_LOAD at 2217 passes owned=true for an object. Line 4070 says 'releasing their Strings'. Line 428 says 'Each Struct's symbol', but the constructor (196-198) fills m_declared_types with every TYPE symbol, and line 206 reads Interfaces from it. Line 2921 still says 'Statements past M1 never pass analysis'.

## A6-11 (low): A slice panic shows a UInt64 bound past Int64's range as negative

Status: fixed in 63f7ed9 (a slice's panic shows its bounds as they are), with tests/programs/panic_slice_unsigned.fb

Where: compiler/src/lower.cpp:4968

lower_expr(slice_expr_t)'s wide() casts each bound to Int64 for the checks and for the message. A UInt64 bound above Int64's maximum then shows as a negative number. The panic itself is correct, but its message misstates the bound.

Reproduced by the reviewer:

```
exp-all/a6/slicebig.fb: `Local big: UInt64 = 18446744073709551615` and `a[1..big]` on a 3-element array panic with `slice 1..-1 is out of range for 3 elements`, in debug and release builds.
```

Suggested fix: Show the bounds as written (TO_TEXT of the original operands) in the message, while keeping the Int64 compares (or compare unsigned ones as unsigned).

## A6-12 (low): --show mir prints some constants wrongly and doesn't mark coroutines

Status: fixed in 135d0e9 (the mir dump writes constants as source code does, and marks coroutines async), with a test in tests/compiler/mir_tests.cpp

Where: compiler/src/mir.cpp:242

literal() says a T? constant prints as its T would, but it calls constant(), not literal(), for the inner value, so a T? of an Enum prints its integer. constant() says constants print as they would in source code, but it escapes only \" \\ \n \t. A CR or NUL goes out raw, and a literal '{' isn't doubled. dump(mir_function_t) marks @Inline, @PanicsAtCaller, and extern, but not is_async. dump(callback_t) leaves the function name unpainted, unlike function_name().

Reproduced by the reviewer:

```
exp-all/a6/dumpmir.fb: `Global g: State? = State.Walk` dumps as `global g: State? = 1`, while `global h: State = State.Walk` dumps correctly. `"tab\there\r\nquote\"" + "\0"` dumps with a raw ^M and ^@. An Async Function's dump (deferc.fb's `function F()`) looks like any other function.
```

Suggested fix: Recurse into literal() for a NULLABLE's inner value, escape \r, \0, other control characters (\xHH), and braces, and print a marker such as `async` for is_async functions.

## A6-13 (low): Misplaced and stale comments in mir.hpp

Status: fixed in c7e6abd (mir's comments sit on what they describe, and match it); runtime_call_t::span's in 9a7c3d1 (a const function's limits point at where it got to, and the depth limit has a test)

Where: compiler/include/fb/mir.hpp:305

Several comments are in the wrong place or no longer match the code. (1) Line 305: "Something a statement computes." is rvalue_t's comment but sits on invoke_t. (2) Lines 523-527: mir_module_t's comment sits above mir_error_t, so the two run together. (3) runtime_call_t::span says it's for Insert and RemoveAt, but CHANNEL_SEND/RECEIVE, ERROR_THROWN, ERROR_UNCAUGHT and the other array calls use it too. (4) global_t::initial says nullopt is for a Struct, but Classes, arrays, and fixed arrays get nullopt too (they dump as `zero`). (5) mir_function_t::is_async says the Task is the coroutine's first parameter, but an Async Method's comes after Self and an Async lambda's after its closure. (6) mir_module_t::functions says "The Functions in source order, then the entry", but main is function 0 and the rest follow as lowering declares them.

Reproduced by the reviewer:

```
Reading mir.hpp against the code. `--show mir tests/programs/async_methods.fb` shows `_0 Self`, `_1 task`. `--show mir interp.fb` lists `function main` first. props.fb's Class Globals dump as `= zero`.
```

Suggested fix: Move rvalue_t's comment above the rvalue_t alias, and mir_module_t's below mir_error_t. Then reword the span, initial, is_async, and functions comments to match what lowering does.

## A6-14 (low): mir.cpp's example comments don't match what it prints

Status: fixed in c7e6abd (mir's comments sit on what they describe, and match it)

Where: compiler/src/mir.cpp:500

dump(lock_t)'s comment shows "lock global.score", but place() prints a Global as "@score". dump(suspend_t)'s comment shows "suspend, then bb3", but the dump also prints the cancel block: "suspend, then bb1, cancel bb2".

Reproduced by the reviewer:

```
`fb check --show mir deferc.fb` prints `suspend, then bb1, cancel bb2`.
```

Suggested fix: Update the two comments to "lock @score" and "suspend, then bb3, cancel bb4".

## A6-15 (low): Stale and misattached comments in lower.cpp

Status: fixed in bdde024 (lowering's comments say what its code does now); a map literal runs Map's New() in 9b61420 (a map literal runs map's new before its adds), with a test in tests/compiler/mir_tests.cpp

Where: compiler/src/lower.cpp:6763

(1) make_in_place()'s comment (lines 6763-6766) starts with lower_call()'s text ("A call's result, or nullopt when it returns nothing ... a?.M() only calls when a holds a value."), and lower_call() at 7102 has none. (2) The map literal comment at 5055 says std's Map has no New, but std/collections/map.fb now has `Public New()` and `New(default: V)`. The literal still skips New, which works only because New() is empty. (3) Line 4701 says expressions "past M1 never pass analysis", which is out of date.

Reproduced by the reviewer:

```
Reading lower.cpp 4701, 5055-5056, 6763-6767, and 7102, and std/collections/map.fb lines 34-42.
```

Suggested fix: Move the call comment onto lower_call(), and keep only the Atomic/Mutex sentences on make_in_place(). Reword the map literal comment, or call Map's New() so a future New body runs. Say that the fallback's expressions always have a value or an overload.

## A7-3 (low): An Extern C symbol named `string` is renamed to `string.1` by LLVM and the program fails to link

Status: fixed in e24a9ad (codegen's own constants have fb. names, so an extern keeps its symbol), with a test in tests/compiler/c_abi_tests.cpp

Where: compiler/src/codegen.cpp:1621

static_string() names every String literal's private global "string", and declare_globals() creates these globals (for any String-holding Global's initial value or zero) before declare_functions() declares the Extern Functions. declare_extern() only checks LLVMGetNamedFunction, which does not see global variables. LLVMAddFunction then gets a name that is already taken, and LLVM silently uniquifies it to "string.1", so the call goes to a symbol that doesn't exist. link_errors() can't map "string.1" back to the Extern, so the user gets a raw lld error naming a symbol they never wrote. The other private names ("text", "location", "classes") are only made after the Externs are declared, so with them only codegen's own globals get renamed, which is harmless.

Reproduced by the reviewer:

```
sym.fb:
Import std.core
Extern "C"
    Function string(): Int32
End Extern
Global greeting = "hello"
Print(greeting)
Print(string())

`fb check --show llvm sym.fb` shows `@string = private unnamed_addr constant {...} c"hello"` and `declare i32 @string.1()`. `fb run sym.fb` fails with "ld.lld: error: undefined symbol: string.1".
```

Suggested fix: Give codegen's private globals names no C symbol can have, for example "fb.string", "fb.text", "fb.location", "fb.classes" (sema already refuses Extern symbols starting with "fb."). Alternatively, declare the Extern Functions before any private global is made.

Skeptic: confirmed=True, medium. Reproduced. `fb check --show llvm sym.fb` shows `@fb.greeting = internal global ptr @string`, `@string = private unnamed_addr constant {...} c"hello"`, and `declare i32 @string.1()`, called as `call i32 @string.1()`. `fb run sym.fb` fails with "ld.lld: error: undefined symbol: string.1". In codegen.cpp, build() runs declare_globals() (line 128) before declare_functions() (129). static_string() names its global "string" (line 1621), and declare_extern() checks only LLVMGetNamedFunction (line 1116). Raised to medium: this is valid code (`string` is an ordinary C identifier) that fails with a raw linker error naming a symbol the user never wrote. That is a wrong diagnostic in an edge case.

## A7-4 (low): Extern "runtime" calls skip the C ABI's zeroext/signext, which the C++ runtime is compiled to expect

Status: fixed in e732fc0 (the runtime's externs extend small integers as c does), with a test in tests/compiler/c_abi_tests.cpp

Where: compiler/src/codegen.cpp:1121

declare_extern() adds the extension attributes only when external.c_abi is set, and lowering sets c_abi = false for the "runtime" convention. The runtime's functions are still ordinary extern "C" functions compiled by the target's C/C++ compiler. On x86-64 SysV and Apple arm64, that ABI has the caller extend arguments narrower than 32 bits, and clang-compiled callees rely on it. std/ffi/mem.fb's fb_mem_fill(destination: Ptr<Void>, value: UInt8, bytes: UInt64) is currently such a call. It has no visible effect yet, because the runtime passes the byte straight to memset, which reads only the low byte. A future runtime function taking a Bool or a small integer, or giving one back, would read garbage upper bits.

Reproduced by the reviewer:

```
With an IR-emitting harness over libfb_compiler (fb::compile + lower + emit_ir with target_triple), Mem.Fill(m, 200, 16) gives `declare void @fb_mem_fill(ptr, i8, i64)` for arm64-apple-macosx14.0.0 and x86_64-unknown-linux-gnu. clang --target=<same> for `void fb_mem_fill(void*, uint8_t, uint64_t)` gives `declare void @fb_mem_fill(ptr noundef, i8 noundef zeroext, i64 noundef)`.
```

Suggested fix: Apply extend_for_c() to an Extern "runtime" Function's scalar parameters and result too, since the runtime is C code built for the target. Only the String/array/Task handling should differ between the two conventions.

Skeptic: confirmed=True, low. Shown by reading the code. lower.cpp:625 sets `.c_abi = external->convention != convention_t::RUNTIME`, and codegen.cpp's declare_extern() calls extend_for_c() only `if (external.c_abi)`. So the zeroext/signext that c_extension() would give on SysV x86-64 and Apple targets never reaches runtime calls. std/ffi/mem.fb:11 declares `fb_mem_fill(destination: Ptr<Void>, value: UInt8, bytes: UInt64)` with the "runtime" convention. runtime/src/runtime.cpp:247 implements it as an ordinary C function taking std::uint8_t, so the caller breaks the target C ABI's extension contract. There's no visible effect today, since it only forwards the byte to memset, which reads only the low byte. No other std runtime Extern takes or gives a Bool or an integer narrower than 32 bits. It's latent, so low is right.

## A7-5 (low): generate_channel_functions' comment says a Channel's visit visits nothing, but it visits the ring's elements

Status: fixed in de229d1 (codegen's and the c abi's comments cover what the code handles)

Where: compiler/src/codegen.cpp:3070

The doc comment ends "Its elements are Sendable, which can't keep it, so its visit visits nothing." The same function then builds a visit that walks the ring through fb_channel_element and calls visit_objects() on each element that holds objects (lines 3097-3119, "The visit goes over the elements in the ring, oldest first, for the leak report"). The comment is stale.

Reproduced by the reviewer:

```
Reading codegen.cpp lines 3070-3072 against 3097-3119.
```

Suggested fix: Reword it to say the visit goes over the elements left in the ring for the leak report, once no thread uses the Channel.

Skeptic: confirmed=True, low. The comment at codegen.cpp:3070-3072 ends "Its elements are Sendable, which can't keep it, so its visit visits nothing." The same function then builds a visit that loops over m_channel_element and calls visit_objects() on each element when holds_objects(element). Its own inner comment says the visit "goes over the elements in the ring, oldest first, for the leak report". The doc comment is stale.

## A7-6 (low): Comments say the Globals' ready flags exist only in debug builds; release builds have them too

Status: fixed in de229d1 (codegen's and the c abi's comments cover what the code handles), and m_ready's comment in batch 1's 6bc4fb7

Where: compiler/src/codegen.cpp:308

m_ready's comment ("In debug builds, for each Global main sets, a Bool ...") and generate(const ready_t&)'s comment ("Debug builds mark a Global main has set, and panic where a function uses one it hasn't yet", line 2338) both say debug builds only. declare_globals() also makes the flag in release builds for any Global whose zero holds a null object (`(checked() || holds_null_object(global.type)) && global.set_in_main`), and generate(ready_t) then checks it. tests/programs/release_panic_global_order.fb relies on that.

Reproduced by the reviewer:

```
Reading codegen.cpp lines 308-309 and 2338-2339 against line 1037.
```

Suggested fix: Say that the flag exists in debug builds, and in release builds for a Global whose zero holds a null object.

Skeptic: confirmed=True, low. m_ready's comment (codegen.cpp:308) says "In debug builds, for each Global main sets, a Bool...". generate(const ready_t&)'s comment (2338) says "Debug builds mark a Global main has set, and panic...". But declare_globals() (1037) makes the flag when `(checked() || holds_null_object(global.type)) && global.set_in_main`, so release builds have it too for a Global whose zero holds a null object, and generate(ready_t) checks it whenever the flag isn't null. Both comments are inaccurate.

## A7-7 (low): Doc comments leave out cases the code handles (Callback as C data, Enums whose zero isn't 0)

Status: fixed in de229d1 (codegen's and the c abi's comments cover what the code handles)

Where: compiler/include/fb/c_abi.hpp:13

In c_abi.hpp, c_scalar_t::INTEGER says "An integer, a Bool, an Enum, a CString, or a Ptr." and c_layout()'s comment lists C data as "a number, Bool, Enum, CString, Ptr, or a fixed array or Struct of these". c_layout() also takes a Callback (CALLBACK maps to an 8-byte INTEGER, as the c_abi test "a Callback in a Struct takes 8 bytes" checks), and spec 12 allows Callback Fields. In codegen.cpp line 595, zero_is_null()'s comment says every type's zero is all zero bytes except a String's and what holds one. The function also returns false for an Enum whose zero (first value) isn't 0, and for what holds such an Enum.

Reproduced by the reviewer:

```
Reading c_abi.hpp lines 13 and 35-36 against c_abi.cpp lines 111-113, and codegen.cpp line 595 against lines 623-625.
```

Suggested fix: Add Callback to both c_abi.hpp comments, and add "an Enum whose first value isn't 0" to zero_is_null()'s comment.

Skeptic: confirmed=True, low. In c_abi.hpp, line 13 (`c_scalar_t::INTEGER`: "An integer, a Bool, an Enum, a CString, or a Ptr.") and lines 35-36 (c_layout's C data: "a number, Bool, Enum, CString, Ptr, or a fixed array or Struct of these") leave out Callback. c_abi.cpp maps `type_kind_t::CALLBACK` to `scalar(8, c_scalar_t::INTEGER)`, spec 12:142-143 makes a Callback C data allowed as a @CLayout Field, and c_abi_tests.cpp:190 tests it. codegen.cpp:595's zero_is_null() comment says every type's zero is all zero bytes but a String's and what holds one. The function also returns `enum_zero(type) == 0` for an Enum, so an Enum whose first value isn't 0, and what holds one, give false. The comments are incomplete.

## A7-new-1 (medium): Every top-level Const known at compile time becomes an MIR global, used or not

Status: fixed in 1448027 (a const known at compile time gets a global only where code needs its place), with a test in tests/compiler/mir_tests.cpp

Where: compiler/src/lower.cpp:497

Found by lane 4 (2026-10-22): a file-level Const in a std file every program loads (std/collections/map.fb, for Map literals) put `global std.collections.map.MostBuckets` in every program's MIR, so 50 mir_tests broke until the Const moved inside Grow. declare_globals() gave each symbol in analysis.globals a global, but every use of a Const whose value is known reads that value, so its global is never read, and one in a library file costs every program a global it can't use.

Reproduced: a main file importing lib/consts.fb (`Const MOST_BUCKETS = 1 << 20`, `Public Const NAME = "lib"`), with `Const LOCAL_LIMIT = 10` and `Const USED = 3`, `Global g = USED + 1`: `fb check --show mir` lists `global lib.consts.MOST_BUCKETS`, `global lib.consts.NAME`, `global LOCAL_LIMIT`, and `global USED` beside `global g`.

Fix: such a Const gets its global only when code asks for its place (place_of()).
