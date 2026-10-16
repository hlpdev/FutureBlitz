# C++ Conventions

`.clang-format` and `.clang-tidy` in the repo root are authoritative. This is the summary.

## Naming

| Thing | Style | Example |
|---|---|---|
| Namespaces | `lower_case` | `fb`, `fb::mir` |
| Classes, structs, unions, enums, aliases, typedefs | `lower_case` + `_t` | `token_t`, `span_t` |
| Functions, methods | `lower_case` | `next_token()` |
| Variables, parameters | `lower_case` | `line_count` |
| Public members | `lower_case` | `begin`, `end` |
| Private / protected members | `m_` + `lower_case` | `m_source` |
| Enum constants | `UPPER_CASE` | `TOKEN_IDENTIFIER` |
| Global constants, `constexpr` variables | `lower_case` | `max_errors` |
| Concepts | `lower_case` | `node_like` |
| Template parameters | `CamelCase` | `T`, `TValue` |
| Macros | `UPPER_CASE` | `FB_ASSERT` |

## Formatting

- 4 spaces, no tabs, 100 columns
- Attached braces (`BreakBeforeBraces: Attach`)
- `int* p` / `int& r` (left pointer and reference alignment)
- Empty functions and blocks may be on one line; other short functions, ifs, and loops may not
- Declarations and assignments are **not** aligned; consecutive bit fields and macros are
- Spaces inside braced lists: `{ .line = 0, .column = 0 }`
- `template <...>` on its own line; access modifiers flush left
- Includes regrouped and sorted case-insensitively

## Language and build

- C++23, `CMAKE_CXX_EXTENSIONS OFF`
- CMake 3.28+, dependencies via FetchContent
- `compile_commands.json` exported
- Warnings via `fb_set_warnings(target)`

## Layout

- Public headers: `<project>/include/fb/...`
- Sources: `<project>/src/...`
- Everything in namespace `fb`

## clang-tidy notes

Enabled groups: `clang-analyzer-*`, `bugprone-*`, `performance-*`, `portability-*`,
`readability-*`, `modernize-*`, `misc-*`.

Notable exclusions:

- `misc-no-recursion` (recursive-descent parser trips it constantly)
- `modernize-use-trailing-return-type`
- `readability-magic-numbers`, `readability-identifier-length`,
  `readability-function-cognitive-complexity`, `readability-isolate-declaration`
- `bugprone-easily-swappable-parameters`
- `misc-non-private-member-variables-in-classes`, `misc-include-cleaner`

Third-party code under `_deps/` is excluded from header diagnostics.

## Editor config

UTF-8, LF line endings, final newline, trimmed trailing whitespace; 4-space indent
(2 for CMake, YAML, TOML).
