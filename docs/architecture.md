# Architecture

## Tech stack (Locked)

| Area | Choice |
|---|---|
| Implementation language | C++23 |
| Build system | CMake, dependencies via FetchContent |
| Compiler backend | LLVM, linking with `lld` (no system linker needed) |
| Renderer | **Own Vulkan backend** from the start (not needed until Phase 2) |
| Platform layer | SDL3 (window, input, events) |
| Audio | miniaudio |
| Physics | Jolt |
| Assets | fastgltf, stb_image, KTX |
| Target platforms | Windows, Linux, macOS first; mobile and consoles later |
| License | MIT |

## Repo layout

```
FutureBlitz/
├── .clang-format  .clang-tidy  .editorconfig  .gitignore
├── CLAUDE.md
├── CMakeLists.txt
├── cmake/warnings.cmake
├── compiler/   include/fb/  src/   lexer, parser, semantic analysis, MIR, LLVM codegen
├── runtime/    include/fb/  src/   refcounting, strings, panics, scheduler, task pool
├── cli/        src/                 new / build / run / test / package manager
├── lsp/        src/                 language server (reuses compiler)
├── std/        standard library, written in FutureBlitz
├── engine/     C++ engine core and its FutureBlitz bindings
├── tools/      asset cooker, bindgen, migrator, .b3d → glTF converter
├── ide/        the IDE (late)
├── tests/      compiler golden tests, runtime tests, sample programs
├── docs/       this documentation
├── LICENSE
└── README.md
```

## CMake targets

Target names match project names.

| Target | Kind | Links |
|---|---|---|
| `compiler` | static lib | LLVM's C API (`LLVM-C`, see `cmake/llvm.cmake`) |
| `runtime` | static lib | |
| `cli` | executable `fb` | `compiler` |
| `lsp` | executable | `compiler` |

- Headers live under `<project>/include/fb/`; everything is in namespace `fb`.
- Sources are globbed with `CONFIGURE_DEPENDS`; each project needs at least one `.cpp`.
- `fb_set_warnings(target)` in `cmake/warnings.cmake` applies the warning set
  (`/W4 /permissive-` on MSVC; `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast -Wnull-dereference` elsewhere).

## Compiler pipeline

```
source → Lexer → Parser → AST → Resolve names → Type check → MIR → LLVM IR → native
                (error-recovering,      (generics, Sendable,   (refcounts, Defer, Try,
                 serves the LSP)         definite assignment)   async, monomorphize)
```

| Stage | Responsibility |
|---|---|
| Source & diagnostics | `source_file_t`, `span_t`, `diagnostic_t`; exact spans everywhere (the LSP depends on them) |
| Lexer | tokens, string interpolation, literals, `#If` directives; comments kept as trivia for the formatter and LSP |
| Parser | hand-written recursive descent; **error recovery designed in from the start** |
| Name resolution | modules, imports, scopes |
| Type checking | inference for locals, generics and constraints, nullability narrowing, definite assignment, `Throws` checking, Sendable/`@MainThread` checks |
| MIR (mid-level IR) | our own IR where heavy features lower to plain operations: refcount insertion, `Defer`, `Try`, async state machines, monomorphization |
| Codegen | MIR → LLVM IR → object → `lld` |

The compiler is a library from day one so `cli` and `lsp` share it.

## Runtime

C++ library linked into every FutureBlitz program:

- refcounting (non-atomic and atomic variants; thread-aware from M2 so concurrency needs
  no rewrite later)
- strings
- panics and stack traces
- task pool (work-stealing), schedulers, channels, mutexes, atomics

## Tooling

| Tool | Purpose |
|---|---|
| `cli` (`fb`) | `build`, `run`, `check`, and `--show` for every stage (M1); `new`, `test`, package management later |
| `lsp` | language server: diagnostics, completion, go-to-definition, etc. |
| VS Code extension | interim editor support until the IDE exists |
| Formatter | enforces style, including inline-vs-`Where` generic constraints |
| `bindgen` | C header → `Extern` module (libclang) |
| Asset cooker | source assets → cooked, compressed, packed |
| Migrator | Blitz3D `.bb` → FutureBlitz (see [migration.md](migration.md)) |
| IDE | own IDE, built late (Phase 2) |
