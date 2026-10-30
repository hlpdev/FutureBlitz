# The review of M8 (step 9, the milestone review)

20 confirmed findings, each with its Status.

## journey-1 (high): In a release, fb-lsp treats the release's std as user code: a std file opened by go-to-definition shows false errors, a rename of a std declaration is allowed and edits the release's std, and Import completion has no std modules

lsp/src/server.cpp:783. The LSP finds std through the compiled-in build_config::std_directory, the CMake build machine's source folder, rather than toolchain().std, which is the release's std/ beside bin/. This happens in server.cpp:783 (import_names), server.cpp:557 (editable), workspace.cpp:41 (std_folder, used by unit_of, the roots, and line 831), and driver.cpp:3241 (check_files' std check). On a user's machine that folder doesn't exist, so: (a) Import completion lists no std module; (b) the release's std/core/print.fb, opened from go-to-definition on Print, is checked as a lone file and gets 2 errors that appear in Problems; (c) with that file open, renaming Print is allowed and edits the release's std/core/print.fb plus the user's files, but not std's other uses of Print. tests/lsp only runs from a build folder, where the compiled-in path is the real std, so none of this is tested.

Shown by: Release installed with `cmake --install <my build of bef5e523+working tree> --prefix ".../journey/Über Tools/fb rel"`, then the build and source folders renamed away so nothing can fall back to them. Package 'Mes Jeux é/game' (main.fb has Print(Greet(p.name)) and Import util.math). LSP driver scripts j5.py, j6.py, and j7.py (Python, JSON-RPC over stdio, PATH="") in .../scratchpad/review-m8/journey/. j7.py was run against both servers: build folder `b/lsp/fb-lsp.exe` and release `Über Tools/fb rel/bin/fb-lsp.exe`.

Expected: The release behaves as the build folder does: 'Import std.' completes std's modules; the release's std files have no diagnostics; renaming a std declaration is refused with 'only a name the workspace declares can be renamed'.

Actual: Build folder: std file diags [] and rename refused. Release: std file diags ['only the standard library calls the runtime; C code's convention is "C"' x2]; the rename in the open std file returns edits {std/core/print.fb: 1, game/main.fb: 2, game/player.fb: 1}; completion for 'Import std.' and 'Import std.coll' returns 0 items, and 'Import s' offers only game's and extras' modules.

Skeptic: I installed a release into review-m8/refute-journey/rel with cmake --install and drove both servers with my own JSON-RPC script, scripts/j1.py, with PATH emptied, on a package made by fb new. Release fb-lsp: go-to-definition on Print opens rel/std/core/print.fb. With that file open it gets 2 diagnostics ('only the standard library calls the runtime; C code's convention is "C"' twice). Renaming Print there returns edits to rel/std/core/print.fb (line 20) and main.fb. Build-folder fb-lsp: the same std file has no diagnostics, and the rename is refused with 'only a name the workspace declares can be renamed'. Std Import completion still worked on this machine, because the compiled-in source std exists here. The code shows it breaks once that folder is gone: lsp/src/server.cpp:557 and :783, workspace.cpp:41, semantic.cpp:201 and driver.cpp:3241 all use build_config::std_directory (the CMake source path), while the rest of the driver uses toolchain().std. That contradicts the release-install done-when.

Fix: Use toolchain().std (fb/toolchain.hpp) wherever the LSP and check_files() mean 'the standard library's folder', in server.cpp, workspace.cpp, semantic.cpp, and driver.cpp:3241, and add an LSP test that runs on a release layout (or a toolchain_t pointing elsewhere) and covers std Import completion, std diagnostics, and the refused rename.

Status: open

## cross-3 (medium): fb doc on a prebuilt package writes the .fbi's line numbers under the source file's name

compiler/src/doc.cpp:1783. item.line and an example's line come from location_of() in the file they were parsed from. For a prebuilt package that file is the whole .fbi, while `file` is package_path(module), the source path such as shapes.fb. The .fbi drops blank lines and bodies, and starts with header lines. So each item's line names a different line of shapes.fb than the one it starts on. The same library gets different JSON from source and prebuilt, for M9's reference pages. The driver test for the .fbi's docs (tests/compiler/driver_tests.cpp) checks only summaries, not file or line.

Shown by: Library C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/lib/geo (Class Circle with New, @Inline Grow with an example, Shrink, and Twice). `fb build` and `fb build --release`, then copy project.toml and build/windows-x64/{debug,release} into lib/geopre/lib/windows-x64/. In lib/, `fb doc geo -o -` and `fb doc geopre -o -`, compared on name, file, and line.

Expected: geopre's items have geo's lines (New 15, Grow 32 with its example at 27, Shrink 39, Twice 46), or else `file` names the .fbi.

Actual: geopre gives file "shapes.fb" with New 14, Grow 28 (example line 23), Shrink 34, and Twice 37. Those are the .fbi's own lines: shapes.fb:28 is `/// ```` and shapes.fb:34 is `///     If by < 0 ...`.

Skeptic: I rebuilt vx/lib/geo with `fb build` and `fb build --release` and copied build/windows-x64/{debug,release} into geopre/lib/windows-x64. `fb doc geo` gave New shapes.fb:15, Grow 32 (example 27), Shrink 39, Twice 46. `fb doc geopre` gave New shapes.fb:14, Grow 28 (example 23), Shrink 34, Twice 37. Those are geo.fbi's own lines: the .fbi has `// fbi` and `// module geo/shapes.fb 35` header lines, and no blank lines or bodies. In shapes.fb, line 28 is `/// Local c = Circle(1)` inside Grow's doc. tooling.md's format says `file` and `line` are 'where it starts, at its first attribute'. doc.cpp:1783-1785 takes the line from the parsed .fbi source but names package_path(module), the source path. So a prebuilt package's JSON points at the wrong lines of shapes.fb.

Fix: Record each declaration's source line in the interface file: the .fbi's `// module` header could give the original line count, or the declarations could keep their line origin. Then read it in doc.cpp for interface modules. Or write the .fbi's path and lines. Add a test that compares a library's JSON from source and from prebuilt.

Status: open

## cross-4 (medium): fb test on a prebuilt package fails with a raw lld undefined-symbol error

compiler/src/driver.cpp:2979. Under TEST (testing true), compile_package compiles a prebuilt package's own interface modules but doesn't link its .fblib. The test program's main references fb.<name>.(library).(init) and (release), which nothing defines. fb build on the same folder says clearly that it's prebuilt with no source, and fb check passes. A prebuilt package with a tests/ folder, which spec 09 allows ('tests that use it from outside, as another package does'), can't run them at all.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/lib/geopre2: geo's project.toml and lib/windows-x64/{debug,release}/geo.{fblib,fbi}, with tests/t.fb holding `@Test Function Grows()`, which makes a Circle and Expects it to grow. Run `fb test geopre2 --color never` (and `fb test geopre`, which has no tests/) from lib/.

Expected: tests/t.fb's test runs against the prebuilt binary, as an outside package's would. With no tests, fb test says 'Running 0 tests' or gives an error like fb build's 'geo is prebuilt ...; there's no source'.

Actual: `error: linking failed:` `lld: error: undefined symbol: fb.geo.(library).(init)` and `... (release)`, referenced by `...\geopre2\(library).fb:0`.

Skeptic: vx/lib/geopre2 holds geo's project.toml, the freshly built lib/windows-x64/{debug,release}/geo.{fblib,fbi}, and tests/t.fb with `@Test Function Grows()`. `fb test geopre2 --color never` printed 'error: linking failed:' and 'lld: error: undefined symbol: fb.geo.(library).(init)' and '(release)', referenced by geopre2\(library).fb:0 / program.o:(fb.main). `fb test geopre`, which has no tests/ folder but has a .fbi example, gives the same lld errors. On the same folder `fb build` says clearly 'geo is prebuilt, ...geo.fbi; there's no source to build it from', and `fb check` passes. A raw linker error, instead of a run or a clear message, is wrong behavior.

Fix: Under TEST, link a prebuilt own package's .fblib for the build's mode, as a dependency's is linked. Or stop early with fb build's message. Add a cli test for fb test on a prebuilt package.

Status: open

## cross-5 (medium): The miscased-keyword note and fix-it fire for a correctly lowercase variable on any syntax error on its line

compiler/src/parser.cpp:454. suggest_keyword_case() (uncommitted, this step) attaches a note and a fix-it to every parser error. It uses the nearest identifier before the error on its line that equals a keyword but for case. Keywords are case-sensitive, so `to`, `default`, `field`, `package`, `select`, `loop`, `new`, and so on are valid names. A stray ')' after such a declared variable gets a note saying the variable is a miscased keyword. The LSP offers a preferred quick fix that rewrites the variable into the keyword, which makes the code worse. The parser test checks only lines where the miscased word really is the mistake.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/kw/a.fb: `Import std.core`, `Local from = 1`, `Local to = 5`, `Local d = to - from)`, `Print(d)`. Run `fb check a.fb --color never`. tools/t5.py sends textDocument/codeAction for its diagnostic to fb-lsp.

Expected: `error: expected end of line, found ')'` with no note about keywords, and no quick fix that renames `to`. The note should fire only where the miscased word could be the keyword, e.g. at the line's start or where the parser wanted that keyword, and not for a declared name.

Actual: `= note: 'to' is a name here; keywords are written as they are, as 'To'`. The LSP returns a quickfix titled "Fix: expected end of line, found ')'" with isPreferred true, whose edit replaces `to` at 4:10-4:12 with `To`.

Skeptic: vx/kw/a.fb (`Local from = 1`, `Local to = 5`, `Local d = to - from)`): `fb check a.fb --color never` printed `a.fb:4:20: error: expected end of line, found ')'` with `= note: 'to' is a name here; keywords are written as they are, as 'To'`. The build folder's fb-lsp answered textDocument/codeAction with a quickfix titled "Fix: expected end of line, found ')'", isPreferred true, whose edit rewrites 3:10-3:12 (`to`) to `To`. spec 01 says the language is case-sensitive, so `to` is a valid name. The decisions.md row (M8 step 9) covers a keyword written in the wrong case, like `if` for `If`, not a correctly spelled variable. suggest_keyword_case() (parser.cpp, now committed in 5f908f5c) takes any identifier on the line before the error that equals a keyword except for case. The parser test checks only real miscasings, plus a miscased word on another line.

Fix: Suggest the keyword only when the identifier isn't a declared or used name in scope, e.g. when it's the first token of its statement, or when the token the parser expected is that keyword. Add parser tests with lowercase variables named like keywords on a line with an unrelated syntax error.

Status: open

## docs-1 (medium): Releases ship nlohmann/json inside fb-lsp without its MIT notice; tooling.md's notices list leaves it out too

cmake/install.cmake:145. In step 4, fb-lsp started compiling in nlohmann/json 3.12.0 (lsp/CMakeLists.txt). The release's notices weren't updated to match. cmake/install.cmake installs LLVM's, toml++'s, MinGW's, GCC's, glibc's, and libxml2's notices plus std's bundled libraries', but not nlohmann/json's LICENSE.MIT. tooling.md's notices bullet and tests/install/install_test.cmake (`set(notices llvm tomlplusplus)`) leave it out as well. Step 2's review fixed the same gap for toml++ (m8-step2.md, toml++'s LICENSE installed), so this is a gap between steps 2 and 4.

Shown by: export PATH=CLion mingw+ninja+cmake; built bef5e523 plus the working tree in scratch (FB_BUILD_TESTS=OFF), then `cmake --install <scratch>/build --prefix <scratch>/rel`. `ls rel/notices` lists 30 files (argon2 ... llvm, tomlplusplus, winpthreads, zstd) and none is nlohmann's. `grep -c nlohmann rel/bin/fb-lsp.exe` gives 37966. `grep -rli nlohmann rel/notices` finds nothing.

Expected: notices/ has nlohmann/json's MIT license, for example notices/nlohmann_json.txt from the dependency's LICENSE.MIT. tooling.md lists it with toml++ as a notice for fb's executables, and the install test checks for it.

Actual: The release ships fb-lsp.exe with nlohmann/json compiled in but has no notice for it. The docs and the install test don't mention it either.

Skeptic: lsp/CMakeLists.txt FetchContents nlohmann_json v3.12.0 and links it into fb_lsp_server, and `grep -c -a nlohmann cmake-build-debug/lsp/fb-lsp.exe` gives 5142. cmake/install.cmake:138-155 installs the bundled libraries' notices from third_party/notices (argon2 ... zstd, none of them nlohmann), cmake/notices/{llvm,mingw-w64,winpthreads,gcc,libxml2 | gcc,glibc,linux-headers}, and toml++'s LICENSE, but nothing for nlohmann_json, although cmake-build-debug/_deps/nlohmann_json-src/LICENSE.MIT exists. tests/install/install_test.cmake:31 is `set(notices llvm tomlplusplus)`, and tooling.md:122-126 lists only LLVM's and toml++'s as being in fb's executables. Step 2's review fixed this same gap for toml++, so this one opened between steps 2 and 4.

Fix: In cmake/install.cmake, after the toml++ lines, add `FetchContent_GetProperties(nlohmann_json)` and `install(FILES "${nlohmann_json_SOURCE_DIR}/LICENSE.MIT" DESTINATION notices RENAME nlohmann_json.txt)`. Add nlohmann_json to install_test.cmake's notices list, and to tooling.md's notices bullet as being in fb-lsp.

Status: open

## docs-2 (medium): The LSP's quick fix for a miscased keyword is titled with the misleading parentheses error

lsp/src/features.cpp:924. Every code action is titled "Fix: " + diagnostic.message. The new miscased-keyword fix-it (the uncommitted suggest_keyword_case in parser.cpp) attaches to whichever syntax error comes first on the line. For `if x == 1 Then` that error is "calls need parentheses, like if(...)". So VS Code's lightbulb offers "Fix: calls need parentheses, like if(...)", which reads as adding parentheses, but applying it replaces `if` with `If`. The note that explains the change is only in the diagnostic's text. decisions.md's new row says the error "notes the keyword and has a fix-it that spells it, which the LSP offers". What the LSP offers is labelled as a different fix.

Shown by: scratch/w/case/a.fb: `Import std.core`, `Local x = 1`, `if x == 1 Then`, `    Print("one")`, `End If`. Ran scratch/scripts/lsp.py (a JSON-RPC client) against rel/bin/fb-lsp.exe with PATH empty: didOpen, then textDocument/codeAction on line 3, columns 0 to 2. Result: `[{"title": "Fix: calls need parentheses, like if(...)", "edit": {... "newText": "If" ...}, "isPreferred": true}]`. `fb check a.fb` prints the same error, with `= note: 'if' is a name here; keywords are written as they are, as 'If'`.

Expected: The action says what it does, like "Write 'if' as 'If'". Ideally the miscased keyword's error leads too, rather than "calls need parentheses, like if(...)".

Actual: The title is "Fix: calls need parentheses, like if(...)", and the edit changes the keyword's case.

Skeptic: With a.fb (`if x == 1 Then` on line 3), `fb check` prints `error: calls need parentheses, like if(...)` with the note `'if' is a name here; keywords are written as they are, as 'If'`. I ran a JSON-RPC client (scratchpad/review-m8/docsv/lsp.py) against cmake-build-debug/lsp/fb-lsp.exe and sent a codeAction on 2:0-2:2. It returned `"title": "Fix: calls need parentheses, like if(...)"` with `"newText": "If"` and isPreferred true. features.cpp:924 always builds the title as "Fix: " + diagnostic.message, and suggest_keyword_case (parser.cpp:454) puts its fix on whichever syntax error comes first on the line. So the lightbulb's label describes a fix it doesn't make.

Fix: Give fix_t an optional title, which the parser sets to something like "Write 'if' as 'If'" and the constraint fix-its set to their own wording. features.cpp uses it before falling back to "Fix: " + message. Add an editing_tests case for the code action's title on `if x == 1 Then`.

Status: open

## journey-2 (medium): The new miscased-keyword fix-it fires on a correctly spelled variable named like a keyword, as a preferred quick fix that breaks the code

compiler/src/parser.cpp:454. suggest_keyword_case (the uncommitted step 9 change) walks back over every identifier on the error's line, up to the error, and suggests the first one whose lowercase matches a keyword. Lowercase names like final, to, in, is, field, default, new, lock, loop, and select are ordinary identifiers, because keywords are case-sensitive. So any syntax error on a line that uses such a variable gets a wrong note and a fix-it that turns the variable into the keyword. fb-lsp sends it as a quickfix with isPreferred true, titled only 'Fix: <the error>', so Auto Fix applies it. The parser test only covers a miscased word on another line and no word at all.

Shown by: kw/k2.fb: `Import std.core` / `Local final = 10` / `Print(final +)` / `Local to = [1, 2]` / `Print(to[0] *)`. Ran `fb check k2.fb`, and j3.py sent textDocument/codeAction for the first diagnostic.

Expected: Only the missing operand is reported (expected an expression, found ')'), with no keyword note or fix-it, since `final` and `to` are declared, correctly spelled variables.

Actual: `k2.fb:3:14: error: expected an expression, found ')'` gets `= note: 'final' is a name here; keywords are written as they are, as 'Final'`, and the same happens for `to` -> 'To'. The code action is {"title": "Fix: expected an expression, found ')'", "isPreferred": true, "edit": replace `final` with `Final`}.

Skeptic: kw/k2.fb is `Local final = 10` / `Print(final +)` / `Local to = [1, 2]` / `Print(to[0] *)`. Both the build fb and the release fb print `k2.fb:3:14: error: expected an expression, found ')'` with `= note: 'final' is a name here; keywords are written as they are, as 'Final'`, and the same for 'to' -> 'To'. parser.cpp:454 suggest_keyword_case walks back over every IDENTIFIER on the line and takes the first one miscased_keyword() matches, with no check of position or declaration, then adds with_fix. features.cpp:924-930 sends every fix as {title: "Fix: " + message, isPreferred: true}. So a correctly spelled, declared variable gets a preferred quick fix that breaks the code.

Fix: Suggest only where the keyword could stand: the identifier is the line's first token, or the token the error is at or right before it, or it's followed by what the keyword's syntax needs. Don't suggest a name the line declares or that was declared earlier (a Local, a parameter). Give the code action a title that says what it does, like "Write 'If'". Add parser tests for `Local final = 10` / `Print(final +)`.

Status: open

## cross-1 (low): LSP rename and references skip the code in doc-comment examples, so a rename breaks fb test

lsp/src/server.cpp:595. Step 4's rename and references work from sema's uses, and step 8's examples are only compiled under purpose TEST (driver.cpp:2938). check_files() never loads the `<name>.doctests` modules, so a symbol's uses inside `///` examples are not found. A rename changes the declaration and the tests/ folder's uses but leaves `/// Local x = Grow(1, 2)` as it was. After that, `fb check` passes and `fb test` stops on the example's errors. `[Grow]` links in docs are not renamed either.

Shown by: Package C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/pkg. shapes.fb has `Public Function Grow(n: Int, by: Int): Int`, whose `# Examples` fence calls `Grow(1, 2)` and `Grow(1, -1)`. tests/grow.fb calls Grow too. The LSP client tools/t1.py (in that scratch folder) sends textDocument/rename at `Grow(n` with newName Enlarge to fb-lsp (freshly linked at scratch fbbuild/lsp/fb-lsp.exe). The same edits are applied in pkg_renamed, then `fb check` and `fb test --color never` run there. textDocument/references, tools/t6.py on lib/app2 (ShrinkIt), shows the same.

Expected: The rename edits the example lines in the doc comment too, or at least references lists them. fb test still passes after the rename.

Actual: The WorkspaceEdit covers only shapes.fb:14 and tests/grow.fb:5-6. references returns the same 3 places. After applying it, fb check prints 'Finished check'. fb test prints `shapes.fb:9:15: error: 'Grow' isn't declared` and `shapes.fb:11:15: error: 'Grow' isn't declared` and runs no test.

Skeptic: I used the build folder's fb-lsp.exe, rebuilt at 20:57 from the current sources, on a copy of pkg at scratchpad/review-m8/vx/pkg. textDocument/rename at `Grow(n` returned edits only for shapes.fb 13:16-20 and tests/grow.fb 4:11 and 5:14. references returned the same 3 places, so the example's `/// Local x = Grow(1, 2)` (shapes.fb:9) and line 11 were left out. With those edits applied (vx/pkg_renamed), `fb check` printed 'Finished check in 0.76s'. `fb test` printed `shapes.fb:9:15: error: 'Grow' isn't declared` and the same at 11:15, then 'couldn't build pkg ... tests because of 2 errors'. So a rename that the LSP offers as complete breaks the package's tests. Rated low because the errors point straight at the doc comment's lines.

Fix: When a check covers files that have examples, include each example module (add_examples with purpose CHECK) in the references and rename checks. Then map their spans back through the line origin to the doc comment's text, so rename edits `/// ...` lines. Optionally also rename `[Name]` links that resolve to the symbol.

Status: open

## cross-6 (low): fb fmt strips trailing spaces inside doc comments, which changes doc tests' results and fb doc's Markdown; format_source's meaning check doesn't see it

compiler/src/format.cpp:423. place_comment() trims spaces and tabs at a comment's end, including `///` lines. Inside an example's fence those lines are code: a line of a triple-quoted String keeps its trailing spaces, so fb fmt changes the example's value and its test's result. In the Markdown, two trailing spaces are a hard line break, which fb doc's markdown loses. same_meaning() compares tokens and dump_ast but not the docs, so the guarantee in tooling.md ('the code means what it did ... leaves a file it would change otherwise as it is') isn't kept for doc tests.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/fmt2/pad.fb: `Public Function Pad(s: String): Int => s.Length`, whose example is `# Import std.text`, `Local s = """`, `    ab  ` (two trailing spaces), `    """`, `Expect(Pad(s) == 4)`. Run `fb test pad.fb`, then `fb fmt pad.fb`, then `fb test pad.fb` again.

Expected: fb fmt leaves the text inside an example's fence alone (or refuses, as it does for code whose meaning changes), and the test still passes.

Actual: Before: `test pad.Pad (example) ... ok`. fb fmt prints `Formatted pad.fb` and the line becomes `///     ab`. After: `expect failed: Pad(s) == 4 at pad.fb:13:5`, `left: 2`, `right: 4`.

Skeptic: I restored the two trailing spaces in vx/fmt2/pad.fb's example line (`///     ab  `). `fb test pad.fb` printed 'test pad.Pad (example) ... ok'. `fb fmt pad.fb` printed 'Formatted pad.fb', and diff shows line 11 became `///     ab` with no trailing spaces. `fb test pad.fb` then printed 'expect failed: Pad(s) == 4 at pad.fb:13:5', left: 2, right: 4. A doc line `/// First line  ` (a Markdown hard break) also lost its trailing spaces. So fb fmt changed what a doc test computes, and its meaning check didn't refuse. Rated low because it needs trailing whitespace inside an example's multi-line String, which is rare.

Fix: Don't trim the trailing whitespace of `///` and `//!` lines, or at least of lines inside a doc comment's code fence. Make same_meaning() compare each doc comment's text, ast_t::doc, so a change to it is refused like a change to the code.

Status: open

## cross-8 (low): Go to definition gives nothing for a prebuilt package's declarations and its Import, though its .fbi is on disk

lsp/src/features.cpp:255. location_of() needs compilation.disk_path(file). An interface file's modules have no disk path noted, so definition returns null for every prebuilt declaration and for `Import geo.shapes`. Hover shows the .fbi's docs, which step 8 added for the LSP's sake, and std's definitions work.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8/cross/lib/app (depends on ../geopre, prebuilt). tools/t3.py and tools/t9.py send hover and definition on `Circle(1)`, `Grow`, `Shrink`, `Radius`, `geo.shapes`, and `Print(`.

Expected: definition opens lib/geopre/lib/windows-x64/debug/geo.fbi at the declaration, as hover already shows its doc.

Actual: Hover returns the signature and docs. definition returns null for Circle, Grow, Shrink, Radius, and the geo.shapes Import, while Print's goes to std/core/print.fb:21.

Skeptic: The build folder's fb-lsp on vx/lib/app, which depends on ../geopre (prebuilt, rebuilt). textDocument/definition on `Print(` went to std/core/print.fb line 20. On `Circle(1)` and on `geo.shapes` it returned null, though geopre/lib/windows-x64/debug/geo.fbi is on disk. features.cpp location_of() returns nullopt when compilation.disk_path(span.file) is empty. note_disk_path is called only at driver.cpp:967, 2055, and 2721, never for interface modules. M8's done-when lists 'goes to definitions', and tooling.md documents no prebuilt exception, while step 8 keeps .fbi docs so hover works for prebuilt packages. Rated low: hover still works.

Fix: note_disk_path() the interface file for each module read from it, so definition and references can open the .fbi. Or say in tooling.md that a prebuilt package has hover only.

Status: open

## docs-3 (low): The TextMate grammar and language configuration don't know step 8's `//!` module doc comments

editors/vscode/syntaxes/futureblitz.tmLanguage.json:137. Step 8 added `//!` module docs to spec 01, whose text is Markdown like `///`'s. The grammar from step 5 only matches `///(?!/)` as comment.line.documentation, with link, code, and heading scopes. A `//!` line is scoped as a plain comment.line.double-slash, so its [links], `code`, and # headings aren't highlighted. The language configuration's onEnterRules carry `///` on to the next line but not `//!`. M9's Shiki uses the same grammar (decision 4), so std's module docs, every std file's first comment since step 8, would show as plain comments on the site too.

Shown by: In the scratch copy of the repo: editors/vscode, `npm ci && npm run compile`, then `node scripts/tokens.mjs w/bang.fb` on a file with `//! Shapes: see [Shape] and `Area`.`, `//! # Examples`, and the same two lines as `///` before an Interface.

Expected: `//!` lines are scoped like `///` lines (comment.line.documentation, markup.underline.link for [Shape], markup.inline.raw for `Area`, markup.heading for # Examples), and Enter on a `//!` line continues it.

Actual: `"//" comment.line.double-slash ... "! Shapes: see [Shape] and `Area`." comment.line.double-slash`, and `"! # Examples" comment.line.double-slash`. The `///` lines get documentation, link, inline.raw, and heading scopes.

Skeptic: Read the files. editors/vscode/syntaxes/futureblitz.tmLanguage.json:137 starts the documentation scope only at `///(?!/)`, and line 188's heading match is `(?<=///)`. A `//!` line falls to the plain `//` rule at line 152. language-configuration.json's onEnterRules have only `^\s*///.*$`. Spec 01:78-80 makes `//!` lines module doc comments whose text is Markdown like `///`'s, and since step 8 every std file opens with `//!` lines (std/core/assert.fb, error.fb). So module docs get none of the Markdown scopes that `///` gets. I didn't run tokens.mjs, but the patterns plainly leave `//!` out.

Fix: Make the doc-comment pattern `//[/!](?!/)` (or add a twin rule for `//!`) with the same Markdown inner patterns, and add an onEnterRule for `^\s*//!.*$` that appends `//! `. Add `//!` cases to test/grammar/comments.fb and language.test.ts, and say so in tooling.md's grammar bullet.

Status: open

## docs-5 (low): tooling.md says to dispatch the release workflow by id because master lacks release.yml, but master has it now

docs/tooling.md:74. tooling.md's Releases section says to dispatch the workflow "by the workflow's id, `gh workflow run 401031694 --ref <branch>`, while master hasn't the file, as GitHub finds one by its name only there". release.yml is on master now (bef5e523, and origin/master), so the condition is no longer true, and `gh workflow run release.yml --ref <branch>` works.

Shown by: `git ls-tree origin/master .github/workflows/` lists release.yml. `gh workflow list` shows `Release active 401031694`. `gh api repos/hlpdev/FutureBlitz/actions/workflows/401031694 --jq .path` gives `.github/workflows/release.yml`.

Expected: tooling.md says to dispatch it as `gh workflow run release.yml --ref <branch>` (or by id), without the stale condition.

Actual: "while master hasn't the file" is still there.

Skeptic: docs/tooling.md:73-75 still says to dispatch by id `gh workflow run 401031694 --ref <branch>`, "while master hasn't the file, as GitHub finds one by its name only there". `git ls-tree origin/master .github/workflows/` lists release.yml (blob e3c97204), the same as HEAD, so the condition no longer holds.

Fix: Rewrite the sentence as "or a dispatch on a branch to check it (`gh workflow run release.yml --ref <branch>`)".

Status: open

## docs-6 (low): README's install commands 404: no GitHub Release exists yet, and M8's done-when needs one

README.md:13. README.md (new in M8) tells users to run `curl -fsSL https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh | sh`, or the install.ps1 equivalent. The repository is public but has no release, draft or published, so both URLs 404 and the scripts' own `releases/latest` lookup fails. M8's done-when, "fb installs from a release", has been met only in release-workflow dispatches, never from a release on GitHub Releases. Publishing a draft is the user's job (decisions.md), so this needs either the user's tag and publish, or a README line saying releases are coming.

Shown by: `gh release list` prints nothing. `curl -sI https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh` gives `HTTP/1.1 404 Not Found`. `gh repo view hlpdev/FutureBlitz --json visibility` gives PUBLIC.

Expected: Before M8 closes, a v0.1.0 tag makes the draft release, which the user publishes, so the README's commands work. Otherwise the README says there's no release yet.

Actual: Both install commands in the README fail with 404.

Skeptic: README.md:13 and :19 give `curl -fsSL https://github.com/hlpdev/FutureBlitz/releases/latest/download/install.sh | sh` and the irm install.ps1 equivalent. `gh release list` prints nothing and `git tag` lists no tags. `curl -sI .../releases/latest/download/install.sh` gives `HTTP/1.1 404 Not Found`. So the README's documented install path fails today. Tagging and publishing is the user's job, but until then the README's instructions are wrong for any reader.

Fix: Leave it for the user's call at step 9's close: push v0.1.0, which matches CMakeLists.txt's version, and publish the draft. Until then, add a line to the README's Installing section that the first release is coming.

Status: open

## docs-8 (low): architecture.md still says `new` and `test` come later, and doesn't show M8's editors/ or lsp library

docs/architecture.md:91. M8 edited architecture.md's Formatter row but left the rest stale. The Tooling table's cli row still says "`build`, `run`, `check`, and `--show` ... (M1); `new`, `test`, package management later", although `fb new`, `fb test`, `fb fmt`, and `fb doc` all exist now and only package management comes later (M10). The repo layout doesn't show editors/ (the VS Code extension), and the CMake targets table still says `lsp` is one executable. lsp/ is now the fb_lsp_server library plus the fb-lsp executable.

Shown by: `fb help` from the release lists new, check, build, run, test, bindgen, fmt, and doc. `ls` at the repo root shows editors/. `sed -n 15,50p docs/architecture.md` and `sed -n 85,100p docs/architecture.md` show the stale rows.

Expected: The cli row lists new, test, fmt, and doc as M8's, with package management in M10. The layout has editors/vscode/. The lsp row names fb_lsp_server and fb-lsp.

Actual: "`new`, `test`, package management later". No editors/ in the layout, and `lsp | executable | compiler`.

Skeptic: docs/architecture.md:91 still reads "`new`, `test`, package management later", but `fb help` from cmake-build-debug lists new, test, fmt, and doc. Line 29's layout has no editors/, though editors/vscode exists. Line 49 is `| \`lsp\` | executable | \`compiler\` |`, while lsp/CMakeLists.txt makes the fb_lsp_server library (linking fb_compiler and nlohmann_json) plus the fb-lsp executable. M8 changed only one line of architecture.md (`git diff 06db1443 bef5e523 --stat`: 1 insertion, 1 deletion).

Fix: Update the three spots: the cli row ("... (M1); `new`, `test`, `fmt`, and `doc` (M8); package management in M10"), a layout line `editors/ vscode/ the VS Code extension (M8)`, and the CMake targets row for lsp (`fb_lsp_server` static lib, `fb-lsp` executable).

Status: open

## docs-9 (low): A stray untracked file `a` in the repo root holds a scratch path

a:. The working tree under review has an untracked file `a` at the repository root. Its only line is a scratchpad path (`.../scratchpad/rv-embed/paths-v/p3b/data/a.txt`), output left by an earlier session's check. A `git add -A` at step 9's commit would commit it, along with the user's temp path.

Shown by: `git -C C:/Users/Hunter/Projects/FutureBlitz status --short` shows `?? a`. `cat a` gives one line, `C:/Users/Hunter/AppData/Local/Temp/claude/.../scratchpad/rv-embed/paths-v/p3b/data/a.txt`.

Expected: No stray files in the tree when step 9 lands.

Actual: `a` is there, untracked.

Skeptic: `git status --short` shows `?? a` in the repo root, and `cat a` is one line, a scratchpad path (.../scratchpad/rv-embed/paths-v/p3b/data/a.txt). The step 9 commit 5f908f5c landed without it, so it hasn't been committed, but the stray file, with the user's temp path, is still in the tree. I left it untouched, since the tree is read-only for me.

Fix: Delete it before committing step 9, and stage the fix-it's files by name.

Status: open

## journey-5 (low): Import completion offers the package's tests/ modules in its own files, which then can't import them

lsp/src/server.cpp:795. import_names adds unit_modules(unit, ...) for the whole unit, and a package's unit includes its tests/ folder. Completing an Import in main.fb therefore offers game.tests, game.tests.messy, and game.tests.outside. Spec 09 and the compiler say a package's own files don't import its tests/ folder.

Shown by: Mes Jeux é/game with tests/outside.fb and tests/messy.fb. j5.py completed `Import game.` in main.fb, and `fb check` was run with `Import game2.tests.outside` in main.fb (game2 is a copy).

Expected: In a package's own file, completion doesn't offer <name>.tests.* modules.

Actual: Completion returns ['game.main', 'game.player', 'game.tests', 'game.tests.messy', 'game.tests.outside']. Accepting one gives "error: tests/outside.fb is in the package's tests/ folder, whose tests use the package from outside, so the package's own files don't import them".

Skeptic: I made a library package util with math.fb and tests/outside.fb, and requested completion at `Import util.` in math.fb through the release fb-lsp (scripts/j5.py). It returned ['util.math', 'util.tests', 'util.tests.outside']. In a copy, util2, with `Import util2.tests.outside` in math.fb, `fb check` says "tests/outside.fb is in the package's tests/ folder, whose tests use the package from outside, so the package's own files don't import them". So completion offers Imports that spec 09 forbids there.

Fix: Leave the tests/ folder's modules out of import_names() for a file outside tests/ (and out of the folder names that unit_modules adds).

Status: open

## journey-6 (low): install.ps1 run with `irm | iex` overwrites and then deletes the session's own $Version, $Archive, and $NoModifyPath

tools/install/install.ps1:146. With iex, the script's top-level param() block assigns $Version, $Archive, and $NoModifyPath in the caller's scope, and the closing finally removes them with Remove-Variable. A session that had its own variables of those names loses them. tooling.md says `irm | iex` 'leaves the session's preferences, TLS, and variables as they were'.

Shown by: ps/stub.ps1: install.ps1 with Install-FutureBlitz's body replaced by Write-Host, so nothing is downloaded or installed. Ran `pwsh -NoProfile -Command '$Version = "mine"; $Archive = "keep"; Invoke-Expression (Get-Content -Raw ./stub.ps1); Write-Host "after: Version=[$Version] Archive=[$Archive]"'`.

Expected: after: Version=[mine] Archive=[keep]

Actual: after: Version=[] Archive=[] (and the install saw version=[] rather than the session's value)

Skeptic: I made ps/stub.ps1: install.ps1 with Install-FutureBlitz's body replaced by a Write-Host, so nothing is downloaded or installed. Then I ran `pwsh -NoProfile -NonInteractive -Command '$Version = "mine"; $Archive = "keep"; Invoke-Expression (Get-Content -Raw ./stub.ps1); Write-Host "after: ..."'`. It printed 'install saw version=[] archive=[]' and 'after: Version=[] Archive=[]'. The script-level param() (lines 10-14) overwrites the session's variables, and line 146's Remove-Variable deletes them. That contradicts tooling.md:96-97: 'irm | iex leaves the session's ... variables as they were'.

Fix: Drop the script-level param() for the iex path: read $env:FUTUREBLITZ_VERSION inside the function, keep param() only when $PSCommandPath is set, or wrap the whole script in `& { param(...) ... }` so its variables are a child scope's. Then don't Remove-Variable names the session may own.

Status: open

## journey-7 (low): A failing doc test's stack trace names a generated function, util.doctests.3.DocTest__Example, not the test

compiler/src/doc.cpp:593. The trace for a panic inside an example names the generated test function, with the doctests package's module index in it. tooling.md and spec 09 call the test 'util.math.Half (example)', and @Test frames print their FB names, like game.player.PanicsInside. The line it gives (math.fb:25, the comment's line) is right.

Shown by: Mes Jeux é/util/math.fb, Half's example `/// Local h = Half(3)`, where Half panics on odd numbers. Ran `fb test` in util.

Expected: A frame named after the example, like `at util.math.Half (example) (math.fb:25)`, or the module's own name.

Actual: `panic: index 4 is out of range for 1 element at math.fb:29:30` / `    at util.math.Half (math.fb:29)` / `    at util.doctests.3.DocTest__Example (math.fb:25)`

Skeptic: In util/math.fb, Half's example `Local h = Half(3)` panics. `fb test` (release) printed `panic: index 4 is out of range for 1 element at math.fb:24:16` / `    at util.math.Half (math.fb:24)` / `    at util.doctests.2.DocTest__Example (math.fb:19)`, while the test itself is listed as 'util.math.Half (example)'. Spec 09's Stack traces section names frames by the user's functions and modules, and leaves out code the compiler makes itself. The frame here is a compiler-generated name (doc.cpp:593, DocTest__Example) inside a module named by an index, '2', which isn't an identifier. The line number is right, so this is cosmetic: low.

Fix: Give the example's generated Function a trace name of the test's name (the call-site table's FB name), or name its module after the declaration rather than an index.

Status: open

## journey-8 (low): fb doc's warnings ignore FB_COLUMNS, unlike every other command's diagnostics

cli/src/commands.cpp:1135. doc_command renders its link and doc-comment warnings with { .color = console.err().color }, which drops console.diagnostics()'s columns. Its columns therefore stay in code points under FB_COLUMNS=utf-16, which tooling.md says makes fb's diagnostics count UTF-16 units.

Shown by: docw/a.fb: `/// A 😀 thing, see [Missing].` on a Public Function, in a library package. Ran `FB_COLUMNS=utf-16 fb doc -o -`.

Expected: a.fb:3:21: warning: 'Missing' names nothing that code here sees (UTF-16: the emoji is 2 units)

Actual: a.fb:3:20: warning: ..., the same as without FB_COLUMNS

Skeptic: math.fb line 29 is `/// A 😀 thing, see [Missing].`. Both `fb doc -o -` and `FB_COLUMNS=utf-16 fb doc -o -` print `math.fb:29:20: warning: 'Missing' names nothing that code here sees`; in UTF-16 the column would be 21. For comparison, cols/c.fb `Print("😀", 1 +)` under fb check gives 2:15 normally and 2:16 with FB_COLUMNS=utf-16. commands.cpp:1135 renders doc warnings with { .color = console.err().color }, while every other command passes console.diagnostics() (lines 356, 456, 587, 671, 684).

Fix: Pass console.diagnostics() to render_pretty there, as the other commands do.

Status: open

## journey-9 (low): fb new with a keyword folder name says it isn't an identifier

cli/src/new.cpp:77. package_name_error's is_identifier rejects keywords, but the message says the name isn't an identifier at all. That's misleading for names that are identifier-shaped, like If, Self, or End.

Shown by: In new/: `fb new If`, `fb new Self`, `fb new End`.

Expected: Something like: error: If is a keyword, which a package's name can't be; it's the folder's name

Actual: error: a package's name is an identifier, like game or my_physics, not "If"; it's the folder's name

Skeptic: `fb new If` and `fb new Self` (release, PATH emptied) print `error: a package's name is an identifier, like game or my_physics, not "If"; it's the folder's name` (and the same for Self). Both names are identifier-shaped; they're refused because they're keywords, and the message doesn't say so. It's misleading but minor.

Fix: In package_name_error, check keyword_kind(text) first and say it's a keyword.

Status: open
