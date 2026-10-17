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
12 steps (docs/milestones.md). Step 6: interfaces and constraints.

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
Operator ==.

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
