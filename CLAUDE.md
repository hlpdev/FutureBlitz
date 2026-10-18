# FutureBlitz

From-scratch Blitz3D successor: language (.fb), compiler, tooling, then engine.
Source of truth: docs/spec/

## How we work
- I type all C++ and other real code myself. Write it in chat; I'll rewrite it.
  Keep snippets focused on the current step.
- You write the tests (tests/) directly, in my code style.
- You may do project setup yourself: CMake, tool configs, dependencies, stub files, and docs
  when I ask.
- When I ask for a review, read my files and point out problems. Fix minor issues yourself
  (formatting, naming, comments, small portability fixes); leave real changes to me.
- Keep explanations short.

## C++ conventions
- C++23, CMake, deps via FetchContent
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE; template params CamelCase
- .clang-format and .clang-tidy are authoritative
- Style: never align declarations or assignments; spaces inside braced lists `{ .line = 0 }`
- Headers in <project>/include/fb/, namespace fb
- Tests: Catch2 via FetchContent; compiler unit tests in tests/compiler/ (fb_compiler_tests), run with ctest

## Projects
compiler (lib), runtime (lib), cli (exe), lsp (exe)

## Current step
M3 (generics, interfaces, `Extend`, compile-time functions, reflection), planned 2026-10-17 in
13 steps (docs/milestones.md), all done. Next: plan M4 (concurrency). docs/milestones.md's
Deferred work lists everything left for later, and where it's planned.

Done in M3: step 1, programs of several files: the driver adds every std/**/*.fb file after the
main file (`standard_library()`, `build_config::std_directory`; tests pass `library_file_t`s to
compile()); parse_into() puts them all in one AST, library items first, and
`ast_t::from_library()` tells them apart; sema takes the source_manager_t, finds Error in
std/core/error.fb (`find_error_class()`), and reports code that runs in a library file; lowering
declares library functions in function_of() and Classes in use_class() when the program uses
them, then lower_library() lowers them after main; codegen names the module after the main file
and gives panics the right file. Step 2, tuples: TUPLE types in the type table, interned by
`tuple()`, with fields "0", "1", and so on; `tuple_expr_t`, `tuple_type_t`, and
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
function values. Step 4, generics: PARAMETER types; a generic Struct or Class has `type_t::parameters`
and is named "Box<T>", and `type_table_t::instance()` makes "Box<Int32>" (`generic`, `arguments`),
whose fields set_fields() fills in; `substitute()` rebuilds a type with arguments in place;
`symbol_t::type_parameters` for a function's own; sema's `m_type_scope` resolves names in types,
infer() and unify() find a call's type arguments, `m_substitution` puts them in parameter and
return types, and `expr_info_t::type_arguments` records a call's own; lowering copies the type
table into `mir_module_t::types` (a shared_ptr), concrete() substitutes the instance's
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

Done: M2 (data: structs, classes, refcounting, arrays, errors, `Defer`), planned 2026-10-16 and
finished 2026-10-17, in 11 steps. Step 1, the type table (`type_table_t` in types.hpp: built-ins
at fixed ids, other types interned; the analysis owns it and MIR points at it). Step 2, structs: `Struct` with
`Field`, `Method`, `ByRef Method`, and `New`; the generated constructor's parameters are the
field symbols; MIR places have field paths, plus `aggregate_t` and `zero_t`; retain and release
walk a Struct's Strings in codegen. Step 3, classes: objects with a header (refcount, `fb_class_t`)
from runtime/src/object.cpp; `new_t` in MIR; codegen makes each Class's `fb_class_t` and destroy;
`RefEquals`; Strings' refcounts are atomic. Step 4, nullability: a `T?` in the type table has
one field, "value", which a place steps into once a null check narrows it (`flow_t::narrowed`);
`Null` is a const_value_t holding nullptr; `?.` and `??` lower to branches; a String? or Class? is
a pointer, and any other T? is {flag, T}; every New assigns the fields without a zero. Step 5,
inheritance: `type_t::base`; sema's link_classes() puts base fields first and sets `overrides`;
a Class's New is an MIR function that sets up an object its caller made (`base_new`, Super.New);
`mir_class_t::methods` is the Method table that `call_t::slot` calls through; `is_t` and
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

Done: M1, finished 2026-10-16, in six steps: source and diagnostics; tokens and lexer; AST and
parser with error recovery (tests compare `fb::dump_ast`); semantic analysis (types, const_value,
sema); codegen (runtime, MIR, lowering, LLVM C API codegen, linking with lld; end-to-end tests in
tests/programs); and the `fb` CLI (`check`, `build`, `run`, `--show` for every stage, color).
`#If` directives are deferred past M1; the lexer reports them as not supported yet.
