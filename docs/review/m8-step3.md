# The review of M8 step 3 (debug info and stack traces)

21 confirmed findings, each with its Status.

## dbg-1 (high): A line breakpoint misses whenever the line's lowest address is in a branch that isn't taken

compiler/src/codegen.cpp:3595. The panic/ok blocks of every overflow, bounds, and divide check are appended at the end of the LLVM function (LLVMAppendBasicBlockInContext), so the code that follows a check lands after the MIR blocks generated later. In memory, a line's code is then out of source order. gdb resolves a file:line breakpoint to the lowest address that has the line, and lldb, which CodeLLDB uses, also keeps one location per block and function, the lowest. On a line like `If x Then ...` that comes after any arithmetic, that lowest address is inside the Then body. When the condition is false, the breakpoint never stops, although stepping shows the line running. This is the step's done-when ('a debugger steps through a program'): in a debug build, any line after `a + b` or `xs[i]` that branches can't take a breakpoint.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/h.fb:
  Function Check(n: Int)
      Local total = n + 1
      If total > 5 Then Print("big")
      Print("total {total}")
  End Function
  Check(1)
fb build h.fb -o h.exe; gdb -batch with 'break h.fb:5', 'break h.fb:6', 'run'. llvm-objdump -d -l h.exe shows line 4's code ending in `jo panic; jmp ok(+0x107)` and line 5's first address (+0x20) being the Print in the Then body. g.fb shows it again: a line with `n ?? 0` on a Null n, after `arr[1] = 5`.

Expected: The program stops at line 5 (the If), then at line 6.

Actual: Breakpoint 1 is set at Check+32, which is the Then body. The program stops only at line 6 ('Thread 1 hit Breakpoint 2 ... at h.fb:6'), and line 5's breakpoint never stops. In g.fb a breakpoint at line 17 never stops, and the program runs to exit.

Skeptic: I rebuilt h.fb in v/ and ran gdb with 'break h.fb:5', 'break h.fb:6', 'run'. Breakpoint 1 went to 0x140001660, and the program stopped only at 'Thread 1 hit Breakpoint 2, Check (n=1) at h.fb:6'. In llvm-objdump -d -l, line 4 ends in 'jo 0x...734; jmp 0x...747', and line 5's lowest address, +0x20, is the Then body's fb_array_new/Print. The real compare (cmpl $0x5; jne +0x20) is at +0x117, after the panic block. In codegen.cpp, panic_if (line 3594) appends both 'panic' and 'ok' with LLVMAppendBasicBlockInContext, so this happens after every checked operation. So in a debug build, a line breakpoint on an If after arithmetic misses whenever the condition is false. That breaks the done-when 'steps through a program at a breakpoint'.

Fix: Put each check's ok block, and its panic block, right after the block being generated (LLVMInsertBasicBlockInContext before the next block, or LLVMMoveBasicBlockAfter), so a line's code stays in source order. Add a debugger test that breaks on a line with a false If after arithmetic.

