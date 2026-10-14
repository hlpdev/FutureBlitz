# FutureBlitz

From-scratch Blitz3D successor: language (.fb), compiler, tooling, then engine.
Source of truth: docs/spec.md

## How we work
- I type every line of code myself. Never create or edit files.
- Write code in chat; I'll rewrite it. Keep snippets focused on the current step.
- When I ask for a review, read my files and point out problems. Don't fix them.
- Keep explanations short.

## C++ conventions
- C++23, CMake, deps via FetchContent
- snake_case; types end in _t; private/protected members use m_; enum constants UPPER_CASE; template params CamelCase
- .clang-format and .clang-tidy are authoritative
- Headers in <project>/include/fb/, namespace fb

## Projects
compiler (lib), runtime (lib), cli (exe), lsp (exe)

## Current step
M1 step 1: source and diagnostics types (source_file_t, span_t, diagnostic_t)
