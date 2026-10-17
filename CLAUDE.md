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
M2 (data: structs, classes, refcounting, arrays, errors, `Defer`), planned 2026-10-16 in 11
steps (docs/milestones.md). Step 6, arrays, is built except runtime/src/array.cpp, which Hunter
types; then step 7: enums.

Done in M2: step 1, the type table (`type_table_t` in types.hpp: built-ins at fixed ids, other
types interned; the analysis owns it and MIR points at it). Step 2, structs: `Struct` with
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
Methods as symbols owned by the array type.

Done: M1, finished 2026-10-16, in six steps: source and diagnostics; tokens and lexer; AST and
parser with error recovery (tests compare `fb::dump_ast`); semantic analysis (types, const_value,
sema); codegen (runtime, MIR, lowering, LLVM C API codegen, linking with lld; end-to-end tests in
tests/programs); and the `fb` CLI (`check`, `build`, `run`, `--show` for every stage, color).
`#If` directives are deferred past M1; the lexer reports them as not supported yet.
