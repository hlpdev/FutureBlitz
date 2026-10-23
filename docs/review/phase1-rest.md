# Phase 1's review: lowering, front end, runtime, and std

The review of phase 1's fixes (ecaa974..36ab52a), on 2026-10-23: six reviewers by area, each finding reproduced by a skeptic. 21 findings for lane 8: 1 high, 4 medium, 16 low. Fix each with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why.

## P1-lowering-1 (high): For ... In now copies a fixed array as one LLVM aggregate, so iterating a 256x256 Global or Field crashes the compiler

Status: fixed in 428a3a3 (for in holds a large fixed array's copy in a t[], and a value holding a fixed array is copied and made a part at a time), with tests/programs/for_in_large_fixed.fb

Where: compiler/src/lower.cpp:3552

A5-2's fix (52b9667) holds every fixed array that isn't a whole temporary by copying it into a local (`_5 = @tiles` in the MIR). Codegen lowers that copy as one first-class aggregate load and store (`%34 = load [65536 x i32], ptr @fb.tiles` / `store [65536 x i32] %34, ptr %2`). LLVM can't handle an aggregate that size: the fb compiler segfaults while compiling, and a --release build doesn't finish compiling in 100 s. Before the fix, `For t In tiles` read `@tiles[i]` in place, so the same code compiled. Indexing the same Global with nested For loops still compiles and prints 7. A Class field does the same: `Field Tiles: Int32[256, 256]` / `For t In map.Tiles` crashes, while a 1,000,000-element field indexed by hand works. An explicit `Local copy = g.Cells` of 100000 elements already crashed the same way, but the fix now sends every For ... In over a big fixed array down that path. A 256x256 tile map, which is ordinary game code, is enough: Int32[40000] compiles, and Int32[65536] crashes. Where it does compile, each loop start also copies the whole array onto the stack. Checked with build-rev3's fb.exe, a Debug build of the compiler.

```
Import std.core

Global tiles: Int32[256, 256]
tiles[255, 255] = 7
Local total = 0
For t In tiles
    total += t
End For
Print(total)
```

Expected: 7, in debug and release builds (as before the fix, and as a nested `For y = 0 To 255 / For x = 0 To 255 / total += tiles[x, y]` over the same Global prints now)

Actual: `fb run`: "Compiling forin_global2.fb", then the compiler dies with a segmentation fault (exit 139), and no exe is built. `fb run --release`: still compiling after 100 s (timeout, exit 124). `fb check --show llvm` shows `load [65536 x i32], ptr @fb.tiles` and `store [65536 x i32]` for the loop's copy.

Suggested fix: Don't copy a fixed array with an aggregate load and store. Either have codegen copy fixed arrays and other large aggregates with LLVMBuildMemCpy, retaining the elements' references afterwards, or hold the object or T[] the fixed array is reached through, as Lock now does, and copy only when the root is a variable the body can assign. Add a program test with a For ... In over an Int32[256, 256] Global and Field.

Skeptic: I put the programs in C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/p1/lowering-v/. `fb run forin_global.fb`, the finding's program with `Global tiles: Int32[256, 256]` and `For t In tiles`, printed 'Compiling forin_global.fb' and then segfaulted (exit 139). `fb run --release` was still compiling at the 60 s timeout (exit 124). forin_field.fb, `Field Tiles: Int32[256, 256]` with `For t In map.Tiles`, also segfaulted (139). nested.fb, nested For loops over the same Global, printed 7. Int32[200,200] and Int32[40000] both printed 7. `fb check --show llvm forin_small.fb` shows `load [40000 x i32], ptr @fb.tiles` and `store [40000 x i32] %34, ptr %2`, so the loop copies the array as one aggregate. In `git show 52b9667`, the old code only held a T[] (`if (is_array(type))`) and read a fixed-array place in place. The new code copies every fixed array that isn't a whole temporary (`const local_id_t held = add_local(type); initialize(held, value)`). The fix therefore sends every For ... In over a big Global or Field through the aggregate copy that crashes LLVM. That makes an old codegen weakness much worse: a 256x256 tile map, which used to compile, no longer does.

## P1-front-1 (medium): fb bindgen -o still overwrites a header that the named header includes

Status: fixed in 34af2094 (fb bindgen writes no module over a header the header reads), with a CLI test: bindgen keeps the files libclang read for the header, and an -o, or the module path it would pick, that names one of them is a usage error

Where: tools/bindgen/src/main.cpp:174

The A9-4 fix only refuses an -o that equivalent() matches to the header given on the command line. Any other header clang reads for it, such as one it #includes from the same folder, can still be overwritten. The user's C source is destroyed and the run reports success, which is the same consequence A9-4 was about. fb build's own guard checks every file of the program, not just the main one.

```
// Not a .fb program: a C header pair and one command, run in C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/p1/front/bg
// vec_types.h:
typedef struct { float x, y; } vec2;
// vec.h:
#include "vec_types.h"
float vec_length(vec2 v);
// command:
//   fb bindgen vec.h -o vec_types.h
```

Expected: error: '-o' names vec_types.h, a header vec.h reads; give another path (exit 2), with vec_types.h left as it was

Actual: warning: vec.h:2:7: skipped vec_length, which takes vec2, which another header defines
Wrote vec_types.h  (exit 0). vec_types.h now starts with '// Generated by fb-bindgen from vec.h for x86_64-w64-mingw32.' and its typedef is gone. Running bindgen on vec.h again then fails with "vec_types.h:4:1: unknown type name 'Public'".

Suggested fix: Before writing, refuse an output that is equivalent() to any file the translation unit included (walk clang_getInclusions, or collect every CXFile clang opened), with the header's own message. Alternatively, also refuse an -o that doesn't end in .fb. Add a CLI test where -o names an included header.

Skeptic: Ran the pair in rev-scratch/p1/front-v/bg (vec_types.h holds the typedef, vec.h #includes it). `fb bindgen vec.h -o vec_types.h` printed the 'skipped vec_length' warning and 'Wrote vec_types.h', exit 0. vec_types.h then began '// Generated by fb-bindgen from vec.h for x86_64-w64-mingw32.' and the typedef was gone. `fb bindgen vec.h -o vec.h` is refused with exit 2, so only the named header is protected. tools/bindgen/src/main.cpp:174 compares -o only with request.options.header. fb build's guard (cli/src/commands.cpp:299-311) instead loops over every file in compilation.sources. A8472018's test covers only the named header. The user's C source is still destroyed while the run reports success, which is the A9-4 consequence, now reached through a header that header includes (for example after tab-completing -o to the wrong .h). That makes this medium.

## P1-lowering-2 (medium): Static Fields, and generic instances' Statics, are released before the Globals declared below them, so a Destroy that reads one panics or reads a zero

Status: fixed in e39576d (globals are let go of in the reverse of the order main sets them, static fields at their class's place), with Static Field and generic Static cases in tests/programs/globals_released.fb

Where: compiler/src/codegen.cpp:1994

A7-1's fix (6bc4fb7) releases m_mir.globals from last to first, assuming the index order is the initialization order. It isn't. declare_globals() lists the top-level Globals first and the Static Fields after them, and instance_global() appends generic instances' Statics as lowering reaches them. Main still initializes a Static Field at its Class's place in the source: in the program, `ready @Settings.Current` comes before `ready @watcher`. Its global, though, is index 1, after watcher's index 0, so it's released first. watcher's Destroy then panics reading a Static Field declared, and initialized, above it, in both builds. Spec 02 promises a Destroy then 'can use the Globals above its object's'. This is a regression: with the old forward order, watcher went first and the program worked. A generic Static goes the same way. With `Class Tagged<T>` / `Static Field Label = "tagged"` above `Global watcher`, the watcher's Destroy prints `[]` instead of `[tagged]`, although a value known at compile time 'has it from the start'. The finding itself said Static Fields and generic Statics have the problem, but globals_released.fb tests neither.

```
Import std.core

Class Holder
    Field Name: String
    New(name: String)
        Name = name
    End New
    Method Destroy()
        Print("{Name} goes")
    End Method
End Class

Class Settings
    Static Field Current = Holder("settings")
    Static Field Title = "title {1}"
End Class

Class Watcher
    Method Destroy()
        Print("the watcher sees {Settings.Current.Name} and {Settings.Title}")
    End Method
End Class

Global watcher = Watcher()
Print("main done")
```

Expected: main done
the watcher sees settings and title 1
settings goes

Actual: main done
settings goes
panic: 'Settings.Current' is used after it's released, since Globals are released last to first when the program ends at static_order.fb:20:34
(exit 101, the same in debug and release builds)

Suggested fix: Release the Globals in the reverse of the order main initializes them. For example, record that order as lower_top_level()/initialize_global() go, or order m_module.globals by declaration, and place each generic instance's Statics where its type is declared. Add Static Field and generic Static cases to globals_released.fb.

Skeptic: static_order.fb (the finding's program), debug and release both: 'main done', 'settings goes', then `panic: 'Settings.Current' is used after it's released, since Globals are released last to first ... at static_order.fb:20:34` (exit 101). generic_static.fb, with `Class Tagged<T>` / `Static Field Label = "tagged"` above `Global watcher`, prints '[]' instead of '[tagged]' in both builds. The control global_order.fb, which uses a plain Global above the watcher, prints 'the watcher sees settings' correctly. In the code, codegen.cpp generate_main() releases `for (i = m_mir.globals.size(); i-- > 0;)` in index order. lower.cpp declare_globals() walks m_analysis.globals, which lists top-level Globals before Static Fields (Settings.Current gets released before watcher), and instance_global() appends generic Statics whenever lowering reaches them. Spec 06 says a Static Field 'starts with its value ... in source order with the Globals'. Spec 02 says the Globals are let go of 'the last first, so a Destroy that runs then can use the Globals above its object's'. Both are broken. With the old forward order this program worked, so it's a regression. Medium, not high: it panics or reads a zero, with no memory unsafety, since a released Global is zeroed and the null-zero ones have ready flags.

## P1-lowering-3 (medium): Return of a ByRef parameter (or a Global) isn't copied before the function's cleanups run, so a Destroy that changes it frees what's returned

Status: fixed in 7adecc1 (a return copies a global or byref parameter it reads before the cleanups, so a destroy can't change what it gives), with tests/programs/return_through_reference.fb

Where: compiler/src/lower.cpp:4235

A5-7's fix made through_reference() count a ByRef parameter's root, and pinned() and store() copy from one. The Return path makes the same 'a step-less place is the function's own variable' assumption, and the fix didn't cover it. lower_stmt(return_stmt_t) takes the result (`retain _0`) and copies it only when defers_from(0). outliving() copies only places with steps. Then release_scopes(0) releases the Locals, and a Destroy that runs can assign the caller's variable. The MIR of Give is `retain _0` / `release _2` / `return _0`: it retains 'first 1', the Destroy replaces g, and then it returns the new String without a reference of its own. The old String leaks, and the caller's `got` shares g's only reference, so once g changes, `got` is freed memory. A Global (`Return g`) has the same pattern, and a Struct's ByRef Self (`Return Self`) shares the root. This is the neighbor A5-7's 'check store() for the same assumption' pointed at.

```
Import std.core

Function Build(word: String, n: Int): String => "{word} {n}"

Global g = Build("first", 1)

Class Changer
    Method Destroy()
        g = Build("second", 2)
    End Method
End Class

Function Give(ByRef s: String): String
    Local c = Changer()
    Return s
End Function

Local got = Give(ByRef g)
g = Build("third", 3)
Local junk: String[] = []
For i = 1 To 50
    junk.Add(Build("junk", i))
End For
Print("got {got}")
```

Expected: got first 1 (the value Return computed, before the cleanups ran), and no leak

Actual: Debug: "got junk 3" then "leak: 1 String was never released". Release: "got junk 12" then "leak: 1 String was never released". Without the reassignment and junk, Give returns "second 2".

Suggested fix: In lower_stmt(return_stmt_t), and in outliving(), copy a result whose place through_reference() says is reached through a reference (a Global, a ByRef parameter, or ByRef Self) into a temporary before release_scopes(0), whenever a cleanup runs, as is already done for Defers. Add the program above as a test.

Skeptic: return_byref.fb (the finding's program) printed 'got junk 19' then 'leak: 1 String was never released' in debug, and 'got junk 20' plus the leak in release. return_byref2.fb, without the reassignment and junk, prints 'got second 2' plus a leak. The return should give 'first 1'. `--show mir` of Give is `_1 = new Changer / _2 = _1 / retain _0 / release _2 / return _0`. The retain is taken before the Destroy replaces g, and the return reads g's new value with no reference of its own, so the result is used after it's freed. In lower.cpp lower_stmt(return_stmt_t) (~4225), the result is copied only `if (holds_alternative<place_t>(*result) && defers_from(0))`. outliving() returns step-less places unchanged (`place->steps.empty()`), so a ByRef root reaches release_scopes(0) uncopied. This is the same 'a ByRef local is the function's own variable' assumption that A5-7 fixed in through_reference(), pinned() and store(), and its suggested fix asked to check other spots for it. The Return path is still open. The same holds for `Return g` on a Global (return_global.fb: 'got junk 4' debug, 'got junk 8' release, plus a leak), so the root predates phase 1, but the ByRef case is a case of A5-7 that its fix left open.

## P1-std-1 (medium): Deque overflows at 2^31 - 1 values; the sweep for A11-3 and A12-9 missed it

Status: fixed in f146218: Balance takes HalfUp(count), count - count / 2, and AddFirst, AddLast, Add, and AddAll panic at the caller, 'a Deque holds at most 2147483647 values', once HasRoom(front, back) says an Int can't count one more (std_tests.cpp's "std's counts and indexes don't overflow near the most an Int holds")

Where: std/collections/deque.fb:164

Phase 1 fixed std's arithmetic near 2^31 in T[]'s and PriorityQueue's SiftDown, Queue's compaction check (HalfEmptied), RingBuffer's slots (SlotFrom), and Map's buckets. The commits call these "std's overflow-prone counts", and std_tests.cpp's test is named "std's counts and indexes don't overflow near the most an Int holds". Deque has the same bug and the sweep didn't catch it. Balance does `Local moved = (from.Count + 1) / 2`, which overflows when one side holds 2147483647 values. Since f8cffbc, an array can hold exactly that many. So RemoveFirst on a Deque filled with AddLast panics with the wrong message on a valid call: in a debug build an integer overflow, in a release build a negative slice. A second overflow sits next to it: Count is `Front.Count + Back.Count`, which wraps negative in release builds once the two sides together pass 2^31 - 1, after which every index is 'out of range for -2147483648 elements'. Arrays panic at that point with 'an array holds at most 2147483647 elements'; Deque has no such check. Both runs below needed about 2 to 3 GB and 13 to 17 s.

```
Import std.core
Import std.collections

Local deque = Deque<Byte>()
For i = 0 Until 2147483647
    deque.AddLast(7)
End For
Print(deque.Count)
Print(deque.RemoveFirst())
Print(deque.Count)
```

Expected: 2147483647
7
2147483646

Actual: Debug: 2147483647, then 'panic: integer overflow in '+' at deque_big.fb:9:7'. Release (--release): 2147483647, then 'panic: slice -1073741824..2147483647 is out of range for 2147483647 elements at deque_big.fb:9:7'. Adding deque.AddFirst(1) after the loop instead and printing Count gives -2147483648 in release, and deque[0] then panics 'index 0 is out of range for -2147483648 elements'.

Suggested fix: Use `from.Count - from.Count / 2` (the same as (n + 1) / 2, with no overflow) in Balance. Make AddFirst and AddLast panic at the caller once Count would pass what an Int holds, as an array does, or work out Count without the overflow. Test it the way std_tests.cpp tests the other helpers, with a Package helper and counts near 2^31.

Skeptic: Ran deque_big.fb (the finding's program) under rev-scratch/p1/std-a11a12/. Debug printed 2147483647, then 'panic: integer overflow in '+' at deque_big.fb:9:7' (17 s). Release printed 2147483647, then 'panic: slice -1073741824..2147483647 is out of range for 2147483647 elements at deque_big.fb:9:7'. deque_sum.fb (the same loop, then AddFirst(1)) failed in debug with 'panic: integer overflow in '+' at std/collections/deque.fb:15:16'. In release it printed -2147483648, then 'index 0 is out of range for -2147483648 elements'. deque.fb:164 still has `Local moved = (from.Count + 1) / 2`, and line 15 has `Get => Front.Count + Back.Count`; no phase 1 commit touched either. a231ba5 ('std's overflow-prone counts in package helpers') and 1e7a107 swept the same class of overflow beyond what A11-3 named (Queue's HalfEmptied), but missed Deque. A RemoveFirst on a full Deque is a valid call, and it panics with the wrong message. Medium matches A11-3 and A12-9 (skeptic), which are the same class of bug.

## P1-front-2 (low): After a recoverable error, a valid line continuation is refused, so one mistake gives extra errors

Status: fixed in 278425de (a line break after an operator still continues the line after an error the parser gets past; only a missing expression stops it), with parser tests: a line whose error the parser got past still continues after an operator, so `x = 1e999 +` then `    2.0` gives one error, and so does an Implements line after a modifier in the wrong order

Where: compiler/src/parser.cpp:310

The A8-11 fix makes skip_continuation_newlines() do nothing once m_line_failed is set. Many errors leave the parser on track and keep parsing the rest of the line: a decimal literal out of range, attributes and modifiers in the wrong order, New Foo(), and a one-value tuple. After any of these, a correct continuation after an operator, ',', '=', or '&' (including the forms A8-4 just added) is no longer joined. The next line is then parsed as a statement of its own, and its fallout errors are reported, because the per-line suppression has already been reset. Before d4c9477 these programs gave one error.

```
Import std.core

Local x = 1e999 +
    2.0
Print(x)

// A second case, with Implements' new continuation: one modifier-order mistake gives 4 errors.
// Public Interface Named / Method Name(): String / End Interface
// Public Interface Aged / Method Age(): Int / End Interface
// Public @Deprecated("old") Class Person Implements Named,
//         Aged
//     Method Name(): String => "p"
//     Method Age(): Int => 3
// End Class
// Local p: Aged = Person()
```

Expected: One error: 'cont3.fb:3:11: error: decimal literal is out of range' (and in the second case only 'Public goes after a declaration's attributes ...')

Actual: cont3.fb:3:11: error: decimal literal is out of range
cont3.fb:4:5: error: expression has no effect
error: cont3.fb has 2 errors
(The second case adds 'expression has no effect' and 'a Class can only hold Fields, Methods, Properties, and News' at 12:9, and 'expected Aged, found Person' at 17:17: 4 errors.)

Suggested fix: Narrow the guard to what A8-11 needed: stop joining only when the left operand, or the token just consumed, is the failure itself, i.e. an error_expr_t or missing_expr on a failed line. Alternatively, have parse_primary consume an unexpected binary-operator token. Keep the continuation after errors the parser recovers from. Add a parser test with an out-of-range decimal followed by '+' and a line break.

Skeptic: cont3.fb (`Local x = 1e999 +` / `    2.0` / `Print(x)`) gives 'decimal literal is out of range' at 3:11 plus a fallout 'expression has no effect' at 4:5: 2 errors. The same expression on one line (cont5.fb) gives the single error. The Implements case (cont4.fb) gives 4 errors: the modifier-order error, then 'expression has no effect' and 'a Class can only hold Fields, Methods, Properties, and News' at 10:9, then 'expected Aged, found Person' at 14:17. d4c9477's diff changed skip_continuation_newlines() from `while (NEWLINE)` to `while (!m_line_failed && NEWLINE)`. Any error() call sets m_line_failed, including ones the parser recovers from, such as the out-of-range decimal at parser.cpp:3209. A correct continuation after those is now refused, and finish_line() resets the suppression before the next line is parsed. Before the fix, the continuation joined unconditionally and gave one error. This is a regression the fix itself introduced.

## P1-front-3 (low): New recovery: Blitz's 'New Foo' without parentheses gets a second, fallout error, and a qualified type is suggested by its module name

Status: fixed in b215139f (blitz's new foo, without its parentheses, is the call it means, and new's error names the type as it's written), with parser and sema tests: `New P` without parentheses gets one error, and `New m.P` suggests `New m.P()`

Where: compiler/src/parser.cpp:3258

The A8-17 fix says that what follows New 'is parsed as the call it should be'. It only becomes a call when '(' is written. Blitz code writes `New Foo` with no parentheses, which is exactly the form the finding was about. That parses as the bare name Foo, and sema then adds "'Foo' is a type, not a value". The suggested spelling also uses only the first identifier after New: `New shapes.Circle()` is told 'like shapes()', which suggests calling the module, and `New Stack<Int>()` is told 'like Stack()'.

```
Import std.core

Class Foo
    Field v: Int = 3
End Class

Local f = New Foo
Print(f.v)
```

Expected: One error: 'new1.fb:7:11: error: an object is made by calling its type, like Foo(); New only declares a constructor'. A qualified `New shapes.Circle()` should suggest 'like shapes.Circle()'.

Actual: new1.fb:7:11: error: an object is made by calling its type, like Foo(); New only declares a constructor
new1.fb:7:15: error: 'Foo' is a type, not a value
error: new1.fb has 2 errors
(With `Import "./shapes"` and `Local c = New shapes.Circle()`: 'error: an object is made by calling its type, like shapes(); New only declares a constructor'.)

Suggested fix: After the error, when no '(' follows the parsed primary, wrap it in a call with no arguments (or return an error_expr_t so sema says nothing more). Build the suggestion from the whole type as written: qualifier, name, and type arguments. Add parser tests for `New Foo` and `New m.Foo()`.

Skeptic: new1.fb (`Local f = New Foo` with no parentheses) gives 'an object is made by calling its type, like Foo(); New only declares a constructor' and also ''Foo' is a type, not a value' at 7:15: 2 errors. new2.fb (`Local c = New shapes.Circle()`) gives '... like shapes(); New only declares a constructor', which suggests calling the module. new3.fb's `New Stack<Int>()` suggests 'Stack()'. At parser.cpp:3259 the comment says Blitz's 'New Foo' is 'Foo()' here, which is what's parsed after the error. But the code just returns parse_primary(), which yields the bare name Foo, so the code contradicts its own comment. The suggestion takes only text(peek()) of the first identifier. The test in 8be1f4c covers only `New Foo()` with parentheses. (new2's extra 'r is private' error was my program's mistake: a missing Public. It isn't part of the finding.)

## P1-front-4 (low): --show ast and --show types print an @Deprecated message raw: control characters, quotes, and braces unescaped

Status: fixed in a6cc357b (the dump writes a @deprecated message as source writes it), with a parser test

Where: compiler/src/dump.cpp:672

A8-14's fix routes every string the AST dump shows through quote(), which escapes control characters and doubles braces. The one exception is the @Deprecated modifier, which is still written with std::format("@Deprecated(\"{}\")", message) on the decoded message. ESC sequences in it reach the terminal (\x1B[2J clears the screen), a \n splits the dump line, and quotes and braces aren't written the way source writes them. The @Deprecated message was the first example in the A8-14 finding.

```
Import std.core

@Deprecated("use \x1B[2J\x1B[31mG instead, {{soon}} \"now\"")
Function F()
End Function

F()
// run: fb check --show ast dep.fb   (or --show types)
```

Expected: (function @Deprecated("use \x1B[2J\x1B[31mG instead, {{soon}} \"now\"") F)

Actual: (function @Deprecated("use <ESC>[2J<ESC>[31mG instead, {soon} "now"") F), with raw 033 bytes (od -c: 'u s e   033 [ 2 J 033 [ 3 1 m G'). A '\n' in the message breaks the dump line in two.

Suggested fix: Write it as std::format("@Deprecated({})", quote(*modifiers.deprecated)). Add a parser or dump test with a control character, a quote, and a brace in a @Deprecated message.

Skeptic: dep.fb holds `@Deprecated("use \x1B[2J\x1B[31mG instead, {{soon}} \"now\"")` on F. In both `fb check --show ast` and `--show types`, the Deprecated line under od -c shows `( f u n c t i o n   @ D e p r e c a t e d ( " u s e   033 [ 2 J 033 [ 3 1 m G ... { s o o n }   " n o w " " )   F )`: raw ESC bytes, single braces, and unescaped quotes. dump.cpp:672 still builds it with std::format("@Deprecated(\"{}\")", *modifiers.deprecated) instead of quote(). Every other dumped string, at lines 387, 394, 795, 819, 822 and 1029, goes through quote(), as e35849a's status says all of them do.

## P1-front-5 (low): A program fb builds that's stopped with Ctrl+C or Ctrl+Break leaves the console's code page at UTF-8

Status: fixed in 726c4c43 (a program ended by ctrl+c, ctrl+break, or a signal gives the console and stdin back as it found them, and fb reads both streams' modes before it changes either), with P1-runtime-1's test

Where: runtime/src/runtime.cpp:287

A9-9's fix for programs registers restore_console with the runtime's at_end(). That list only runs from fb_finish (a normal end) and end_program (a panic or an uncaught error). Ctrl+C and Ctrl+Break end the program through the default console-control handler, which calls ExitProcess, so the code page set by fb_start stays at 65001, along with stderr's VT mode. The code's comments say the console gets its code page back 'however the program ends', and A9-13 calls Ctrl+C the normal way to stop a game loop. fb itself does restore the code page when interrupted the same way, because its restore is an atexit handler: in the same harness, `fb build --release` interrupted mid-compile leaves 437. The CLI test only covers a normal end and a panic.

```
Import std.core

PrintError("started")
Local n = 0
While True
    n = (n + 1) % 1000
End While
// Built with `fb build loop.fb`, then run with C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/p1/front/cc/launch.exe, a small C launcher (launch.c beside it). It sets the code page to 437, starts the program with CREATE_NEW_PROCESS_GROUP, waits for 'started', sends CTRL_BREAK_EVENT, waits, and prints GetConsoleOutputCP().
```

Expected: before=437 ... after=437 (the console gets its code page back, as it does when the same launcher runs a program that ends normally: 'sent=0 exit=0x00000000 after=437')

Actual: before=437
during=65001
sent=1 exit=0xC000013A after=65001

Suggested fix: Restore in the cases the console ends a program too. Either register restore_console with atexit (which, as fb's own restore shows, runs under ExitProcess with this CRT), or install a SetConsoleCtrlHandler handler in fb_start that restores the console and returns FALSE so the default handling still ends the program. Extend the console_probe CLI test with a program stopped by Ctrl+Break.

Skeptic: I built my own copy of the reporter's launcher (front-v/cc/launch.c, gcc -municode), then loop.fb and quick.fb with fb build. `launch.exe quick.exe` with NOSEND=1 printed 'before=437 during=65001 sent=0 exit=0x00000000 after=437'. Ctrl+Break on loop.exe printed 'before=437 during=65001 sent=1 exit=0xC000013A after=65001', and the same for a --release build (loopr.exe). runtime.cpp's fb_start() (line 287) registers restore_console only through fb::runtime::at_end(), whose run_end() is called from the normal end and the panic path. Nothing hooks SetConsoleCtrlHandler or atexit for programs, while cli/src/terminal.cpp:118 does use std::atexit. The comment in fb_start says the console gets its code page back 'however the program ends', which Ctrl+C and Ctrl+Break plainly don't do.

## P1-lowering-4 (low): A Const Function limit hit before any call or operation still has no note, though spec 13 now says it points into the function

Status: fixed in f0e7499 (a const function's limit points at a local's declaration, or else the function, when it reached no call or operation), with two cases in tests/compiler/evaluate_tests.cpp

Where: compiler/src/evaluate.cpp:128

A6-9's fix (9a7c3d1) points a limit's error at m_at, the last call, binary, unary, cast, or runtime call reached. MIR assignments and zero_t carry no span, so when nothing with a span came first, m_at is empty and the error has no `note: here`. The finding named zero_of() among the failures with no span, and that case is unchanged: `Local cells: Int64[40000000]` as a Const Function's first statement gives only the error at the call. A step limit in a loop of plain assignments is the same: `While True / x = 1 / End While` reports '...runs more than 10000000 steps' at `Spin()` with no note. Spec 13 now says these errors point 'at where in the Const Function it happened: for a limit, the last call or operation it got to', and evaluate_tests.cpp only covers loops that reach a binary or a call.

```
Import std.core

Const Function Big(): Int
    Local cells: Int64[40000000]
    Return 1
End Function

Const B = Big()
Print(B)
```

Expected: big_local.fb:8:11: error: 'Big' can't run at compile time: it holds more than 256 MiB, with `note: here` at `Local cells: Int64[40000000]` (line 4)

Actual: big_local.fb:8:11: error: 'Big' can't run at compile time: it holds more than 256 MiB, with no note. The same for `Const Function Spin(): Int / Local x = 0 / While True / x = 1 / End While / Return x` with `Const S = Spin()`: '...it runs more than 10000000 steps' and no note.

Suggested fix: Give the evaluator a span for the statement it's running: put a span on the zero_t/assign that declares a Local, or fall back to the frame's function declaration or the last statement's span. Add evaluate_tests cases for a big Local and for an assignment-only loop.

Skeptic: `fb check big_local.fb` (the finding's program) reports only 'big_local.fb:8:11: error: 'Big' can't run at compile time: it holds more than 256 MiB' at `Const B = Big()`, with no note. spin.fb, with only `x = 1` inside `While True`, gives '...it runs more than 10000000 steps' with no note. The control spin2.fb, with `x = x + 1`, does get `spin2.fb:6:13: note: here` at `x = x + 1`. In evaluate.cpp, too_much() and run() pass m_at, which only calls and operations set, so when none came first the note is missing. A6-9 named zero_of() among the failures with no span, and its fix left that case. Spec 13 says the error points 'at where in the Const Function it happened', so a big Local's error should point into the function. Low: the call is still pointed at.

## P1-lowering-5 (low): lane2-lowering.md's second-batch Status lines name commits that aren't on master

Status: fixed in 67b1a09 (lane 2's second-batch status lines name their commits on master)

Where: docs/review/lane2-lowering.md:225

Sixteen Status lines from lane 2's second batch still name the lane branch's commits from before the rebase. `git merge-base --is-ancestor <c> 36ab52a` fails for each, and the same subjects are on master under other hashes: 5039a78 is d6d271c, 592ec85 ea76821, f4a334a a9223f1, bb2b5a2 1e8e8c1, cd89838 d19254f, 73798ad 9a7c3d1, a3143b3 f8cffbc, 2e736be bdde024, 9699c5e 63f7ed9, 99912f3 135d0e9, 32ebe5d c7e6abd, 888ba7d 9b61420, e96e301 e24a9ad, e81551e e732fc0, 2a003b1 de229d1, and 4ab8b33 1448027. Lane 1's file was moved to its master commits (f0b5eec), but lane 2's wasn't, so 'fixed in <commit>' points at commits the history doesn't have. Batch 1's lines (080d511 to 6bc4fb7) are right.

```
// Docs only: no program shows it. Check with git:
//   git -C rev-p1 merge-base --is-ancestor 5039a78 36ab52a   (exit 1: not on master)
//   git -C rev-p1 log --oneline --grep "lock reaches its mutex once" ecaa974..36ab52a   (d6d271c)
Import std.core
Print("see description")
```

Expected: Each Status line names the commit on master that has the fix, as the file's header says ('fixed in <commit>')

Actual: 16 Status lines (A5-6, A6-3, A6-6, A6-7, A6-8, A6-9, A6-10, A7-2, A5-8, A5-9, A6-11, A6-12, A6-13, A6-14, A6-15, A7-3, A7-4, A7-5, A7-6, A7-7, A7-new-1) name lane-branch commits that aren't on master

Suggested fix: Replace each lane-branch hash with its master commit, using the mapping in the description.

Skeptic: I looped over every `Status:` line in docs/review/lane2-lowering.md with `git merge-base --is-ancestor <hash> 36ab52a`. The batch-1 hashes (080d511, 52b9667, 3323788, 58c0bd7, 5a38a39, ff43b91, 9f428a7, fa05eff, 178a50c, 6bc4fb7) are on master. Lines 225 to 584 name 5039a78, 592ec85, f4a334a, bb2b5a2, cd89838, 73798ad (twice), a3143b3, 2e736be (three times), 9699c5e, 99912f3, 32ebe5d (twice), e96e301, e81551e, 2a003b1 (three times), and 4ab8b33, and all of them fail the ancestor check. `git log ecaa974..36ab52a -- compiler/src/lower.cpp` shows the same subjects under master hashes, for example d6d271c for 'lock reaches its mutex once', 9a7c3d1 for 'a const function's limits point...', and 1448027 for 'a const known at compile time gets a global...'. So 'fixed in <commit>' points at commits master's history doesn't contain.

## P1-lowering-6 (low): Releasing Globals at the end has no release-build test, though the fix and its test's comment rely on release builds

Status: fixed in d822eeb (release builds of letting globals go at the end, and of using one after it's gone), with tests/programs/release_globals_released.fb and release_panic_global_released.fb

Where: tests/programs/panic_global_released.fb:2

panic_global_released.fb's comment says the Destroy's use of a released Global 'panics, in release builds too'. A7-1's Status and spec 02 rely on that for a Global with a release-build ready flag. program_tests.cpp builds a file in release mode only when its name starts with 'release_', and neither panic_global_released.fb nor globals_released.fb does. No release_ test covers the new code: generate_main's last-to-first let_go(), the 'released' flag state, or generate(ready_t)'s new 'used after it's released' panic. The finding asked for tests 'in both build modes'. Both programs behave correctly in a release build now, as run by hand, but nothing would catch a release-only regression.

```
// A Destroy that runs when the program ends and uses a Global further down, which the program
// has let go of already, panics, in release builds too, since the Global holds no object then.
Import std.core

Class Early
    Method Destroy()
        Print("early goes, and reads late")
        Print(late.Name)
    End Method
End Class

Class Late
    Field Name = "late"
End Class

Global early = Early()
Global late = Late()
Print("main done")
```

Expected: A release_panic_global_released.fb (and a release_globals_released.fb) in tests/programs, so ctest runs this in a release build

Actual: Only panic_global_released.fb and globals_released.fb exist, and both build in debug only. `fb run --release` of the program above prints 'main done', 'early goes, and reads late', and the panic correctly, but no test checks it.

Suggested fix: Add tests/programs/release_panic_global_released.fb/.out and release_globals_released.fb/.out (copies of the two, built in release).

Skeptic: program_tests.cpp:235-238 builds a program in release mode only when its name starts with 'release_'. tests/programs has release_panic_global_order and release_global_zero, but no release_ copy of globals_released or panic_global_released. So nothing in ctest builds generate_main's last-to-first release, the 'released' ready state, or ready_t's 'used after it's released' panic in a release build. A7-1 asked for tests 'in both build modes', and panic_global_released.fb's comment says the panic happens 'in release builds too'. A manual `fb run --release` of both test programs (copies pgr.fb and gr.fb) matches their .out files today, so the behavior is right and only the test is missing.

## P1-runtime-1 (low): Ctrl+C or Ctrl+Break on a program run directly leaves the console at code page 65001 with VT processing on, despite 'however the program ends'

Status: fixed in 726c4c43 (a program ended by ctrl+c, ctrl+break, or a signal gives the console and stdin back as it found them, and fb reads both streams' modes before it changes either), with CLI tests: tests/native/console_probe sets code page 437, runs fb and the programs it builds (one that ends, one that panics, and one stopped with Ctrl+Break), and checks the code page and both modes are what they were; on Linux and macOS, a program reading a pipe that SIGINT or SIGTERM ends gives the pipe's flags back, which CI runs. The runtime's console handler and signal handlers run the program's end before it stops

Where: runtime/src/runtime.cpp:271

A10-9's fix saves the code page and stderr's mode in fb_start() and restores them through at_end()/run_end(), which only fb_finish() and end_program() call. fb_start's comment, internal.hpp's at_end() doc, and docs/history.md all say this happens 'however the program ends'. Ctrl+C or Ctrl+Break, the usual way to stop a game loop run straight from cmd, ends the process through the default console control handler (ExitProcess with 0xC000013A). That path never reaches run_end, so the shell's console keeps CP 65001 and ENABLE_VIRTUAL_TERMINAL_PROCESSING, which is the state A10-9 set out to fix. Under `fb run` the console comes back only because fb restores its own saved code page. Shown with C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/p1/runtime/launch.c and modes.c: each sets CP 437 and a mode without VT, starts loop.exe in a new process group, sends CTRL_BREAK_EVENT after 1.5 s, then reads the console back. The POSIX side has the same gap: a SIGINT after Input read a pipe leaves stdin O_NONBLOCK. I could not run that here.

```
Import std.core

Print("looping")
Local n: Int64 = 0
While True
    n += 1
End While
```

Expected: Build it with `fb build loop.fb`. In a console set to `chcp 437`, run loop.exe and press Ctrl+C (or Ctrl+Break). Afterwards `chcp` shows 437, and the screen buffer's mode is what it was before (0x3 here).

Actual: launch.exe/modes.exe print `before: cp=437 mode=0x3`, then `after: exit=0xc000013a cp=65001 mode=0x7`. The console stays at UTF-8 with VT processing on after the program is interrupted.

Suggested fix: In fb_start, also install SetConsoleCtrlHandler with a handler that calls fb::runtime::run_end() for CTRL_C_EVENT, CTRL_BREAK_EVENT and CTRL_CLOSE_EVENT and returns FALSE, so the default handler still ends the process with 0xC000013A. On POSIX, add a SIGINT/SIGTERM/SIGHUP handler that runs the restorers, resets the signal to SIG_DFL and re-raises it; restore_stdin only calls fcntl, which is async-signal-safe. Add a test beside the code-page test that Ctrl+Breaks a looping program. If this is out of scope, reword the comments and history to 'at a normal end, a panic, or an uncaught error'.

Skeptic: I built loop.fb (Print, then While True) with `fb build`. My probe, rt-verify/probe.c, sets CP 437 and a screen-buffer mode without VT, starts loop.exe in a new process group with stdout and stderr on CONOUT$, and sends CTRL_BREAK_EVENT after 1.5 s. It printed `before: cp=437 mode=0x3`, `during: cp=65001 mode=0x7`, `after: exit=0xc000013a cp=65001 mode=0x7`. The same probe on hi.exe, which ends normally, gives back cp=437 mode=0x3, so only the interrupted path fails. In the code, run_end() is called only from end_program() and fb_finish() (runtime.cpp, object.cpp). Nothing in runtime/src installs SetConsoleCtrlHandler or a signal handler, so the default handler's ExitProcess skips the restorers. This contradicts docs/decisions.md line 703, the phase 1 row: 'the runtime puts the console back however a program ends ... the terminal is left as it was found'. It also contradicts the fb_start comment, internal.hpp's at_end() doc ('however it ends'), and docs/history.md line 299. The A10-9 finding's main harm is the shell left at CP 65001, and cmd doesn't restore the code page after a child, so the harm is real. I didn't check POSIX.

## P1-runtime-2 (low): fb leaves ENABLE_VIRTUAL_TERMINAL_PROCESSING on: stdout's and stderr's modes are one screen buffer, and the restore runs in save order

Status: fixed in 726c4c43 (a program ended by ctrl+c, ctrl+break, or a signal gives the console and stdin back as it found them, and fb reads both streams' modes before it changes either), with P1-runtime-1's test: prepare_terminal() reads both streams' modes before it sets either

Where: cli/src/terminal.cpp:110

A9-9's fix (same commit as A10-9) saves each stream's console mode in prepare_terminal() and restores them at exit. In a console, stdout and stderr are both the same screen buffer. prepare_terminal() reads stdout's mode (m) and sets m|VT. It then reads stderr's mode, which is now m|VT, and saves that. restore_console() puts stdout's m back first, then stderr's m|VT, so VT processing stays on after every fb command. A9-9's finding names that bit specifically ('ENABLE_VIRTUAL_TERMINAL_PROCESSING also stays set'). The CLI test checks only the code page: console_probe prints GetConsoleOutputCP, and stdout goes to nul. Compiled programs are not affected, since the runtime saves only stderr's mode. Shown with C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/p1/runtime/modes2.c, which runs the command with stdout and stderr on two CONOUT$ handles.

```
Import std.core
Print("hi")
```

Expected: Run `fb --version` or `fb run hi.fb` in a console whose mode is 0x3. The console's mode is 0x3 again after fb exits.

Actual: modes2.exe gives `before: cp=437 mode=0x3` and `after: exit=0x0 cp=437 mode=0x7` for both `fb.exe --version` and `fb.exe run hi.fb`. The same check on hi.exe alone gives 0x3 after.

Suggested fix: Read every stream's mode before changing any, or restore in reverse order (stderr, then stdout) so the first saved value wins. Have console_probe also print the console mode (through CONOUT$) and check it in the code-page test.

Skeptic: Ran my probe (rt-verify/probe.c) on `fb.exe --version`, first with stdout and stderr on one CONOUT$ handle, then on two. Both runs gave `before: cp=437 mode=0x3`, `after: exit=0x0 cp=437 mode=0x7`. hi.exe on its own gives back 0x3, because the runtime saves only stderr's mode. The cause is in cli/src/terminal.cpp's prepare_terminal(). The loop reads stdout's mode m and sets m|VT. It then reads stderr's mode, which is the same screen buffer and now reads m|VT, and saves that. restore_console() puts back m, then m|VT, so the stale value wins. The CLI test only reads GetConsoleOutputCP (console_probe.c prints only out=<cp>), so it can't catch this. One caveat on impact: in my run under cmd.exe (rt-verify/pages3.cmd), cmd had already turned VT on (0x7) before fb ran, so the leftover bit only shows when the parent had VT off. It still breaks terminal.hpp's 'gets its code page and modes back' and decisions.md's 'the terminal is left as it was found', and A9-9 named this bit.

## P1-runtime-3 (low): The leak report's overflow line still has no singular: 'leak: and 1 more cycles'

Status: fixed in 843aec29 (the leak report's last line counts one more cycle in the singular), with tests/programs/leak_cycles_more.fb ("and 1 more cycle") and leak_cycles_many.fb

Where: runtime/src/object.cpp:492

A10-8's fix gave the count lines a singular ('1 Node object was', '1 String was', '1 CString copy of a String was'). report_cycles in the same report still prints 'leak: and %llu more cycles' for any count, so six one-object cycles end with 'and 1 more cycles'. No test covers this line.

```
Import std.core

Class Node
    Field next: Node?
End Class

For i = 1 To 6
    Local n = Node()
    n.next = n
End For
Print("end")
```

Expected: end
leak: 6 Node objects were never released
leak: a cycle of 1 object: Node.next -> the first Node   (5 times)
leak: and 1 more cycle

Actual: end
leak: 6 Node objects were never released
leak: a cycle of 1 object: Node.next -> the first Node   (5 times)
leak: and 1 more cycles

Suggested fix: Choose 'cycle'/'cycles' from cycles - most, as the cycle lines already do, and add a program test (a leak_ program) with six cycles.

Skeptic: Ran rt-verify/cycles6.fb (six Nodes, each pointing at itself) with `fb run` (debug). Output: `end`, `leak: 6 Node objects were never released`, five lines `leak: a cycle of 1 object: Node.next -> the first Node`, then `leak: and 1 more cycles`. The finding's expected output shows '(5 times)', but the report actually prints five separate lines. That detail doesn't affect the point. object.cpp line 492 always prints 'more cycles'. The lines just above choose 'object%s' by count, and A10-8's fix gave singulars to the count lines in the same report but missed this one. Release builds print no cycle report, so this is debug only.

## P1-runtime-4 (low): The new 'an array holds at most 2147483647 elements' panic gives no location, even from Insert, which has one

Status: fixed in bf2a2297 (a full array's panic says where, from add and from insert), with release_panic_array_full.out's place, and release_panic_array_full_insert.fb: fb_array_push takes a location, which codegen gives from the Add

Where: runtime/src/array.cpp:77

A7-2's fix panics in reserve_one() with fb_panic(..., nullptr). Spec 09 says a panic prints `panic: <message> at <file>:<line>:<column>`, and A10-6 was fixed in the same phase because Reserve's out-of-memory panic gave no place. fb_array_insert receives the caller's location but doesn't pass it to reserve_one. fb_array_push has no location parameter at all, though codegen has node.span for ARRAY_ADD. release_panic_array_full.out locks in the bare message.

```
Import std.core

Struct Nothing
End Struct

Local items: Nothing[] = []
items.Reserve(2147483647)
Local added: Int64 = 0
While added < 2147483647
    items.Add(Nothing())
    added += 1
End While

Print("{items.Count} {items.Capacity}")
items.Insert(0, Nothing())
Print("never")
```

Expected: (fb run --release)
2147483647 2147483647
panic: an array holds at most 2147483647 elements at full_insert.fb:15:1

Actual: 2147483647 2147483647
panic: an array holds at most 2147483647 elements
(exit 101, no ' at file:line:column')

Suggested fix: Give reserve_one a location: pass fb_array_insert's location through, and add a location parameter to fb_array_push, filled from ARRAY_ADD's span in codegen (and null from string.cpp's fills). Update release_panic_array_full.out to expect the place.

Skeptic: Ran rt-verify/full_insert.fb with `fb run --release`: Reserve(2147483647), then add 2147483647 empty Structs, then Insert(0, ...). Output: `2147483647 2147483647`, then `panic: an array holds at most 2147483647 elements` with no ' at file:line:column', exit 101. In array.cpp, reserve_one() calls fb_panic(..., nullptr). fb_array_insert has `location` but doesn't pass it to reserve_one, and fb_array_push has no location parameter (codegen declares it as { m_ptr, m_i64 }). Spec 09 line 110 says 'A panic prints `panic: <message> at <file>:<line>:<column>`'. This panic is new in A7-2's fix, unlike the old out-of-memory panics. The same phase fixed A10-6 for exactly this kind of missing place, and release_panic_array_full.out locks in the bare message.

## P1-runtime-5 (low): A7-2's Status line names a3143b3, a commit no branch contains; master's fix is f8cffbc

Status: fixed in f2177141 (the reviews' status lines name master's commits, not their copies from before a rebase), A7-2's line names f8cffbc, and each Status line in docs/review/ that named a commit master doesn't have (17 in lane1-sema.md, 21 in lane2-lowering.md, and 25 in step9.md) names master's commit with the same subject and patch id; a git merge-base --is-ancestor check of every sha a Status line names finds none missing now

Where: docs/review/lane2-lowering.md:339

The Status line says 'fixed in a3143b3'. In the checkout, `git merge-base --is-ancestor a3143b3 36ab52a` fails, and `git for-each-ref --contains a3143b3` lists nothing, in both the review checkout and the main repo. The commit on master with the same message and changes is f8cffbc, probably a rebased copy, so the Status line points at an unreachable commit that a gc can drop. Every other runtime finding's Status commit (e34634d, a9de0e2, efb0195, 9173550, c2a30a1, 6a9c3f4, 9f1a524, c1e8630, 2eabc0c, 8e5ca4a) is on master. The program below is the fix's own test. The defect is in the doc, checked with git.

```
Import std.core

Struct Nothing
End Struct

Local items: Nothing[] = []
Local added: Int64 = 0
While added < 2147483647
    items.Add(Nothing())
    added += 1
End While

Print("{items.Count} {items.Capacity}")
items.Add(Nothing())
Print("never: the array is full")
```

Expected: The Status line names a commit on master: `git merge-base --is-ancestor <sha> master` succeeds.

Actual: `git merge-base --is-ancestor a3143b3 36ab52a` exits 1, and no ref contains a3143b3. f8cffbc ('an array holds at most as many elements as an int counts') is the commit on master.

Suggested fix: Change the Status line to 'fixed in f8cffbc'.

Skeptic: In the review checkout, `git merge-base --is-ancestor a3143b3 36ab52a` exits 1, and `git for-each-ref --contains a3143b3` prints nothing. The same holds in the main repo against master. `git patch-id` gives the same id (c86d03bd...) for a3143b3 and f8cffbc, and f8cffbc is the commit on master. The problem goes beyond A7-2. A scan of every 'fixed in <sha>' in docs/review/ found about 29 more Status lines in lane1-sema.md and lane2-lowering.md that name pre-rebase commits not on master. Examples: 1252d4a is on master as 2ea3568, and 401a85f as bfe0081. Each has a commit with the same subject on master. Lane 3's Status lines are all on master. The fix should rewrite every stale sha, not just A7-2's.

## P1-std-2 (low): A NaN in a PriorityQueue quietly misorders the other values; A12-3's NaN rule stops at the sorted collections

Status: fixed in db37ec2: in the smallest-first order, Add and AddAll panic at the caller at a value that isn't equal to itself; a PriorityQueue with a before function, and Clone, keep their order's rule (panic_priority_queue_nan, panic_priority_queue_nan_all); spec 14's row in c1e5905; decisions.md's NaN row is the coordinator's. T[].Sort() is left as it is: the finding's fix is the PriorityQueue's

Where: std/collections/priority_queue.fb:27

A12-3's fix and its decisions.md row: a key that isn't equal to itself panics where it's added to a SortedMap, SortedSet, or IntervalTree, because 'an order can't place it'. A PriorityQueue built on the same `<` order (its Comparable New) has the same problem and was left alone. A NaN pushed into the heap compares false both ways, so SiftUp leaves smaller values stuck below it, and Pop then gives values out of order with no panic. This is the 'quietly corrupts' failure A12-3 was about, in the one other std collection that keeps an order across operations. T[].Sort() has the same problem: [3, NaN, 1].Sort() leaves [3, nan, 1].

```
Import std.core
Import std.collections

Local zero: Double = 0
Local nan = zero / zero
Local queue = PriorityQueue<Double>()
queue.Add(10)
queue.Add(nan)
queue.Add(20)
queue.Add(1)
Local order: Double[] = []
While queue.Count > 0
    order.Add(queue.Pop())
End While
Print(order)
```

Expected: A panic at the caller where the NaN is added (line 8), as SortedSet.Add gives: 'a PriorityQueue's value can't be NaN, or anything else that isn't equal to itself'. At the least, 1 should come out before 10.

Actual: [10, 1, 20, -nan] in a debug build and [10, 1, 20, nan] in a release build. 1 is popped after 10, and nothing panics.

Suggested fix: Where the PriorityQueue uses its Comparable order (New() without a before), check `value != value` in Add and AddAll, the way sorted.fb's CheckKey does, and panic at the caller. Say so in spec 14's PriorityQueue row and in decisions.md's NaN row, and add a panic_priority_queue_nan.fb test. Or record in decisions.md why a PriorityQueue is exempt.

Skeptic: Ran pq_nan.fb. Debug printed [10, 1, 20, -nan] and release [10, 1, 20, nan], with no panic. Spec 14's PriorityQueue row says Pop gives 'the smallest when T is Comparable', yet 10 comes out while 1 is still in the queue. priority_queue.fb's Add (SiftUp via `a < b`) and AddAll have no `value != value` check. decisions.md:691 and spec 14:235 apply the NaN rule to SortedMap, SortedSet, and IntervalTree only, on the ground that 'an order can't place it'. The same reasoning covers the heap. The code is old, but it borders A12-3's fix, which stopped short of it. I rated it low, not medium: it extends a rule to a structure A12-3 didn't name, and no phase 1 change made it worse.

## P1-std-3 (low): Counter's overflow panic points into std for Add(value) and AddAll, not where the value is added

Status: fixed in 4291c8d: Add(value) and AddAll are @PanicsAtCaller (panic_counter_overflow_one, panic_counter_overflow_all)

Where: std/collections/counter.fb:30

A12-5's fix makes Add(value, times) @PanicsAtCaller. Spec 14's Counter row now says 'a count past what an Int holds panics where it's added'. But the one-argument Add(value), the commonest call, and AddAll aren't @PanicsAtCaller. Both reach the check through `Add(value, 1)` at counter.fb:31, so the panic names std's own line, not the program's. A11-2 complained about this same thing: a panic pointing into std.

```
Import std.core
Import std.collections

Local counts = Counter<String>()
counts["a"] = 2147483647
Print(counts["a"])
counts.Add("a")
Print("not reached")
```

Expected: 2147483647
panic: a Counter's count can't be more than an Int holds, but this one would be 2147483648 at c1.fb:7:1

Actual: 2147483647
panic: a Counter's count can't be more than an Int holds, but this one would be 2147483648 at std/collections/counter.fb:31:9 (debug and release alike). `counts.AddAll(["a"])` gives the same std location.

Suggested fix: Mark Add(value) and AddAll @PanicsAtCaller. Have panic_counter_overflow.fb end with a one-argument Add, so the test pins the location.

Skeptic: Ran c1.fb and c2.fb (AddAll(["a"])) in debug and release. All four runs printed 2147483647, then 'panic: a Counter's count can't be more than an Int holds, but this one would be 2147483648 at std/collections/counter.fb:31:9'. counter.fb:30-32 has `Public Method Add(value: T)` calling `Add(value, 1)` with no @PanicsAtCaller, and AddAll (line 51) has none either, so the panic names std's line. A12-5's new spec text says the overflow 'panics where it's added'. tests/programs/panic_counter_overflow.fb ends with the two-argument Add("a", 1), so it doesn't pin the location for the one-argument form.

## P1-std-5 (low): Sendable's comment, rewritten for A11-11, leaves out a Weak<T> of a Shared Class

Status: fixed in c1e5905 (a comment, so no test)

Where: std/core/interfaces.fb:55

A11-11's fix rewrote Sendable's comment to list what the compiler counts as Sendable: numbers, Bools, Strings, Enums, Callbacks, Channels, Mutexes, Atomics, a Shared Class's objects, and Structs, tuples, and fixed arrays of them. A later phase 1 change (21b60cf) made a Weak<T> of a Shared Class Sendable and added it to spec 10's matching bullet ('and so is a Weak<T> of a Shared Class'). interfaces.fb's list didn't get it, so the comment no longer matches spec 10 or the compiler.

```
Import std.core

Shared Class Hub
    ReadOnly Field Title: String

    New(title: String)
        Title = title
    End New
End Class

Function Send<T: Sendable>(value: T): Int
    Return 1
End Function

Local hub = Hub("a")
Local weak: Weak<Hub> = hub
Print(Send(weak))
```

Expected: It prints 1, and interfaces.fb's Sendable comment lists a Weak<T> of a Shared Class among what's Sendable, as spec 10 does.

Actual: It prints 1, so the compiler counts Weak<Hub> as Sendable, but the comment's list ends at 'a Shared Class's objects' and leaves it out.

Suggested fix: Add it to the comment, for example: '... Atomics, and a Shared Class's objects and Weaks of them, and the Structs, tuples, and fixed arrays ...'.

Skeptic: Ran snd.fb: it prints 1 in both builds, so the compiler counts Weak<Hub> as Sendable. std/core/interfaces.fb:55-58's list (from A11-11's fix, 472e909) reads 'numbers, Bools, Strings, Enums, Callbacks, Channels, Mutexes, Atomics, and a Shared Class's objects, and the Structs, tuples, and fixed arrays ...', with no Weak. Phase 1's 21b60cf (A4-8's fix) added 'and so is a Weak<T> of a Shared Class' to spec 10:299, plus decisions.md:730, but didn't touch interfaces.fb. So the comment A11-11 aligned with spec 10 no longer matches it.

## P1-std-6 (low): Spec 14 says every Class is Equatable and Hashable, but std's Map, Grid, and LinkedList aren't when their Where fails

Status: fixed in c1e5905: spec 14's Equatable sentence, its Map key sentence, and its Map == sentence give spec 07's Where-line exception (docs only, so no test)

Where: docs/spec/14-strings-collections.md:156

A11-5's spec text, as 2185793 rewrote it, says: 'A Struct is when it lists Equatable, and every Class is, ... so enemies.Remove(e) needs nothing more.' The Map bullet says keys need Hashable 'as every Class is unless it has an == of its own without a Hash'. Spec 07 and decisions.md (phase 1 row) carve out an exception: a Class whose == has a Where line, like std's Map, Grid, and LinkedList, implements Equatable and Hashable only as it lists them. Spec 14 leaves that exception out. A few lines on, it says 'a Map of values that aren't Equatable compares as the same object, like any Class', which reads as if such a Map is Equatable by identity. It isn't: == and T[]'s built-in Contains accept it, but IndexOf, Remove, the collections' Contains, and Set<...> refuse it.

```
Import std.core
Import std.collections

Local handlers = Map<String, Func(): Int>()
Local all = [handlers]
Print(all.Contains(handlers), handlers == handlers)
Print(all.IndexOf(handlers) ?? -1)
```

Expected: By spec 14 ('every Class is' Equatable, and such a Map 'compares as the same object, like any Class'), this compiles and prints 'True True' and '0'.

Actual: eq1.fb:7:7: error: Map<String, Func(): Int32> doesn't implement Equatable, which 'IndexOf' needs for T. `Set<Map<String, Func(): Int>>()` likewise fails: it doesn't implement Hashable.

Suggested fix: Add spec 07's exception to both spec 14 sentences, for example: '... and every Class is, unless its == has a Where line, as a Map's, a Grid's, and a LinkedList's do, which are Equatable only where it holds'. Say the same where the Map bullet gives Hashable.

Skeptic: Ran eq1.fb. Both builds fail with 'eq1.fb:7:7: error: Map<String, Func(): Int32> doesn't implement Equatable, which 'IndexOf' needs for T'. map.fb:399-402 gives Map its == through `Extend Map<K, V> Implements Equatable / Where V: Equatable`. Spec 07:216-217 and decisions.md:723 (phase 1) say a Class whose == has a Where line implements Equatable and Hashable only as it lists them. Spec 14:155-162, rewritten by the same commit (2185793), says 'every Class is [Equatable], comparing its objects themselves without an == of its own'. It says Map keys need Hashable 'as every Class is unless it has an == of its own without a Hash', and that a Map of non-Equatable values 'compares as the same object, like any Class'. None of the three mentions the Where-line exception, which covers std's own Map, Grid, and LinkedList. So spec 14 contradicts spec 07 and the compiler for exactly these types.

## A-new-5 (medium): A Map panics when a value's Destroy, run by Remove, removes another key from it

Status: fixed in de04d77: Take, Clear, Add replacing a value, and Update hold what they let go of in a Local, so it goes as the Method returns, once the Map is whole again (map_destroy, which fails on master with the finding's panic)

Where: std/collections/map.fb:352

Found by step 18's review (S18-collections-2's skeptic): a plain std.collections Map whose value's Destroy, run by Remove, removes another key from the same Map panics with "index -1 is out of range for 4 elements at std/collections/map.fb:352:15", in Unlink. Take's Move overwrites the removed entry while the bucket chains are half relinked, and the Destroy that lets go of it walks a chain that's broken for the moment. Clear lets go of the entries while the buckets still point at them, and Add and Update let go of a replaced value in the middle of the Method. A value's Destroy should be able to use the Map, as the node that removes its child's key does.
