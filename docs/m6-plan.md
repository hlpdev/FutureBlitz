# M6's plan: lanes, reviews, and landing

How the rest of M6 gets built, settled with the user on 2026-10-22: first the whole-codebase
review's findings, then M6's steps 5 to 32. Four lanes work at once, each a Claude Code session in
a worktree of its own, and a coordinator session hands out the work, reviews it, and lands it. The
rules in CLAUDE.md and docs/conventions.md hold for every lane.

## Decisions

Settled with the user on 2026-10-22, before the lanes start:

- **A base New calling an Override** (A3-1) gets Swift's rule: a Class that extends another
  assigns its own Fields that have no zero before `Super.New`, by name, without using Self, so a
  base New's virtual calls never see one unassigned. Spec 06 changes to say so.
- **Language calls in findings and steps**: the lane decides where the spec or docs/decisions.md
  points one way, says so in its report, and the coordinator records it in decisions.md. Only a
  real trade-off with no clear answer goes to the user.
- **Findings**: every one is reproduced before it's fixed. One that doesn't reproduce, or that the
  spec settles the other way, is dropped, with why, in its Status line. The rest are fixed with a
  test: highs first, then mediums, then lows. Nothing found goes to Deferred work.
- **Reflection's `f.Set` on a ReadOnly Field** is an error where the loop's copy for it is made
  (A3-5, done); step 19's serialization decodes ReadOnly Fields through New.
- **Libraries**: a lane adds the C libraries docs/stdlib.md and docs/milestones.md name, through
  FetchContent, pinned to a release, with their licenses in the bundled notices as libuv's are.
  Any other library goes to the user first.
- **Models**: every lane and reviewer runs on the coordinator's model and effort.
- **Static linking** (2026-10-22): every third-party library std uses links statically; a
  program that uses only std is one executable that needs only the OS's own libraries. The
  toolchain may use DLLs.
- **Bcrypt** (2026-10-22) goes over Solar Designer's crypt_blowfish, bundled like the others.

## Roles

- **The coordinator** (the session that started the lanes) hands out work, answers the lanes'
  questions, runs each step's review, lands every branch on master, and alone edits CLAUDE.md,
  docs/milestones.md, docs/decisions.md, docs/history.md, and this file. It keeps every lane
  busy, moving unstarted work from a busy lane to a free one.
- **A lane** owns one unit of work at a time, a lane's findings or one M6 step, in a worktree and
  branch of its own: code, tests, the spec and docs/stdlib.md for what it changes, verify.py, and
  CI on its branch. It reports to the coordinator when the unit is ready, or when it's blocked.

## Phase 1: the review's findings

The review of master at ba70e73 found 161 things, 25 of them high. docs/review/ has a file per
lane, each finding with a Status line the lane keeps current.

| Lane | Findings | Owns |
|---|---|---|
| 1: sema | A1 to A4, docs/review/lane1-sema.md: 65, 14 high | sema.cpp, sema.hpp, types.cpp, types.hpp, const_value.cpp |
| 2: lowering and codegen | A5 to A7, lane2-lowering.md: 31, 10 high | lower.cpp, mir.cpp, mir.hpp, evaluate.cpp, codegen.cpp, c_abi.cpp, c_abi.hpp, link.cpp |
| 3: front end, driver, CLI, runtime | A8 to A10, lane3-front-runtime.md: 42, 1 high | lexer, parser, ast, literal, utf8, source, diagnostic_render, dump, driver, module.hpp, manifest, process, cli/, lsp/, tools/bindgen/, runtime/ |
| 4: std | A11 and A12, lane4-std.md: 23 | std/ |

- Lane 1 starts from branch m6-review, which already fixes A3-2 to A3-12, and A3-1 with Swift's
  rule is its first fix. The others start from master.
- A fix that needs a file another lane owns asks the coordinator, who has that lane make it or
  lets the change through when it's small.
- Tests go beside the tests of what they check (sema_tests.cpp, mir_tests.cpp, codegen_tests.cpp,
  runtime tests, tests/programs), not at the end of a file another lane also adds to.
- A lane lands its findings in batches of about ten, each batch through the landing steps below,
  so master moves forward all night and lanes rebase often.
- When phase 1's lanes are done, one review of what they changed runs (as in "Review"), and its
  findings are fixed the same way.
- A fifth lane, added on 2026-10-22, builds the std steps that need no compiler change and no
  other step while phase 1 runs: 9 (Math I), then 27 (algo), then 21 (crypto), each reviewed
  and landed as phase 2's steps are.

## Phase 2: M6's steps

One lane is the **compiler lane**: only it changes the compiler (sema, lowering, codegen, the
lexer and parser), one step at a time, so sema.cpp and lower.cpp never conflict. The other three
are **std lanes**, which write std, its tests, and its C libraries. The compiler lane starts once
lanes 1 and 2 have landed their findings; the std lanes start as soon as any lane is free.

The compiler lane builds each step's language part first, in this order, and a std lane takes the
step's library part once it lands:

1. Step 5: format specifiers in interpolation, checked at compile time, `Formattable`, `Format`,
   and parsing (the whole step).
