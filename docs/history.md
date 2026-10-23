# History

What each milestone and step added, by name, and each review's fixes, as CLAUDE.md kept
them while each was built. Newest first.

## M6, the full standard library

Done in M6: step 0, CI: .github/workflows/ci.yml builds Debug and runs ctest on windows-2025
(MSYS2's MinGW GCC, the LLVM installer in C:\LLVM), ubuntu-24.04 (g++-14, apt.llvm.org's LLVM 22),
and macos-15 on arm64 (AppleClang, Homebrew's llvm@22), then tools/verify.py's program checks;
tools/verify.py is the local check too (build, ctest, release outputs but RELEASE_DIFFERS,
concurrent programs ten times each way, clang-format and clang-tidy on what changed since
`--base`). .gitattributes checks text out with LF; fast_float reads floats (Apple's libc++ has no
float from_chars); every file includes what it uses (misc-include-cleaner); is_ascii() is
Windows-only. The AArch64 C ABI: `c_abi_t::AAPCS64` (aarch64 and arm64 triples), aapcs64() and
homogeneous_floats() (an HFA as `c_passing_t::together` pieces, one [N x float] or [N x double];
others of 16 bytes or less as i64 or [2 x i64], a result of 8 or less as an integer its size;
bigger ones INDIRECT or MEMORY), c_layout() listing a fixed array's fields up to 32 bytes, and
codegen's pieces_types() and entry_alloca() of at least 16 bytes; codegen registers X86 and
AArch64 (cmake/llvm.cmake's stand-in Targets.def too), so either's IR is made and tested on any
machine; c_extension() leaves a Bool alone on AArch64 outside Apple's targets, as clang does,
thunks too. driver_tests' packages_t takes off both spellings of its folder (macOS's /var is
/private/var); link_error() reads ld64.lld's "library not found for -l"; native_calls calls sqrt
(exact everywhere) where it called cbrt; bindgen's foreign target is riscv64, and the intrinsics
test reads the host's header. Two tests are Windows-only by nature: the ANSI code page one and
Windows thread handles.
Step 1, the foundation: std/core is loaded but imported like any module (the driver adds no core
import; `module_import_t::statement`), so every std file and test imports what it uses; sema's
`m_unknown_bounds` (unknown_bounds()) quiets what follows from a constraint naming nothing, and
std_import_hint() says which Import. `attribute_t::qualifier` (`@C.Rename`, never a built-in),
sema's attribute_of() and attribute_argument(); next_line_is_body() takes '@', so attributes go
before required Interface members. `modifiers_t::panics_at_caller` (@PanicsAtCaller):
`mir_function_t::panics_at_caller` and `call_t::place`, which lowering's give_place() fills from
`m_span` (scoped_value_t); codegen gives such a function a trailing ptr, location() takes
caller_place() or own_location(), an Interface table's slot is a without_place() thunk
(`m_placeless`), and function values and Callback thunks pass null; check_inherited() and
check_inherited_property() copy it to an Override and refuse adding it; std's Pops, Peeks, Nexts,
Mem, and CString.Length have it. std/core/error.fb: Place, Error's Cause and Place
(`fb_error_fields_t`, first in the object), and the area Errors with their Kind Enums; MIR's
ERROR_THROWN fills a Place (fb_error_thrown()), and fb_error_uncaught() prints it and up to 100
causes. Built-in DebugAssert (DEBUG_BUILD, true in the evaluator), Unreachable, and Todo
(ends_program()); std/core/assert.fb's AssertEqual. `symbol_t::shows` makes a `Printable...`
parameter a String[] whose arguments lowering's texts_of() makes text, with rank()'s fits and
choose_overload()'s `unpacked` tie-breaker; std/core/print.fb's Print, Write, PrintError,
WriteError (fb_write(), which flushes stdout before writing stderr), Flush, Input, and InputAsync;
process.hpp's run_process() takes stdin, which the program tests give from <name>.in (console.fb,
input_async.fb). third_party/ fetches libuv 1.52.1 (uv_a); runtime/src/io.cpp is fb_runtime_io,
the I/O thread (io_t, reader_t for a console, pipe, or file, line_waiter_t), with internal.hpp's
pending_requests(), which stuck() reads, and at_finish(), which fb_finish() runs before the leak
report; build_config's `bundled_library_t`s (prefix, runtime part, archive, notice, system
libraries), the driver's bundled_libraries(), `native_libraries_t::bundled`, and the CLI's
"Bundled" lines. `symbol_t::gives_task`: an Extern "runtime" Function that gives a Task<T> is
passed a new Task first (extern_call()); a top-level Await is TASK_WAIT and a rethrow
(lower_await() without `m_async_task`). Its review's fixes: a DebugAssert's condition lets go of
what it made before a release build's way past joins (lower_panic()); Await and Wait settle()
their Task first; the I/O thread's finish_request() signals once the count is down
(start_request(), requests_pending()); the scheduler's tick() panics off the main thread;
showable() refuses runs_elsewhere() types; an Override Property and an Abstract one's
required_accessor() keep @PanicsAtCaller; unknown_bounds() quiets For ... In, static_members(),
and unsendable(), and an INVALID Throw quiets its Catch; an Attribute can take a built-in's name;
attribute_of() says a type isn't a module; std's CString.Free panics at the caller over
fb_cstring_free_known(); libuv's notices are one generated file (third_party's notices/libuv.txt)
under `FB_LIBUV_LICENSES`; reader_t keeps stdin's flags on POSIX (a terminal's copy, a pipe's
F_GETFL); run_process() writes the input from a thread of its own; an uncaught error shows 100
Causes and says there are more.
Step 2, indexing, iteration, and ranges: `Operator []` and `Operator []=` are Methods of Self
(parse_operator_name()'s "[]" and "[]=", sema's indexes() and indexing(), check_index_operator(); a
Struct's or Interface's []= is ByRef); the parser makes a hidden call beside each index
(`index_expr_t::call`, "object.[](indices)") and each assignment, op=, ++, or -- to one
(`index_set`, "object.[]=(indices, value)", the value "object[indices] op value"), which sema
checks only for a type that has_index_operator() (check_index_call(), check_index_set(), with
`m_checked` so a shared part is checked once, takes_indices(), a []= that takes the indices as []
does) and lowering lowers (`m_reused`, settled, in lower_index_set()); check_receiver() refuses a
ByRef Method on what a Property gives and lets an object receiver through. Interface defaults
outside the table: generic ones, conditional ones (Where lines on the Interface's own type
parameters, implemented_bounds() in take_default(), and a type that can't meet one doesn't get
it), and Final ones (newly allowed) run as written, through a value as a direct call with the
Interface for Self (sema's and lowering's outside_table(), copies_outside_table() out of slotted()
and is_virtual()); a type's own Method can't take their place (implementation_of()), a Class
can't override the copy (check_inherited()), and a required generic Method makes an Interface only
a constraint; type_arguments_follow() takes Self. Iterable<T>: boxes_into() lets a Method giving a
Struct or Class that implements Iterator<T> implement one giving Iterator<T>; a table gets a thunk
("X.Iterate() as Iterator<T>", table_thunk(), lower_table_thunk()) and generic code boxes at the call
(boxed(), std's Package `Boxed<S, T>`, `analysis_t::boxed_iterator`); std's collections implement
it. std/core/iterator.fb has Iterator<T>'s Final adapters and consumers over private Final
Classes (Mapped, Filtered, Taken, ... Ordered over the Package MergeSort, moved from array.fb),
Repeated, and Empty; Numeric has One (lowering's builtin_method()); @PanicsAtCaller goes on a
Final, generic, or conditional default. Ranges: `range_expr_t` from parse_range() at
`precedence::range`, between ? : and ?? (`m_counting` keeps a For loop's bounds and a Case's
values as they were), sema's check_expr(range_expr_t) (`m_range`, T by the For loop's rule,
integers only), and lowering's lower_expr(range_expr_t), which fills std/core/range.fb's
Range<T> (Current, Step, Left, More), its count of steps worked out unsigned with wrapping
arithmetic, a run-time direction picked by a branch, and an empty range its zero.
Its review's fixes: a table thunk (`table_thunk_t`) passes ByRef parameters on, gives a Method an
error variable where it takes one and the Interface's member doesn't ("X without an error
variable") or passes its own where the member throws, which a covariant Iterate may now; an
overridden Method takes an error variable where an override does, and a static call decides by
the callee; a @PanicsAtCaller call inside another passes that one's caller's place as it is,
null too (codegen), and std's consumers and every box's and adapter's Next are @PanicsAtCaller;
check_implementation() compares bounds through as_implemented(); implementation_of() decides a
default the type can't have before its place checks, and takes_instance_of() refuses a Method
taking a generic default's parameters with some types for its own, there and in check_inherited()
(report_outside_override()); check_refinements() refuses an Interface redeclaring an
outside-table default, and check_instance_defaults() an Extend of one instance doing it;
check_expansion() follows each default's Interface parameters apart (`expansion_node_t`,
runs_as_default()), calls of a type's copy (runs_copy()), and the defaults a Class's tables run
when it's made, so a default wrapping Self without end is an error, not a hang
(check_table_defaults() went); implemented_bounds() keeps a bound on a type made of the type's
parameters, like Wrap<T>, which argument_for() and mentions() check per instance; set_field_type()
reaches Classes extending an instance; outer_functions() lets a call by name in a type's code mean
a top-level Function where none of its Interfaces' defaults of the name fits; report_uninferred()
names only what told_arguments() can't tell, or a bound's failure; the parser's begin_of()
(`m_grouped`) starts an expression made with a parenthesized one at its parenthesis; Step takes a
whole expression; Range has an Operator ==; a Const Function's For ... In is refused at Next.
Its second review's: lower_function() decides a copy's error slot by the copy (lower_body()'s
`copy`), and give_error_variable() passes one in to_string(), hash_of(), and reflection's Call;
hide_in_place() lets a Method of a conditional default's types hide it where the instance doesn't
have it, which then hides nothing, check_instance_defaults() skips what !instance_has(), and
implementation_of() decides a default the type can't have before its matches; takes_instance_of()
checks the generic default's Where lines on its own type parameters; drop_clashing_extension()
refuses an unseen Extend's Method in a generic default's place, covers_outside_default() an
extending Interface's, and check_default_places() another Interface's default (any listing order);
implemented_bounds() keeps a bound whose Interface has the Method's own type parameters, which
told_type() lets told_arguments() infer from (Flatten on Iterator<Stack<Int>>); check_expansion()
reads each call's full type arguments (`m_call_arguments`) and gives runs_copy() the copy owner's
instance for Self; lowering stops at an instance whose type nests past too_deep() and sets
`mir_module_t::error` (`mir_error_t`), which codegen's build() and the CLI report; lower_place()
lowers a member without a symbol, like `s.Bytes[0]`, as a value; the parser's begin_of() starts
binary, ternary, and tuple-element spans too; Range's == is in a conditional Extend, and Range<T>
is only Where T: Numeric; report_uninferred() writes every type argument in its advice and says
nothing more after a count error.
Its third and fourth reviews': implements_for_some() makes a Where line on a default's own type
parameters hold when some types for them make it, for bounds_hold(), instance_has(), never_has(),
and implemented_bounds() (a type that implements no instance of the bound's Interface, like an
Iterator<Int32> for Flatten, gets no copy, and missing_default_hint() says why at "has no member
named"); told_type() counts a parameter told as itself as told; report_uninferred() reads a bound
on a type made of the parameters, and report_bounds() leaves out "for" on a copy's bound;
check_default_places() compares a type's own copies with the outside defaults its bases (through
never_has()) and its generic type have, and runs over Extends of one instance; check_changeable()
refuses changing an array's Count, alone in an Extend of T[] too, or a String's Chars or Bytes
(report_worked_out()), by assignment, op=, ByRef, or AddressOf, so an Extend's Property on arrays
can be set; a Map literal's Adds get an error variable; the type table refuses an instance whose
argument is made of more than max_size types (measure()) and fills one the depth limit cut short
again where it's asked for itself (`m_incomplete`), value_expression() can't overflow, and a fixed
array keeps a size past max_count unresolved; lowering stops growth along a chain of instances
made one from another (`origin_t`, chain_length() past max_chain), at a type too_large() (past
max_depth, 1000, or max_size), saying which, or a Method whose owner's instance lost its
arguments, and check_arguments() and check_values() report a Const type argument an instance
worked out outside 1 to max_count or its parameter's type (fits()); a String's Extend Property
reads as a Property; unify() is const. The lows: hide_in_place() lets an Extend's Method that a
call tells apart by ByRef stand beside a default, check_default_places() drops the copy it
reports, drop_clashing_extension() gives a generic default's clash its own message,
implemented_bounds() gives a tuple's or Func's bound no copy when no type fits (unextendable()),
report_uninferred() and implements_for_some() stay quiet after a constraint naming nothing,
run_const() reports a module lowering stopped, a Const Function runs for a Const type argument's
value, and a Const its own error left without a value is quiet where it's used
(`symbol_t::failed`, `m_read_failed_const`).
Step 3, collections I: std/core/values.fb's Min, Max, Clamp, Swap, and HashCombine;
interfaces.fb's Cloneable and Defaultable (`analysis_t::cloneable`, `defaultable`), which
builtin_implements() gives arrays (Clone) and the numbers, Bool, String, and Enums (their zero,
lowering's builtin_method()); a T?'s built-in Expect and OrElse (nullable_methods(),
`m_nullable_methods`, lowering's nullable_call()), on an array's T? and on a variable narrowed to
its T too; an array's Capacity (is_array_size(), `expr_info_t::count` for Count or Capacity alone
in an Extend of T[]), Reserve, and TrimExcess (MIR ARRAY_CAPACITY, ARRAY_RESERVE, ARRAY_TRIM over
the runtime's fb_array_reserve() and fb_array_trim()); TypeName<T>() (check_type_name(),
is_value_builtin(), `expr_info_t::type_name` for generic code), the declared name without its
module. Arrays go where Printable or an Interface an Extend of T[] or of the one array type gives is
expected: lowering's use_array_tables() fills `mir_module_t::array_classes` (array_interfaces(),
array_table_entry(), implementation() mapping a Property's Get and Set), which codegen's array
classes list; unify() takes an Interface's type arguments from the instance a Class, Struct, array,
or extending Interface implements (instance_for()); an array literal where an Iterable<T> of a known
T is expected is a T[]; converts_plainly() passes INVALID. std.collections: array.fb's search, ends,
bulk (AddAll and InsertAll from any Iterable<T>), order (Sort, SortUnstable's heap sort over
SiftDown and Swap, SortBy/SortByDescending's Sorting<T> with ThenBy), shape, Fold, Reduce, View,
and AsReadOnly, @PanicsAtCaller where they panic, going over what's left when a function changes
the array; map.fb in insertion order (each entry's Before and After), `==` and Hash by contents,
Operator [] and []=, `Map(default: v)` (Fallback, one shared value), Get(key, fallback), GetOrAdd,
TryAdd, Update (a removed key stays removed), Reserve; set.fb's algebra; the Try twins, ToArray,
AddAll, Drain, and Deque's index; Pool's IterateHandles and Capacity; view.fb's ArrayView (Place()
names the view's index) and ReadOnlyArray, ReadOnlyMap, and ReadOnlySet. Its review's fixes are in
those: the T? Methods on arrays and narrowed variables, Extends of one array type and Extend
Properties through tables, a T?? of an Interface, Try before TypeName or SizeOf, Capacity refused
ByRef and set, literals for Iterable<T>, inference through an extending Interface, views' and
sorts' panics, RemoveAll, FindLast, and Reduce under a changing array, Update, and PriorityQueue's
AddAll of nothing.
Step 4, collections II, std only: std/collections/sorted.fb's SortedMap and SortedSet (a B-tree,
SortedNode, Order 16, Find(), Split(), Fill(), Merge(), SortedMapIterator's stack of nodes),
ring_buffer.fb, grid.fb's Grid and Grid3 (GridIterator), bit_set.fb (UInt64 words, BitCount()),
lru_cache.fb and counter.fb over a Map, multi_map.fb (a Map of arrays), trie.fb (TrieNode by
byte), linked_list.fb (LinkedListNode with a Weak Before and Owner, LetGo() unlinking each
node), interval_tree.fb (an AVL tree, IntervalNode's Top), spatial.fb (PlacedValue, Bounds in
Doubles, Around(), SpatialHash's cells, and SpaceTree, which QuadTree and Octree wrap,
SpatialIterator), and immutable.fb
(ImmutableArray, ImmutableMap, and ImmutableSet, Structs over AVL trees of Shared ArrayNode,
HashNode, and HashEntry); each is checked against a plain array or Map over random operations
(tests/programs/<name>.fb), with a panic program for each panic. The runtime's let_go() lets
go of what an object's fields held one object at a time (`letting_go_t`, release_fields()), as
tests/programs/long_chains.fb shows with a million of each kind of object, and lowering's store()
copies a value read through a reference, or a Global's, before it releases the old one, as in
`Root = Root.Children[0]` (assign_through.fb).

Phase 1, lane 2's first batch (A5 to A7's highs, and A5-5): `++`/`--` finish their statement; For
... In holds a fixed array in a local of its own unless it's a whole temporary; a Select's type
match goes through concrete(); lower_jump() finds its loop by position again after release_scopes();
through_reference() counts a ByRef parameter's or ByRef Self's root, and store() copies from one;
evaluate's unary folds a @Flags Enum's integer and keeps the Enum; `leaving_t`, `m_leaving`, and
left(), so the cancel block of an Await in a Defer or Finally on a way out releases only what that
way out still holds; lower_try() sends an Await through lower_effect(); reflected_methods() leaves
out Async Methods and Methods with a ByRef parameter (spec 13); generate_main() releases the Globals
last to first through let_go(), which zeroes each reference before releasing it, and the ready flag
is an Int32 (0, `ready`, `released`) whose check panics "used after it's released" (spec 02). Tests:
increment_temporaries, for_in_fixed, scheduler_cancel_cleanups, globals_released,
panic_global_released, and cases in generic_bases, errors, borrowed, compile_time, async_throws, and
reflection_more.

Phase 1, lane 4's first batch, the review's A11 and A12 mediums and four lows: a Queue's Items are a
`T?[]` and Dequeue empties the slot it takes, so a dequeued value goes when the program lets go of
it, and its compaction compares Head with Count - Head (HalfEmptied()); T[]'s SiftDown and
PriorityQueue's find a child through HeapChild(), which stops at count / 2, so no index overflows
past 2^30 elements; a Map's Grow stops at 2^30 buckets (BucketsFor()) and returns when that leaves
them as they are; a RingBuffer's index wraps through SlotFrom(); tests/compiler/std_tests.cpp runs
those Package helpers through an extra std file with counts up to 2^31 - 1; spatial.fb's Within
takes Doubles, so each offset is a Double's, and takes every value for an infinite radius, one at
the center's infinity too; sorted.fb's CheckKey panics at a key that isn't equal to itself, like
NaN, in SortedMap.Add and []= and SortedSet.Add, IntervalTree.Add does at such an end, and Floor,
Ceiling, Range, Overlapping, Containing, Contains, and Remove find nothing for one; sema's
unmet_extension() lets report_unshowable() name the type argument that keeps an Extend's Printable
from a value; spec 14 gives FindLast a `T?`, says what compares values needs them Equatable, and
fixes LinkedList's sentence, spec 10 lists what's Sendable, and stdlib.md and interfaces.fb's
comments match. New tests: queue_releases, panic_sort_unstable_big, equatable_objects, spatial's
Edge cases, panic_sorted_map_nan, panic_sorted_set_nan, panic_interval_nan, std_tests' overflow
checks, and panics for a Map, Queue, PriorityQueue, SpatialHash, and Octree changed in a loop, every
spatial NaN check, and an empty LinkedList's Last.

Phase 1, lane 4's lows: Counter.Add works a count out with CheckedAdd and panics past what an Int
holds, in every build, and its comments and spec 14 say a value whose count went to 0 comes back
last; ImmutableMap.AddAll and ImmutableSet.AddAll, as ImmutableArray's; every collection's Changes
comment says it counts values added or removed (a Deque's setter in a loop is tested); comments
fixed in map.fb (Buckets and Shift), values.fb (Clamp's panic), trie.fb, and panic_chain_changed.fb.
New test: panic_counter_overflow. Lane 4's 23 findings are all fixed.

Phase 1, lane 2's second batch (A5 to A7's mediums and lows, and A7-new-1): raise() keeps the error
on its way out in `m_raising`, which an Await's cancel block releases; Lock reaches its Mutex once
and holds the innermost object it's in for the block (not Self's); combine() steps a Ptr with
pointer_t; set_property() settles the receiver for op= and ++; evaluate() takes a `build_mode_t`,
which analyze() (sema.hpp) gets from the driver, so a release build's Const Function leaves
DebugAssert out (spec 13); `call_t::span`, runtime calls' spans filled in by give_place(), and the
evaluator's `m_at` and reach(), so the step, depth, and size limits point at the last call or
operation reached (spec 13); the runtime's reserve_one() stops the capacity at 2147483647 and panics
past it (spec 14); codegen's constants are `fb.string`, `fb.text`, `fb.location`, and `fb.classes`;
declare_extern() extends a runtime Extern's small integers as C does; declare_global(), and a Const
known at compile time gets its global only when place_of() needs it; a slice's panic shows its
bounds as written; the MIR dump escapes \r, \0, control characters, and braces, prints a T? of an
Enum by member, marks `async` functions, and paints a Callback's function; a map literal runs Map's
New(); comments fixed in lower.cpp, mir.hpp, mir.cpp, codegen.cpp, and c_abi.hpp. Tests:
tests/compiler/evaluate_tests.cpp, release_const_debug_assert, release_panic_array_full,
panic_slice_unsigned, a runtime test, mir_tests and c_abi_tests cases, and cases in
scheduler_cancel_cleanups, atomic_mutex, pointers_more, and property_kinds.

Phase 1, lane 1's first batch (sema), the review's A3 findings: a Class's own Fields without a zero
are assigned before Super.New (Swift's rule, A3-1): super_new_of() takes the first of New's own
statements that's a Super.New, check_class_call() reports an own field unassigned there and a
Super.New that's nested or repeated, check_body() reports a New that never calls it while the Class
has such fields, and check_inherited_ready(), `Self.f`, and Return keep New's code before Super.New
to its own fields, which spec 06 says (the Boss example); a lambda or Parallel For body in New isn't
New's own code (in_new_of()), so it can't change ReadOnly Fields (A3-2); lambda_context_t's
`unready` keeps a lambda made before New's fields are assigned from capturing Self (A3-3); assigning
a Property by name, or as Self.P, checks check_self_ready() (A3-4); lower_reflection() refuses f.Set
on a ReadOnly Field, as spec 13 says (A3-5); a fixed array's count is checked for overflow before it
multiplies (A3-6); only a Static member hides T.Has and T.Attribute (A3-7); For Method skips Async
Methods (A3-8, with lane 2's A6-5); For ... In over a nullable Channel gets the right error (A3-9);
m_finally_returns and finish_returns() let a Finally assign what New's Returns in its Try need
(A3-10); For over literals alone takes Float32 when one has a decimal (A3-11); a throwing call in a
Defer or Finally says it needs Try? or a Try block inside it, through the SEALED handler (A3-12).
New tests: tests/programs/fields_before_super and reflect_skips.

Phase 1, lane 3's findings (A8, A9, A10): let_go() keeps a Weak to an object waiting its turn
(weak_waiting.fb). The parser's enter() counts each level once, exactly 1000 passing: blocks,
operators, calls, members, indices, brackets and interpolations, lambdas, each part of a type
(parse_type's suffixes, type arguments, Weak, Func, tuples, Const sums), Defer's and a single-line
If's statement, an Extern block, and nested_statement() for what a reported mistake leads to; the
lexer's #If condition has max_condition_depth. A single-line If refuses a labeled loop, an Unsafe
block, and an Extern block. skip_continuation_newlines() follows every statement's ',', '=', and
constraint '&', and stops on a line that already failed. An inactive #If branch is lexed for its
directives (m_skipped_from drops its tokens, report() sends its errors to m_unshown, but invalid
UTF-8 and what runs past the branch), finish_directive() skips trivia, and skip_inactive_lines() is
gone (multi_line_strings.fb). A multi-line string in a single-line string's interpolation is an
error. The parser takes '>=' as '>' then '=' (m_split), m_accessors closes End Get and End Set,
describe_current() reads the very next token, New in an expression says to call the type, and an #If
condition cut off at the end of the file stays inside it. fb::escape_controls() escapes messages,
paths, and notes, and dump.cpp's escape_token() and quote() escape every control character, quote()
doubling braces. key_of() keys a file by real_path(), and on_disk() falls back to the entry the disk
finds that isn't a link; a folder Import of links is one error; a [native] folder path or one ending
in '/' says it's a folder; a module's file ends in .fb (respelled_file()); project.toml takes '\' as
'/' (with_slashes()). fb build words a default output that's a folder apart from -o; parse_options()
gives a usage_error_t with its --color; fb-bindgen refuses an -o that's the header, empty, ends in
'/', or is a folder, and _Static_asserts each integer macro, `(NAME) * 0 + 1`, with
-Werror=gnu-folding-constant. scheduler_t::stuck() counts the I/O thread's requested_tasks() as
coming. at_end() and run_end() put back stdin's flags (POSIX) and the console's code page and
stderr's mode (Windows) however a program ends, and prepare_terminal() restores the console at exit
(tests/native/console_probe). run_attached() waits out Ctrl+C (ignore_interrupt() on Windows, SIGINT
and SIGQUIT ignored on POSIX), and spawn() starts programs with SIGINT, SIGQUIT, and SIGPIPE at
their defaults. A thread of its own pushes its id before m_pending goes down; shown_class_name()
names an uncaught error's chain (uncaught_alike.fb); resize_buffer() says Reserve's place and count
(panic_reserve_huge.fb); leak counts take the singular. cli_tests reads a program's PE, ELF, or
Mach-O imports and checks they're all the system's own. Comments: ast.hpp, manifest.hpp, module.hpp,
manifest.cpp, process.hpp, commands.cpp, and take().

Step 9, Math I: std/math/. numbers.fb: `Bounded` (MinValue, MaxValue), `Integer` (BitWidth, LowBits,
FromLowBits), and `FloatingPoint` (Epsilon, FromDouble, ToDouble, and libm's Floor through Fma),
Interfaces the ten number types implement through Extends with Private Static Methods over `Extern
"C"` libm, so std.math's Functions are generic and take their type from their arguments (`Lerp(0,
10, t)`); `MinValue<T>`, `MaxValue<T>`; Float32's and Float64's `ToBits()` and `FromBits()`, by
Unsafe Ptr casts. constants.fb: Pi, Tau, E, Epsilon, Pi64, Tau64, E64, Epsilon64, and Inf and NaN,
Consts worked out at run time since the compiler refuses a non-finite one. scalar.fb: Abs and Sign
(Numeric & Comparable), Floor, Ceil, Round (half away from 0), Trunc, Fract, Mod (floored) and Rem
for integers (IntegerRem(), safe at -1) and floats (fmod), Gcd, Lcm, IntSqrt (digit by digit in a
UInt64), Lerp, InverseLerp, Remap, SmoothStep, Wrap, PingPong, MoveTowards, IsClose (a tolerance, or
0.00001 of the larger), Sqrt through Atanh, Log(x, base), Atan2, IsNaN, IsFinite, CopySign, Fma,
Deg, Rad, ToRadians (with Int overloads), and ToDegrees; no Pow or IntPow, since `**` is the power.
bits.fb (std.math.bits): PopCount (SWAR), LeadingZeros, TrailingZeros, RotateLeft, RotateRight,
ByteSwap, IsPowerOfTwo, NextPowerOfTwo, AlignDown, and AlignUp, generic over Integer through
LowBits, and each integer's SaturatingAdd, SaturatingSubtract, and SaturatingMultiply over its
Checked Methods (Saturated()). vector.fb and int_vector.fb: Vec2, Vec3, Vec4, Vec2i, and Vec3i,
Numeric, Hashable, Printable, and Defaultable, with Operators by component and by a scalar on either
side, `[]`, Vec3's Up through Back and xy, Vec4's xy and xyz, and ToVec2i, ToVec3i, ToVec2, and
ToVec3; Dot, Cross, Length, LengthSquared, Distance, DistanceSquared (lengths in Doubles,
Magnitude()), Normalize (zero stays zero), Reflect, Project, Angle, Rotate, Perpendicular,
ManhattanDistance, ChebyshevDistance, and the component-wise Lerp, Min, Max, Clamp, Abs, Floor,
Ceil, Round, MoveTowards, and IsClose. quat.fb: Quat, Identity, FromAxisAngle, FromEuler (Y, X, Z),
FromMat3 (DirectXMath's four cases), FromTo, LookRotation, ToMat3, ToMat4, ToAxisAngle, ToEuler,
ToEulerDegrees, `q * v`, `a * b`, Slerp, Lerp (normalized), Inverse (the conjugate), Normalize, Dot,
Length, Angle (the relative rotation's Atan2), and IsClose (q and -q alike). matrix.fb: Mat3 and
Mat4, their columns the Fields x, y, z, and w, Identity, Scale, Rotation, RotationX, RotationY,
RotationZ, Translation, Compose, LookAt, Perspective, and Orthographic (glm's RH_ZO),
TransformPoint, TransformVector, ProjectPoint, Decompose, ToMat3, ToMat4, `[column, row]`,
Transpose, Determinant, TryInverse (glm's cofactors, four at a time), Inverse (panics), and IsClose.
color.fb: Color (linear Floats) and Color32 (sRGB bytes), the named colors, Hex and TryHex (#rgb to
#rrggbbaa), ToHex, Srgb, Hsv, Hsl, OkLab, ToHsv, ToHsl, ToOkLab, ToColor, and ToColor32 (sRGB's
curve in Doubles; ByteOf() keeps 0 to 255, NaN as 0), Operators, Lerp, and IsClose. geometry.fb:
Rect (X, Y, Width, Height), AABB (Min, Max), Sphere, Ray, Plane (Normal, Distance), and Frustum (six
Planes from a matrix, Gribb and Hartmann for 0..1 depth), with Contains, Intersects, Raycast (slabs;
0 from inside), Intersection, Union, Grow, ClosestPoint, Transform (Arvo's), SignedDistance,
FromPoints, and FromPointNormal. sema's is_later_name() lost Vec2 through Color and Rect, so an
Extend of one names std.math's Import. Tests: math_scalars, math_bits, math_vectors, math_quat,
math_matrices, math_color, and math_geometry, printing libm's results rounded to four places, and 36
panic_math_ programs.

Step 9's review (25 findings, 2 high): Sphere.Raycast in Doubles from the ray's nearest point;
integer Wrap and PingPong through UInt64 distances, for unsigned types and wide ranges; float Mod
and Wrap never give b or high; Lerp exact at 1; PlaneOf's zero-normal rows; Grow stops at nothing;
ToEuler's fold within 0.02 degrees; Vec2i, Vec3i, Mod, Rem, Gcd, Lcm, and Abs @PanicsAtCaller;
Sphere tests, Project, Angle, MoveTowards, and Quat's Length in Doubles; matrix inverse and
determinant in Doubles; FromTo without its shortcut; Green as CSS's, with Lime; colors' linear
values rounded once; Mat3 and Mat4 scale from the left and divide; ToDegrees(Int); stdlib.md's SIMD
claim dropped, the compiler's item 13; spec 02 and 06's math types.

Step 21, std.crypto: third_party/ fetches mbedTLS 4.1.1 (TF-PSA-Crypto's PSA API, with
`MBEDTLS_THREADING_C`), xxHash 0.8.4, BLAKE3 1.8.7, Argon2 20190702, crypt_blowfish 1.3, and
Monocypher 4.0.3 (Ed25519 only), each built -O2 as its own archive beside a runtime archive
(fb_runtime_crypto, with crypto_pem.cpp apart, fb_runtime_xxhash, fb_runtime_blake3,
fb_runtime_argon2, fb_runtime_bcrypt, fb_runtime_ed25519; fb/crypto.hpp), and
`bundled_library_t::archives` takes several. std/crypto: `Hasher`, `Md5`, `Sha1`, `Sha256`,
`Sha512`, `Blake3` (keyed, derived, any length); `Hmac`, `Hkdf`, `Pbkdf2`; `SecureRandom` (`Bytes`,
`Below`), `ConstantTimeEquals`; `Crc32`, `XxHash`; `Argon2id` (`Hash`, `Verify`, `Derive`) and
`Bcrypt`; `Aes256Gcm` and `ChaCha20Poly1305` (`NewKey`, `Seal`, `Open`); `Ed25519` (RFC 8032's
encodings only), `EcdsaP256`, `Rsa` (`Verify`, `VerifyPss`, `PublicKey`), `X25519`; `KeyKind`, `Key`
(`FromPem`, `ToPem`), and `Certificate` (`FromPem`, `AllFromPem`). Tests from RFC 1321, 2202, 4231,
5869, 6070, 6234, 6979, 7515, 7748, 7914, 8032, 8410, 8439, and 9106, BLAKE3's and xxHash's vectors,
crypt_blowfish's, and Wycheproof's.

Step 21's review (13 findings): crypto_internal.hpp's `stored_key_t` (glibc has a key_t); Hmac
hashes a key longer than its block first, as RFC 2104 does; third_party/ gives mbedTLS's libraries
-O2 after their own -O0 in debug builds (fb_options_after), which driver_tests checks through
compile_commands.json; crypto_pem.cpp reads PKCS #8, SubjectPublicKeyInfo, and certificates with a
DER reader of its own, so a certificate of any key reads, names come out as OpenSSL writes them
(T61, BMP, and Universal strings decoded, RFC 4514's escapes, no length limit), Key.FromPem takes
the first key block and throws SYNTAX for a damaged one, X25519 keys stay unclamped, and mbedx509
isn't linked; PSA_VENDOR_RSA_MAX_KEY_BITS is raised to 8192 and RSA verifies through mbedTLS's RSA
module, any size; Bcrypt.Verify refuses $2x$; crypto_macs checks RFC 7914's PBKDF2-SHA256 vectors
and HKDF and PBKDF2 by SHA-512, crypto_signatures RFC 7748's 5.2, and crypto_certificates and
crypto_rsa_keys are new.

## M5, modules, packages, FFI, and bindgen

Done: M5 (modules, packages, `project.toml`, FFI, bindgen), planned 2026-10-18 and finished
2026-10-20, in 11 steps, then a review of each project (compiler, runtime, cli, std).
Step 1, module scopes: compiler/include/fb/module.hpp's `module_t` (name, file, is_main)
and `module_table_t` (of(), is_main(), main()), which `ast_t::modules()` holds and the driver fills,
naming each library file by its path with module_name() ("std.core.error") and the main file by its
stem; `ast_t::outside_main()` replaces from_library(); sema's `m_modules` are `module_scope_t`s
(their own `names`, and `imports`: every module but the main one, for now), with `m_builtins` after
them; declare_in_module() replaces declare_in_file() and still refuses a built-in's name with the
same parameter types; top_level(name, span) finds a name from the module of the file it's written in
(module_of()), a module's own type or variable hiding imported ones and its Function hiding an
imported one of the same parameter types (same_signature(), which lines up generic ones' type
parameters in order and compares what their constraints ask, requirements(), and pairs declared
before every Interface knew what it extends, `m_unsure_overloads`, again in check_overloads();
Methods keep same_parameters()), imported Functions not hiding each other, so a call or function
value (check_function_value()) that two fit equally is ambiguous, and report_ambiguous() reports a
type or variable that two imports give, once per place (`m_ambiguous`, only for what reaches
`m_reported`, not an early run's scratch bag), and again after quietly_keeping_ambiguous()
(`m_quiet_ambiguous`), which an Extend of one instance and type_attribute() try names with;
std_type() finds Error, the built-in Interfaces, and Map in std.core's and std.collections.map's own
names, so a program's own Error, Map, or Stack hides std's only for its code
(tests/programs/own_names.fb); `type_t::module` (sema's qualifier_of() at each declaration) and
`type_table_t::qualified_name(type, main)` name types in MIR and LLVM, lowering's qualifier_of(),
name_start(), and function_name() start functions and Globals outside the main module with their
module ("lib.a.Make()", "std.collections.map.Map<Int32, String>.Add(Int32, String)"), and
`mir_module_t::main_module` names the main module; lowering's laziness (function_of(), use_class(),
lower_library()) keys on outside_main(); function_class() puts a function value's Class in its
Function's module with qualified parameter types ("lib.a.Make(lib.a.Node) as a value"); every
message showing two or more types uses name_of(type, other), name_of(type, { others }), or
names_of(types) over name_among() and alike(), which add each module (qualified()) when names match
or the types are, or are made of (declared_parts()), two declarations of one name, signature_text()
compares with the Methods that don't match, and std_name(type, beside, span) qualifies std's Error
and Printable where the name means something else in the code, in Throw's, Throws', Catch's, and
strings' messages (tests/programs/own_interfaces.fb declares each built-in Interface's name);
unsendable() gives an `unsendable_t` (the `held_part_t`s on the way, and a reason), field_misfit()
a `misfit_t`, and a `main_only_t` keeps its type apart (`type`, `rest`), so reason_text(),
misfit_text(), and code_name(code, shown) name their types among the message's; declared_here()
asks whether a variable, the Method's type's member, or a top-level name has a name without
reporting an ambiguous one, for in_place_maker() and spawner(); `fb_class_t` and
`fb_interface_t` gained `full_name`, which the runtime's shown_name() uses in leak reports (one
message: report_cycles() takes every leaked Class) and type panics when its alike() finds two of one
name, or two declarations of one name among what each is made of (declared_parts(), the words_of()
its full name spells with modules).
Step 2, `Import`, `From`, and qualified names: `import_stmt_t` (target, quoted, target_span, last,
alias, from, names) from parse_import() and parse_import_target() ("From" is a keyword only where it
starts a line before a module; KW_IMPORT's "not supported yet" and `unsupported_t::LINE` went),
`named_type_t::qualifier` ("C.Map<K, V>", parse_named_type()), `type_stmt_t::base_qualifier`, '.'
in type_arguments_follow(), and "(import ...)"/"(from ... Import ...)" in dump_ast; the driver's
`loader_t` loads the main file, std/core/, std/collections/map.fb, and what imports reach from a
`file_set_t` (compile()'s `library_file_t`s in memory, or compile_file()'s disk, from the main
file's folder), once per key_of() path or same_file() (equivalent() on disk, for a path that
differs only in letter case), reporting an Import that finds nothing (report_missing())
and two files of one module name; the pure resolve_import(target, quoted, importer) gives
`import_paths_t` (the file, and a dotted name's folder, which folder_files() lists); add_modules()
fills the table in initialization_order() (std/core/, std.collections.map, then depth-first from the
main module, last), each `module_t` with its `module_import_t`s (the unwritten std/core/ one first,
then each line's statement and modules), and `ast_t::sort_items()` follows it; add_std_names()
parses the std files the program doesn't load for `std_name_t`s (`module_table_t::std_home()`).
Sema's declare_imports() makes each module's `import_scope_t`s (modules, unqualified, From names,
alias) and its qualifiers, MODULE symbols (`symbol_t::modules`, declare_qualifier()), and reports
misplaced lines (report_misplaced_import(), and check_stmt() for one in a block); check_from_names()
checks From's names; top_level() goes through the module's names, its qualifiers, givers_of() (the
imports that bring a name in unqualified), and the built-ins, and in_module() looks after a
qualifier; qualifier_named() and qualified_name() (`qualified_t`) find "collections.Stack", which
check_qualified() (with check_variable(), split from check_expr()), lookup_callee()'s callable(),
type_named() and enum_named() (written_name()), and resolve_qualified_type() and named_type()
handle, marking `expr_info_t::qualified`, which lowering's lower_expr() and lower_place() treat as
the name alone; type_symbols() serves resolve_interface(), constraint_only(), extension_target(),
resolve_argument(), and declare_base() (report_not_in_module()); report_undeclared() and
import_hint() (home_of()) say
which module has a name ("'Stack' is in std.collections, which this file doesn't import", or how
an alias or From leaves it out); visible_from() shows an Extend's members only where
imports_module() finds its module: members_of(type, name, from) skips what code at `from` doesn't
see before a member further down hides one, and one type's members hide none of each other's;
members_here() and members_seen() (which own_members() uses, reporting only for lookup_value())
drop imported ones that the code's own module's take the place of (prefer_own_extensions(),
extensions_clash()) and report a Property two imports add (report_ambiguous_member(),
module_list()), leaving same-typed Methods to the call;
check_inherited() ignores unseen Extends; resolve_operator() takes an Extend's Operators only where
seen, saying which module has a hidden one, but == everywhere, and add_member() lets two modules'
Extends add one name except Operator ==; report_hidden() names the module; implementation_of()
and property_implementation() take what code at the Interface's listing sees, through
members_seen(), reporting two imported modules' same-typed Methods there (report_clashing()) and
naming an unseen Extend's module (unseen_hint()), and iteration_of() takes the Iterate the loop's
file sees; check_assignable()
reports a function, type, or module, as in `hud.Draw = f`; check_read() names the module of a
Global that initializes later; means_another() in name_among(), with `m_file` set in check_any(),
check_stmt(), the initializers, and for declarations by each_item() and in_file() (link_class(),
conform() per listing, link_constructors(), check_throws(), check_pending_bounds(),
check_worker()), qualifies a type whose name means another there, and shown_name(type) asks it
for every message showing one type (name_of(type) is the written name). std/collections/set.fb
imports std.collections.array and .map; collections.fb, generics.fb, own_interfaces.fb, and
own_names.fb import std.collections; the program runner passes tests/programs/<name>/'s files
(program_files()), for tests/programs/modules.fb (whose Classes extend S.Shape and S.Box<String>),
module_extends.fb, and panic_module.fb.
Step 3, visibility: `visibility_t` (PRIVATE, PROTECTED, PACKAGE, PUBLIC, in that order; spelling())
and `modifiers_t::visibility`/`visibility_span`, which parse_modified() takes among the other
modifiers (take_visibility(), visibility_of()), parse_visible() for an Enum or `Public Attribute`
(whose Fields get its visibility), parse_attributed() after attributes, and parse_enum() refuses on
a member; `unsupported_t::PREFIX` went, and dump_ast shows a written one; the driver's add_modules()
gives each `module_t` a `package_id_t` (`module_table_t::add_package()`, `package_name()`: "std",
and the main module's name for the program's files), and declared_names() gives std hints only
Public names. Sema's has_visibility() (declared symbols, News, and the defaults take_default()
copies, which keep their Interface's visibility even where an Extend gives a Public type a private
Interface, a default Operator but == too, which resolve_operator() and implementation_of() check;
not Interface members or other Operators), sees() and seen_names()
for top-level names (givers_of(), top_level(), in_module(), home_of(), and check_from_names() bring
in only those), can_use(declared, span, through) for every declaration (Private: the file; Package:
package_of(); Protected: class_at() a Class whose declaration holds the use, `m_class_spans`, that
is_or_extends() the owner, through Self, Super, or one of its own objects, or Static), and
unusable_text()/report_unusable()/label_declaration() ("'Entries' is private to
std/collections/map.fb", a Struct's constructor through least_visible_field()); members_seen(type,
name, from, use, through) drops what code can't use, recording it in `m_dropped` (`dropped_t`) with
usable_constructors() and drop_unseen(), so check_expr() of a call reports one that
only_unusable_fits(); members_here() and lookup_member() pass the object's type, or the code's Class
for Super, report_hidden(), unseen_hint(), unusable_own_member() (in report_undeclared()),
report_not_in_module(), and report_hidden_name() (attributes) say why, and import_hint() gives a
`hint_t` (text, declared) that report_hint() labels; callable() and lookup_super() filter News,
link_constructors() checks base_new, and inherit_constructors() takes only usable News;
check_not_protected() (top level, Structs', Extends' members), check_interface_visibility(),
check_operator(), and check_variable_modifiers(node, top_level) (Locals, and Consts in a block)
refuse modifiers where they don't go, and Interface members and Operators take their type's
visibility; the New a type gets has the type's, a Struct's no more than its least visible Field;
check_inherited() and check_inherited_property() refuse a hidden original for a Class's own member
(report_hidden_override(); an Extend's sits beside one) and narrowing (narrows(),
report_narrowing(), a default aside), and nothing else hidden clashes there or in add_member() for
an Extend's members, while members_seen()'s hide_in_place() lets a Class's member hide its bases' of
the name (Methods aside) and a type's own hide what an Extend adds in its place, where code can use
both; check_signatures() (after check_defaults()) runs check_shown_by() over each declaration seen
as far as reach_of() says, check_shown() reporting each less visible type its parameters, result,
Throws, constraints, type, base, or listed Interfaces show, naming the written visibility
(reach_text(): "'hp' is Protected in Enemy, which is Public"). std's API is Public (its Fields and
helpers stay private), the step 2 tests and programs mark what they share, and
tests/programs/visibility.fb runs with visibility/. Fixed on the way: lookup_super() finds the News
of an instance base, like Queue<String>, in its definition, and known_arguments() gives Super.New
the instance's type arguments; check_target_place() types an inherited field assigned by name with
own_view(); a generic type's Static Field or Property used by name, read or assigned, records whose
instance it is (static_arguments(): the type's parameters in its own code, the instance's in a Class
extending one or an Extend of one), and lowering's instance_global() gives a `T? = Null` initializer
the instance's type; debug builds run globaldce before the coroutine passes, so an Async Function
nothing calls is split too; check_assignable() lets any of a Struct's Methods set its Static
Property by name, since it's no part of Self, and calls a Property a Property in its error; an
Extend of one instance's Operators are found (operators_of() and members_of() walk each base as its
instance, with that instance's Extends, so a Class extending Box<String> has them too), an Operator
that isn't generic or a default copy beats generic ones that take the same types (beats_generic())
in resolve_operator(), the convertible ones too, and implementation_of(), take_default() marks a
default on one instance `extended` (no Method table slot), conform_instances() conforms a Class's
instance bases before check_inherited(), conform_instance_operators() gives an instance whose Extend
adds such an Operator a conformance of its own to the generic type's Interfaces (listing_of(),
put_instance_operators(), as the listing sees), and lowering's implementation() looks at each base's
instance, then its definition; what Extends of one instance add is `analysis_t::instance_members`,
which lowering's equal_operator() reads, walking the type and its bases, each instance's before its
definition's own (compares_two()), so an instance's == holds in Structs, arrays, tuples, T?s, and
generic code; matches_required() and property_implementation() give a generic type's own members the
instance's types (member_type()); check_instance_extensions() runs check_inherited() on what an
Extend of a Class's instance adds against the instance's base; synthesized() records each built-in
Hash in `m_built_in_hashes` (`built_in_hash_t`), and check_hashes() reports an Operator == that came
later, from an Interface's default or another file's Extend, or an instance's, through a generic
Class extending one too (instance_extending(), hashes_built_in(), report_unhashed()), while a
Class's built-in == no longer asks its fields to be comparable; and type_table_t::fill() keeps the
Interfaces an Extend of the instance itself lists, while conform() counts what an instance's generic
type gives it (given_interfaces()), so an Extend of the instance can't list another instance of one
of those. implementation_of() takes an Interface's own default before another Interface's of one
name and types that the type gets itself, so a type can have both, while one it has from the Class
it extends, or an instance from its generic type, implements what it fits (the Interface's own
first), a Method that holds a default's place with another result is a mismatch
(matches_required()'s `result`), and report_ambiguous_defaults() (with spoken_list()) reports an
Override of two inherited defaults or an Interface's member without a default that two fit;
redeclares() and drop_redeclared() (in members_of() and operators_of()) make an Interface's member
like one of an Interface it extends (refines()) one member, and conform() conforms an extending
Interface first; known_arguments() and own_view() give a member of an extended Interface used by
name in a default this Interface's Self; resolve_operator() lets a T? and a T compare the built-in
way; extension_implements(), interfaces_of(), conformance_query_t, and lowering's implemented() give
a Class what a conditional Extend gives a Class it extends; check_inherited_property() finds a
hidden Property wherever it is, and looks past it for an Extend's; lowering's compares_values()
compares a Class? through its Class's Operator ==, narrowed or not, and outliving() copies what a
Return reads from a temporary, like Make()[1], before the statement releases it. A conditional
Extend's defaults are conditional: conform() keeps each listing's own Where lines (an unconditional
listing of one Interface wins), add_conformance() and implementation_of() pass them on, and
take_default() marks a default taken for them `extended`, with them as its constraints;
implementation_of() and property_implementation() skip a member whose Where lines don't hold where
the Interface is listed (unmet_where_listed()), reporting a written one (report_conditional());
hide_in_place() lets a Method every instance has hide a conditional_default() in its place; sema's
reflected_types() and lowering's reflected_methods() and reflected_members() (has_member()) go over
only what an instance has; conformance_query_t gives a Class nothing of a constraint-only Interface
from its bases (`analysis_t::constraint_only`); lowering's lower_instance() gives a generic default
its own type arguments, and an array's copy keeps them after its T. A conditional Operator == is
only the instances' that meet its Where: sema's instance_has() leaves it out of resolve_operator()'s
candidates and comparable() for the others, which compare the built-in way, as lowering's
equal_operator() does with has_member(); check_inherited() and check_inherited_property() leave out
what never_has() says the instance a Class extends never has, like Base<Rock>'s, and an Override
beside a Method every instance has overrides that one; lowering's implemented() adds the Interfaces
a conditional one extends. Finishing step 3 also fixed what verification found: a conditional Method
can't be overridden (check_inherited()); a Const Function whose body didn't parse doesn't run
(`m_unparsed`); Self in a [Weak Self] lambda reads as a T? (weak_self_in_scope()); a Static through
a Class extending an instance takes its type arguments (known_arguments()); lowering's may_change(),
settled(), and changes_after() read operands left to right, and an index's T[] first; an Abstract
member takes its Class's visibility and an Override with none takes what it overrides'
(inherit_visibility(), report_unusable_abstract()); Pool's generations start at 1; reach_of() takes
an Extend's reach from every type it's added to (declared_parts()); unseen_hint() and
report_hidden() hint only usable members; resolve_argument() reports a hidden Const; an inherited
New is no more visible than the base's; members_of() lets nothing the code can't use hide what's
above it; lowering's member_chain() and sees_extension() give reflection an instance's Extends and
only the ones its file sees, and For Property replaces only what an Override overrides;
take_default() makes what an Extend's listing gives the Extend's (listed_in_declaration()),
check_default_inherited() checks a default like a written Method, and check_equality_defaults()
reports two Interfaces' default == at the type (`m_ambiguous_equality`), and implementation_of()
leaves it to that report. The second round made lowering's lower_arguments() evaluate a call's
arguments in written order (written_arguments()) and object_of() settle its object, in
call_interface_member() too; may_change() counts what runs_code() says a value's text or == can run;
settled_place() copies the reference an element or an assignment's target is reached through, and
op= and slices read their operands first; outliving() copies a Return's value read through a place
when anything's released before it; and closures' captured copies (`m_unchanging`) are never
settled, since a Parallel For's threads share them. The third round made settled() give a borrowing
temporary, like a cast of a variable, a reference of its own, and settles a `?.` call's receiver
(object_of()), interpolation's parts, a Property assignment's receiver (set_property()'s `changes`),
the objects a ByRef argument is reached through, and reflection's Set and Call.

Step 4, `Extern` and `CString`: `extern_stmt_t` (convention, library, functions, modifiers) from
parse_extern(), whose Functions parse_function() gives an `external_t` (`convention_t` C, STDCALL,
or RUNTIME, the `= "symbol"`, the library) and the block's visibility (`m_extern`); `"(extern ...)"`
in dump_ast. CSTRING is a built-in type id after STRING: a `const char*` whose zero is null, not
Printable (showable()), Equatable (comparable()), or Sendable. Sema's declare_extern() reports
`"runtime"` outside std (in_std()) and `Lib`, and check_extern_function() what C hasn't (generics,
Throws, Async, Const, @Inline, defaults, varargs), types beyond is_c_scalar() (a String parameter
only for the runtime, a String or String? result), and the symbols "", `main`, `fb_...`, `fb.`, and
`llvm.`, marks CString parameters `symbol_t::converts_string`, which rank() and check_argument()
let a String fit, or `string_escapes` for a Function that gives a CString or takes one ByRef, and
fills `m_externs`, whose check_extern_symbols() wants one signature per symbol, an Enum counting as
its integer and a String result as a CString. match() ranks a built-in's untyped parameter
`match_t::ANY`, below CONVERTIBLE. A Spawn's work can't give a CString (holds_cstring()), and
in_std() asks module_table_t::is_standard(), which only add_package()'s `standard` one is.
Lowering's declare_function() gives an Extern Function a `mir_function_t::external` (`mir_extern_t`:
symbol, c_abi), its parameters, and a CString return type for a String one, but no blocks, and
extern_call(), which a Func value's closure code calls too, copies a String into a CString
(CSTRING_NEW) before the call and frees it (CSTRING_FREE) right after, copies a String C gives
(STRING_FROM_CSTRING, before the frees), panicking on NULL for a String, and checks an Enum C gives
or sets ByRef (to_enum()); lowering's zero() of a CString is null; codegen's declare_extern() reuses
a declaration the module has, like the runtime's, and extend_for_c() adds zeroext for a Bool, and on
x86-64 outside Windows and on Apple's targets signext/zeroext for small integers. The parser skips
an Extern Function written with a body (extern_body_follows()). The runtime has fb_cstring_new(),
fb_cstring_free(), fb_cstring_live_count(), which fb_finish() reports, and fb_string_from_cstring(),
which puts U+FFFD for what isn't UTF-8, as fb_string_chars() does through decode(). Print moved into
std/core/print.fb over `Extern "runtime"`'s fb_print, and top_level() lets a module's own or
imported Function of a built-in's types hide it (declare_in_module() no longer refuses one).
tests/programs/extern_c.fb calls the C runtime, with tests/programs/extern_c/libc.fb's Public block,
and panic_extern_enum.fb and panic_extern_null.fb panic at the call.

Step 5, `project.toml` and packages: compiler/include/fb/manifest.hpp's read_manifest() reads a
project.toml with toml++ (FetchContent, TOML_EXCEPTIONS=0) into a `manifest_t` (name, version, main,
`dependency_t`s), each table in_order(), reporting at its lines; the driver's `package_loader_t`
reads the main package's and its dependencies' (each once by folder, by equivalent(), reporting a
missing or unreadable one, a name that differs, two of one name, or a cycle; an unreadable main
one is compile_file()'s error, unreadable()), and compile_package() makes the program from the
main file, given or project.toml's (a missing `main` is an error at its line), or for a library
(`package_info_t::library`), an empty "(library).fb" (named apart from the package's files) with
library_files() loaded as
`file_set_t::extra` modules. `file_set_t::packages` (`package_t`: name, version, root, prefix,
dependencies, main) maps every package's paths, the main one's too, by its name ("game/ui/hud.fb",
"physics/shapes.fb") to its folder (disk_path(), package_named()); resolve_import() takes an
`import_from_t` (importer, package, folder, dependencies), so `Import physics.shapes` reaches only
a dependency or the package itself, a folder Import skips a package's main file (`package_t::main`,
compared on disk) and the program's main module, a package's main module is named by its path
("game.main"), a quoted Import naming a folder is an error, and
nested_package() refuses an Import into a folder with a project.toml of its own
(report_nested()); each package is its own package_id_t; warn_respelled() warns where an Import's
written parts spell a folder or file in other letter cases. compile_file() takes a .fb file,
which builds in the package find_package() finds above it, or a package's folder;
`compilation_t::package` names the program (`program`), and the CLI takes nothing for the current
folder's package, writes a package's build to <root>/build/<program>, and won't build or run a
library. tests/packages holds game, which depends on the physics library (game.out), run by
program_tests.cpp's package runner.
Step 6, native libraries: manifest.hpp's `native_t` (name, `native_kind_t` STATIC or SYSTEM, path)
from [native] (read_natives(), read_native(); `dynamic` is an error until M8); the driver's
package loader finds each archive (find_archive(), archive_names(): the file, libx.a, x.a, or
x.lib) or reports it (report_no_archive()), `package_t` keeps its `natives` names and `linked`
libraries, natives_of() puts every package's in link order (finish(): a package's before its
dependencies') in `compilation_t::natives`, a link.hpp `native_libraries_t` (archives, system)
that link_program() puts after the objects, and -l<name> after the runtime; the module table's
add_package() takes each package's natives (`natives()`), which sema's check_library() checks
`Lib "name"` against; link_errors() (`link_errors_t`, complete when they're all the linker's)
turns lld's errors into errors at an undefined symbol's first Extern Function (undefined_symbol(),
with Apple's '_' only there) and a missing system library's project.toml line
(`compilation_t::system_spans`), which the CLI's report_build_error() shows before the rest;
link_program() passes `--error-limit=0`, and on Windows paths that aren't ASCII by their short
names or as ASCII copies (linker_path(), `link_folder_t`), writing such an executable in the
temporary folder and moving it (move_output()); the loader checks each archive starts as one
(is_archive()) and finds a file named in full whatever its extension (extension_is()).
tests/native's vecmath C library (CMake target fb_vecmath, built for UCRT on MinGW, FB_TEST_VECMATH)
is linked by tests/packages/vmath's [native], which native_calls depends on, with `m = { system =
"m" }`; program_tests.cpp's staged_packages() copies tests/packages into the build folder with the
archive in libs/.
Step 7, pointers and `Unsafe`: a PTR type kind (`type_table_t::pointer()`, "Ptr<Void>" for NONE)
with one field, "Value", so `p.Value` and `p[i]` are place steps that codegen's address_of() and
element_address() go through (pointee(), which panics on Null in debug builds); `unsafe_stmt_t`
and `modifiers_t::is_unsafe` (parse_unsafe(), line_ends(), next_line_is_body() for "Unsafe
Method" in an Interface); sema's `m_unsafe` (in body_t) and require_unsafe(), pointer_of() and
not_c_data() (Structs checked after declaring, check_pending_pointers()), null_beside() for a
Ptr's and CString's Null, Ptr<Void> in converts_plainly(), check_pointer_arithmetic(),
check_pointee(), check_pointer_index(), pointer_cast() for As, check_address_of() (`expr_info_t::
address_of`) and check_size_of() with c_layout(), check_unsafe_function() and the Override and
Interface rules, and a CString's Unsafe ToString (`analysis_t::cstring_text`); MIR's
`address_of_t` and `pointer_t`, which lowering emits for AddressOf, `p + n`, `p - q`, and op=;
std/ffi/mem.fb's Mem; tests/programs/unsafe.fb, panic_null_pointer.fb, panic_null_cstring.fb,
and vecmath's pointer functions through vmath (Sum, Filled, PairTotal, Shout, Numbers).
Its review's fixes: type_table_t::has_references() stops at a Ptr, so a Struct can point at its
own, and check_struct_cycles() runs before anything asks for a size; sema's is_c_pointer() types
aren't shown in a string (showable()) or taken by `??` and `?.` (check_coalesce(),
check_safe_object()), while a generic's T? of a Ptr is a real T?; points_at_text() decides which
Extern Functions take no String for a CString; AddressOf goes through check_changeable() and
takes a Struct's Self; through_element() lets what a Ptr points at go ByRef; in_c() makes every
pointer one type for check_extern_symbols(); For ... In needs Unsafe code for an Unsafe Iterate;
an Unsafe Function's defaults are Unsafe code; c_layout() saturates and SizeOf refuses what an
Int64 can't count; lowering's settled_place() settles a Ptr like a T[], and reflected_methods()
leaves Unsafe Methods out; tests/programs/pointers_more.fb. Its second review's: check_size_of()
declares what it measures first while types are declared (declare_held(), declare_early(),
`m_declaring`, `m_enum_types`), and refuses a Struct whose size depends on itself, as an early
Const Function run waits for one (holds_itself() in reach_type()); not_c_data() and
needs_objects() pass a Struct once; settle_string_parameters() decides the escape rule for
`m_c_functions` once types are declared, Ptr parameters too; AddressOf takes no ReadOnly Static
Field, temporary's field, or Self in a lambda; a Spawn's work gives no Ptr (holds_cstring());
through_pointer() places aren't checked for overlapping ByRef; lowering's CString ToString takes
Self by name.
Step 8, `@CLayout` and the C ABI: `modifiers_t::is_c_layout` (parse_attributed() takes it only
before a Struct); sema's check_c_layout() after link_structs(), is_c_layout(), plain_struct_in(),
and check_c_struct() for what an Extern Function sees; compiler/include/fb/c_abi.hpp's
c_layout() (which SizeOf uses), c_abi_of(), and the pure classify() (`c_passing_t`: DIRECT
pieces, INDIRECT, MEMORY; `c_registers_t`); codegen's declare_c_structs() and call_c_structs()
(`c_signature_t` per Extern, `m_c_signatures`), with sret and byval attributes, entry_alloca(),
and piece_type(); tests/compiler/c_abi_tests.cpp checks classify() for both ABIs and the IR for
x86_64-unknown-linux-gnu and x86_64-w64-windows-gnu, and vecmath's by-value Structs run through
vmath's Structs(). Its review's fixes: lowering's check_enum_fields() (holds_checked_enum()) checks
each Enum C gives in a Struct, by value or ByRef, through nested Structs and fixed arrays; the
parser checks @CLayout before an Enum; codegen refuses a by-value Struct for a C ABI it hasn't
(`m_unsupported`); c_layout() lists fields only for values of 16 bytes or less; vm_spans takes
seven Spans, which SysV passes partly on the stack; tests/packages/panic_enum_result and
panic_enum_byref, whose dependency's paths program_tests.cpp's from_folder() makes relative.
Its second review's: check_c_struct() refuses a by-value Struct of 4 GiB or more, the Enum-field
panics name the Field (check_enum_fields() takes a message function), and the parser leaves
@CLayout before Shared Struct to the Shared error.
Step 9, `Callback`: `function_type_t::callback` (parse_function_type() for KW_CALLBACK) and a
CALLBACK type kind (`type_table_t::callback()`); sema's callback_of() keeps it to C scalars,
check_callback_function() and check_callback_lambda() make one where a Callback is expected
(`expr_info_t::callback`, a worker each), check_operands() gives code beside a Callback its type
(is_code()), and check_invoke() calls one in Unsafe code; lowering's `callback_t` (a Function's
code from function_of(), or a lambda's code_of()) and invoke() for CALLBACK; codegen's
value(callback_t) makes a thunk per function ("fb.callback.<name>", `m_thunks`) with C's
extensions (c_extension(), extend_call_for_c()), and call_callback() calls through one;
tests/programs/callbacks.fb sorts with qsort, and vecmath's vm_apply, vm_count_if, vm_each, and
vm_measure_all call back through vmath's Callbacks(). The thunk checks each Enum C passes
(is_enum_value(), at `callback_t::span`), as vm_paint shows, which
tests/packages/panic_enum_callback panics on. Its review's fixes: check_callback_function()
refuses a ByRef parameter and reports two imports' Functions that fit, check_qualified() and
is_code() take a qualified Function, match() ranks code against an overload's Callback, quietly()
drops the workers its try added, type_table_t's copy keeps `m_callbacks`, codegen's convert()
and c_layout() treat a Callback as a pointer, lowering's invoke() checks an Enum a Callback gives
(`invoke_t::span`, which debug builds' call_callback() panics at on Null), lookup_callee() calls
`p.Value(5)`, and an array of Callbacks is named "(Callback(Int32): Int32)[]"; vecmath's
vm_colorer_of and vm_handle, tests/packages/panic_enum_callback_result, and
tests/programs/panic_null_callback.fb.
Step 10, bindgen: tools/bindgen's fb_bindgen library (include/fb/bindgen.hpp's
generate_bindings(), `bindgen_options_t`, `bindings_t`) over libclang (cmake/libclang.cmake's
fb_libclang, beside LLVM-C; without it nothing of bindgen is built), and the fb-bindgen tool next
to fb, which `fb bindgen` runs (`command_t::BINDGEN`, `options_t::passed`, process.hpp's
current_executable()); its generator_t names the header's structs and enums (a typedef's name
first, a '_' for a keyword or a taken name), maps C types (map_type(): integers by size,
CString for const char*, Callbacks, Ptr<Void> for void* and opaque structs, fixed arrays),
checks each struct's layout (problem_of()), makes enums Enums or Consts and number macros Consts
(read_macros(), evaluate(), constant_of()), wraps lines at 100 columns, and skips the rest with a
comment and a warning; bindgen_config.hpp's system_includes are the C compiler's include folders;
tests/bindgen/bindgen_tests.cpp (fb_bindgen_tests) checks each mapping and that the output
compiles, and the CLI tests run `fb bindgen`. Its review's fixes: decide_records() decides every
struct before anything's written (`m_deciding` makes a pointer a Ptr<Void> meanwhile), parameters
are the canonical type's, so an array is a pointer, read_macros() and evaluate() have clang work
out each macro's last definition in a values file (constant_of(), integer_literal(),
float_literal()), undefined() finds #undefs, could_be_number() keeps statements out, namer_t
names each scope (valid_name(), reserved()), gather() and emit_definition() bind what structs and
unions define inside them, each_constant() skips enum attributes, in_main_file() and place_of()
take a macro's expansion, wrapped() breaks after a comma or a parenthesis, errors_of() gives a line
per error, bindgen_config.hpp has c_standard and resource_dir (cmake/libclang.cmake's
FB_LIBCLANG_RESOURCE_DIR) and leaves the compiler's own folders out, `bindgen_options_t::standard`
is `--std`, and fb takes `--color` itself and passes `--fb` first, so fb-bindgen names itself
`fb bindgen`. Its second review's: evaluate() works out every macro in one values file, with
`#ifndef` for #undefs (undefined() went), a second copy after `#line` (position-dependent values),
names under `m_prefix`, which no macro starts with, and an error, -Werror warning, or has_comma()
on a macro's lines skipping it (each_diagnostic() reads a unit's diagnostics from one set, since
libclang makes it again for each clang_getNumDiagnostics()); could_be_number() follows the macros a
body uses
(`m_every_macro`, `m_could_be_numbers`) through could_be_expression(), which refuses "::";
convention_of() and described() skip a function or callback of another calling convention than
the target's C one; add_type() makes a flag enum Consts; parameter_name() takes a name from any
declaration (`function_t::declarations`); place() breaks a line inside its first parentheses,
each part again where it doesn't fit; the C library's folders come after clang's own headers
(-idirafter), `bindgen_config::gnuc_version` is -fgnuc-version, target_parts() compares triples,
and unreadable() names a target or C standard libclang doesn't know.
Step 11, the done-when package: tests/packages/bound calls vecmath only through vecmath.fb, which
fb-bindgen generated from vecmath.h (numbers, strings, pointers, structs, an enum, constants, a
callback, and vm_bag, an opaque handle), and bindgen_tests.cpp makes it again and compares the
module after its first lines (FB_TEST_VECMATH_HEADER, FB_TEST_PACKAGES), on x86-64. Its review's
fixes: sema's same_c_type() makes two @CLayout Structs of one shape one C type for
check_extern_symbols(), whose signature_difference() says what differs
(tests/programs/extern_twice.fb binds div twice); fb-bindgen skips a by-value struct where
c_abi_of() is OTHER (by_value_struct()); a package's files are named from the current folder
(shown_disk_path()); vecmath handles a failed allocation, and bound frees its bag with Defer.

Finishing M5, the lows: instances carry their generic type's module, and the compiler's own
Classes, like Cell<T>, none (type_t::made); name_start() compares modules; an ambiguous name's
uses give INVALID; std_constraint() and std_hidden() hint where std's Printable is meant;
declare_qualifier() refuses a built-in's name; import_advice() (module_t::path, add_dependency(),
depends_on(), has_manifest()) says how a file imports another or which package it would depend
on; modifiers_lead_to() and the attribute-order messages; a variable without a name is an
error_stmt_t; unseen_hint() gives a `hint_t` that label_hint() labels; written_instance() names a
generic constructor; check_shown() names the Interface instance; a plain Extend Method sits beside
a hidden one; from_failed_import() quiets uses of what a failed Import would bring;
std_member_home() and `std_name_t::extended`; check_read() and initialize_early() order a Static
Field's reads; hides_qualifier(); check_written_arguments() for Super.New and types that aren't
generic; name_static_instance(); what the code can't use is still what it names once reported;
`m_misplaced_externs`; narrows() treats Protected and Package as neither covering the other;
`unsendable_t::parameter` names a type parameter among a message's types;
drop_clashing_extension() refuses an Extend's Method that a default the type's declaration lists
gives it; codegen's field_label() names a hidden base Field by its Class in leak reports
(tests/programs/leak_hidden.fb); owner_in() names an instance base, like A<Int32>, in
check_inherited()'s messages, which report a hidden Method of the types only when the code can
use none; refined_result() and check_refinements() report, once at the Interface, a member that
takes the parameters of one it extends but gives another result; refines_inherited() and
drop_overridden() give a Class that lists an Interface refining a default its base took that
Interface's default, as an override, as in Java; check_replaced_default() refuses another
Interface's default, seen less far than the type, in place of an Interface's own; manifest.cpp's
error_span() puts invalid UTF-8 at its byte; process.hpp's real_path() and the loader's
linked_away() and report_linked() keep Imports, and library_files() (is_link()), from leaving a
package through a link or junction, and nested_package() asks the disk (equivalent()); dump_tree()
and dump_types() take `package`, and the CLI's package_tokens() shows a library's own files;
lowering's number_alike_names() numbers MIR names that come out alike ("x.y.Z()#2"). std.ffi is
mem.fb's Mem (Alloc, Resize, Free, Copy, Fill, Compare, over the runtime's fb_mem_*) and
cstring.fb's `Extend CString` (From, over fb_cstring_new, Length, over fb_cstring_length, Free),
with tests/programs/leak_cstring.fb and panic_cstring_length.fb.

The project reviews' fixes: nested optionals, `T??`: substitute() keeps the nesting, a T? of a
nullable type is {flag, T?}, sema's converts() and convert() go by nullable_depth() (a value wraps
into a deeper T?; a Null constant of a shallower one isn't folded, and the evaluator refuses one),
narrowing, `??`, and `?.` peel one level, lowering's variable_place() steps into "value" once per
level, and codegen's to_nullable() keeps a flag of the same depth
(tests/programs/nested_optionals.fb); a function an initializer calls that reads a Global main
hasn't set panics (MIR `ready_t`; global_order.fb, panic_global_order.fb, panic_global_cycle.fb),
in release builds only where codegen's holds_null_object() says its zero would crash
(release_global_zero.fb, release_panic_global_order.fb, which program_tests.cpp builds in release
mode);
check_expansion() refuses polymorphic recursion, only_gives_value() a SizeOf alone, and
check_not_protected() a Protected Extern block; the parser's max_nesting (1000) and
cmake/stack.cmake's 64 MiB stack; the evaluator's 256 MiB `m_held`; an Extern Function as a
Callback is the C function; lowering's pinned() and pinned_place() keep what a call borrows from a
Global, field, or element alive through it (borrowed.fb); a Parallel For can't read other indexes
of an array it changes; a Static Field or Property holding a Func is called through it
(static_value()); ambiguous_at() keeps an ambiguous name's uses from giving a type; bindgen's
name_field_structs() names a field's unnamed struct "<struct>_<field>". The runtime: a Scheduler
lives through its own Tick, realloc() never takes 0 bytes, the scheduler's end() destroys timers
nobody awaits and coroutines that wait on nothing that will run (stuck()), fb_channel_element()
visits a Channel's elements for leak reports, a finished Thread is joined at the next start,
Parallel For shares one range and runs first through last inclusive, panics keep long names and
NULs, and freeing a CString the runtime didn't copy panics. The CLI: display_width() for carets,
a window on long lines and 100 diagnostics at most, an ASCII temporary folder for the linker, -o
checks, `fb run x.fb -- args`, exit codes, and the help forms. std: iterators fail fast when
their collection changes (`Changes`), Deque's readers don't move it, PriorityQueue has Iterate and
Pool has Clone and `Used`, Handle is ReadOnly and Hashable, Map mixes its hashes, and Map's and
Set's text is JoinedText().

## M4, concurrency

Done: M4 (concurrency: Sendable, Shared Class, tasks, `Parallel For`, async and coroutines,
channels, Mutex and Atomic), planned 2026-10-17 and finished 2026-10-18, in 11 steps.
Step 1, threads in the runtime: runtime/src/thread.cpp's `pool_t` starts its workers
on the first job, one per core but one, each with a `queue_t` (own newest first, then steals the
oldest); `fb_job_start()`, `fb_job_done()`, `fb_job_wait()` (which runs the jobs a worker started
itself meanwhile, run_own()), and
`fb_job_release()`; `fb_parallel_for()` hands out chunks from a counter, the caller taking some;
main calls `fb_pool_finish()` before releasing the Globals, and fb_finish() again;
`fb_job_start_thread()` for a job on a thread of its own; fb_print() holds `output()`,
and fb_panic() and fb_error_uncaught() hold `ending()` and end with end_program() (`_Exit`).
Step 2, `Sendable`: an Interface in std/core/interfaces.fb (`builtin_interfaces_t::sendable`)
that sema's unsendable() decides from a type's parts, saying why not; implements() and the
exported fb::implements() (conformance_query_t::sendable()) ask it; conform() checks a type that
lists it or an Interface extending it; it's constraint_only, and Interfaces extending it aren't.
Step 3, `Atomic` and `Mutex`: ATOMIC and MUTEX type kinds (`type_table_t::atomic()`, `mutex()`;
a Mutex's fields are state, owner, and value, as `fb_mutex_t` and then T); sema's in_place() types
are kept where they're made by keep_in_place() at declarations and parts of types, and
check_any() reports a copy unless `m_place_use` marks a Method's object, a Lock's Mutex, a ByRef
argument, or a Global's or Class Field's start; check_in_place_made() checks `Atomic<T>(value)`;
atomic_methods() are ByRef Methods; `lock_stmt_t` (parse_lock()) declares a ByRef PARAMETER for
the value; lowering's make_in_place(), atomic_call() (MIR `atomic_t`), and a Lock's `lock_t`
with an `unlock_t` cleanup, its value in `m_aliases`; codegen uses LLVM's atomics (an
Atomic<Bool> is a byte) and the runtime's fb_mutex_lock() and fb_mutex_unlock().
Step 4, `Shared Class`: `modifiers_t::is_shared` (parse_modified() keeps it to types),
`type_t::shared` (set_shared(), copied to instances); sema's check_shared() in link_class()
checks bases and Fields, and unsendable() lets Shared Classes through; `fb_class_t::shared`,
which codegen's class_value() fills, makes the runtime's retain, release, Weak, and discard
atomic for its objects (destroy_shared(), finish_shared(), `dying`, and the object's own Weak).
Step 5, `Spawn` and `Task`: TASK and THREAD type kinds (`task()`, `thread()`; fields job,
closure, and a Task's result, as `fb_task_t` and then T); sema's spawner() and check_spawn()
check the work and its captures, and task_methods() give Wait, Join, and IsDone; lowering's
spawn() fills a new object and starts it (TASK_START, THREAD_START), generated() RUN makes each
Task type's Class, whose first Method runs the closure and keeps the result, and task_call()
waits (TASK_WAIT, TASK_DONE); the runtime's fb_task_start() and fb_task_start_thread() run it
through fb_job_start() or fb_job_start_thread(), whose threads fb_pool_finish() joins.
Step 6, main-thread rules: `modifiers_t::is_main_thread` (@MainThread); sema's note_use() calls
note_thread_use(), which records each function's or lambda's (current_code()) first
`main_only_t` use and what it reaches in `m_reaches`; check_spawn() and check_shared() add
`worker_t`s, and check_threads() walks from each, through overrides, conformances, defaults, and
Destroys, reporting the shortest path to a use (check_worker()).
Step 7, `Parallel For`: `for_stmt_t::parallel` (parse_parallel()); sema's check_parallel_for()
checks the body as a lambda with `lambda_context_t::parallel`, whose capture() copies Locals,
parallel_outside() and check_parallel_element() keep outside variables unchanged and arrays
changed at the loop's own index, and `m_element_use` marks an outside array's allowed uses; the
closure_t has `body`, and stmt_info_t `closure`; lowering's lower_parallel_for() makes it and
calls PARALLEL_FOR, and lower_parallel_body() loops over a chunk, which the runtime's
fb_parallel_closure() runs.
Step 8, `Channel`: a CHANNEL type kind (`channel()`); sema's check_channel_made() and
channel_methods(); lowering makes one with MIR `channel_t`, channel_call() copies through a slot
local (CHANNEL_SEND, TRY_SEND, RECEIVE, TRY_RECEIVE, CLOSE), and lower_channel_iteration() is For
x In ch; codegen's channel_class() and generate_channel_functions() release what's left; the
runtime's `fb_channel_t` waits with pool_t::wait_until(), which runs no jobs.
Step 9, `Async` and `Await`: `modifiers_t::is_async` and `await_expr_t`; sema's
check_async_function(), an Async call typed as its Task, and the Await check (`m_locks`);
`mir_function_t::is_async`, whose code takes its Task first (`m_async_task`), keeps its
parameters, and finishes with finish_async() (ASYNC_FINISH); a call makes the Task (ASYNC_BEGIN)
and calls the coroutine; lower_await() emits AWAIT and a `suspend_t`; codegen's begin_coroutine()
uses LLVM's coroutine intrinsics (presplitcoroutine, coro passes in debug builds too); the
runtime's `scheduler_t` resumes coroutines whose Tasks are done, in fb_scheduler_finish() and in
fb_task_wait() on the main thread.
Step 10, coroutines and schedulers: the built-ins NextFrame, Seconds, RealSeconds (@MainThread),
and MainThread give a Task, which lowering's timing_call() makes and the runtime's timers finish
(WAIT_FRAME, WAIT_SECONDS); a SCHEDULER type kind, made by `Scheduler()` (check_scheduler_made(),
main-only) with scheduler_methods() Run, Tick, and IsIdle (scheduler_call()); AWAIT passes the
coroutine's own Task too; the runtime's scheduler_t has its time, ticks, timers, and
current_scheduler(), take() moves a coroutine and what it waits for, and fb_scheduler_free()
destroys what still waits, through each `suspend_t`'s cancel block, which releases what the
coroutine holds.
Step 11, loose ends: Async Throws, whose Task type is `task(result, throws, error)` ("Task<T>
Throws E", `type_t::throws`, with an "error" field last), its Wait symbol has the Throws, and
sema's Await check propagates them (untried()); lowering's takes_error() is false for Async code,
raise() puts the error in the Task (error_field()), and rethrow() checks it after a Wait or an
Await. Async Methods: call_on() passes the Task after Self, through the Method table's slot, and
Overrides match, but not in Interfaces. Async lambdas: `lambda_expr_t::is_async`, from
parse_lambda() after Async; the closure's code makes the Task and calls a coroutine of its own
(`m_coroutine_code`, lower_async_closure()). block_lambdas() lets collect_field_bounds() reach
member loops in lambdas in generic code. tests/programs/concurrency_mix.fb runs it all at once.

## M3, generics, interfaces, Extend, compile time, and reflection

Done: M3 (generics, interfaces, `Extend`, compile-time functions, reflection), planned and
finished 2026-10-17, in 13 steps. Step 1, programs of several files: the driver adds every
std/**/*.fb file after the main file (`standard_library()`, `build_config::std_directory`;
tests pass `library_file_t`s to compile()); parse_into() puts them all in one AST, library
items first, and `ast_t::from_library()` tells them apart; sema takes the source_manager_t,
finds Error in std/core/error.fb (`find_error_class()`), and reports code that runs in a
library file; lowering declares library functions in function_of() and Classes in use_class() when
the program uses them, then lower_library() lowers them after main; codegen names the module after
the main file and gives panics the right file. Step 2, tuples: TUPLE types in the type table,
interned by `tuple()`, with fields "0", "1", and so on; `tuple_expr_t`, `tuple_type_t`, and
`destructure_stmt_t` in the AST, and parse_element() reads ".0" and splits "t.0.1"; sema makes
each tuple's elements FIELD symbols (`tuple_elements()`), so member access and lowering's field
steps work as for Structs; a tuple assignment's target is a tuple_expr_t, and
`stmt_info_t::matches` holds a destructure's Locals; lowering makes tuples with `aggregate_t`, and
`has_fields()` covers Structs and tuples for ==, zeros, and strings. Step 3, function values:
FUNCTION types ("Func(A): R", `type_t::parameters`); `lambda_expr_t` and `function_type_t`; sema
checks a lambda's body in place, with `m_lambdas` marking where each one's scopes start, and
capture() records `capture_t`s (VALUE, CELL, WEAK) in each lambda crossed; `analysis_t::closures`
has each closure's Class ("Function at file:line:col"), Func type, and captures, and
`expr_info_t::closure` points at it; a captured Local has `symbol_t::cell`, a "Cell<T>" Class;
lowering's make_closure() fills a new object, code_of() declares the closure's code and its
mir_class_t, and lower_closure() lowers it after the function, copying captures into locals;
`invoke_t` calls slot 0 of the object's Method table; codegen's is_object() covers Classes and
function values. Step 4, generics: PARAMETER types; a generic Struct or Class has
`type_t::parameters` and is named "Box<T>", and `type_table_t::instance()` makes "Box<Int32>"
(`generic`, `arguments`), whose fields set_fields() fills in; `substitute()` rebuilds a type with
arguments in place; `symbol_t::type_parameters` for a function's own; sema's `m_type_scope` resolves
names in types, infer() and unify() find a call's type arguments, `m_substitution` puts them in
parameter and return types, and `expr_info_t::type_arguments` records a call's own; lowering copies
the type table into `mir_module_t::types` (a shared_ptr), concrete() substitutes the instance's
`m_parameters`/`m_arguments`, instance_of() declares an instance like "First<Int32>(Int32[])",
and add_class() gives each Class instance its own Method table. Step 5, operators:
`function_kind_t::OPERATOR`, parsed with parse_operator_name(); an Operator is a static METHOD
symbol named by its operator, like "+"; sema's resolve_operator() picks among the operands'
types' operators_of() and records the call on the expression (`symbol`, `arguments`,
`type_arguments`), or on an "op=" statement's stmt_info_t; lowering's lower_operator() and
call_operator() call it ("!=" negates "=="), and equal_operator() makes every == on a type use its
Operator ==. Step 6, interfaces: INTERFACE types (`add_interface()`, whose `inner` is its Self
parameter) and `type_t::interfaces` for Implements and Extends; `type_stmt_t::is_interface`,
`constraint_t`s on functions and types, and `required` members, which parse_function() and
parse_property() tell from the next line; sema's declare_interface_members() declares members with
Self in the type scope, conform() records each `conformance_t` and copies defaults into the type
(`symbol_t::default_method`), `constraint_only` marks Interfaces that can't be types, `m_bounds`
holds the constraints in scope (bounds_of(), implements()), and report_bounds() checks calls and
instances, or check_pending_bounds() once types are declared; a call of an Interface's member keeps
Self in `expr_info_t::type_arguments`; lowering's call_interface_member() and call_operator() call
what implements it, or go through the Interface's table (`call_t::interface`), add_class() makes
each `mir_implementation_t`, and accessor_of() handles Interface, Abstract, and Static Properties;
fb_class_t lists `fb_implementation_t`s, which fb_interface_methods() and fb_object_implements()
search. Step 7, built-in interfaces: std/core/interfaces.fb declares Equatable, Comparable,
Hashable, Printable, and Numeric, which sema finds by name (`analysis_t::interfaces`);
builtin_implements() says which the built-in types implement, builtin_methods() gives them
ToString and Hash, and synthesized() lets a Struct or Class that lists Equatable or Hashable keep
the built-in == or Hash, whose conformance member is the Interface's own; showable() goes through
Printable, and Strings order with `<`; lowering's implementation() is nullopt for the built-in
meaning, which builtin_operator() and builtin_method() lower, hash_of() and to_string() give a
value's Hash and text, and generated() makes each array type's ==, Hash, and text functions; the
runtime has fb_string_compare(), fb_string_hash(), fb_hash_integer(), fb_hash_float(),
fb_hash_object(), and fb_hash_combine(). Step 8, `Extend`: `extend_stmt_t`, from parse_extend();
sema's declare_extension() runs after every declare_item(), finds the target with
extension_target() (a type, a generic type's definition, or T[] over `m_array_element`), and adds
the members, marked `symbol_t::extended`, to a Struct's or Class's members or to `m_extensions` /
`m_array_extensions`, which members_of() reads; T[]'s members take T as their first type
parameter, which known_arguments() fills from the array; an Extend's Interfaces join
`type_t::interfaces`, or with a Where or for a built-in type, `analysis_t::extension_interfaces`,
which extension_implements() checks; link_extensions() conforms the built-in types, Enums, and
T[]; lowering keeps extended members out of the Method table, and extension_implementation()
finds a built-in type's conformance. Step 9, sequences: generic Interfaces, named like
"Iterator<T>", whose instances fill() gives substituted `interfaces`; an Interface's
owner_parameters() are Self, then its own, so its members' `type_arguments` are Self, then the
instance's (`self_arguments()`); a type implements one instance of each (`instance_for()`,
`interfaces_of()`), a conformance records it, `symbol_t::default_interface` holds a default's, and
infer() takes type arguments from constraints; `For ... In` over an `Iterator<T>`
(std/core/iterator.fb, `builtin_interfaces_t::iterator`) or what an `Iterate()` gives
(`stmt_info_t::next` and `iterate`), lowered by lower_iteration() with call_method(); `.Chars` and
`.Bytes` fill a new array with the runtime's fb_string_chars() and fb_string_bytes();
`slice_expr_t`, lowered with a bounds check and an ARRAY_ADD loop; variadic parameters
(`parameter_t::variadic`, `symbol_t::variadic`), whose arguments are `binding_t::packed` and
`expr_info_t::packed`, which lowering's array_of() makes into an array. Step 10, compile time:
`#If`, `#ElseIf`, `#Else`, and `#End If` in the lexer, which takes the symbols from
`defined_symbols()` (compile() with a library and compile_file() take the build mode) and skips
the lines it leaves out; `modifiers_t` has is_const, is_inline, and deprecated, from
parse_attributed(), and sema's note_use() warns about @Deprecated uses; `mir_function_t::is_inline`
becomes LLVM's alwaysinline, with the always-inline pass in debug builds; Globals and Consts are
declared first, and initialize_early() checks a Const's initializer when something needs its
value, in a fresh `body_t` (swap_body()); run_at_compile_time() runs a Const Function's call with
known arguments: check_const_body() checks its body on demand, lower_function() lowers it, and
evaluate() (compiler/src/evaluate.cpp) runs the MIR on a stack of frames; a Const type parameter
is a PARAMETER with an `inner`, its arguments are VALUE types (`type_table_t::value()`), a fixed
array's size can be one (`type_t::parameters`), `value_type_t` is a value in type arguments,
resolve_argument() reads one, and `expr_info_t::parameter` marks a Const type parameter used as a
value, which lowering gives its instance's number. Step 11, reflection: `Attribute Name(params)`
is a `type_stmt_t` with is_attribute, from parse_attribute_declaration(), and a use is an
`attribute_t` in `modifiers_t::attributes`, with `argument_t`s; sema's check_attributes() runs
after the defaults and puts each one's `attribute_value_t` in `symbol_t::attributes`;
`for_member_stmt_t` is For Field or For Method, whose variable is a MEMBER_VARIABLE symbol and
whose stmt_info_t `type_arguments` are {T, F}; collect_field_bounds() makes F (`m_loop_fields`)
when the function is declared and turns the Where lines into `bound_t`s with `field` set, which
field_misfit() checks at calls; check_member_variable() and check_member_call() check f.Name,
f.Get(value), and the rest; lowering's lower_stmt() makes a copy of the body per field or
reflected_methods() Method, with F in m_parameters and the member in `m_reflected`, and
lower_reflection() lowers the calls. Step 12, std.collections: std/collections/*.fb (array.fb's
`Extend T[]`, Map, Set, Stack, Queue, Deque, PriorityQueue, and Pool), which the user typed;
`map_expr_t` is a `{key: value}` literal, which sema checks against std's Map (`m_map`, from
find_map_class()) with common_type_of(), recording its Add in `expr_info_t::symbol`, and
lowering makes with new_object() and a call of Add per entry; own_members() finds an array's
Methods in an Extend of T[], and `expr_info_t::count` marks Count alone there; sema's symbols are
a std::deque, so references to them survive add_symbol(); changed_in() reaches calls inside
array and Map literals, indexes, slices, and Is. Step 13, loose ends: `is_later_name()` for
names later milestones declare; an integer's Wrapping and Checked Methods (`integer_methods()`,
`integer_method()` and `fold_integer_method()` in const_value, `binary_t::overflows`, lowered by
integer_call()); Interface constraints, Const type parameters, Throws (`m_takes_error` in
lowering), and generic Methods (`own_type_parameters()`, skipped in interface_slots());
`For x In Try items` (check_iterate_call(), checked_call()); Extends of one instance or array type
(`is_instance()`, `is_every_array()`, own_view()), and conditional Interfaces as values through
`fb::implements()`, which lowering's implemented() asks; generic bases (`base_arguments`,
`member_type()` and owner_arguments() through the bases); generic Statics (instance_global(),
global_of(), Static Methods keeping their instance's type arguments); generic Const Functions
(`m_runs` and `m_const_modules` by instance), run_early() with `m_member_types` for sizes in
declarations' types, and `value_op_t` VALUE types for `N + M` (computed_value()); reflection's
`member_kind_t`, reflected_types(), method_signature(), `expr_info_t::reflected` for `T.Has(A)`,
check_default_loops(), and closure_copy() for lambdas in the loops.

## M2, data

Done: M2 (data: structs, classes, refcounting, arrays, errors, `Defer`), planned 2026-10-16 and
finished 2026-10-17, in 11 steps. Step 1, the type table (`type_table_t` in types.hpp: built-ins
at fixed ids, other types interned; the analysis owns it and MIR points at it). Step 2, structs:
`Struct` with `Field`, `Method`, `ByRef Method`, and `New`; the generated constructor's parameters
are the field symbols; MIR places have field paths, plus `aggregate_t` and `zero_t`; retain and
release walk a Struct's Strings in codegen. Step 3, classes: objects with a header (refcount,
`fb_class_t`) from runtime/src/object.cpp; `new_t` in MIR; codegen makes each Class's `fb_class_t`
and destroy; `RefEquals`; Strings' refcounts are atomic. Step 4, nullability: a `T?` in the type
table has one field, "value", which a place steps into once a null check narrows it
(`flow_t::narrowed`); `Null` is a const_value_t holding nullptr; `?.` and `??` lower to branches; a
String? or Class? is a pointer, and any other T? is {flag, T}; every New assigns the fields without
a zero. Step 5, inheritance: `type_t::base`; sema's link_classes() puts base fields first and sets
`overrides`; a Class's New is an MIR function that sets up an object its caller made (`base_new`,
Super.New); `mir_class_t::methods` is the Method table that `call_t::slot` calls through; `is_t` and
`downcast_t` use the runtime's fb_object_is and fb_object_expect. Step 6, arrays: `T[]` is an
object (`fb_array_t`, runtime/src/array.cpp) with a Class per array type that codegen makes;
FIXED_ARRAY types are inline; MIR places have `steps`, each a field or an element with index
locals; `array_t` makes arrays; runtime_function_t has the T[] operations; sema makes each T[]'s
Methods as symbols owned by the array type. Step 7, enums: ENUM types keep their integer type
and `enum_value_t`s in the type table; sema folds Enum constants as their integers; `As` to an
Enum checks membership in lowering; lowering makes a "State.Text(State)" function for strings;
`stmt_info_t::exhaustive` marks a Select that covers every value. Step 8, `Defer`: sema checks
the statement where it's written in a copy of the flow, forgetting narrowing for what the rest of
the block changes; lowering's scopes hold `cleanup_t`s (a variable to release or a statement to
run), and each way out lowers the deferred statements again. Step 9, errors: the Error Class
(`analysis_t::error_class`, now from std/core/error.fb); `symbol_t::throws`; sema's
`handler_t` stack checks that each error is caught or in Throws; a function that throws takes its
caller's `Error?` as a hidden ByRef last parameter, checked after each call; lowering's `raise()`
goes to the innermost Try? or Try block, or out through that parameter; a Finally is a scope's
`cleanup_t`, like a Defer; Panic and Assert are built-ins (`panic_t::text`); a Class without a New
takes its base's News. Step 10, weak references and leak reports: a WEAK type kind; sema's
check_any() reads a Weak<T> as a T? (`read_type()`), except as an assignment's target; lowering's
convert() makes the WEAK_LOAD and the borrowing cast; objects have a 32-bit refcount and weak
count; `fb_class_t` has destroy, release, and visit, which codegen makes; debug builds call
fb_track_objects(), and fb_finish (now in runtime/src/object.cpp) reports cycles; a New can throw,
and OBJECT_DISCARD lets its object go without Destroy. Step 11, class extras: `modifiers_t` has
is_static and is_read_only, and variable_stmt_t has modifiers; a Static Field is a GLOBAL symbol
whose owner is its type, in `analysis_t::globals`, lowered as the global "Enemy.count"; a Static
Method has no Self; `property_stmt_t` holds two function_stmt_t accessors that the parser makes,
and a PROPERTY symbol has `getter`/`setter`; lowering's get_property()/set_property() call them,
through the Method table; check_changeable() catches ReadOnly Fields and Property copies.

## M1, the first compiler

Done: M1, finished 2026-10-16, in six steps: source and diagnostics; tokens and lexer; AST and
parser with error recovery (tests compare `fb::dump_ast`); semantic analysis (types, const_value,
sema); codegen (runtime, MIR, lowering, LLVM C API codegen, linking with lld; end-to-end tests in
tests/programs); and the `fb` CLI (`check`, `build`, `run`, `--show` for every stage, color).
`#If` directives are deferred past M1; the lexer reports them as not supported yet.
