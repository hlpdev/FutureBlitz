# Lane 3: front end, driver, CLI, LSP, bindgen, and runtime

The whole-codebase review's findings for A8, A9, A10, from a review of master
at ba70e73 (2026-10-22). Only A3's were checked by a skeptic, which confirmed all 12;
reproduce every other one before fixing it. Files this lane owns: compiler/src/lexer.cpp, parser.cpp, ast.cpp, include/fb/ast.hpp, literal.cpp, utf8.cpp, source.cpp, diagnostic_render.cpp, dump.cpp, driver.cpp, include/fb/driver.hpp, include/fb/module.hpp, manifest.cpp, process.cpp, cli/, lsp/, tools/bindgen/, runtime/.

42 findings: 1 high, 16 medium, 25 low. Fix the highs, then the mediums, then the lows.

Each finding's Status line is one of: open; fixed in <commit> (with its test);
dropped, with why (it doesn't reproduce, or the spec says otherwise).

## A10-1 (high): Freeing the last Weak to an object waiting in let_go frees its memory, then let_go uses it (heap corruption)

Status: fixed in 31d90d2 (an object waiting its turn keeps its memory when its last weak goes), with tests/programs/weak_waiting.fb

Where: runtime/src/object.cpp:174

While another object's fields are being released, let_go() puts an object whose last owner let go in t_letting_go.waiting with refcount 0 and nothing holding its memory. If the last Weak to that object goes before its turn, fb_object_release_weak() sees weak_count == 0 && refcount == 0 and calls std::free on it. Two ways this happens: a Weak field that comes after the strong field in the same object, or a Destroy() of an object queued earlier. let_go() then calls end(next, false) on the freed block: it reads type->destroy, runs Destroy, releases fields, and frees the block a second time. finish() protects only the object being finished (++weak_count), not the ones queued. Shared objects are safe because they hold a Weak to themselves. Any non-Shared Class with a strong and a Weak reference to the same object, in that field order, corrupts the heap when it goes. That is a common parent/child or cache pattern.

Reproduced by the reviewer:

```
exp-all/A10r/../A10/weak_wait2.fb: Class B (Field name: String, Destroy prints); Class A with `Field strong: B` then `Field weak: Weak<B>`, New sets both to the same B; `For i = 1 To 3 / Local a: A? = A(B("b{i}")) / a = Null / End For`. `fb run` gives: error: weak_wait2.exe exited with code 0xC0000374 (STATUS_HEAP_CORRUPTION). The release build does the same and prints nothing. With the two fields swapped (Weak first, weak_first.fb), it runs correctly. A Destroy variant also crashes: exp-all/A10r/weak_global.fb, where A holds d: D then b: B, a Global `watch: Weak<B>` points at b, and D's Destroy sets watch = Null. It exits with 0xC0000374 too.
```

Suggested fix: Keep a queued object's memory while it waits: in let_go(), when state.releasing and the object isn't Shared, do ++object->weak_count before pushing it. In end()/finish(), drop that extra count the way finish() already drops its own, so whichever of finish() or the last fb_object_release_weak comes later frees it. Add a program test with a strong field followed by a Weak to the same object, and one where an earlier Destroy clears the last Weak.

Skeptic: confirmed=True, high. Reproduced. exp-all/skA10/w1.fb: Class B (Field name, Destroy prints), Class A with `Field strong: B` then `Field weak: Weak<B>`, one `Local a: A? = A(B("one")); a = Null`. Debug run: "terminate called after throwing an instance of 'std::logic_error' ... basic_string: construction from null", exit 0xC0000409. `fb run --release`: exit 0xC0000374 (heap corruption), nothing printed. weak_first.fb (Weak field first) prints all three Destroys and "end". weak_global.fb (an earlier Destroy clears the last Weak) also exits 0xC0000374. The code matches the claim. In let_go(), while state.releasing is set, the object goes into t_letting_go.waiting with refcount 0 and no extra weak_count. fb_object_release_weak (object.cpp:629-631) then frees it when weak_count == 0 && refcount == 0, and end(next, false) runs on the freed block. Only finish() bumps weak_count, and only for the object it is finishing.

## A8-1 (medium): Type expressions don't count toward the nesting limit, so deep types crash the compiler with a stack overflow

Status: fixed in aba60f7 (types, defer, and #if conditions count toward the nesting limit), with parser tests

Where: compiler/src/parser.cpp:3388

parse_type() and the functions it recurses through (parse_tuple_type, parse_named_type/parse_type_arguments, parse_function_type, Weak<...>) never touch m_nesting. Types of any depth are accepted without the 'nested more than 1000 deep' error, and the parser and later passes then recurse until the stack runs out. Even the iterative suffix loop (T[][]..., T??...) builds a nested type that sema recurses through. The limit exists to make this case an error and not a crash (stack.cmake, parser comment, docs/spec/01-lexical.md).

Reproduced by the reviewer:

```
Generated files `Import std.core` + `Local x: ` + '('*n + 'Int' + ')'*n + ` = 1`, then ran `fb check` on each. n=20000 gives no error at all (exit 0). n of about 43800 makes fb.exe die with 0xC00000FD (STATUS_STACK_OVERFLOW) and print nothing. Approximate crash depths for other forms: `Int` + '[]'*48500, `Int` + '?'*48500, 'Stack<'*27400, 'Func('*42300. A driver that only lexes and parses (linked against libfb_compiler.a) also overflows at 50000 nested parens, generics, or Funcs.
```

Suggested fix: Count type nesting like expression nesting: check and increment m_nesting in parse_type, and in its suffix loop for each ?, [] and [N] level. Report report_too_deep() past max_nesting. Add parser tests next to the existing nesting test for (((Int))), Stack<Stack<...>>, Func(Func(...)) and Int[][]...

## A8-2 (medium): Statement prefixes (Defer, labels, Parallel, modifier/attribute alternation) recurse without a nesting count and overflow the stack

Status: fixed in aba60f7, with parser and lexer tests: Defer's statement, a single-line If's, an Extern block, and the statements a mistake leads to on its line go a level deeper; an #If condition nests up to 1000 deep

Where: compiler/src/parser.cpp:550

Several paths re-enter parse_statement() on the same line without touching m_nesting. They are KW_DEFER (`Defer` + parse_statement), parse_labeled()'s error path (`a: a: ...`), parse_parallel()'s error path (`Parallel Parallel ...`), and parse_modified() -> parse_attributed() -> parse_modified() for `Static @A Static @A ...` (and `@A Public @A Public ...` through parse_visible). Long chains crash in sema and then in the parser itself. The lexer's #If condition parser (parse_any/parse_one) is also unbounded, but only crashes at about 300k nested '(' or '!' on one line.

Reproduced by the reviewer:

```
`Function G()` / `    ` + 'Defer '*20000 + `F()` / `End Function` (a 120 KB file): `fb check` exits 0xC00000FD with no output. Approximate crash thresholds for fb check: Defer about 19600, 'a: '*17300 + 'F()', 'Parallel '*18900 + 'F()', 'Static @A '*30600 + 'Function F()'. The lex+parse-only driver overflows at 50000 for each. `#If ` + '('*300000 + 'DEBUG' + ')'*300000 overflows in lex().
```

Suggested fix: Bump m_nesting (with the max_nesting check) for each Defer's statement. Make the label, Parallel and modifier error paths loop or count nesting. Give the #If condition parser a depth limit too. Add tests to 'code nested deeper than the compiler goes is one error, not a crash'.

## A8-3 (medium): A single-line If accepts multi-line block statements through a label or Unsafe

Status: fixed in d76ec1a (a single-line if refuses a labeled loop, an unsafe block, and an extern block), with parser tests; an Extern block there was accepted with no error too

Where: compiler/src/parser.cpp:2062

parse_single_line_body() refuses If, While, For, Loop, Try, Lock and others by their first keyword, then hands anything else to parse_statement(). A labeled loop starts with an IDENTIFIER, so parse_labeled() parses a whole While/For/Loop/Repeat block. `Unsafe` alone at the line's end goes to parse_unsafe() and parses a whole Unsafe block. Both compile and run. docs/spec/04-control-flow.md says the single-line form's 'statements can't be block statements (If, loops, Select)'.

Reproduced by the reviewer:

```
This program builds and prints 1 2 3 / after / in unsafe / 0 1:
```
If True Then L: While n < 3
    n += 1
    Print(n)
End While
If n == 3 Then Unsafe
    Print("in unsafe")
End Unsafe
If False Then Print("x") Else M: For i = 0 To 1
    Print(i)
End For
```
`If True Then Outer: Loop ... End Loop` also compiles.
```

Suggested fix: In parse_single_line_body, also refuse an IDENTIFIER followed by ':' (a labeled loop) and a KW_UNSAFE that ends the line, with the same 'a single-line If can't contain ...' error. Add parser tests for both.

## A8-4 (medium): The continuation rule after ',', '=' and '&' is missing in several statement forms, so valid line breaks are refused

Status: fixed in bf250bf (a line break can follow every statement's comma, '=', and constraint '&'), with parser tests

Where: compiler/src/parser.cpp:1952

docs/spec/01-lexical.md says a statement continues onto the next line when the line ends with a binary operator, '=', a compound assignment, ',', or '=>'. Many places do call skip_continuation_newlines(): Local/Const '=', assignments, binary operators, Import names, Case values, Enum commas. These don't, and they report an error at the line break: parse_type_list (Implements/Extends lists), the Throws list, parse_constraint ('&' on a Where line), an Enum member's '=', a For loop's '=', an Extern Function's '= "symbol"', parse_destructure's names, parse_simple's tuple-assignment targets, and a For ... In index ('For i,').

Reproduced by the reviewer:

```
The lex+parse driver gives these errors:
- `Enum E` / `A =` / `1`: 'expected an expression, found newline'
- `For i =` / `0 To 5`: same error
- `Class C Implements A,` / `B`: 'expected a type, found newline'
- `Function F() Throws E1,` / `E2`: same
- `Where T: Hashable &` / `Printable`: same
- `Extern "C"` / `Function Abs(n: Int32): Int32 =` / `"abs"`: 'expected the C symbol's name..., found newline'
- `Local a,` / `b = (1, 2)`: 'expected identifier, found newline'
- `a,` / `b = (b, a)`: 'expected an expression, found newline'
- `For i,` / `x In items`: 'expected identifier, found newline'
Each is followed by fallout like 'expression has no effect' on the next line.
```

Suggested fix: Call skip_continuation_newlines() after each of those ',', '=' and '&' tokens (parse_type_list, the Throws loop, parse_constraint, parse_enum's '=', parse_for's '=', parse_function's Extern '=', parse_destructure, parse_simple's tuple loop, parse_for's index comma). Add parser tests for each form.

## A8-5 (medium): A '#' line inside a block comment or multi-line string is a directive in an inactive branch but not in an active one, so a file can check in one build mode and fail in the other

Status: fixed in 0b48b1b (a branch that isn't taken is lexed for its directives), with lexer tests: the inactive branch is lexed with its tokens dropped and its errors unshown, so comments and multi-line strings hide '#' lines in every build

Where: compiler/src/lexer.cpp:662

skip_inactive_lines() treats every line starting with '#' as a directive and doesn't track block comments or strings. In an active branch the same line is skipped as comment text, or scanned as string text by scan_string. So #If/#Else/#End If inside a commented-out region or a multi-line string pair up differently depending on which branch is active. docs/spec/01-lexical.md defines a directive as a line whose first non-blank character is '#'.

Reproduced by the reviewer:

```
ifc.fb:
```
Import std.core
#If RELEASE
Print("release")
#Else
Print("debug")
/*
#End If
*/
#End If
Print("end")
```
`fb check` passes. `fb check --release` gives 'expected an expression, found '*'' at 8:1, '#End If needs an #If before it' at 9:1, and ''Print' doesn't return a value' at 10:1. With a multi-line string holding a `    #End If` text line in the #Else branch, debug passes and release gives 'unterminated string'.
```

Suggested fix: Pick one rule and apply it in both modes. Either have skip_inactive_lines track block comments and multi-line strings (skip_block_comment, and the quotes of a """ string), or have active code report a directive-looking line inside a comment or string. Add lexer tests that lex such a file with both symbol sets.

## A8-6 (medium): A block comment that starts on a directive's line is cut off at the line break, so its later lines are lexed as code

Status: fixed in 0b48b1b, with lexer tests: finish_directive skips trivia, block comments across lines included

Where: compiler/src/lexer.cpp:650

finish_directive() accepts '/*' after a directive as a comment, then calls skip_to_line_end(), which stops at the line break. A block comment that goes on past that line ends early, and its remaining lines become tokens. condition_holds() and the #End If and #Else paths all end through finish_directive. docs/spec/01-lexical.md says a block comment counts as a space even when it spans lines.

Reproduced by the reviewer:

```
`Import std.core` / `#If DEBUG /* a` / `b */` / `Print("x")` / `#End If` gives: ''b' isn't declared' at 3:1, 'expected an expression, found '/'' at 3:4, and ''Print' doesn't return a value' at 4:1. The `#Else /* a` / `#End If` / `*/` form gives 'expected an expression, found '*'' and '#End If needs an #If before it'.
```

Suggested fix: In finish_directive, skip a '/*' comment with skip_block_comment() (and any trivia after it) before checking for the line end. Add a lexer test with a multi-line block comment after #If and after #Else.

## A8-7 (medium): Invalid UTF-8 in an inactive #If branch isn't reported, so the same file fails in one build mode and passes in the other

Status: fixed in 0b48b1b, with lexer tests

Where: compiler/src/lexer.cpp:671

skip_inactive_lines() steps over the lines after the directive with `++m_pos`. The directive's own line goes through skip_to_line_end() and skip_character(), which do report invalid UTF-8. skip_character's comment says it is for 'one character of a comment or skipped line, reporting invalid UTF-8'. docs/spec/01-lexical.md: 'Invalid UTF-8 is a compile error.' The same bytes in a comment are reported.

Reproduced by the reviewer:

```
utf2.fb = `Import std.core` / `#If RELEASE` / `Print("\xFF")` (a raw 0xFF byte) / `#End If` / `Print("ok")`. `fb check` passes (exit 0). `fb check --release` reports 'utf2.fb:3:8: error: invalid UTF-8'. A raw 0xFE after `//` on the #If line itself is reported in debug.
```

Suggested fix: Use skip_character() for the bytes of skipped lines, checking for line breaks the same way. Add a lexer test with a bad byte in a branch that isn't taken.

## A8-8 (medium): A single-line string's interpolation can span lines through a multi-line string nested in it

Status: fixed in 42290bc (a single-line string's interpolation can't hold a multi-line string), with lexer tests

Where: compiler/src/lexer.cpp:427

lex_newline() closes waiting single-line strings at a line break, but scan_string() handles line breaks inside a nested """ string itself, so lex_newline never runs. After the nested string closes, the outer single-line string continues on a later line and compiles. docs/spec/14-strings-collections.md says `"..."` is single line and 'In a single-line string, an interpolation must end on the same line.'

Reproduced by the reviewer:

```
ml3.fb:
```
Import std.core
Print("a{"""
    x
    """}b")
```
`fb run` builds it and prints `axb`. A block lambda or a bracketed list spanning lines in the same place is refused with 'unterminated string: a '{' in it isn't closed on its line'.
```

Suggested fix: When a multi-line string starts while a single-line string is waiting on m_strings (or reaches a line break in that state), report the outer string the way lex_newline does. Add a lexer test.

## A8-9 (medium): No test covers a multi-line string whose interpolation spans lines (the 'line that starts inside an interpolation isn't checked' rule)

Status: fixed in a4e34dd (tests for a multi-line string whose interpolation spans lines): lexer and parser tests, and tests/programs/multi_line_strings.fb

Where: tests/compiler/lexer_tests.cpp:476

docs/spec/14-strings-collections.md promises: 'A line that starts inside an interpolation isn't string text, so it isn't checked.' lexer.cpp implements it: check_indentation() skips lines that line_starts doesn't record. decode_text() also relies on it, since text after the '}' isn't stripped. Every multi-line string test in lexer_tests.cpp, literal_tests.cpp and parser_tests.cpp keeps its interpolation on one line, and no program in tests/programs uses a """ string.

Reproduced by the reviewer:

```
Searched tests/compiler/*.cpp and tests/programs/*.fb for """ strings. The only interpolated multi-line cases are `"""\n  {a}\n  """` (lexer), `"""\n  x {` / `}\n  y\n  """` (literal) and `x = {x}` (parser), all on one line. The behavior itself works: a program with `two {x +` / `1} three` at column 0 inside a """ string runs and prints the right text.
```

Suggested fix: Add a lexer test with an interpolation that spans lines and a continuation line at a smaller indentation, expecting no error. Add a parser or program test that checks the decoded text around it.

## A9-1 (medium): On Windows, one file reached by two spellings loads as two modules (links, junctions, non-ASCII letter case)

Status: fixed in d9fc26d (a file is keyed by its real path, so a link or another letter case loads it once), with driver tests: key_of() uses real_path(), and on_disk() falls back to the entry the disk finds by the name, which isn't a link, for letters other than ASCII's

Where: compiler/src/driver.cpp:415

key_of() keys a loaded file by weakly_canonical(), which in MinGW's libstdc++ follows neither junctions nor symlinks and keeps the letter case as typed. same_file() only catches the rest when folded() keys match, and folded() lowers ASCII letters only. So a file reached through a junction or symlink inside the program's own folder, or through a path whose non-ASCII letters differ in case, loads twice, as two modules with separate types. That breaks the promise in key_of()'s comment and the driver test that 'a file reached by two paths loads once'. On Linux, canonical() resolves the symlink and the same project compiles. For the same reason, warn_respelled()/on_disk() compare names with folded(). They find nothing for 'ÉTÉ' against 'été', so no letter-case warning appears either, though the Import only works on a disk that ignores case.

Reproduced by the reviewer:

```
Junction: s12/ui/hud.fb declares `Public Struct Box` with `Public Field v: Int32`. s12/alias is a junction to s12/ui. b.fb has `Import "./alias/hud"` and `Public Function Make(): Box` returning Box(1). main.fb has `Import std.core`, `Import "./ui/hud"`, `Import "./b"`, `Local a: Box = Make()`. `fb run main.fb` gives `main.fb:4:16: error: expected ui.hud.Box, found alias.hud.Box`. A directory symlink, slink, gives `found slink.hud.Box`. Non-ASCII: été/x.fb declares Box and Make. b.fb imports "./été/x", and main.fb imports "./ÉTÉ/x" and "./b": `error: expected ÉTÉ.x.Box, found été.x.Box`, and ÉTÉ/x.fb and été/x.fb each report their own errors. A main2.fb that only imports "./ÉTÉ/x" builds with no respelling warning.
```

Suggested fix: In key_of(), key an existing file by real_path(), which uses GetFinalPathNameByHandleW and gives the disk's spelling with links followed, and fall back to weakly_canonical() for a missing one. same_file() then becomes a fallback only. In on_disk() and warn_respelled(), compare an entry with the written name by real_path() or equivalent(), not by ASCII-only folded(). Add a driver test with a junction or symlink inside the program's folder, and one with a non-ASCII folder name in another letter case.

Skeptic: confirmed=True, medium. Reproduced in exp-all/V9s/j. With alias a junction (mklink /J) to ui, b.fb importing "./alias/hud" and main.fb importing "./ui/hud" and "./b", `fb run main.fb` gives `main.fb:4:16: error: expected ui.hud.Box, found alias.hud.Box`, so the file loaded as two modules. key_of() uses weakly_canonical(), and same_file() first needs folded() keys to match, which happens only for ASCII case. Non-ASCII case, in exp-all/V9s/u: été/x.fb imported as "./ÉTÉ/x" and "./été/x" gives `expected ÉTÉ.x.Box, found été.x.Box`. A main2.fb that imports only "./ÉTÉ/x" builds with no respelling warning, while the ASCII "./ASCII/x" does get the warning. on_disk() compares with ASCII-only folded().

## A9-2 (medium): fb build refuses a default output path that's a folder, blaming a '-o' nobody gave; on Linux/macOS game.fb beside game/ can't build

Status: fixed in 67ace87 (fb build blames -o only when it's given), with a CLI test: without -o it says 'the executable would be X, which is a folder; give its path with -o'

Where: cli/src/commands.cpp:286

build() computes the default output, next to the source or in build/, and then refuses it when it's a folder, with the message "'-o' names a folder, X; give the executable's file name", even when no -o was given. On Linux and macOS the extension is "", so `fb build game.fb` writes `game`. A main file beside a folder of its modules with the same name, game.fb and game/, is a natural layout without a project.toml (`Import "./game/player"`). It fails with exit code 2 and a message about an option the user never typed. The same project builds on Windows, where the output is game.exe.

Reproduced by the reviewer:

```
On Windows, s14 holds hello.fb and a folder hello.exe/. `fb build hello.fb` prints `error: '-o' names a folder, hello.exe; give the executable's file name` and exits 2, with no -o on the command line. On POSIX, build() sets output = path("game.fb").replace_extension("") = "game", and is_directory("game") is true for the folder beside it, so the same message and exit 2 follow.
```

Suggested fix: Word the error by where the path came from: "-o names a folder" only when options.output is set, and otherwise something like "the executable would be game, which is a folder; give -o". Consider a default output that can't clash on POSIX, and add a CLI test for a default output that's a folder.

Skeptic: confirmed=True, medium. Reproduced in exp-all/V9s/o with hello.fb next to a folder hello.exe/. `fb build hello.fb` with no -o prints `error: '-o' names a folder, hello.exe; give the executable's file name` and exits 2. commands.cpp build() computes the default output and then runs the same is_directory check with the '-o' wording. driver.hpp sets executable_extension to "" off Windows, so replace_extension("") on game.fb gives game, which clashes with a game/ folder next to it.

## A9-3 (medium): fb-bindgen binds macros whose values C leaves undefined (INT_MIN / -1, INT_MIN % -1, out-of-range float casts)

Status: fixed in fbcae77 (a bindgen integer macro must be an integer constant expression), with bindgen tests: each integer number also goes in a _Static_assert((NAME) * 0 + 1) with -Werror=gnu-folding-constant; the shifts follow clang's constant expressions, so 1 << 31 stays bound and -1 << 1 is skipped, as before, and spec 12 says so

Where: tools/bindgen/src/bindgen.cpp:588

Spec 12 says a macro is a number only when clang reads it without 'what C leaves undefined, like an overflow or a negative shift'. evaluate() relies on -Werror=integer-overflow and the shift warnings, but clang raises none of them for a division or remainder that overflows, or for an out-of-range float-to-integer cast. So these macros come out as Consts with whatever value clang folds. The handling is also uneven: `(-1 << 1)` is skipped through -Werror=shift-negative-value, while `(1 << 31)`, which ISO C also leaves undefined and GNU C defines in the same way, is bound.

Reproduced by the reviewer:

```
s.h has `#define S6 (-2147483647 - 1) / -1` and `#define S9 ((-2147483647 - 1) % -1)`. f.h has `#define C1 ((int)1e10)`, `#define C2 ((unsigned char)-1.5)`, and `#define C5 ((long long)9.3e18)`. `fb bindgen` writes `Public Const S6: Int32 = -2147483648`, `S9: Int32 = 0`, `C1: Int32 = 2147483647`, `C2: UInt8 = 0`, and `C5: Int64 = 9223372036854775807`. `gcc -std=gnu17`, the compiler that links programs, warns `integer overflow in expression of type 'int' [-Woverflow]` for S6 and S9. In the same run, `(-1 << 1)` is skipped and `(1 << 31)` is bound as -2147483648. `clang -std=gnu17` on `enum { e1 = (S6), e2 = (S9), e3 = (C1) };` reports each as 'not an integer constant expression; folding it to a constant is a GNU extension [-Wgnu-folding-constant]'.
```

Suggested fix: Also evaluate each integer-typed number where C needs an integer constant expression, such as an enum constant or a _Static_assert, with -Werror=gnu-folding-constant, and skip what fails there. Decide whether `1 << 31` follows GNU or ISO C, and make the shift warnings and the spec's wording agree. Add the cases above to the 'number macros' tests.

Skeptic: confirmed=True, medium. Reproduced in exp-all/V9s/bg/s.h. `fb bindgen s.h` writes `Public Const S6: Int32 = -2147483648` for (-2147483647 - 1) / -1, `S9: Int32 = 0` for INT_MIN % -1, `C1: Int32 = 2147483647` for (int)1e10, `C2: UInt8 = 0` for (unsigned char)-1.5, `C5: Int64 = 9223372036854775807`, and `SH2: Int32 = -2147483648` for (1 << 31). In the same run it skips (-1 << 1), INT_MAX + 1, and 1 << 32. spec 12-ffi.md:206 says a macro is a number only without 'what C leaves undefined, like an overflow or a negative shift'. bindgen.cpp:595-601 passes only the integer-overflow, shift, and division-by-zero -Werror flags, which miss these cases.

## A9-4 (medium): fb bindgen -o can write over the header it reads, destroying the user's C source

Status: fixed in e4a8bae (fb-bindgen won't write its module over the header), with a CLI test, for -o and for the default path

Where: tools/bindgen/src/main.cpp:195

run() opens request.output and writes the module with no check that the output isn't the input header. fb build refuses an -o that names one of the program's own files. fb-bindgen has no such guard, so a mistyped -o replaces the C header with the generated module and reports success.

Reproduced by the reviewer:

```
bg3/t.h is a C header. `fb bindgen t.h -o t.h` prints `Wrote t.h` and exits 0. t.h now starts with `// Generated by fb-bindgen from t.h for x86_64-w64-mingw32.`, and the original declarations are gone.
```

Suggested fix: Before writing, refuse an output that std::filesystem::equivalent() finds to be the header, saying so, as fb build does for its sources. Add a CLI or bindgen test.

Skeptic: confirmed=True, medium. Reproduced in exp-all/V9s/bg. `fb bindgen t.h -o t.h` prints `Wrote t.h` and exits 0. t.h then starts `// Generated by fb-bindgen from t.h for x86_64-w64-mingw32.` and the C declaration is gone. tools/bindgen/src/main.cpp's run() opens request.output with no check against the header. fb build's build() has an equivalent() guard against the program's own files.

## A9-5 (medium): No test checks that --release defines RELEASE (not DEBUG), or which platform symbol #If sees

Status: fixed in 4afc43e (tests that a build defines one mode and its platform, and fb check --release checks release code): a driver test of defined_symbols() and compile_file() in both modes, and a CLI test

Where: compiler/src/driver.cpp:1408

Spec 13 says a build defines DEBUG or RELEASE as `fb build --release` says, plus the platform it's built on, and that `fb check --release` checks the release build's code. The only end-to-end check is tests/programs/compile_time.fb's `#If DEBUG || RELEASE` and `#If PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MACOS`. That passes even when both modes, or every platform, are defined. lexer_tests pass their own symbol arrays. No CLI or driver test sends RELEASE from --release through compile_file() to defined_symbols() and the lexer.

Reproduced by the reviewer:

```
The behavior works today: rel.fb has `#If RELEASE` around an undeclared `x =`, and `fb check rel.fb` passes while `fb check --release rel.fb` reports `'x' isn't declared`. But grep shows no test in tests/cli or tests/compiler/driver_tests.cpp that uses --release with #If or compile_file(..., RELEASE), and none that checks DEBUG is absent in release or that only the host platform is defined.
```

Suggested fix: Add a driver test: compile_file() in RELEASE and in DEBUG mode on a file with `#If RELEASE` / `#If DEBUG` branches, and `#If` on each platform symbol, checking that exactly one of each is taken. Add a CLI test for `fb check --release`.

Skeptic: confirmed=True, medium. No test sends RELEASE through to #If. A grep of tests finds RELEASE, DEBUG, and PLATFORM_ symbols for #If only in lexer_tests.cpp, which uses its own arrays, and in compile_time.fb's `#If DEBUG || RELEASE` and `#If PLATFORM_WINDOWS || PLATFORM_LINUX || PLATFORM_MACOS`, which pass even if every symbol is defined. The release_*.fb programs built in RELEASE mode use no #If, and cli_tests' one --release build doesn't either. Nothing tests defined_symbols() (driver.cpp:1408) or `fb check --release`, though spec 13 line 216 promises both.

## A10-2 (medium): Any pending I/O request keeps a stuck coroutine from being detected, so the program can't end until unrelated stdin input arrives

Status: fixed in f4da315 (a stuck coroutine is found stuck beside an input request it doesn't wait for), with a program test whose stdin stays open: the I/O thread's Tasks count as coming, as timers' do, in place of requests_pending()

Where: runtime/src/thread.cpp:517

scheduler_t::stuck() returns false whenever fb::runtime::requests_pending() is true, as if the pending request could finish any Task. A program that has an InputAsync() nobody awaits, plus a coroutine waiting on a Task on a Scheduler nothing ticks, therefore doesn't end when top-level code ends. It waits until stdin gives a line or EOF. On a console that means waiting until the user presses Enter, though the coroutine can never go on and the spec says it is destroyed. Without the stuck coroutine, the same pending request doesn't hold the end at all (fb_finish's run_finish answers it with Null at once). The runtime could count the I/O thread's Tasks as `coming`, as timers are, instead of treating every request as a reason to wait.

Reproduced by the reviewer:

```
exp-all/A10r/stuck_io.fb: `Global paused = Scheduler()`; `Async Function Inner() Await NextFrame()`; `Async Function Outer(t: Task) Await t`; `Local inner = Inner(); paused.Run(inner); Outer(inner); Local pending = InputAsync(); Print("top done")`. Run as `(sleep 4) | ./stuck_io.exe`: it prints "top done", then the program takes 4051 ms to exit, because it waits for the pipe's EOF. io_open.fb is the same pending InputAsync without the stuck coroutine; it exits in 64 ms under the same pipe. On a console it waits for a line.
```

Suggested fix: In stuck(), don't give up on any pending request. Have io.cpp report the Tasks it will finish (or a predicate for them), and add those to `coming` as timers are, so a coroutine counts as able to go on only when it depends on one of them. Update stuck_input.fb's comment, and add a test where stdin stays open.

Skeptic: confirmed=True, medium. Reproduced. s1.fb: a Global paused = Scheduler(), Outer() moves Inner (Await NextFrame) to paused and awaits it, plus an unawaited `InputAsync()`. s2.fb has only the InputAsync. I timed only the program inside `(sleep 3) | { ...; }`. s1 took 3008 ms because it waits for the pipe's EOF. s2 took 50 ms. stuck() (thread.cpp:517) returns false whenever requests_pending() is true, so fb_scheduler_finish's run_until keeps waiting even though no coroutine depends on the input Task. Spec 10-concurrency.md:183 says such a coroutine is destroyed once top-level code ends. stuck_input.fb only passes because its .in is empty, so EOF comes at once.

## A10-3 (medium): POSIX: a panic or uncaught error leaves a pipe stdin O_NONBLOCK for every process sharing it

Status: fixed in 2fd2f9b (stdin gets its flags back however the program ends), with the POSIX CLI test extended to a panic and an uncaught error: io.cpp registers its restore with the new at_end(), which end_program() runs before _Exit

Where: runtime/src/runtime.cpp:127

libuv's uv_pipe_open sets O_NONBLOCK on fd 0, whose open file description the shell and the program's siblings share. reader_t::close() (io.cpp:73) puts back the flags saved in m_flags. It runs only through io_t::stop, which fb_finish's run_finish calls. panic_with() and fb_error_uncaught() end through end_program(), which calls std::_Exit and skips run_finish. So after Input/InputAsync read a pipe and the program panics or lets an error out, stdin stays non-blocking. The next reader of that pipe, such as `cat` in `producer | { ./prog; cat; }`, gets EAGAIN. M6 step 1 fixed exactly this for a normal end; the abnormal ends still skip it. Found by reading; it can't run on Windows.

Reproduced by the reviewer:

```
Code path: fb_io_input → io_t::start → reader_t::open (UV_NAMED_PIPE: m_flags = fcntl(F_GETFL); uv_pipe_open makes fd 0 non-blocking), then fb_panic → panic_with → end_program → std::_Exit(101). at_finish's stop_io and reader_t::close's fcntl(STDIN_FILENO, F_SETFL, m_flags) never run. fb_error_uncaught → end_program does the same.
```

Suggested fix: Restore stdin's saved flags in end_program before _Exit, through a small hook io.cpp registers. It must only call fcntl, not stop the loop. Or keep the flags in internal.hpp state that end_program reads.

Skeptic: confirmed=True, medium. Confirmed by reading. libuv's unix uv_pipe_open (build-real/_deps/libuv-src/src/unix/pipe.c:211) calls uv__nonblock(fd, 1), which changes fd 0's shared open file description. The only restore is reader_t::close()'s fcntl(STDIN_FILENO, F_SETFL, m_flags) (io.cpp:73-77), which runs from stop_io through run_finish(). Only fb_finish (object.cpp:515) calls that. panic_with() and fb_error_uncaught() end through end_program() (runtime.cpp:127-131): fflush and std::_Exit, with no run_finish or fcntl. So a pipe stdin stays O_NONBLOCK after a panic or an uncaught error. This is the same defect M6 step 1 fixed for a normal end. It can't be run here on Windows.

## A8-10 (low): The nesting limit is reached before 1000 levels and is reported even when nothing deeper follows

Status: fixed in aba60f7, with parser tests at the exact boundary: 1000 levels pass, 1001 don't, and a level is reported only when another one follows

Where: compiler/src/parser.cpp:2518

parse_expression() increments m_nesting for the expression itself, and parse_unary() checks `>= max_nesting`, so a plain primary already costs a level. After each operator (parse_expression) or postfix (parse_postfix, line 2726), the loop re-checks `m_nesting < max_nesting` and calls report_too_deep() as soon as the count reaches 1000, even when the next token isn't another operator. docs/spec/01-lexical.md says code nests 'up to 1000 deep, counting each block, operator, call, member, index, and pair of parentheses'.

Reproduced by the reviewer:

```
Lex+parse driver: 998 pairs of parentheses around 1 is accepted, 999 is refused. A chain `1 + 1 ...` with 998 '+' is accepted, 999 is refused (fb check reports it at column 4007, after the last operand). '- '*998 is accepted, 999 refused. f + '()'*998 is accepted, 999 refused. 997 nested Ifs around `F()` are accepted, 998 refused. 999 nested empty Ifs are accepted, 1000 refused. Each refusal says 'this is nested more than 1000 deep'.
```

Suggested fix: Count so that exactly 1000 levels are accepted. Only report in parse_expression and parse_postfix when another operator or postfix actually follows at the limit. Test the exact boundary (1000 accepted, 1001 refused) and not only 990 and 5000.

## A8-11 (low): After a missing operand, an operator at the line's end pulls the next line into the failed expression

Status: fixed in ed05884 (parser recovery), with a parser test: skip_continuation_newlines() does nothing on a line that already failed

Where: compiler/src/parser.cpp:3155

parse_primary()'s default case reports 'expected an expression' without consuming the token. When that token is a binary operator, the parse_expression loop then takes it as an operator, and skip_continuation_newlines() joins the next line. The next, valid statement becomes the right operand of a broken expression and gets a sema error of its own. The parser's own comment says that after an error 'the rest of the line is skipped, and parsing picks up on the next line'.

Reproduced by the reviewer:

```
`Import std.core` / `Local x = 1 + *` / `Print("a")` gives 'expected an expression, found '*'' and then 'rec.fb:3:1: error: 'Print' doesn't return a value'. The AST is `(local x = (+ (int 1) (* (error) (call Print (string "a")))))`. A stray `*/` line gives the same fallout on the next line.
```

Suggested fix: Stop the binary-operator loop (or don't skip continuation newlines) when the left operand is an error_expr_t on a line that already failed. Or have parse_primary consume an unexpected binary operator token. Add a parser test that the next line still parses as its own statement.

## A8-12 (low): describe_current() for 'End' reads the next token across a line break inside brackets, so the message holds the next line's text

Status: fixed in ed05884, with a parser test: describe_current() joins only the very next token

Where: compiler/src/parser.cpp:357

describe_current() uses peek(1) to show 'End <keyword>'. Inside brackets, peek() skips NEWLINE tokens, so the 'keyword' can come from the next line. The message then quotes all the source text in between, line break included.

Reproduced by the reviewer:

```
`Function F()` / `    G(1, End` / `    H())` / `End Function` gives the error message 'expected an expression, found 'End\n    H'' at 2:10. The rendered message spans two lines.
```

Suggested fix: Look at the raw next token (m_tokens[index + 1]) and only join it when it's on the same line (no NEWLINE in between). Add a parser test.

## A8-13 (low): A missing End inside a Property's Get or Set takes the 'End Get'/'End Set' as its own, giving a second, wrong error

Status: fixed in ed05884, with a parser test: m_accessors keeps the open Get or Set, whose End isn't taken as an inner block's

Where: compiler/src/parser.cpp:450

Get and Set blocks aren't pushed onto m_blocks, and their End's second word is an IDENTIFIER. So expect_end()'s stray-End rule (`!closes_outer_block(peek(1).kind)`) takes `End Get` as the inner block's End. The Get body then runs on to End Property, which is reported as a missing End Get. In a Function the same mistake gives one error.

Reproduced by the reviewer:

```
`Class C` / `Property P: Int` / `Get` / `If x` / `Return 1` / `End Get` / `End Property` / `End Class` gives 'expected 'End If', found 'End Get'' and also 'expected 'End Get', found 'End Property'', though End Get is there. The same missing End If in a Function gives only the first error.
```

Suggested fix: Treat `End Get`/`End Set` as closing an outer block in expect_end while an accessor body is open (for example, track accessors on m_blocks or a flag). Add a recovery test.

## A8-14 (low): Control characters from the source reach the terminal raw through diagnostic messages and --show dumps, and the AST dump doesn't re-escape braces

Status: open

Where: compiler/src/diagnostic_render.cpp:55

cells_of() escapes control characters in snippets 'so it can't change the terminal'. header() prints the message raw, so source text in messages passes ESC and other controls straight through: an @Deprecated message, an Import path, a convention string. In dump.cpp, escape_controls() (--show tokens) escapes only \n, \r and \t. quote() (--show ast/tree/types), documented as writing text 'the way source code writes it', escapes only \n, \t, \\ and \". It writes \r, \0, ESC raw and writes a decoded '{' as '{', which source would read as an interpolation.

Reproduced by the reviewer:

```
`@Deprecated("use \x1B[2J\x1B[31mG")` on F, then `F()`: the warning bytes are `deprecated: use 033 [ 2 J 033 [ 3 1 m G` (od -c of stderr). The same holds for `there's no module "./<ESC>[31mred"`. For `Print("a<ESC>[31mb\r\x1B\0z{{")`, `--show ast` prints `"a<ESC>[31mb<CR><ESC><NUL>z{"` and `--show tokens` prints the raw ESC.
```

Suggested fix: Escape control characters (and invalid bytes) in header() messages, label messages, notes and paths, the same way cells_of() does. Make escape_controls() and quote() escape every control character as \xHH, and make quote() write '{' and '}' as '{{' and '}}'. Add render and dump tests.

## A8-15 (low): '>=' or '>>=' right after type arguments isn't split, so `Stack<Int>= ...` is refused

Status: open

Where: compiler/src/parser.cpp:2842

The parser splits '>>' into two '>' tokens for nested generics. The lexer joins '>=' into GREATER_EQUAL, and parse_type_arguments() (and Weak<...>'s expect at line 3480) only accept GREATER. A declaration whose type ends in '>' directly before '=' fails to parse. The spec gives no spacing rule for this; only '>>', 'As?' and 'T?' are spacing-sensitive (docs/spec/03-operators.md).

Reproduced by the reviewer:

```
`Local t: Stack<Int>= Stack<Int>()` gives 'gt.fb:10:19: error: expected '>', found '>='' (with 'opened here' at the '<'), and then the fallout ''t' is read before it's assigned'. `Local s: Stack<Stack<Int>>= ...` fails the same way on the '>=' after the first '>'.
```

Suggested fix: When closing type arguments at a GREATER_EQUAL token, consume its '>' and leave an '=' (for example, keep a split-token offset as the parser does for '>>='), and do the same for Weak<...>. Add a parser test.

## A8-16 (low): Stale and misleading comments in ast.hpp and parser.cpp

Status: open

Where: compiler/include/fb/ast.hpp:66

Several ast.hpp comments describe old milestones: COALESCE 'Not supported in M1.' (66), null_expr_t 'Not supported in M1.' (128), cast_expr_t '...which isn't supported in M1.' (210), is_expr_t 'Not supported in M1.' (217), nullable_type_t 'Not supported in M1.' (314), weak_type_t 'M2 has no other generic types.' (326), error_stmt_t '...a construct after M1 that was reported and skipped' (360). character_expr_t says 'U+FFFD when the literal is empty or invalid' (110-111), but decode_character() gives the first character for 'ab'. parser.cpp:899 has '// Lows 71, 78, 96, 120, 123: attributes come first...', which cites review item numbers and isn't a plain sentence (CLAUDE.md conventions).

Reproduced by the reviewer:

```
Read ast.hpp lines 66, 110-111, 128, 210, 217, 314, 326, 360 and parser.cpp:899. `Local j = 'ab'` decodes to 'a' through decode_character, which only strips the quotes and decodes the first character.
```

Suggested fix: Remove the 'Not supported in M1/M2' remarks. Reword error_stmt_t ('A statement that failed to parse'). Say character_expr_t's value is U+FFFD when the literal is empty and its first character otherwise. Replace the 'Lows ...' comment with a plain sentence.

## A8-17 (low): `New Foo()` gets ''New' isn't supported yet', promising a form nothing plans

Status: open

Where: compiler/src/parser.cpp:3153

parse_primary reports KW_NEW in an expression as '{} isn't supported yet'. The language makes objects by calling the type (Foo(), docs/decisions.md rows 96 and 103), the spec has no New expression, and docs/milestones.md's Deferred work has no row for one. A Blitz user who writes `New Foo` is told to wait for a feature instead of being shown how to write it.

Reproduced by the reviewer:

```
`Class Foo` / `End Class` / `Local f = New Foo()` gives 'newx.fb:4:11: error: 'New' isn't supported yet'.
```

Suggested fix: Say how to make one, for example 'objects are made by calling their type, like Foo(); New only declares a constructor'. Update the parser test that expects the old message.

## A8-18 (low): An #If condition missing its last symbol at end of file gets a diagnostic span past the end of the file

Status: open

Where: compiler/src/lexer.cpp:731

parse_one() reports 'expected a symbol, like DEBUG' at span(m_pos, m_pos + 1) without checking at_end(). When the condition is cut off at the end of the file, the span ends one byte past the text. That breaks source_file_t::slice()'s and location_of()'s preconditions (they assert end <= size), which a consumer of span.end, like the M7 language server, would trip in a debug build.

Reproduced by the reviewer:

```
A lex-only driver printing diagnostic spans: a 5-byte file `#If !` gives '5-6: expected a symbol, like DEBUG'. `#If (` gives 5-6. A 12-byte `#If DEBUG ||` gives 12-13.
```

Suggested fix: Use an empty span at the end (span(m_pos, m_pos)) when at_end(), as the parser's point() does. Add a lexer test for `#If !` at EOF.

## A9-6 (low): A folder Import whose files are links gets a second, false 'no folder X/ of modules' error

Status: open

Where: compiler/src/driver.cpp:645

In resolve_imports()' folder branch, a file that linked_away() reports is skipped with its own error. Then targets is empty, so report_missing() also runs. It says there is no folder of modules, though the folder is there with .fb files in it.

Reproduced by the reviewer:

```
s7/app/project.toml names the package app. s7/app/shared/a.fb is a file symlink to s7/outside/a.fb. main.fb has `Import app.shared`. `fb check` reports two errors at 2:8: `shared/a.fb is a link to '.../outside/a.fb', outside the package's folder; ...`, then `there's no module app.shared: no shared.fb, and no folder shared/ of modules`.
```

Suggested fix: Remember that the folder had candidates it reported or skipped, and call report_missing() only when it had none.

Skeptic: confirmed=True, low. Reproduced in exp-all/V9s/s7/app, with shared/a.fb a file symlink (mklink) to ../../outside/a.fb. `fb check` for `Import app.shared` reports `shared/a.fb is a link to '.../outside/a.fb', outside the package's folder; ...` and then a second, false error at the same place: `there's no module app.shared: no shared.fb, and no folder shared/ of modules`.

## A9-7 (low): A [native] path naming a folder says the folder isn't there, and a trailing '/' lists nonsense archive names

Status: open

Where: compiler/src/driver.cpp:1235

report_no_archive() gives its 'is a folder' message only when archive_names() returns one name, that is, when the path has a .a or .lib extension. For a plain folder path it lists the candidates, the folder itself first, as missing. For a path ending in '/' the filename is empty, which gives 'libs/lib.a, libs/.a, libs/.lib'.

Reproduced by the reviewer:

```
s5/project.toml has `[native]`, `vec = "libs/vec"` with libs/vec/ an existing folder, and `other = "libs/"`. `fb check` prints `"libs/vec" names no static library: there's no libs/vec, libs/libvec.a, libs/vec.a, or libs/vec.lib, from the package's folder`, though libs/vec is there. It also prints `"libs/" names no static library: there's no libs/, libs/lib.a, libs/.a, or libs/.lib, from the package's folder`.
```

Suggested fix: Check is_directory(root / written) first, whatever the extension, and say it's a folder. Refuse a path ending in a separator, or one with an empty filename, before listing candidates.

Skeptic: confirmed=True, low. Reproduced in exp-all/V9s/s5 with libs/vec/ an existing folder. `vec = "libs/vec"` gives `there's no libs/vec, libs/libvec.a, libs/vec.a, or libs/vec.lib`, though libs/vec is there. `other = "libs/"` gives `there's no libs/, libs/lib.a, libs/.a, or libs/.lib`. Only `xa = "libs/x.a"`, which has an extension, gets the message `is a folder, not a static library`.

## A9-8 (low): A module file with an upper-case .FB extension: a quoted Import finds it silently on Windows, while folder Imports and library checks never see it

Status: open

Where: compiler/src/driver.cpp:533

warn_respelled() looks the last part up as `part + ".fb"` with folded() and then strips the extension. A file Up.FB therefore matches `Import "./Up"` without the letter-case warning, though only a disk that ignores case finds it, and Linux reports the module missing. folder_files() and library_files() compare `extension() == ".fb"` case-sensitively, so the same file is left out of a folder Import or a library's check on every platform. compile_file(), meanwhile, takes a main file's extension in any letter case.

Reproduced by the reviewer:

```
In s9, Up.FB declares F(), and main.fb has `Import "./Up"`. `fb run main.fb` prints 7 with no warning. With a project.toml naming the package pk, sub/Down.FB declares G(), and main.fb's `Import pk.sub` gives `there's no module pk.sub: no sub.fb, and no folder sub/ of modules`.
```

Suggested fix: In warn_respelled(), compare the found file's extension too and warn when it isn't exactly '.fb'. Either take .FB in folder_files() and library_files() as compile_file() does, or refuse it consistently.

Skeptic: confirmed=True, low. Reproduced in exp-all/V9s/s9: Up.FB is found by `Import "./Up"`, which prints 7 with no letter-case warning. warn_respelled() looks up part + ".fb" with folded() and strips 3 characters, so the found name matches what was written. In s9b, with a project.toml and sub/Down.FB, `Import pk.sub` gives `there's no module pk.sub: no sub.fb, and no folder sub/ of modules`. Meanwhile compile_file() takes a main file's extension in any letter case, through folded() at driver.cpp:1574.

## A9-9 (low): fb and fb-bindgen leave the console's output code page at UTF-8 after they exit

Status: open

Where: cli/src/terminal.cpp:75

prepare_terminal() calls SetConsoleOutputCP(CP_UTF8) and never restores the old value. The output code page belongs to the console, not the process, so the user's shell and every legacy program it runs afterwards keep CP 65001. ENABLE_VIRTUAL_TERMINAL_PROCESSING also stays set.

Reproduced by the reviewer:

```
`cmd /c "chcp 437 >nul & cp.exe & fb.exe --version & cp.exe"`, where cp.exe prints GetConsoleOutputCP(), prints `out=437` before fb runs and `out=65001` after it exits.
```

Suggested fix: Save GetConsoleOutputCP() and the console modes in prepare_terminal() and restore them at exit (atexit, or a guard in main), or write to the console with WriteConsoleW instead of changing the code page.

Skeptic: confirmed=True, low. Reproduced in exp-all/V9s/cp. `cmd /c "chcp 437 >nul & .\cpq.exe & .\fb.exe --version & .\cpq.exe"` prints out=437, then fb 0.1.0, then out=65001. terminal.cpp's prepare_terminal() calls SetConsoleOutputCP(CP_UTF8) and never restores it. runtime.cpp's fb_start() does the same, so every compiled program also leaves the console at 65001.

## A9-10 (low): fb-bindgen silently ignores an empty -o, and an -o naming a folder fails as 'Permission denied' after making the folder

Status: open

Where: tools/bindgen/src/main.cpp:145

`-o ""` sets request.output to empty, and the later `if (request.output.empty())` quietly substitutes <header>.fb. That goes against decisions.md's rule that nothing typed is ignored without a word, and fb's own -o refuses an empty path. `-o out/` creates the folder out with create_directories(parent_path()) and then fails to open it, reporting errno's 'Permission denied'.

Reproduced by the reviewer:

```
`fb bindgen t.h -o ""` prints `Wrote t.fb` and exits 0. `fb bindgen t.h -o out/` prints `error: couldn't write out/: Permission denied`, exits 1, and leaves an empty out/ folder.
```

Suggested fix: Refuse an empty -o value and one that ends in '/' or '\' or names an existing folder, with a usage error before generating, as fb's parse_options() does for -o.

Skeptic: confirmed=True, low. Reproduced in exp-all/V9s/bg. `fb bindgen t2.h -o ""` prints `Wrote t2.fb` and exits 0, because main.cpp's `if (request.output.empty())` swaps in the default. `fb bindgen t2.h -o out/` prints `error: couldn't write out/: Permission denied`, exits 1, and leaves an empty out/ folder behind.

## A9-11 (low): A usage error ignores a --color that was given and parsed

Status: open

Where: cli/src/commands.cpp:535

report_usage_error() always uses color_t::AUTO, on the grounds that the options didn't parse. But `fb --color never bogus` or `fb --color always build x.fb --nope` parse --color successfully before the error, and the choice is dropped: a terminal still gets color after `--color never`, and a pipe gets none after `--color always`.

Reproduced by the reviewer:

```
`fb --color always nosuchcmd 2>&1 | od -c` shows `error: unknown command 'nosuchcmd'` with no escape sequences, though --color always was given.
```

Suggested fix: Have parse_options() return the color it parsed with the error, for example an error struct carrying `color`, and pass it to report_usage_error().

Skeptic: confirmed=True, low. `fb --color always nosuchcmd 2>&1 | od -c` shows `error: unknown command 'nosuchcmd'` with no escape sequences. `fb --color always check nonexist.fb` does print `033[1;31merror`. commands.cpp:533-536's report_usage_error() always uses color_t::AUTO, with the comment 'The options didn't parse, so there's no `--color` to follow', which is wrong when --color itself parsed.

## A9-12 (low): project.toml paths with backslashes work only on Windows

Status: open

Where: compiler/src/driver.cpp:1123

is_package_file() in manifest.cpp checks 'main' for '..' across both '/' and '\', and the Import resolver treats '\' as a separator on every platform (path_parts()). But the manifest's main, dependency path, and [native] archive path all go through `root / path_from_utf8(text)`. On Linux and macOS, `main = 'src\main.fb'`, `physics = { path = '..\physics' }`, or `joltc = 'libs\joltc'` is then a single file name containing a backslash. A package written on Windows breaks elsewhere, with 'there's no src\main.fb in the package's folder' or 'there's no project.toml in .../..\physics'.

Reproduced by the reviewer:

```
Found by reading: driver.cpp lines 1123 (main), 1157 (dependency folder) and 1193 (find_archive) join the written text with path_from_utf8, which keeps '\' as an ordinary character on POSIX. On Windows the same text names src/main.fb.
```

Suggested fix: Normalize '\' to '/' in the manifest's paths, or refuse '\' there with an error suggesting '/', so one project.toml means the same on every platform.

Skeptic: confirmed=True, low. Confirmed by reading. manifest.cpp's is_package_file() treats '\\' as a separator, and driver.cpp's path_parts() splits Imports at '\\' too. But main (driver.cpp ~1123), dependency paths (~1157), and find_archive() (~1193) join with `root / path_from_utf8(text)`. path_from_utf8 (utf8.cpp:244) builds a std::filesystem::path straight from the text, and on POSIX a '\\' in it is an ordinary filename character. So `main = "src\\main.fb"` names src/main.fb on Windows and a file literally called src\\main.fb on Linux and macOS. No doc says backslashes are refused. Not run on POSIX.

## A9-13 (low): Ctrl+C during fb run kills fb first, leaving its temporary build behind and the program unwaited

Status: open

Where: cli/src/commands.cpp:335

run() waits in run_attached() with no console-control or SIGINT handling. Ctrl+C goes to fb and the program alike. fb dies at once, by the default handler, ExitProcess or SIGINT, so the remove_all(folder) cleanup never runs and the shell prompt can come back while the program is still shutting down. fb's exit status is then its own death, not the program's exit code that run() means to pass on. Ctrl+C is the normal way to stop a running game loop, so a %TEMP%/futureblitz/<name>-xxxxxxxx folder holding the executable is left after each such run.

Reproduced by the reviewer:

```
Found by reading: neither commands.cpp nor process.cpp installs SetConsoleCtrlHandler or ignores SIGINT around run_attached(). The cleanup at lines 339-340 runs only after run_attached() returns.
```

Suggested fix: While the child runs, ignore Ctrl+C in fb (SetConsoleCtrlHandler(nullptr, TRUE) on Windows, SIGINT/SIGQUIT set to SIG_IGN on POSIX, put back before returning), so fb waits, cleans up, and returns the program's code, as cargo run does.

Skeptic: confirmed=True, low. Reproduced on Windows with Ctrl+Break, which the default handler treats the same as Ctrl+C. A launcher (exp-all/V9s/cc/launch.c) started `fb run ctrlloopv9.fb` (an endless loop) in a new process group and sent CTRL_BREAK_EVENT. fb exited with 0xC000013A, not the program's code, and %TEMP%/futureblitz/ctrlloopv9-dd3c7fdf/ctrlloopv9.exe was left behind; I deleted it. Nothing in cli/, compiler/src, or runtime/src calls SetConsoleCtrlHandler or handles SIGINT. run()'s remove_all only runs after run_attached() returns.

## A9-14 (low): run_process() ignores SIGPIPE for the whole process, and every program it later starts inherits that

Status: open

Where: compiler/src/process.cpp:316

To keep the input writer from being killed, run_process() calls std::signal(SIGPIPE, SIG_IGN) process-wide and never restores it. An ignored disposition survives posix_spawn and exec, so every later child started by run_process() or run_attached() in that process also runs with SIGPIPE ignored. In the test binaries that includes fb itself and the programs fb runs. Such a program then gets EPIPE where it would have been ended by SIGPIPE.

Reproduced by the reviewer:

```
Found by reading: line 316 runs on the first run_process() call that has input, as in cli_tests' 'a program's input goes in...'. Later spawn() calls pass no posix_spawnattr, so the children inherit SIG_IGN for SIGPIPE.
```

Suggested fix: Block SIGPIPE only in the writer thread (pthread_sigmask) and drain a pending one with sigtimedwait, or reset SIGPIPE to SIG_DFL in children with posix_spawnattr_setsigdefault, or restore the old disposition once the writer joins.

Skeptic: confirmed=True, low. Confirmed by reading. process.cpp:316 calls std::signal(SIGPIPE, SIG_IGN) for the whole process whenever input is given, and never restores it. spawn() passes nullptr attributes to posix_spawn, so later children inherit SIGPIPE ignored, as POSIX keeps SIG_IGN across exec. Only the test binaries (cli_tests, driver_tests, program_tests) pass input; fb's own link.cpp call has none. So the effect is on the test harness's children, which is why low fits. Not run on POSIX.

## A9-15 (low): Misplaced and stale doc comments in the area

Status: open

Where: compiler/include/fb/manifest.hpp:60

Several doc comments sit on the wrong declaration or describe old behavior. (1) In manifest.hpp:60-66, read_manifest()'s comment sits above is_library_name()'s, so it documents is_library_name(), and read_manifest() has none. (2) In module.hpp:128-136, std_home()'s comment ('The module that declares `name` ... for `array_method`') sits above std_member_home()'s comment, and std_home() has none. (3) In manifest.cpp:120-123, span_of()'s comment ('Where a part of the file is. toml++ counts lines and columns from 1, in code points.') sits on error_span(). (4) process.hpp:20-23 says run_process()'s input 'has to fit in a pipe's buffer, a few KiB', but it has been written from a thread of its own since M6 step 1, and cli_tests send more than that. (5) module.hpp:36 and 39 say 'The main module is named by its file's stem' and 'The main file's is its name', but in a package the main module is named by its path, game.main, and its path is game/main.fb (add_modules()). (6) commands.cpp:389's example '"game v0.1.0 (C:\games\game)"' shows backslashes, but to_utf8() prints 'C:/games/game'.

Reproduced by the reviewer:

```
Found by reading the declarations after each comment. For (5), driver.cpp's add_modules() names a package's main module with module_name(loaded.path). For (6), `fb run s1` prints `Compiling game v0.1.0 (C:/Users/.../s1)`.
```

Suggested fix: Move each comment onto its own declaration. Drop the pipe-buffer limit from run_process()'s doc. Say in module_t's docs that without a project.toml the main module is named by its stem and its path is its name, and in a package by its path. Use forward slashes in the commands.cpp example.

Skeptic: confirmed=True, low. All six read as claimed. (1) manifest.hpp: read_manifest()'s comment ('Reads `file`, a project.toml ... nullopt after an error.') runs straight into is_library_name()'s comment and sits above is_library_name(), and read_manifest() has none. (2) module.hpp: std_home()'s comment ('...among their Methods of T[] for `array_method`') is stacked above std_member_home()'s, and std_home() has none. (3) manifest.cpp: 'Where a part of the file is. toml++ counts lines and columns from 1, in code points.' sits on error_span(), not span_of(). (4) process.hpp:20-23 still says the input 'has to fit in a pipe's buffer, a few KiB', though process.cpp writes it from a thread of its own. (5) module.hpp:36-39 says the main module is named by its stem, but driver.cpp:842 uses module_name(loaded.path) when there are packages. (6) commands.cpp:389's '"game v0.1.0 (C:\\games\\game)"' has backslashes, while the real output is `Checking app v0.1.0 (C:/Users/.../s7/app)`.

## A10-4 (low): A Thread that finishes during pool_t::finish() leaves its id in m_finished, so a later start_thread can join a running thread under m_mutex

Status: open

Where: runtime/src/thread.cpp:209

The thread lambda in start_thread() lowers m_pending and only then locks m_mutex to push its id into m_finished. finish() can see m_pending == 0, lock, clear m_finished and move m_threads out to join, all before that push. The id then stays in m_finished after the thread is joined and its handle closed, so the OS may give the same id to a new thread. finish() runs twice per program: at the end of top-level code and in fb_finish. A Destroy that starts Threads while the Globals are released can therefore start one whose id matches. The next start_thread() then calls join() on that running thread while holding m_mutex, and the thread deadlocks when it locks m_mutex itself, at its own end or in any signal().

Reproduced by the reviewer:

```
Code path: thread lambda `m_pending.fetch_sub(...)` (line 116); finish()'s wait loop sees 0 (line 188), then under m_mutex it clears m_finished (line 209) and joins; the lambda then takes m_mutex and pushes its id (line 121). Later fb_job_start_thread → start_thread → erase_if finds a reused id in m_finished → thread.join() on a live thread while holding m_mutex (line 109).
```

Suggested fix: Push the id to m_finished before lowering m_pending, in the same locked section. Or have finish() clear m_finished only after joining. Or track done threads by a flag in a per-thread record instead of by std::thread::id.

Skeptic: confirmed=True, low. Confirmed by reading thread.cpp. The start_thread lambda (lines 114-124) does m_pending.fetch_sub, and only then takes m_mutex to push its id into m_finished. finish() (lines 186-218) checks m_pending without the lock at its loop top, so it can take m_mutex first, clear m_finished, move m_threads out and join. The late push then leaves a stale id. A later start_thread's erase_if (lines 101-109) calls join() under m_mutex on any thread whose id matches, and a live thread with that id deadlocks when it locks m_mutex. This needs thread-id reuse, which glibc's pthread_t (the TCB address) does readily. winpthreads' ids mostly don't repeat, so it's mainly a POSIX hazard. It also needs Threads started in a Destroy during Globals release, between the two finish() calls. The window is tiny, so I couldn't reproduce it, but the race is plain in the code. Low is right.

## A10-5 (low): An uncaught error's report names Classes by their short names, so two alike Classes read the same

Status: open

Where: runtime/src/runtime.cpp:313

fb_error_uncaught prints at->type->name for the error and each Cause. Type panics and leak reports use shown_name()/alike() to add modules when two names in one message are the same (M5's full_name work), but this message doesn't. An error and its Cause of two different Classes with one name, from two modules, print identically.

Reproduced by the reviewer:

```
exp-all/A10r/uncaught/: a.fb and b.fb each declare `Public Class Oops Extends Error`. main.fb has `Import "./a" As A`, `Import "./b" As B`, and a Fail() that catches A.Oops("inner") and throws B.Oops("outer", e), then `Try Fail()` at top level. Output: "uncaught Oops: outer at main.fb:9:9" / "caused by Oops: inner at main.fb:7:9". fb_object_expect would say e.g. "expected main... a.Oops, found b.Oops" for the same pair.
```

Suggested fix: Collect the chain's Classes first, up to max_causes, then print each with shown_name(type, span of the others), as fb_finish does for leaks.

Skeptic: confirmed=True, low. Reproduced with A10r/uncaught's files (copied to exp-all/skA10/unc). main.fb prints "uncaught Oops: outer at main.fb:9:9" / "caused by Oops: inner at main.fb:7:9", and the two Classes can't be told apart. For the same pair, cast.fb (`e As B.Oops` on an A.Oops) panics with "expected b.Oops, found a.Oops". fb_error_uncaught (runtime.cpp:313) prints at->type->name directly, unlike leak reports and type panics, which go through shown_name()/alike(). That breaks M5's rule that a message showing two types qualifies alike names.

## A10-6 (low): An array's Reserve that can't get the memory panics with no location

Status: open

Where: runtime/src/array.cpp:40

fb_array_reserve takes a location, but only its negative check uses it. A capacity that overflows (capacity > PTRDIFF_MAX / element_size) or that realloc can't satisfy panics inside resize_buffer with fb_panic("out of memory", nullptr). The program gets a bare "panic: out of memory". Mem.Alloc's out-of-memory panic, by contrast, names the size and the caller's place.

Reproduced by the reviewer:

```
exp-all/A10r/reserve_huge.fb: `Struct Big / Field cells: Int64[1000]`, `Local items: Big[] = []`, `items.Reserve(2000000000)`. Output: "panic: out of memory" with no " at reserve_huge.fb:8:1", exit 101.
```

Suggested fix: Pass the location through resize_buffer from fb_array_reserve, and say what was asked for, e.g. "out of memory: Reserve couldn't make room for 2000000000 elements".

Skeptic: confirmed=True, low. Reproduced. res.fb: Struct Big with `Field cells: Int64[1000]`, `items.Reserve(2000000000)` prints "panic: out of memory" with no place, exit 101. res2.fb: `items.Reserve(-1)` prints "... not -1 at res2.fb:4:1". fb_array_reserve has the location (array.cpp:113-119), but resize_buffer panics with fb_panic("out of memory", nullptr) at lines 40 and 45.

## A10-7 (low): take()'s comment says other threads' queue comes first, but for a worker it comes after later workers' queues

Status: open

Where: runtime/src/thread.cpp:289

The comment says "The other threads' queue first, then each other worker's, starting after this one's." For a worker t, start = t + 1 and index = (m_size + start + i) % (m_size + 1). i = 0 gives t itself (skipped), and the shared queue m_size comes only after workers t+1..m_size-1. Worker 0 with three workers looks at 1, 2, then the shared queue. Only a thread without a queue sees the shared queue first.

Reproduced by the reviewer:

```
m_size = 3, t_queue = 0: start = 1; i = 0 → (3+1+0)%4 = 0 (own, skipped); i = 1 → 1; i = 2 → 2; i = 3 → 3 (the shared queue, last).
```

Suggested fix: Either fix the comment to describe the order, or start a worker's scan at the shared queue (e.g. index = (m_size + i) % (m_size + 1), skipping its own) if that order was meant.

Skeptic: confirmed=True, low. Confirmed by reading thread.cpp:289-291. For a worker t, start = t+1, and index = (m_size + t + 1 + i) % (m_size + 1). That gives t (skipped), then t+1 .. m_size-1, then m_size (the shared queue) at i = m_size - t, then 0 .. t-1. So for a worker, the shared queue does not come first, as the comment "The other threads' queue first" says. Only a non-worker (start = 0, i = 0 gives m_size) takes the shared queue first. The comment is wrong; the behavior is otherwise harmless.

## A10-8 (low): Leak counts don't take a singular: "leak: 1 Hub objects were never released"

Status: open

Where: runtime/src/object.cpp:538

fb_finish's count lines always use the plural ("%lld %s objects were never released", "%lld Strings were never released", "%lld CString copies of Strings were never freed"). report_cycles in the same report does handle one ("a cycle of 1 object"). The expected outputs lock the wrong form in, e.g. leak_channel.out: "leak: 1 Hub objects were never released", leak_cstring.out: "leak: 1 CString copies of Strings were never freed".

Reproduced by the reviewer:

```
tests/programs/leak_channel.out line 2: "leak: 1 Hub objects were never released"; leak_closure.out: "leak: 1 Cell<Timer> objects were never released".
```

Suggested fix: Pick "object"/"objects", "String was"/"Strings were", and "copy ... was"/"copies ... were" by count, as report_cycles does, and update the .out files.

Skeptic: confirmed=True, low. object.cpp:520-541 always uses the plural: "%lld Strings were never released", "%lld CString copies of Strings were never freed", "%lld %s objects were never released". report_cycles (line 475) picks "object%s" by count. The expected outputs lock the wrong form in: tests/programs/leak_channel.out has "leak: 1 Hub objects were never released" and "leak: 1 Channel<Hub> objects...", leak_cstring.out has "leak: 1 CString copies of Strings were never freed", and program_tests.cpp:302-304, 333, 429, 477 check "leak: 1 ... objects" strings. Nothing in Deferred work covers it.

## A10-9 (low): fb_start changes the console's output code page and stderr's mode for the whole console, and never restores them

Status: open

Where: runtime/src/runtime.cpp:217

SetConsoleOutputCP(CP_UTF8) and SetConsoleMode(..., ENABLE_VIRTUAL_TERMINAL_PROCESSING) apply to the console, which the parent shell shares, not just to the process. Nothing saves or restores them: not at a normal end, a panic, or an uncaught error. After running any FutureBlitz program from cmd.exe, the console stays on code page 65001 (`chcp` shows it), and legacy programs run later in that window that print in the OEM code page show garbled non-ASCII. Found by reading the Windows API's documented behavior; nothing in the runtime saves the old values.

Reproduced by the reviewer:

```
Code path: fb_start → SetConsoleOutputCP(CP_UTF8) (line 217) and SetConsoleMode(error, mode | VT) (line 226); end_program/_Exit and fb_finish never call SetConsoleOutputCP or SetConsoleMode again.
```

Suggested fix: Save GetConsoleOutputCP() and the original mode in fb_start, and put them back at the end, in fb_finish and in end_program before _Exit. Or write to the console with WriteConsoleW and leave the code page alone.

Skeptic: confirmed=True, low. Reproduced. From PowerShell: `cmd /c "chcp 437 >nul & cpq.exe & hi.exe & cpq.exe"`, where cpq.exe is a probe that prints GetConsoleOutputCP(). Before hi.exe it gives out_cp=437; after a FutureBlitz hello-world it gives out_cp=65001 in the same console. fb_start (runtime.cpp:217, 226) calls SetConsoleOutputCP(CP_UTF8) and SetConsoleMode(stderr, mode | VT). Nothing saves or restores either value, not in fb_finish nor in end_program. Neither docs/decisions.md nor the Deferred work table covers it.