2. Step 6: the built-in `Char`, `'a'` literals, and `s.Chars`.
3. Step 16: the lexer reading `2.Seconds`, and the timing built-ins taking a Duration.
4. Step 10: `ThreadLocal Global`.
5. Step 11: computed Const type arguments with `-`, `/`, and `%`.
6. Step 12: the `CData` constraint and `Ptr<T>` of a type parameter.
7. Step 13: Async Methods in Interfaces.
8. Step 15: `Embed`, `EmbedText`, and `EmbedFolder`.
9. Step 17: filling a T Field by Field without its New, and `OnLoaded()`.
10. Step 18: `Task<T> Throws E` as a type.
11. Step 7: checking literal regex patterns at compile time.
12. Step 20: reading `.proto` files at compile time.
13. Step 9's SIMD: codegen's optimize() turns on LLVM's SLP and loop vectorizers in release
    builds, so Vec4's Operators and Mat4's `*` become SIMD (the math lane found they don't yet).

A step is ready for a std lane when the steps it builds on have landed:

| Step | Needs |
|---|---|
| 9 Math I, 21 crypto, 27 algo (all but Delaunay and Voronoi) | nothing more |
| 23 uuid and db | 21 (Uuid.V4's SecureRandom), 16 (V7's clock), and the compiler lane's step 17 (Query<T> fills a Struct Field by Field) |
| 24 image I | 12 (an Image is over a Buffer), and 14 to load files |
| 27's Delaunay and Voronoi | 9 |
| 6 Text I | the compiler lane's step 6 |
| 7 Text II | 6, and the compiler lane's step 7 |
| 10 Math II, 11 Math III, 12 ffi | 9 and the compiler lane's 10, 11, 12 |
| 13 io streams | the compiler lane's step 13 |
| 14 files, 22 compress, 26 term | 13 (26 needs 6 too) |
| 15 io extras | 14 and the compiler lane's step 15 |
| 16 time | the compiler lane's step 16 |
| 17 os | 6 and the compiler lane's step 17 |
| 18 concurrent, log, events | 16 and the compiler lane's step 18 |
| 19 data I | 6, 13, and 17 |
| 8 Text III, 20 data II | 19 (20 needs the compiler lane's step 20) |
| 25 image II | 24 |
| 28 net I | 13, 14, 16, 18 |
| 29 net II | 21, 22, 28 |
| 30 net III, 31 net IV | 29 (31 only needs 28) |
| 32 loose ends | everything else |

The coordinator gives a free std lane the ready step that unblocks the most others, and keeps a
compiler step or two ahead of the std lanes that wait on one.

## How a lane works

1. **Set up.** The worktree is the lane's own. Build in a folder beside it, never in another
   lane's:

   ```bash
   export PATH="/c/Users/Hunter/AppData/Local/Programs/CLion/bin/mingw/bin:/c/Users/Hunter/AppData/Local/Programs/CLion/bin/ninja/win/x64:/c/Program Files/CMake/bin:/c/Users/Hunter/AppData/Local/Programs/CLion/bin/clang/win/x64/bin:/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/Llvm/x64/bin:$PATH"
   cmake -S . -B ../build-<lane> -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=c++ "-DFB_LLVM_ROOT=C:/Program Files/LLVM"
   cmake --build ../build-<lane>
   ```

2. **Do the unit.** For a finding: reproduce it with the reviewer's program, fix it, add a test
   that fails without the fix, and set its Status line. For a step: build what docs/milestones.md
   and docs/stdlib.md give it, with tests that check its done-when, and the spec and stdlib.md
   for what it adds.
3. **Check it.** `python tools/verify.py --build ../build-<lane> --base master` passes: build,
   ctest, release outputs, concurrency runs, clang-format, and clang-tidy. A program test's .out
   is the program's real output, read and checked by hand, never pasted blind; an async one runs
   20 times each way first.
4. **Commit and push.** As hlpdev <hunterpollock2006@gmail.com>, short lowercase messages, small
   commits, no Claude attribution, no "wip" left in a message. Push the branch. CI runs only on
   master and the coordinator's landing branch, m6-land, so verify.py is the lane's own check.
5. **Report** to the coordinator: the branch, the commits, verify.py's result, the decisions taken, the
   findings dropped and why, and the paragraph docs/history.md should get, in CLAUDE.md's old
   style.

Gotchas: bash heredocs mangle backslash escapes, so edit C++ through the Write and Edit tools or
a script the Write tool wrote; clang-tidy's trailing-comma check misfires on an empty `{}`
argument, so write `span_t()`; a Catch2 filter needs `\,` and `\[\]`; run a compiled test program
with the mingw bin on PATH.

## Review

Each M6 step, and phase 1's changes as a whole, get one review before landing. The coordinator
runs it as a workflow over the lane's branch: about three reviewers, each over a part of the
change, and one skeptic per reviewer that reproduces each finding. The lane fixes every
confirmed finding with a test, runs verify.py again, and pushes. A fix is checked by its tests
and verify.py, not by another review, unless it rewrites a large part of the compiler.

## Landing

The coordinator lands a branch once verify.py and CI are green on all three platforms:

1. Rebase it onto master, keeping its commits (no squash), and rerun CI if master moved.
2. Fast-forward master and push it.
3. Update CLAUDE.md's Current step, docs/milestones.md, docs/decisions.md, and docs/history.md,
   commit, and push.
4. Delete the branch, local and remote, and its worktree once the lane is done with it.

## Done

- Every finding in docs/review/ is fixed with a test, or dropped with why.
- M6's steps 5 to 32 are on master, each reviewed, with CI green on Windows, Linux, and macOS.
- Nothing a review found is in Deferred work.
- CLAUDE.md's Current step says M6 is done.