Status: fixed: the blocks a statement's code adds go before the next MIR block's (codegen's add_block()), so a line's code is in source order; tests/compiler/debug_info_tests.cpp checks the check's panic and ok blocks come before bb1, and gdb now stops at h.fb:5

## dbg-2 (medium): Same-named Locals in sibling blocks share one scope, so the debugger shows the wrong one (Catch e, For i)

compiler/src/codegen.cpp:2720. declare_variables puts every named Local in the subprogram's scope, with no DILexicalBlock for the block it's declared in. The language lets sibling blocks reuse a name, as in `Catch e As FileError ... Catch e As ParseError` or two `For i` loops in one function. Each one becomes a separate variable of the same name in the same scope. The debugger then picks the first one everywhere. In the second Catch it shows the first Catch's stale error, with the wrong type.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/c.fb: Report(kind) has `Catch e As FileError` and then `Catch e As ParseError` (line 25 prints e.line). gdb: break c.fb:25, run, info locals, print e, print e->line. Also a.fb: two sibling `For i` loops, each with `Local x`. Break in the second loop (a.fb:21) and print x and i.

Expected: In the second Catch, e is the ParseError and e->line is 77. In a.fb's second loop, x = 15 and i = 5.

Actual: info locals lists two variables named e. `print e` gives '(FileError) 0x42d470', the first Catch's error from Report(1), and `print e->line` fails with 'There is no member named line.' In a.fb, `print x` gives 2 and `print i` gives 1, the first loop's values, while info locals also shows i = 5 and x = 15.

Skeptic: c.exe in gdb at c.fb:25 (the second Catch, Report(2)): info locals shows 'e = 0x57d450' and 'e = 0x57d9c0'. 'print e' gives '(FileError) 0x57d450', and 'print e->line' fails with 'There is no member named line.' a.exe at a.fb:21 (the second For): info locals lists i = 1, x = 2 and i = 5, x = 15, and 'print x' gives 2 and 'print i' gives 1, the first loop's values. declare_variables scopes every Local to m_subprogram, with no DILexicalBlock, so the debugger shows the wrong Local whenever a name is reused in sibling blocks.

Fix: Make a DILexicalBlock for each MIR scope that declares Locals: lower records the scope's span, and declare_variables uses it as each variable's scope and as the scope of the code's locations. Or at least for blocks that reuse a name.

Status: fixed: lowering numbers each scope of the source (mir_function_t::scopes, mir_place_t, local_t::scope), a For's too, whose variable comes before its body's scope, and codegen makes a DILexicalBlock for each, the Locals' scope and their code's; debug_info_tests checks two Fors' i and x and two Catches' e each have a lexical block of their own, and gdb shows the ParseError's e and the second loop's i and x

## dbg-3 (medium): Code with no span takes the line of whatever block was generated just before it, so stepping shows lines that don't run

compiler/src/codegen.cpp:2703. locate() keeps m_line across blocks in the order they're generated, not in control-flow order. When a block starts with a statement or terminator that has no span, like an If's join block, its `br` gets the line of the last block generated, which is the Else body. Stepping through the Then branch then shows the Else line as if it ran. The same location goes into the trace table for any call with no span at the start of a block.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/d.fb: Work(n) has a For loop over `If i % 2 == 0 / total += i / Else / total -= 1 / End If`. gdb: break Work, run, then `next` repeatedly. `fb check --show llvm d.fb` shows `bb5: br label %bb6, !dbg !23`, where !23 is line 10, the Else.

Expected: For i = 2 the steps go 6, 7, 8, 6.

Actual: For i = 2 the steps go 6, 7, 8, 10 ('10  total -= 1'), then 6. Line 10 doesn't run for i = 2. The If's join block has the Else line's location.

Skeptic: d.exe in gdb, 'break Work' and then 'next' repeatedly, steps 6, 7, 8, 10, 6 for i = 2: line 10 ('total -= 1') shows although the Else doesn't run. In `fb check --show llvm d.fb`, the join block is 'bb5: br label %bb6, !dbg !23', and !23 is DILocation(line: 10, column: 13). locate() (codegen.cpp ~2703) keeps m_line from the last block generated when a statement or terminator has no span, so the join's br gets the Else body's line.

Fix: Start each block's m_line fresh, with no location or line 0, or with the block's first spanned statement, or carry the location from the block's predecessor, instead of the last block generated. Lower could also give join blocks' gotos the span of the statement they end.

Status: fixed: each block's code starts at line 0 (start_block_line()) until a statement with a place, so an If's join has no line; debug_info_tests checks no block that is only a jump has the Else's line, and d.fb steps 6, 7, 8, 6

## dbg-4 (medium): A beside library built from source has no compile directory, so a debugger can't find its source or bind full-path breakpoints in it

compiler/src/driver.cpp:3106. build_library makes its codegen_options_t without .folder, and names files by their paths in the program, like 'mylib/tools.fb'. Its compile units then have a relative filename and an empty directory. A beside library built from source as part of `fb build` is in this state, and a prebuilt one is too. A debugger can't open the file. A breakpoint set by absolute path, which is how VS Code and CodeLLDB send them, never binds. A source dependency compiled into the program gets an absolute path and works.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/bs/: mylib (type = "library", tools.fb with Triple) and app (mylib = { path = "../mylib", link = "beside" }). In bs/app: fb build -o app.exe. gdb: 'break tools.fb:5', run, list. Also 'break C:/.../bs/mylib/tools.fb:5', run. Compare pk/game with the physics path dependency, where 'break C:/.../pk/physics/shapes/circle.fb:7' works.

Expected: gdb lists tools.fb's lines, and the full-path breakpoint stops in Triple, as it does for a source dependency.

Actual: 'warning: 5 mylib/tools.fb: No such file or directory'; 'info source' shows no compilation directory. The full-path breakpoint stays pending ('No source file named C:/.../bs/mylib/tools.fb'), and the program runs to exit.

Skeptic: I copied bs/ into v/bs and ran fb build -o app.exe in app ('Built mylib.dll beside the executable'). With pending breakpoints on, 'break tools.fb:5' stops at 'mylib.tools.Triple (n=4) at mylib/tools.fb:5' and then prints 'warning: 5 mylib/tools.fb: No such file or directory'. 'info source' gives 'Current source file is mylib/tools.fb' and no compilation directory. A breakpoint on the absolute path .../v/bs/mylib/tools.fb:5 stays pending, and the program prints 12 and exits normally. In driver.cpp, build_library (~3106) builds codegen_options_t{ .mode = mode } with only program-relative .paths and no .folder.

Fix: When the library is built from source on this machine, give its debug info the files' disk paths, or set the compile directory to the folder the program-relative paths resolve from. For a prebuilt library, a debugger source map could cover the gap. Add a debugger check that breaks in a beside library.

Status: fixed: a file panics name by another path than its own, as a library's build names its modules, gets the folder that path is from where the file is (folder_of()), and a library beside the executable built from its source for the program's build gets the folder fb runs in (build_library's new folder); debug_info_tests checks DIFile(filename: "tools.fb", directory: "/work/game/lib"), and debugger_tests checks a beside library built from source has its files' folder. A prebuilt library's build keeps no folder, as its build has to be the same wherever it's made (cli_tests' 'a prebuilt library's binary runs its code as its source would' checks that); a debugger finds its files through a source map

## tools-1 (medium): An object seen through its base Class or an Interface shows only the base's Fields when its own Class's debug type was never emitted

tools/lldb/futureblitz.py:230. ObjectProvider.update looks up the object's real Class with FindFirstType("fb." + name). codegen's debug_type() only makes a Class's DWARF struct when some Local, parameter (Me included), or field has that type. A Class with no Methods and no New that is only ever held through its base (or an Interface) gets no fb.<Class> struct, so the lookup fails and the script quietly falls back to the base's struct. The module docstring promises the opposite: "an object seen through a Class it extends shows them all". lldb here can't run Python (python311.dll is missing), so I showed this from the DWARF, not in lldb.

Shown by: scratchpad/review-m8s3/tools/sub.fb: `Class Shape / Field Name: String`, `Class Circle Extends Shape / Field Radius: Float64`, `Local shape: Shape = Circle()`, `shape.Name = "wheel"`, `Print(shape.Name)`. Built with `fb.exe build sub.fb -o sub.exe`, then dumped with `objdump --dwarf=info sub.exe | grep 'DW_AT_name.*: fb\.'` (MinGW objdump).

Expected: There is an fb.Circle struct in the DWARF, so `frame variable shape` in lldb shows Name and Radius.

Actual: The DWARF has fb.Shape, fb.String, fb.String[], and only the functions fb.Circle.release and fb.Circle.visit, with no fb.Circle struct. FindFirstType("fb.Circle") finds nothing, so shape shows only Name. Radius is missing.

Skeptic: I built scratchpad/review-m8s3/v-tools/sub.fb (Shape, then Circle Extends Shape, then `Local shape: Shape = Circle()`). It printed 'wheel'. `objdump --dwarf=info sub.exe` shows structs fb.Shape, fb.String and fb.String[], and of Circle only the functions fb.Circle.release and fb.Circle.visit. There is no fb.Circle struct. In codegen.cpp, debug_type() is called only from declare_variables (local.type) and from its own recursion into field and inner types, so a Class that no Local, parameter or field has as its type never gets a struct. ObjectProvider.update's FindFirstType("fb.Circle") then fails and the script falls back to Shape's struct. That contradicts the module docstring's promise that an object seen through a Class it extends shows them all, and tooling.md's 'Classes as their values'.

Fix: Emit a debug struct for every Class the module defines, e.g. in finish_debug_info or wherever class_value() is made, not only on demand from Locals. Then add a debugger_tests case where the derived Class is never a Local's type.

Status: fixed: every Class's struct is in the debug info, through an artificial parameter of its release function; debug_info_tests checks fb.main.Circle is there when no variable has Circle's type, and the debugger test shows a Circle held only as a Shape

## tools-2 (medium): Same-named Classes in two modules share one debug struct name, so the script can pick the wrong one's Fields

tools/lldb/futureblitz.py:230. debug_struct names a Class's struct "fb." + types->name(type), and fb_class_t.name is the same short name. So lib.a.Cat and lib.b.Cat are both "fb.Cat" in the DWARF. ObjectProvider asks FindFirstType("fb.Cat") and keeps the result whenever it has at least as many fields as the reference's own struct. An a.Cat object (one Int32 Field, 24 bytes) can then be shown with b.Cat's layout: Name, Age, and Owner, read from memory past the end of the object. Which of the two lldb returns depends on DWARF order, which follows the order the Locals are declared. This is shown from the DWARF only, because lldb here can't run Python.

Shown by: scratchpad/review-m8s3/tools/same/: lib/a.fb has `Public Class Cat` with `Public Field Lives: Int32` and MakeA(); lib/b.fb has `Public Class Cat` with Fields Name: String, Age: Int64, Owner: String and MakeB(). main2.fb is `Local y = MakeB()` then `Local x = MakeA()`. Built with `fb.exe build main2.fb -o main2.exe`, then `objdump --dwarf=info main2.exe | grep -E 'DW_AT_name.*: (fb\.Cat|Lives|Name|Age|Owner)$'`.

Expected: Each Class's debug struct has a unique name (e.g. its qualified_name, as object_type()'s LLVM struct already uses), and the script matches the object's Class by that name (fb_class_t.full_name).

Actual: There are two DW_TAG_structure_type entries named fb.Cat: first the 5-member one (Name, Age, Owner), then the 3-member one (Lives). For x, FindFirstType("fb.Cat") would return the 5-member struct, which passes the `>=` check.

Skeptic: I rebuilt main2.fb with lib/a.fb (Cat: Lives) and lib/b.fb (Cat: Name, Age, Owner) under v-tools/same. It ran and printed '0 0'. objdump shows two DW_TAG_structure_type entries both named fb.Cat: the first is 40 bytes with header.counts, header.class, Name, Age and Owner, the second is 24 bytes with header.counts, header.class and Lives. debug_struct names a struct "fb." + types->name(type), the short name, and _class_name reads fb_class_t.name (offset 0), which is also short. ObjectProvider accepts the found struct whenever it has at least as many fields (`>=`), so x (an a.Cat, 3 fields) can be laid out as b.Cat's 5-field, 40-byte struct, reading past its 24-byte object.

Fix: Name the debug structs by qualified_name (fb.lib.a.Cat), and look up "fb." + the class's full_name read from fb_class_t at offset 8 (falling back to name). Or compare the struct's byte size with fb_class_t.size before accepting it.

Status: fixed: a Class's struct is "fb." and its full name, as its fb_class_t's full_name, and the script looks that up and takes it only when its size is the fb_class_t's; debug_info_tests checks fb.lib.a.Cat and fb.lib.b.Cat

## trace-1 (medium): Release builds still drop recursive frames: tail-call elimination runs before disable-tail-calls is set

compiler/src/trace.cpp:338. fill_trace_table() puts "disable-tail-calls"="true" on each function only after run_passes() has run default<O2>. The backend obeys it, but the middle end's TailCallElim pass checks the same attribute and has already run without it. So in a release build, self-recursion becomes a loop, including accumulator recursion like `n + Sum(n - 1)`, and every recursive frame is gone from the trace. This breaks spec 09's "no call is made a tail call, which would take its caller out of the trace" and the user's call that no build makes tail calls. release_panic_trace_inlined only checks a sibling tail call, which the backend does keep, so no test catches this.

Shown by: scratchpad/review-m8s3/trace/tailrec.fb:
  Function Down(n: Int): Int
      If n == 0 Then Panic("bottom")
      Return Down(n - 1)
  End Function
  Function Start(): Int
      Local x = Down(3)
      Return x + 1
  End Function
  Print(Start())
fb build tailrec.fb -o d.exe; fb build tailrec.fb --release -o r.exe. The same happens with accum.fb (`Return n + Sum(n - 1)`, n read from Input()): `echo xxx | accum_r.exe`.

Expected: Release prints the same trace as debug: at Down (tailrec.fb:4), three lines of at Down (tailrec.fb:5), at Start (tailrec.fb:9), at top-level code (tailrec.fb:13).

Actual: Debug prints all four Down frames. Release prints only `at Down (tailrec.fb:4)`, `at Start (tailrec.fb:9)`, `at top-level code (tailrec.fb:13)`. accum.fb in release prints only `at Sum (accum.fb:4)` and `at top-level code (accum.fb:8)`.

Skeptic: I rebuilt tailrec.fb and accum.fb in debug and release in refute-trace/. tailrec_d.exe prints Down:4, Down:5 three times, Start:9, top-level:13. tailrec_r.exe prints only `at Down (tailrec.fb:4)`, `at Start (tailrec.fb:9)`, `at top-level code (tailrec.fb:13)`. With `echo xxx`, accum_r.exe prints only Sum:4 and top-level:8, while debug prints four Sum frames. In codegen.cpp, run_passes (line 435) runs before fill_trace_table (line 452), and fill_trace_table is where disable-tail-calls is added (compiler/src/trace.cpp:337-338). So the middle end's TailCallElim never sees the attribute and turns the recursion into a loop. That breaks spec 09:147 ('no call is made a tail call, which would take its caller out of the trace') and the user's call that no build makes tail calls.

Fix: Put disable-tail-calls on each defined function before the optimizer runs, for example when functions are declared or in add_trace_table(), keeping the musttail exception. Or leave tailcallelim out of the release pipeline. Add a release_panic_trace_ program with self-recursion and accumulator recursion.

Status: fixed: keep_frames() puts disable-tail-calls and uwtable on every function before the passes run (run_passes()), and again before the markers; release_panic_trace_recursion and release_panic_trace_accumulate (with a .in) print every Down and Sum frame

## trace-2 (medium): A frame whose call has line 0 is left out of the trace, so merged calls in release lose their function

runtime/src/trace.cpp:203. write_trace() skips any frame whose line is 0 and goes on to its parent. In a release build, when SimplifyCFG merges or sinks identical calls written on different lines into one, the merged call gets a line-0 location in the right function's scope. That function then vanishes from the trace, though it is on the stack and named in the table. Spec 09 says a call a release build inlines "still has its line of its own", and the trace should have a line for each call of the program's code.

Shown by: scratchpad/review-m8s3/trace/m2_in.fb: Pick(flag, b, x) calls b.Check(x) in each of three If/ElseIf/Else branches (lines 16, 18, 20). Go(f) calls Pick (line 25). The top level runs Go((Input() ?? "").Chars.Count), and Big.Check panics. fb build m2_in.fb [--release] -o m2_in[_r].exe; echo x | m2_in_r.exe (also xx and xxx).

Expected: A line for Pick in every build, e.g. `at Pick (m2_in.fb:16)`, or at least `at Pick (m2_in.fb)` when the line is unknown.

Actual: Debug prints `at Pick (m2_in.fb:16/18/20)` for the three inputs. Release prints `at Big.Check (m2_in.fb:9)` then `at Go (m2_in.fb:25)`, with no Pick frame for any input.

Skeptic: I rebuilt m2_in.fb. For inputs x, xx and xxx, debug prints `at Pick (m2_in.fb:16/18/20)` between Big.Check:9 and Go:25. Release prints `at Big.Check (m2_in.fb:9)` then `at Go (m2_in.fb:25)` for all three inputs, with no Pick frame, though Pick is on the stack. runtime/src/trace.cpp:203 does `if (place.line == 0) continue;`, which drops a named frame and moves on to its parent. That fits what I saw: merged calls get a line-0 location. Spec 09 asks for 'a line for each call of the program's code that it's in' and says an inlined call 'still has its line of its own'; losing the function contradicts that.

Fix: Don't drop a named frame because its line is 0. Print it without a line, e.g. `    at Pick (m2_in.fb)`. Keep skipping only what has no name or is the compiler's own artificial code, such as functions finish_debug_info() gives a line-0 subprogram (it could mark those frames with an empty name). Add a release trace test where branches' calls merge.

Status: fixed: a frame with a name and line 0 shows as "    at Pick (file)", and only the compiler's own code, whose subprogram is at line 0, has no name and is left out; release_panic_trace_merged shows Pick

## dbg-5 (low): A Local captured by a lambda shows up as its Cell<T> box instead of its value

compiler/src/codegen.cpp:2916. A Local that a lambda captures lives in a Cell object, and declare_variables describes it with the slot's type. In the function, and in the lambda's own 'count' variable, the debugger shows a Cell<Int32> pointer whose value is in a member named 'value', not an Int. tools/lldb/futureblitz.py doesn't special-case Cell, so VS Code would show it as an object.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/k.fb: `Local limit = 10 / Local scaled = Function(x: Int) => x * limit / limit = 100 / Print("{scaled(2)}")`. gdb: break k.fb:6, run, info locals, whatis limit, print *limit. Also b.fb: MakeCounter's lambda (break b.fb:8) shows count = 0x5dd470.

Expected: limit = 100, of type Int32.

Actual: 'limit = 0x5dd470'; 'type = Cell<Int32>'; print *limit gives {header.counts = 2, header.class = ..., value = 100}.

Skeptic: k.exe in gdb at k.fb:6: info locals gives 'limit = 0x60d450', 'whatis limit' gives 'type = Cell<Int32>', and 'print *limit' gives {header.counts = 2, header.class = ...<fb.Cell<Int32>.class>, value = 100}. tools/lldb/futureblitz.py has no handling for Cell, so it would show limit as an object with a 'value' child. The Local's type and value are wrong in the debugger, but the value can still be reached by expanding it, so I put this at low.

Fix: For a Local whose type is a capture Cell, describe the variable with the inner type and DIExpression(DW_OP_deref, DW_OP_plus_uconst <value's offset>). Or have the lldb script unwrap Cell.

Status: fixed: a Local whose type is a capture's Cell<T> is described as its T, with DW_OP_deref, DW_OP_plus_uconst to the value; debug_info_tests checks limit is an Int32 there, and gdb shows limit = 100

## dbg-6 (low): In a debug build, an Async Function's parameters and Locals that aren't used after an Await show as <optimized out> once it resumes

compiler/src/codegen.cpp:4108. Coroutine splitting moves into the frame only the allocas that live across a suspend. A parameter or Local that's last used before an Await stays an alloca in the ramp function. In the .resume clone, its dbg.declare has nothing to point at, so the debugger can't show it after the first Await, even in a debug build.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/f.fb: `Async Function Describe(name: String, count: Int): String` loops `For i = 1 To count`, with `Local value = Await Sum(i * 10)` and `parts = parts + value` (line 15). gdb: break f.fb:15, run, info args.

Expected: count = 2 at line 15 in a debug build.

Actual: 'count = <optimized out>' at both stops. name, i, parts, and value show.

Skeptic: f.exe (debug build) in gdb at f.fb:15: both stops show 'fb.Describe(String, Int32).resume (task=..., name=..., count=<optimized out>)', and info args shows 'count = <optimized out>', while i, parts, value, and name show. So a parameter isn't shown in a debug build after an Await. It only affects Async Functions' slots that aren't used after the suspend, so I put it at low.

Fix: In debug builds, keep every named Local and parameter of an Async Function in the coroutine frame. One way is to use each slot after every suspend point, for example with a volatile load or an llvm.fake.use, so CoroFrame spills it.

Status: fixed: in a debug build, an Async Function's coroutine reads each named variable at its end, with a volatile load, so the frame keeps it; debug_info_tests checks the .resume piece declares count, and gdb shows count = 2 after the Await

## dbg-7 (low): After an Async Function resumes, the debugger names its frames 'fb.Run().resume', not 'Run'

compiler/src/codegen.cpp:2646. begin_subprogram leaves out a linkage name on purpose, so debuggers show the FB name. CoroSplit then sets the clones' linkageName to the LLVM function's name ('fb.Run().resume', '.destroy', '.cleanup'), and debuggers show that instead. Stack traces read DW name and are unaffected.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s3/dbg/b.fb (Async Run awaits Later): gdb break b.fb:36, run, bt. f.fb: break f.fb:15. `fb check --show llvm b.fb` shows '!DISubprogram(name: "Run", linkageName: "fb.Run().resume", ...)'.

Expected: The frame is named 'Run' ('Describe' in f.fb), as before the first Await.

Actual: '#0  fb.Run().resume (task=0x5dd740) at b.fb:36' and '#0  fb.Describe(String, Int32).resume (task=..., name=..., count=<optimized out>)'.

Skeptic: In f.exe's gdb run, the frame at f.fb:15 is named 'fb.Describe(String, Int32).resume', not 'Describe'. begin_subprogram passes an empty linkage name on purpose ('Without a linkage name, which debuggers would show instead'), and CoroSplit's clones bring the LLVM symbol name back, which defeats that intent. It's cosmetic, and the trace table is unaffected.

Fix: After optimization or coroutine splitting, replace the clones' subprograms' linkage names with the FB name, or with none (DISubprogram::replaceLinkageName through a small C++ shim, or by rebuilding the subprogram). Or set linkageName to the debug name up front, if CoroSplit keeps it.

Status: fixed: after the passes, a subprogram of the program's code has no linkage name again (drop_linkage_names()); debug_info_tests checks there's no linkageName "fb.Describe(String, Int32)...", and gdb names the frame Describe

## tools-3 (low): macOS release builds lose their line tables, which spec 09 says release builds have

compiler/src/driver.cpp:3449. Codegen emits LLVMDWARFEmissionLineTablesOnly in release. On Mach-O, linking leaves the DWARF in the object files, and build_program deletes the temporary folder that holds program.o and beside{i}.o. write_debug_symbols (dsymutil) runs only when mode == DEBUG, so a macOS release executable and its beside libraries end up with no line info. Spec 09's new Stack traces section says "release builds have the line tables". I found this by reading the code; it can't be run here.

Shown by: compiler/src/driver.cpp build_program: `.and_then([&] { ... return mode == build_mode_t::DEBUG ? write_debug_symbols(binaries) : std::expected<void, std::string>(); })`, then `remove_temporary(folder)`. docs/spec/09-errors.md: "release builds have the line tables".

Expected: On macOS a release build also gets a .dSYM (or the spec says macOS release builds have no line tables), so a debugger or crash symbolication of a release build shows lines.

Actual: Only debug builds run dsymutil. A release build's line tables go with the deleted object files.

Skeptic: From the code: codegen.cpp:2574 emits LLVMDWARFEmissionLineTablesOnly for release. driver.cpp build_program puts program.o and beside{i}.o in a temporary folder that remove_temporary(folder) deletes, and at line 3449 it runs write_debug_symbols (dsymutil) only when mode == DEBUG. On Mach-O the DWARF stays in the .o files, which the executable refers to only through its debug map, so a macOS release binary is left with no reachable line tables. docs/spec/09-errors.md line 150 says 'release builds have the line tables', and tooling.md says 'line tables in release'. I rate it low because stack traces use fb's own table and still work. Only a debugger or symbolication of a macOS release build loses lines.

Fix: Run write_debug_symbols for release builds too on Apple targets (dsymutil on a line-tables-only build is cheap), or reword the spec.

Status: fixed: dsymutil runs for release builds on macOS too, and the spec says a build's debug info is in its .dSYM there; debugger_tests checks a .dSYM beside a debug and a release build, on macOS (not run here)

## tools-4 (low): The trace filter drops a normal program's own '    at ' lines after any line that merely contains 'uncaught ' or 'panic: '

tools/verify.py:134. without_trace (verify.py, and the same code in program_tests.cpp line 92) starts dropping lines that begin with "    at " after any line that contains "panic: " or "uncaught " anywhere, even in a program that exits normally. It is applied to every program not named for traces, but never to the .out file, so such a program's test fails even when its output is right.

Shown by: scratchpad/review-m8s3/tools/uncaught_word.fb: `Import std.core`, `Print("uncaught errors: 0")`, `Print("    at most 3 retries")`. Built and run (exit 0), then `python -c "import verify; print(verify.without_trace(open('uw.out','rb').read()))"` with tools/ on sys.path.

Expected: b'uncaught errors: 0\n    at most 3 retries\n' is compared unchanged, since the program neither panicked nor let an error out.

Actual: without_trace gives b'uncaught errors: 0\n'. The program's second line is dropped, so its correct .out would never match.

Skeptic: I ran verify.without_trace(b'uncaught errors: 0\n    at most 3 retries\n') with tools/ on sys.path and bytecode writing off. It returned b'uncaught errors: 0\n'. Both verify.py's without_trace and program_tests.cpp's without_trace (lines 85-104) start dropping after any line that merely contains 'panic: ' or 'uncaught '. They are applied to every program not named for traces, but never to the .out, so a normally exiting program whose output looks like this could never match its correct .out.

Fix: Only filter for programs whose names start with panic_ or uncaught_ (after release_), and start dropping only after a line that matches the runtime's report, e.g. a line starting (after escapes) with "panic: " or "uncaught ". Do the same in program_tests.cpp, and add the case to verify_tests.py.

Status: fixed: only a program or package whose name says it panics or lets an error out has its trace left out, and only after a line that is the report, after any terminal escapes, in program_tests.cpp and verify.py; print_at_lines prints lines like a trace's and ends normally, and verify_tests checks both rules

## tools-5 (low): The debugger test can't fail on the subclass case it claims to check, and never steps

tests/cli/debugger_tests.cpp:119. Under the comment "The Shape is a Circle, whose Fields it shows", the test checks shown.contains("Name = \"wheel\"") and shown.contains("Radius = 2.5"). The program also has `Local round = Circle()` pointing at the same object, and frame variable prints round's own Circle Fields. So both CHECKs pass even if shape showed only Shape's Fields. That is how tools-1 slipped through. Also, the milestones row's done-when is "a debugger steps through a program and shows its Locals", but the test only stops at one breakpoint and runs frame variable. It never runs next or step.

Shown by: tests/cli/debugger_tests.cpp's source (Locals round: Circle and shape: Shape = round) and its CHECKs at lines 116-119. The lldb commands are only breakpoint set / run / frame variable / kill.

Expected: The test checks shape's children on their own (e.g. `frame variable shape`, using a Class held only as its base), and steps at least once (next / step into a Function) and checks the line it lands on.

Actual: The subclass check is satisfied by round. No stepping is tested.

Skeptic: In tests/cli/debugger_tests.cpp, the program has `Local round = Circle()` (with Radius 2.5) and `Local shape: Shape = round`. The test runs only 'frame variable', so round's own Circle children print Name = "wheel" and Radius = 2.5. That means the checks at lines 118-119 pass even if shape showed only Shape's Fields. 'shape = Circle' comes from the summary, which reads the header's class name and doesn't depend on the struct. This is how tools-1 got through. The lldb commands are breakpoint set, run, frame variable and kill, with no next or step, although milestones.md row 3's done-when is 'a debugger steps through a program and shows its Locals'.

Fix: Print `frame variable shape` separately and check Radius in that output, using a Circle that no Local has as its type. Add `next` and `step` commands and check the stop lines.

Status: fixed: the debugger test's Circle has no Method and no variable of its type, `frame variable shape` is checked on its own, and the test steps over a line and into Describe, checking where each stops. It skips here, where lldb can't run Python, and runs on the user's Mac

## tools-7 (low): The lldb Python probe looks for the substring '42' in whatever lldb printed, including a crash dump

tests/cli/debugger_tests.cpp:78. The skip check is run_lldb({"--batch","-o","script print(6 * 7)"}).contains("42"). Where lldb has no Python, as on this machine, lldb crashes and prints a stack dump of hex addresses and module offsets, which ASLR changes from boot to boot. If any of them contains "42", the test won't skip; it runs and its CHECKs fail. The exit code is ignored too.

Shown by: `"C:/Program Files/LLVM/bin/lldb.exe" --batch -o "script print(6 * 7)"` (the lldb ctest found here). It prints "error: unable to find 'python311.dll'" and then a stack dump like `#1 0x00007ff6fba48dcb (...lldb.exe+0x38dcb)`. ctest -I 913,913 skips today only because no address happens to contain 42.

Expected: The probe accepts only a clean run that printed a line that is exactly 42 (e.g. exit code 0 and a "\n42\n" / starts-with check).

Actual: Any 42 anywhere in the crash output counts as "lldb can run its Python".

Skeptic: I ran `"C:/Program Files/LLVM/bin/lldb.exe" --batch -o "script print(6 * 7)"` twice. Both printed "error: unable to find 'python311.dll'" and then a stack dump with 7 ASLR-dependent addresses, e.g. 0x00007ff6fba48dcb (lldb.exe+0x38dcb) and 0x00007ffaf5f5408a (KERNELBASE.dll+0xc408a). The probe at debugger_tests.cpp line 78 is only `.contains("42")` on the whole output and ignores the exit code. With about 8 varying hex digits in each of 7 addresses, some boots will give an address containing '42'. On those boots the test runs instead of skipping and fails.

Fix: Use the process result's exit_code == 0 and look for a whole line, e.g. print a marker like `script print('fb-python-' + str(6 * 7))` and search for "fb-python-42".

Status: fixed: the probe needs exit code 0 and "fb-python-42", which only Python's print gives

## tools-8 (low): futureblitz.py's docstring says a String's object has a header.class member

tools/lldb/futureblitz.py:7. The module docstring says a String, a Class, and a T[] each have a header "in members named \"header.counts\" and \"header.class\"". A String's debug struct (codegen debug_type STRING) has only header.counts and Length, and the script's own _kind check is names == ["header.counts", "Length"].

Shown by: tools/lldb/futureblitz.py lines 5-9 vs compiler/src/codegen.cpp debug_type()'s STRING case and runtime.hpp's fb_string_t {refcount, length}.

Expected: The docstring says a String's header is only its count of owners.

Actual: It says every one of the three has header.class.

Skeptic: futureblitz.py's docstring (lines 5-8) says a String, a Class and a T[] each have a header 'in members named "header.counts" and "header.class"'. codegen.cpp debug_type's STRING case builds only header.counts and Length (a {i64, i64} layout, matching fb_string_t), and the script's own String check expects exactly those two names. The docstring is wrong for String. Fixing it is a comment change.

Fix: Reword it, e.g. "whose header comes first, \"header.counts\" and, but for a String, \"header.class\"".

Status: fixed: the docstring says a String's header is header.counts alone

## trace-3 (low): A thrown error that is kept costs about 600 bytes in the side table, roughly 7x the error itself

runtime/src/trace.cpp:221. capture_stack() reserves 32 frames (512 bytes) and keeps every raw frame, the runtime's included. keep_error_trace() moves that vector into the map unchanged, so each live thrown error holds the reserved capacity plus a map node. In a deep stack it holds up to 310 frames, about 5 KB. A program that collects caught errors, such as validation or parse errors in a list, uses several times the memory it would without traces.

Shown by: scratchpad/review-m8s3/trace/keep.fb: 200000 times, `Try Fail() Catch e As LoadError kept.Add(e)`, where Fail throws. keep_nothrow.fb is the same, but each LoadError is made and added without being thrown. Both built --release. Peak working set sampled with PowerShell's Process.PeakWorkingSet64 while each runs.

Expected: Keeping a thrown error costs about what keeping an unthrown one does, plus a compact trace.

Actual: keep_nothrow.exe peaks at 20.9 MB; keep.exe peaks at 140.4 MB, about 600 extra bytes per error. Errors thrown and caught but not kept are released correctly: 1M throw/catch in a beside DLL or an embedded library peaks at about 4 MB.

Skeptic: Release builds of keep.fb (200000 thrown, caught and kept errors) and keep_nothrow.fb (the same errors kept but never thrown). Sampling PeakWorkingSet64 every 5 ms gave keep_nothrow 11 MB and keep 140.4 MB, about 650 extra bytes per kept error. In runtime/src/trace.cpp, capture_stack() does frames.reserve(32) (32 x 16 bytes, at least 512 bytes, whatever the depth), keeps every raw frame including the runtime's, and keep_error_trace() moves that vector into the unordered_map without shrinking it. That is a real and easily avoided cost for any program that collects caught errors.

Fix: In keep_error_trace(), store only the frames that have markers (or just their addresses), or call shrink_to_fit(), so a kept trace is a few dozen bytes per frame of the program's code.

Status: fixed: a thrown error keeps only the frames of the program's code, each with its function's start, in a vector of their size; tests/runtime/trace_tests.cpp checks an error thrown where no frame is the program's keeps none

## trace-4 (low): Windows capture_stack re-walks the stack in batches of 62, about 3.7x slower than one call, making caught throws in deep stacks slow

runtime/src/trace.cpp:172. capture_stack() calls RtlCaptureStackBackTrace(skip, 62, ...) in a loop, because the comment says it "takes fewer than 63 at once". That limit was Windows XP/Server 2003 only. Each batch walks the stack again from the top, so 310 frames cost 62+124+186+248+310 = 930 frame unwinds. Every Throw runs this (keep_error_trace), caught throws included, so the cost of a caught throw grows with stack depth. The loop also captures up to 310 frames, not the max_frames = 256 its comment promises.

Shown by: scratchpad/review-m8s3/trace/cap.cpp (g++ -O1, at a 400-deep stack): RtlCaptureStackBackTrace(0, 256, ...) returns 256 frames in one call, taking 5.2 us, against 19.2 us for the 62-frame batches. In fb, dc0.fb and dc300.fb (debug) throw and catch 20000 errors at stack depth 0 and 300: 0.054 s versus 0.442 s.

Expected: One RtlCaptureStackBackTrace(0, max_frames, ...) call, capped at max_frames, so a caught throw's cost doesn't triple with depth.

Actual: A caught throw costs 2.7 us at depth 0 and about 22 us at depth 300 (debug), with up to 310 frames captured.

Skeptic: cap.cpp (g++ -O1, 400 deep): a single RtlCaptureStackBackTrace(0, 256, ...) call returned 256 frames, taking 5.4 us, against 18.6 us for 62-frame batches. In fb (debug), dc0.exe ran in 0.048 s and dc300.exe in 0.439 s for 20000 caught throws. The '<63' limit in the comment at runtime/src/trace.cpp:171 is the documented XP/Server 2003 restriction. The loop checks frames.size() < max_frames only before each batch, so it goes 0, 62, 124, 186, 248 and reaches 310. That contradicts max_frames' own comment, 'A trace shows at most this many frames' (256).

Fix: Capture once with a 256-entry buffer and FramesToCapture = max_frames, and drop the batching loop and its comment.

Status: fixed: Windows walks the stack once, RtlCaptureStackBackTrace for max_frames + 1; trace_tests checks a 600-deep stack gives 512 frames and says it's cut, the same limit as the other platforms

## trace-5 (low): A trace cut at the frame limit gives no sign that frames are missing

runtime/src/trace.cpp:34. capture_stack() stops at a frame limit, and write_trace() then prints what it has with no note. In a deep recursion the trace ends partway down, with no `top-level code` line and nothing saying frames were left out. That reads as a complete trace, against spec 09's "a line for each call of the program's code that it's in". The limit also differs by platform: 256 on Linux and macOS, up to 310 on Windows (see trace-4).

Shown by: scratchpad/review-m8s3/trace/deep.fb: `Function Down(n: Int): Int / If n == 0 Then Panic("bottom") / Local r = Down(n - 1) / Return r + 1`, with Print(Down(1000)). fb build deep.fb -o deep.exe; deep.exe 2>&1 | tail -3; deep.exe 2>&1 | wc -l

Expected: The innermost frames, then a line such as `    ...more frames, which aren't shown`, as the uncaught report already does for long Cause chains.

Actual: 308 lines: the panic line and 307 `at Down (deep.fb:5)` lines. The last line is a Down frame, with no note and no top-level code.

Skeptic: deep.fb (Down(1000), debug): `deep.exe 2>&1 | wc -l` gives 308 lines, and the last three are `at Down (deep.fb:5)`. There is no top-level code line and no note that frames were left out. write_trace() has no truncation marker, though runtime.cpp:840 already prints 'caused by more errors, which aren't shown' for long Cause chains. The trace reads as complete, which goes against spec 09's 'a line for each call'. The limit also differs by platform: 256 in add_frame and 310 on Windows.

Fix: Have capture_stack() record that it hit the limit (capture one frame more than it keeps), and have write_trace() print a line saying more frames weren't shown. Use the same limit on every platform.

Status: fixed: a trace shows at most 100 places, then "    and more calls, which aren't shown", also when the walk was cut, on every platform; panic_trace_deep checks it

## trace-6 (low): Releases at a function's end are put on the last statement's line, so a Destroy() panic points at an unrelated call

compiler/src/codegen.cpp:2714. locate() gives code with no span the previous statement's line (m_line). The releases of Locals at the end of a function have no span, so a Destroy() that panics as a Local goes out of scope is traced to the function's last statement. That line is a Print that didn't cause it, rather than the function's end.

Shown by: scratchpad/review-m8s3/trace/sp1.fb: Class Res whose `Method Destroy()` does Assert(N > 0, ...). `Function Scoped()` holds `Local r = Res()` (line 17) and `Print("in scoped")` (line 18), and `End Function` is line 19. Top level calls Scoped(). fb build sp1.fb -o sp1.exe (and --release).

Expected: `at Scoped (sp1.fb:19)`, the End Function where r is released (or line 17, where r is declared).

Actual: `at Res.Destroy (sp1.fb:7)` then `at Scoped (sp1.fb:18)`, the Print line, in both debug and release.

Skeptic: sp1.fb (which=1), debug and release, both print `at Res.Destroy (sp1.fb:7)` then `at Scoped (sp1.fb:18)`. Line 18 is `Print("in scoped")`, which isn't the call that's running: that is r's release at End Function (line 19). In codegen.cpp, locate() reuses m_line when the code has no span, so the scope-exit releases inherit the last statement's line. The trace sends the reader to an unrelated call. The spec doesn't settle implicit releases' lines, so this is low.

Fix: Give the scope-exit releases lowering emits the span of the block's end (End Function, End If, and so on), or the Local's declaration, rather than inheriting the last statement's line.

Status: fixed: where no statement is, as at a Function's end, a Local's release is at its declaration (release_scope()); panic_trace_destroy traces the Destroy to Scoped's Local, line 15, in debug and release

## trace-7 (low): Docs and comments describe a table and capture that differ from the code

docs/tooling.md:79. Three mismatches. (1) tooling.md says the table holds "its functions' address ranges", but the table has only call markers, frames and names, and the runtime takes each function's start from the unwinder (RtlLookupFunctionEntry, _Unwind_GetRegionStart). (2) runtime/src/internal.hpp:137 says capture_stack() gives the stack "from the caller of capture_stack() outward", but write_trace() says, correctly, that "The first frame is capture_stack()'s". On Windows RtlCaptureStackBackTrace(0, ...) and on the others _Unwind_Backtrace both start in capture_stack itself. (3) Spec 09's Stack traces lists how Methods, generics, function values and top-level code are named, but not that a Parallel For's body is "Parallel For at f.fb:9:5" or that a Property is "Dog.Loud".

Shown by: Reading only; parfor.fb's output shows `at Parallel For at parfor.fb:9:5 (parfor.fb:10)`.

Expected: Docs and comments that say what the code does.

Actual: Each of the three says something else (above).

Skeptic: (1) tooling.md:78 says the table holds 'its functions' address ranges'. compiler/src/trace.cpp:367-400 emits only markers, frames {name,file,line,parent} and names. The runtime gets each function's start from RtlLookupFunctionEntry or _Unwind_GetRegionStart (runtime/src/trace.cpp:99-107, 156). (2) internal.hpp:137 says 'from the caller of capture_stack() outward', but RtlCaptureStackBackTrace(0, ...) and _Unwind_Backtrace both start in capture_stack itself, as write_trace's own comment at trace.cpp:193 says. (3) parfor.exe prints `at Parallel For at parfor.fb:9:5 (parfor.fb:10)` (sema.cpp:15821), and spec 09's naming list doesn't mention Parallel For bodies. All three are doc or comment fixes.

Fix: In tooling.md, say the table has each call's place and the unwinder gives the functions. Fix internal.hpp's comment to say the first frame is capture_stack's own. Add Parallel For bodies (and Properties) to spec 09's naming list.

Status: fixed: tooling.md says the table has each call's place and the unwinder gives each frame's function, internal.hpp says the first frame is capture_stack()'s, and spec 09 names Properties, Parallel For bodies, other modules' functions, merged calls, and the 100-place limit
