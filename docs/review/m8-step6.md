# The review of M8 step 6 (the formatter)

24 confirmed findings, each with its Status.

## cli-1 (medium): The constraint warning counts an Extend's target type, and a type parameter's own name, as a relation, so it wrongly asks for a Where line

compiler/src/sema.cpp:1935. For an Extend's members, `outer` is every word in the Extend's type text (`words_of(text(m_ast.get(extension->type).span))`). That text includes the type's own name, such as String or Int, not just its type parameters. `relates` (line 2024) then calls any constraint whose Interface mentions that name a relation between type parameters. So a single constraint written inline, as spec 07 asks, gets a warning and a fix-it that moves it to a Where line. `relates` also matches the constrained parameter itself, so `T: Ordered<T>` gets the same warning. Spec 07 says a relation is to another type parameter, and tooling.md says 'relates two type parameters'.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s6/cli/w/c.fb (Interface Ordered<T>; Struct N Implements Ordered<N>; `Function Least<T: Ordered<T>>(a: T, b: T): T`; `Extend Int` / `Method Pick<S: Ordered<Int>>(s: S): Int`), and w/a.fb (`Extend String` / `Method JoinAll<S: Source<String>>(items: S): String`). Ran C:/Users/Hunter/Projects/build-m8-fmt/cli/fb.exe run --color never c.fb. The harness scratchpad/review-m8s6/cli/harness/fixes.exe multi.fb, linked against libfb_compiler.a, prints the fix-it, which rewrites it to `Method JoinAll<S>(items: S): String` plus `Where S: Source<String>`.

Expected: No warning. Each declaration has one constraint on one of its own type parameters, and it relates to no other type parameter, so inline is the form spec 07 asks for.

Actual: c.fb:9:16 and c.fb:13:17, and a.fb:6:20: 'warning: a declaration with more than one constraint has each on a Where line', with a fix-it that moves the constraint to a Where line.

Skeptic: I ran `fb.exe run --color never c.fb` in review-m8s6/cli/w. It warned 'a declaration with more than one constraint has each on a Where line' at c.fb:9:16 (`Function Least<T: Ordered<T>>`) and at c.fb:13:17 (`Extend Int` / `Method Pick<S: Ordered<Int>>`). `fb check a.fb` gave the same warning at 6:20 for `Extend String` / `Method JoinAll<S: Source<String>>`. I ran harness/fixes.exe multi.fb with build-m8-fmt/cli on PATH for LLVM-C.dll: its fix-it rewrites the method to `Method JoinAll<S>(items: S): String` plus `Where S: Source<String>`. In sema.cpp, the Extend branch builds `inside` from `words_of(text(extension->type span))`, so the type's own name (String, Int) counts as an outer type parameter. `relates` also matches any name in `names`, including the constrained parameter itself. Spec 07 says a relation is to 'another' type parameter, and tooling.md says 'relates two type parameters'. A concrete String or Int is neither, so for the Extend case the code plainly contradicts both docs. Severity: it is only a warning, but its fix-it pushes code into the non-canonical form.

Fix: For an Extend, build `outer` only from the names in the target type's type arguments that are type parameters, not from its own name or a concrete type such as Int or String. In `relates`, skip the constrained parameter's own name. Add sema tests for `Extend String` with `<S: Source<String>>` and for `<T: Ordered<T>>`.

Status: fixed: an Extend's type parameters are the names in its type that aren't types, after its own name, and a constraint on a type parameter of itself, as T: Ordered<T>, relates none (sema test 'one constraint goes inline...')

## format-1 (medium): A '>=' or '>>=' right after type arguments makes format_source refuse a valid file, calling it a bug

compiler/src/format.cpp:456. add_token splits a GENERIC '>=' into '>' and '=' so the output is `Map<String, Int> = {...}`. meaning() then compares the lexed token text of input and output. The input lexes `>=` as one token and the output lexes '>' and '=', so the check always fails. fb fmt can't format any file that writes `List<Int>=[]`, `Map<String, Int>={...}`, `List<Int?>=[]` or `List<List<Int>>=[]` without spaces. The code meant to handle this case never gets to run.

Shown by: scratch/review-m8s6/format/ge/ge.fb:
Import std.core
Import std.collections

Local m: Map<String, Int>={"a": 1}
Print(m.Count)

fb.exe run ge/ge.fb -> prints 1
fb.exe fmt --color never ge/ge.fb -> exit 1. The same happens for a one-line file `Local x: List<Int>=[]`, for `Local y: List<List<Int>>=[]` and for `Local t: List<Int?>=[]`, each run through f.sh (fb fmt in a package with [format] width).

Expected: `Local m: Map<String, Int> = {"a": 1}`, the output add_token's split is written to give.

Actual: ge.fb:1:1: error: fb fmt would change what this file means, which is a bug in it; the file is left as it is

Skeptic: I rebuilt ge.fb (`Local m: Map<String, Int>={"a": 1}`) and `fb run` printed 1. Then `fb fmt` (width 40 package) on `Local x: List<Int>=[]`, on `List<List<Int>>=[]` and on `List<Int?>=[]` each exited with "error: fb fmt would change what this file means, which is a bug in it". The code agrees: add_token (format.cpp:455-456) splits a GENERIC GREATER_EQUAL into '>' and '='. meaning() (1093) compares describe(kind)+slice for each token from lex_layout, so the input's single '>=' token can never match the output's '>' '=' pair. format_source therefore refuses every valid file that writes type arguments touching '='.

Fix: Make meaning() compare a GENERIC '>=' / '>>=' as its pieces, for example by normalising those tokens with the parser's roles. Another way is to compare the AST plus the token text with all whitespace removed. Add a format test for `List<Int>=[]`.

Status: fixed with layout-2: same_meaning() compares a '>=' as its '>' and '=', as the parser reads it, so 'List<Int>=[]' formats to 'List<Int> = []' (format test 'type arguments that a >= ends')

## format-3 (medium): Greedy breaking leaves a statement as written when a different break would fit

compiler/src/format.cpp:1020. layout_code picks the best candidate on each line (a comma before an opening bracket) and commits to it. If the line after that can't fit, it returns false, and emit_group puts the whole statement back as written. It never tries another break. tooling.md says only a statement that 'can't fit at any break' is left as written, and that a break after an opening bracket comes 'when nothing else fits'. Here a hanging break, or the break after '=', fits, but the line stays over the width.

Shown by: printf 'Local value = Something(aaaa, bbbbbbbbbbbbbbbbbbbb)\n' | f.sh greedy 40   (f.sh = fb fmt in a package with [format] width = 40)
At the default width: printf 'Function F()\n    Local message = Format.Describe(code, "the request could not be completed because the server went away")\nEnd Function\n' | f.sh greedy100 100

Expected: Local value = Something(\n    aaaa, bbbbbbbbbbbbbbbbbbbb)   (31 wide; fb fmt keeps this form unchanged when it's given). At 100 wide, `Local message =` followed by `        Format.Describe(code, "...")` at 97 columns, or a break after `Describe(`.

Actual: Both are left on one line: 51 columns at width 40, and 108 columns at width 100.

Skeptic: At width 40, `Local value = Something(aaaa, bbbbbbbbbbbbbbbbbbbb)` stays on one line (51 columns). The same text broken after `Something(` (31 wide) is kept by fb fmt as given, so a break that fits exists. At width 100, the Format.Describe line stays at 108 columns, though a break after '=' gives about 96. In layout_code (format.cpp ~1020), when an item overflows it takes the min_element candidate, commits to it, and returns false on a later overflow with no candidates; it never tries another. tooling.md says only a statement that "can't fit at any break" is left as written, and that a break after an opening bracket comes "only when nothing else fits".

Fix: When a line after a chosen break can't fit, go back and try the next-best candidate (a bounded backtrack over the earlier lines' choices) before giving up on the statement. Add tests for these two inputs.

Status: fixed: a line too wide tries its breaks best first, and goes back to the next when what follows can't fit, up to 256 tries a line (format test 'tries the next break')

## format-4 (medium): A statement that can't fit is still broken, and its new line is wider than the width

compiler/src/format.cpp:1020. The width check is skipped for the first item on a new line (`!first`). When the item that doesn't fit is the last one in a statement, such as a long string after '=' or after an operator, the formatter breaks before it, puts it alone on the next line over the width, and reports success. tooling.md says a statement that can't fit at any break, 'as with a long string', is left as written. The tests only cover a long string followed by ')', which does make the statement fall back to as written.

Shown by: printf 'Local message = "a string that is much too long for a line of forty"\n' | f.sh ls4 40
printf 'Local x = aaaa + "a string that is much too long for a line of forty"\n' | f.sh ls1 40

Expected: Both left on one line as written, since neither fits at any break.

Actual: Local message =\n    "a string that is much too long for a line of forty"   (56 columns), and Local x = aaaa +\n          "a string ..." (62 columns). New line breaks are added that don't make the statement fit.

Skeptic: At width 40, `Local message = "a string that is much too long for a line of forty"` became `Local message =` plus `    "a string ..."` (56 columns). `Local x = aaaa + "..."` became `Local x = aaaa +` plus `          "..."` (62 columns). In layout_code the check is `if (fit && !first && column + space + width > m_width)`, so the first item on a new line is never measured and the break is reported as success. tooling.md names exactly this case: a statement that can't fit at any break, "as with a long string, is left as written".

Fix: Also check the first item of a line after a break (column + width > m_width means the break didn't help) and return false, so emit_group falls back to as written. Add these two inputs to the 'can't fit' test.

Status: fixed: a line that starts too wide after a break fails it, so a statement that can't fit at any break stays as written (format test 'next line would be too wide too')

## format-6 (medium): A comment at the end of the last Case's body is moved out to the Case's level

compiler/src/format.cpp:573. emit_loose puts a comment that is written deeper than the block end after it at that end's depth plus one step. Before End Select, the block the comment is in is the last Case's body, two steps deeper than End Select. So an already formatted comment at the body's indentation is moved one step out, to the Case's column. tooling.md says such a comment gets 'a block's' indentation, the block it's written in.

Shown by: printf 'Select x\n    Case 1\n        Print(1)\n        // still in the case\nEnd Select\n' | f.sh selc 40

Expected: Unchanged: the comment stays at 8 spaces, under Print(1).

Actual: The comment is written at 4 spaces (`    // still in the case`), at the Case's level. A comment before `Case 2` is handled correctly (8 spaces).

Skeptic: I ran `Select x / Case 1 / Print(1) / // still in the case (8 spaces) / End Select`, which is already formatted. The comment was rewritten at 4 spaces, the Case's level, outside the body it was written in. The same comment before `Case 2` stayed at 8 spaces. tooling.md says a comment written inside a block before its end gets that block's indentation. Here an already-formatted file is changed in a way that misleads the reader.

Fix: Use the depth of the code line before the comment, capped at the depth the comment is written at, rather than next->depth + 1. Or treat End Select as closing two levels. Add a test.

Status: fixed: a comment written inside a block before its end goes at the depth it's written at, from a step deeper than the end to the last line of code's depth (format test 'a Select's last Case')

## format-8 (medium): A declaration with its attribute on the same line gets its hanging parameters 4 deep instead of 8

compiler/src/format.cpp:673. is_declaration skips only modifier keywords before Function/Method/New/Operator. With an attribute on the same line, the first item is '@', so the statement isn't treated as a declaration and its '(' isn't a signature. The parameters of `@Inline Function F(` therefore line up with the body, which the 8-deep rule exists to prevent. The spec writes attributes this way (`@Inline Function Square(...)` in spec 05 and 13), and so do tests/programs/compile_time.fb and main_thread.fb.

Shown by: printf '@Inline Function Lerp(\na: Float, b: Float): Float\n    Return a\nEnd Function\n' | f.sh attr 40
printf '@Inline Function Lerp(aaaaaaaaaaa: Float, bbbbbbbbbb: Float): Float\n    Return a\nEnd Function\n' | f.sh attr3 40

Expected: `        a: Float, b: Float): Float` (8 spaces), as for the same declaration without @Inline.

Actual: `    a: Float, b: Float): Float`: 4 spaces, the same column as `    Return a`. When fb fmt breaks the long one, both parameter lines also go at 4.

Skeptic: `@Inline Function Lerp(` with `a: Float, b: Float): Float` on the next line put the parameters at 4 spaces, the same column as `    Return a`. Without @Inline they went at 8. When fb fmt broke the long @Inline signature itself, both parameter lines also went at 4. tooling.md says 8 for a declaration's parameters, so they stand apart from its body. The spec (05:14, 13:69,74) and tests/programs/compile_time.fb:59 and main_thread.fb:11 all write the attribute on the declaration's line. is_declaration (format.cpp:673) skips only modifiers, not attributes.

Fix: In is_declaration, skip '@' and the attribute's name and arguments (up to the matching ')') as well as modifiers. Add a test.

Status: fixed: a declaration's keyword is found past its attributes and their arguments, and only a '(' after it holds the parameters (format test 'attribute on its line')

## layout-2 (medium): Type arguments closed by a touching '>=' ('Stack<Int>= s', which the parser supports) can't be formatted

compiler/src/format.cpp:455. The parser gives the '>' of a '>=' token the GENERIC role when it closes type arguments, keyed at the token's begin as parser.hpp documents. The formatter's add_token then splits the lexer's GREATER_EQUAL token into '>' and '=' items, and spaced() puts a space between a generic close and '=', so the output has 'Map<Int, Int> = ...'. That re-lexes as GREATER EQUAL instead of GREATER_EQUAL, so meaning() (line 1149), which compares lex_layout's token kinds and texts, sees a change and refuses the file. The same happens for '>>=' after nested type arguments. The parser's constructor comment names 'Stack<Int>= s' as supported input.

Shown by: q11.fb:
Import std.core
Import std.collections
Local s: Map<Int, Int>= Map<Int, Int>()
Print(s.Count)

fb.exe run q11o.fb (the same text) prints 0. fb.exe fmt q11.fb prints: error: fb fmt would change what this file means, which is a bug in it, and exits 1. Same for q4.fb with 'Local xs: Map<Int, Map<Int, Int>>= Map<Int, Map<Int, Int>>()'.

Expected: fb fmt writes 'Local s: Map<Int, Int> = Map<Int, Int>()', or leaves the '>=' touching, and succeeds.

Actual: fb fmt refuses the file, saying the refusal is a bug in fb fmt.

Skeptic: I rebuilt q11.fb ('Local s: Map<Int, Int>= Map<Int, Int>()') and q4.fb (the nested '>>=' form). fb run prints 0 for both. fb fmt refuses both with 'fb fmt would change what this file means, which is a bug in it' and exits 1. parser.cpp:133 says this input is supported: '">=" comes as '>' then '=' ... so "Stack<Int>= s" closes its type arguments before the '='.' So fb fmt refuses valid input that the parser documents.

Fix: Make meaning() compare tokens with each '>=' split into '>' and '=' (and '>>' as two '>'), as the parser's m_split does, so 'X<T>= v' and 'X<T> = v' mean the same. Or keep the '>' and '=' touching in spaced(). Add a format test for 'Local s: Map<Int, Int>= x' and the '>>=' case.

Status: fixed with format-1 (format test 'type arguments that a >= ends', with the '>>=' case)

## layout-3 (medium): Two prefix minuses, '- -a', are refused because the formatter joins them into '--'

compiler/src/format.cpp:795. The parser marks each prefix '-' UNARY (parser.cpp line 3091). spaced() returns false after any unary item, so '- -a' would be written as '--a', which lexes as MINUS_MINUS. The meaning check sees a different token and refuses the file. '! !True' and '~ ~1' become '!!True' and '~~1' safely, because neither pair forms a token, but '- -' does. 'a - - -a' fails the same way.

Shown by: q12.fb:
Import std.core
Local a = 2
Local b = - -a
Print(b)

fb.exe run q12o.fb prints 2. fb.exe fmt q12.fb: error: fb fmt would change what this file means, which is a bug in it; exit 1. q13.fb ('Local b = a - - -a') fails the same way.

Expected: fb fmt writes '- -a' (keeping the space where joining would make '--') and succeeds.

Actual: fb fmt refuses a valid file, saying the refusal is a bug in fb fmt.

Skeptic: I rebuilt q12.fb ('Local b = - -a'). fb run prints 2 and fb fmt refuses with the 'bug in it' meaning-check error, exit 1. q13.fb ('Local b = a - - -a') runs (prints 0, which is correct) and fb fmt refuses it the same way. Joining the two unary minuses would give '--', a different token, so the meaning check rejects the output. Valid and plausible code can't be formatted.

Fix: In spaced(), keep a space between a unary '-' and a following '-' or '--' (any pair whose spellings would lex as one longer token). Add a format test for '- -a'.

Status: fixed: a prefix '-' keeps a space before what starts with '-' (format test 'prefix minuses')

## cli-2 (low): The warning for a constraint that relates two type parameters says there's more than one constraint

compiler/src/sema.cpp:2071. Any declaration that isn't the single-constraint case goes through the second branch, including one with a single constraint that relates two type parameters. That branch always says 'a declaration with more than one constraint has each on a Where line', which is wrong for a single relating constraint.

Shown by: scratchpad/review-m8s6/cli/w/a.fb: `Function Collect<S: Source<E>, E>(items: S): Int`. Ran fb.exe check --color never a.fb.

Expected: A message that gives the actual reason, e.g. 'a constraint that relates type parameters goes on a Where line'.

Actual: a.fb:10:18: warning: a declaration with more than one constraint has each on a Where line (the declaration has exactly one constraint).

Skeptic: `fb check a.fb` printed 'a.fb:10:18: warning: a declaration with more than one constraint has each on a Where line' for `Function Collect<S: Source<E>, E>`, which has exactly one constraint. In sema.cpp, check_constraint_form's second branch always uses this one message, whatever the cause: more than one constraint, a `&`, or a relation.

Fix: Choose the message by cause (more than one constraint, `&`, or a relation) and test each wording.

Status: fixed: the message says why: a relation, constraints on more than one type parameter, or more than one on one (sema test 'one constraint goes inline...')

## cli-3 (low): The fix-its insert LF-only Where lines into CRLF files and delete a Where line's trailing comment

compiler/src/sema.cpp:2082. In the Where branch, each inserted line ends in '\n', even when the file uses '\r\n', so the file ends up with mixed line endings. In the inline branch, the fix deletes the whole Where line (line_of(...) replaced with ''), so a comment after the constraint on that line is lost. diagnostic.hpp describes the fixes as changes that 'fix what a diagnostic says', but these also change other text.

Shown by: scratchpad/review-m8s6/cli/harness/fixes.exe crlf.fb. The harness is linked against build-m8-fmt/compiler/libfb_compiler.a and applies each diagnostic's fixes. crlf.fb is CRLF throughout: `Function Both<A: Equatable, B: Equatable>(a: A, b: B): Int` ... and `Function One<T>(a: T): Int` / `    Where T: Equatable // needs ==`. Checked the bytes with `| od -c`.

Expected: The inserted Where lines end in \r\n like the rest of the file, and the comment '// needs ==' is kept (for example, moved to the header line).

Actual: The output has 'Equatable\n    Where B: Equatable\n' between \r\n lines, and after the second fix '// needs ==' is gone.

Skeptic: I ran harness/fixes.exe crlf.fb, a file that is CRLF throughout, and read its output with od -c. stdout is text mode, so the original line ends show as \r\r\n, while the inserted Where lines show as plain \r\n: they were written with a bare \n. That matches the code's `"{}    Where {}: {}\n"`. For `Function One<T>(a: T): Int` / `Where T: Equatable // needs ==`, the inline fix replaced the whole span [133..169] (the line from line_of) with '', and '// needs ==' is gone from the result. The fix-it deletes a user's comment and mixes line endings.

Fix: Use the file's line ending (from the header line) in the inserted lines. In the inline branch, delete only from the Where keyword to the end of the constraint, or keep the comment. Add tests for both.

Status: fixed: the Where lines a fix adds end with the file's line break, and a comment after a Where line the inline fix takes stays on its own line (sema test)

## cli-4 (low): fb fmt rejects a .FB file that fb check, build, and run accept, and skips such files in folders

cli/src/fmt.cpp:131. collect() compares `path.extension()` with ".fb" exactly, both for a file given on the command line (line 131) and while scanning folders (line 160). The rest of fb takes a .fb extension in any letter case (cli_tests: 'fb takes a .fb file's extension in any letter case').

Shown by: scratchpad/review-m8s6/cli/Upper.FB (`Import std.core` / `Print( 2 )`). Ran fb.exe fmt Upper.FB, then fb.exe run Upper.FB. Also ran fb.exe fmt --check up/, a folder that holds a copy.

Expected: fb fmt formats Upper.FB, as fb run compiles it.

Actual: `error: Upper.FB isn't a .fb file`, exit 2, while `fb run Upper.FB` builds it and prints 2. `fb fmt --check up` silently skips the file and exits 0.

Skeptic: `fb fmt --check Upper.FB` printed 'error: Upper.FB isn't a .fb file' and exited 2. `fb run Upper.FB` built it and printed 2. fmt.cpp:131 compares `path.extension() != ".fb"` exactly, but driver.cpp compares a given file's extension folded to lower case (lines 2329 and 2796), and cli_tests has 'fb takes a .fb file's extension in any letter case'. The folder-scan half is weaker: the driver's own folder scans also compare '.fb' exactly (driver.cpp:2508), so skipping .FB files in folders matches the build. The case of a file named on the command line is the real inconsistency.

Fix: Compare the extension without regard to letter case, as the driver does, and add the case to the fb fmt CLI tests.

Status: fixed: a file named on the command line takes .fb in any letter case; a folder's files stay .fb, as the build's scans are (CLI test 'any letter case')

## cli-5 (low): A file reached by two spellings is formatted and counted twice; --check lists it twice

cli/src/fmt.cpp:89. run() removes duplicates with sort and unique on the path text, so './src/main.fb' and 'src/main.fb' stay separate entries. With --check, the file is listed twice and counted twice in the summary.

Shown by: In scratchpad/review-m8s6/cli/pkg (project.toml, src/main.fb containing `Print( 1 )`), ran: cd pkg && fb.exe fmt --check . src

Expected: src/main.fb listed once, and '1 file isn't formatted'.

Actual: './src/main.fb' and 'src/main.fb' are both listed, then 'error: 2 files aren't formatted; fb fmt formats them'.

Skeptic: In a package with an unformatted src/main.fb, `fb fmt --check . src` listed './src/main.fb' and 'src/main.fb', then 'error: 2 files aren't formatted', exit 1. fmt.cpp's run() removes duplicates with std::ranges::sort and unique on the raw path text. Formatting a file twice is harmless, but the list and the count are wrong.

Fix: Remove duplicates by a normalized key (weakly_canonical or lexically_normal of the absolute path) while keeping the spelling that's shown, and test overlapping arguments.

Status: fixed: files are formatted once each, by their canonical paths, shown by the spelling first found (CLI test 'twice, by two paths')

## cli-6 (low): fb fmt drops project.toml's warnings, so a misspelled [format] key is silently ignored

cli/src/fmt.cpp:197. width_for() reports the manifest's diagnostics only when read_manifest fails. When it succeeds, warnings such as "[format] doesn't use 'widht'" are dropped, and fb fmt formats to the default width of 100 without saying why. Spec 11 says a key project.toml doesn't use is a warning, and fb check does show it.

Shown by: scratchpad/review-m8s6/cli/typo/project.toml has [project] and `[format]` / `widht = 40`. main.fb holds a 52-column Function line. Ran (cd typo && fb.exe fmt --check; fb.exe fmt) and compared with fb.exe check w2/a.fb on a package with `indent = 2`, which does print the warning.

Expected: fb fmt prints 'warning: [format] doesn't use 'widht'' once for the package, as fb check does.

Actual: No output, exit 0. The 52-column line isn't wrapped to 40, and nothing hints at the typo.

Skeptic: I used a package whose project.toml has `[format]` / `widht = 40`. `fb fmt --check` printed nothing and exited 0. `fb check main.fb` in the same package printed 'project.toml:6:1: warning: [format] doesn't use 'widht''. In fmt.cpp, width_for() calls report(diagnostics...) only when read_manifest returns nullopt, so when it succeeds its warnings are dropped. Spec 11 says a key project.toml doesn't use is a warning.

Fix: In width_for, render the manifest's warnings (once per package, since the result is cached) even when it succeeds. Add a CLI test.

Status: fixed: project.toml's warnings are shown once for each package, when it's read too (CLI test 'a misspelled width')

## cli-7 (low): fb fmt's unknown-option message contradicts itself for --check=value and leaves out --color and --help

cli/src/options.cpp:267. Any argument starting with '-' that isn't exactly '--check' or '--color' gets "'<name>' doesn't work with 'fb fmt', which takes --check". For '--check=yes', the name is '--check', so the message says --check doesn't work with a command that takes --check. The message also lists only --check, though fb fmt takes --color and -h/--help too.

Shown by: fb.exe fmt --check=yes pkg ; fb.exe fmt - ; fb.exe fmt --release

Expected: For example "'--check' takes no value" for --check=yes, and a list that names every option fb fmt takes (or 'unknown option').

Actual: error: '--check' doesn't work with 'fb fmt', which takes --check (exit 2). '-' and '--release' get the same 'which takes --check' wording.

Skeptic: `fb fmt --check=yes pkg` printed "error: '--check' doesn't work with 'fb fmt', which takes --check" and exited 2, which contradicts itself. '-' and '--release' get the same 'which takes --check' wording, though `fb fmt --help` lists --check, --color, and -h/--help. Other commands say "unknown option '--release=yes'". The code is options.cpp:266-269, which passes `name` (the part before '=') into the 'which takes --check' message.

Fix: Handle '--check=' separately with a 'takes no value' message, and make the message name --check and --color (or point to 'fb help fmt'). Test --check=yes.

Status: fixed: '--check=yes' says '--check' takes no value, and others name fb fmt's options, --check and --color (CLI usage test)

## cli-9 (low): Spec 11's project.toml section doesn't mention [format]

docs/spec/11-modules.md:98. manifest.cpp now reads a [format] table with width from 40 to 1000 and rejects anything else as an error. Spec 11's Packages section, which lists project.toml's tables and says any other key is a warning, still lists only [project], [dependencies], and [native]. [format] is documented only in docs/tooling.md.

Shown by: grep -n "\[format\]" docs/spec/11-modules.md (no match). fb.exe fmt w1 with `[format] width = 39` gives "[format]'s width is a whole number of columns from 40 to 1000, like 100".

Expected: Spec 11 lists [format] width: its range, its default of 100, and what it's for.

Actual: Spec 11 doesn't mention it.

Skeptic: `grep -rn "\[format\]" docs/` finds only tooling.md:105. Spec 11's Packages section (lines 98-145) lists [project], [dependencies], and [native], and says 'A key project.toml doesn't use is a warning, and its other mistakes are errors'. manifest.cpp:95 now accepts a [format] table, and its width check (line 267) is an error for values outside 40 to 1000. CLAUDE.md calls docs/spec/ the source of truth, and its list of project.toml's tables and their errors is now incomplete.

Fix: Add a bullet for [format] to spec 11's Packages section, linking to tooling.md's formatter section.

Status: fixed: spec 11's Packages section has [format] width, its range and default, linking tooling.md

## format-10 (low): The width counts code points, so wide (CJK) text goes past the width in columns

compiler/src/format.cpp:26. width_of counts UTF-8 code points. format.hpp and tooling.md promise lines up to `width` columns, but East Asian wide characters take two columns and combining marks take none. A line of CJK text that shows 46 columns wide is kept at width 40. Byte versus code-point counting itself is right; Latin text such as é is measured correctly.

Shown by: printf 'Print(aaaa, "中文中文中文中文", bbbbbbbb, ccc)\n' | f.sh cjk2 40   (38 code points, 46 display columns)

Expected: Either the line breaks to fit 40 columns, or the docs say the width is counted in code points.

Actual: Left on one line, 46 columns wide.

Skeptic: `Print(aaaa, "中文中文中文中文", bbbbbbbb, ccc)` is 38 code points and 46 display columns, and it stayed on one line at width 40. width_of (format.cpp:26) counts UTF-8 lead bytes, and its comment says "How wide text shows: its code points". format.hpp:17 promises `width` columns. This is a small mismatch between the docs and the code for wide characters.

Fix: Count East Asian Wide/Fullwidth code points as 2 (and combining marks as 0), or state in format.hpp and tooling.md that the width counts code points.

Status: fixed: the width counts the columns a terminal shows, with utf8.hpp's display_width, and tooling.md says so (format test 'a wide character's columns')

## format-11 (low): No test checks that format_source refuses output whose tokens or tree differ, and the repository test skips refused files

tests/compiler/format_tests.cpp:335. The refusal path in format_source (the 'would change what this file means' error) is never tested. Every format test REQUIRES success, and the repository-wide test uses format_unchecked. Also, 'std, tests/programs, and tests/packages are formatted' (the step's done-when) skips any file where format_source fails (`if (result)`), not only files that don't parse, as its comment says. A std file that fmt refuses, as in format-1 or format-2, would pass the done-when test silently. Today every repo file formats (`fb fmt --check std tests/programs tests/packages` exits 0), so nothing is hidden yet.

Shown by: grep -rn "would change" tests/  (no hits). Code reading: format_tests.cpp:333-338. Example of a refused file: the ge.fb from format-1.

Expected: A test that format_source returns the 'would change what this file means' error for a case where formatting changes the tokens, such as the `Map<String, Int>={...}` file from format-1 until that is fixed. The repository test should fail when format_source returns an error other than a parse error, at least for std.

Actual: Neither is checked.

Skeptic: grep finds no test for the "would change what this file means" error. The only failing format_source test (format_tests.cpp ~311) is for a file that doesn't parse. The done-when test "std, tests/programs, and tests/packages are formatted" (around lines 333-338) wraps its check in `if (result)` under a comment about parse failures, so a refused std file would pass silently. The meaning-keeping repository test uses format_unchecked and never runs the refusal path. format-1 shows the refusal does fire on valid input, and a refusal test would have exercised that path.

Fix: Add a refusal test, for example by giving format_source an input whose output re-lexes differently. In the repository test, fail on errors for std files, or tell parse failures apart from refusals.

Status: fixed: same_meaning() is in format.hpp and tested for what changes meaning and what doesn't, and the 'formatted' repository test fails on a file fb fmt refuses that parses

## format-2 (low): A valid file whose #If wraps a block's opening line can't be formatted, and the reported parse error is wrong

compiler/src/format.cpp:1127. lex_layout drops every #If region, and parse_layout then parses what is left. Spec 13 lets the branches hold any lines, and the build only parses the active branch. When the #If holds a declaration's header (or any block opener) and its End is outside, the layout parse sees an unmatched End. format_unchecked then returns that as a syntax error. fb fmt refuses a file that builds and runs, with an error that isn't true of the file. tooling.md only says #If lines are left as written; it says nothing about such files being unformattable.

Shown by: scratch/review-m8s6/format/if4.fb:
Import std.core

#If DEBUG
Function F(): Int
#Else
Function F(): Int
#End If
    Return 1
End Function

Print(F())

fb.exe run if4.fb -> prints 1
fb.exe fmt if4.fb -> exit 1

Expected: The file is formatted: the #If lines stay as written and the rest is laid out. At the least, the message should say the formatter can't handle an #If that splits a block, not report a parse error.

Actual: if4.fb:9:1: error: unexpected 'End Function' / error: couldn't format 1 file

Skeptic: I rebuilt if4.fb (#If DEBUG / Function F(): Int / #Else / Function F(): Int / #End If, then the body and End Function outside the region). `fb run` printed 1. `fb fmt` printed "if4b.fb:9:1: error: unexpected 'End Function'" and "couldn't format 1 file", exit 1. Spec 13 says an inactive branch needn't parse and that #If directives may sit inside blocks, so the file is valid. The formatter reports a parse error that isn't true of it, and nothing in tooling.md says such files can't be formatted. The file is left untouched, so this is low.

Fix: Parse each #If region's first branch for layout, as one platform's build would. Or detect an unbalanced block across a region and report a specific message. Document the limit in tooling.md and test it.

Status: fixed with layout-4: a file whose #If holds part of a statement, which parses in some build, gets an error at the #If saying fb fmt needs each #If to hold whole statements, and tooling.md says so (format test 'an #If that holds part of a statement'); laying out each branch stays out, as #If lines are left as written

## format-5 (low): Two comments on one line of their own are split onto two lines

compiler/src/format.cpp:422. place_comment adds a comment to the current unit only when the unit already has items on the same line, or when code follows the comment on its line. For a second comment on a line with no code, current is empty and the next token is the NEWLINE, so each comment becomes its own unit and gets its own line. tooling.md says comments stay where they are.

Shown by: printf 'Function F()\n    /* a */ /* b */\n    Print(1)\nEnd Function\n' | f.sh bc6 40
printf '/* a */ // b\nPrint(1)\n' | f.sh bc7 40
printf 'Print(1,\n  /* a */ /* b */\n  2)\n' | f.sh bc8 40

Expected: `    /* a */ /* b */` and `/* a */ // b` kept on one line.

Actual: `    /* a */` and `    /* b */` on separate lines; `/* a */` and `// b` on separate lines; inside the call, `      /* a */` and `      /* b */` on separate lines.

Skeptic: Three inputs, all run through f.sh at width 40. `    /* a */ /* b */` on its own line became two lines, `    /* a */` and `    /* b */`. `/* a */ // b` became `/* a */` and `// b`. Inside a call's arguments, `/* a */ /* b */` was likewise split into two lines. tooling.md says comments "stay where they are". The change is cosmetic and doesn't change what the file means.

Fix: In place_comment, when the previous unit is a comment-only unit that ends on the same source line, append to it instead of starting a new unit. Add a test.

Status: fixed: a comment on the line of a comment of its own joins it (format test 'comments on one line of their own')

## format-7 (low): A blank line at a block's start is kept when the block holds only #If lines or comments

compiler/src/format.cpp:521. blank_before keeps a blank line after code when the depth of the next code line is <= statement_depth. For a comment or an #If region, emit_loose passes the depth of the next code line. In a block that contains nothing but #If lines or comments, that next line is the block's End, at the outer depth. So the blank right after the opening line is kept, though tooling.md says there are no blank lines at the start of a block.

Shown by: printf 'Function F()\n\n#If DEBUG\n    Print(1)\n#End If\n\nEnd Function\n' | f.sh ifb 40
printf 'Function F()\n\n    // c\n\nEnd Function\n' | f.sh cb 40

Expected: Function F()\n#If DEBUG\n    Print(1)\n#End If\nEnd Function   and   Function F()\n    // c\nEnd Function

Actual: The blank line after `Function F()` is kept in both. The blank before End Function is removed, as it should be.

Skeptic: `Function F()`, a blank, `#If DEBUG / Print(1) / #End If`, a blank, `End Function` kept the blank after `Function F()` and removed only the one before End Function. A body of only `// c` with blanks around it behaved the same way. With a code body (`Print(1)`) both blanks were removed. tooling.md says there are no blank lines at the start of a block.

Fix: Pass the comment's own block depth (the depth_before plus the inside-block adjustment emit_loose already makes) to blank_before. Or note when the previous code line opened a block. Add tests with #If-only and comment-only bodies.

Status: fixed: right after the line that opens the block the next line ends, no blank line comes before a comment or #If lines (format test 'a block of comments or #If lines')

## format-9 (low): tooling.md says a break after '=' or '=>' comes last, but the code puts it before an opening bracket when the rest fits

compiler/src/format.cpp:1055. layout_code moves a depth-0 '='/'=>' break up to tier 1, ahead of an opening bracket's tier 2, when the rest fits on the next line. The test 'After '=' or "=>" comes before an opening bracket when the rest fits on the next line' checks this. tooling.md's Long lines rule still says 'after an opening bracket only when nothing else fits, and after = or => last', which is not what the code does.

Shown by: printf 'Local value = Something.Long(argumentsss)\n' | f.sh eq 40 gives `Local value =\n    Something.Long(argumentsss)` (also format_tests.cpp line 270), a break after '=' chosen over the opening bracket.

Expected: The docs describe the rule as coded: after '=' or '=>' before an opening bracket when what follows fits on the next line, last otherwise.

Actual: The docs say '=' / '=>' is always the last choice.

Skeptic: At width 40, `Local value = Something.Long(argumentsss)` gives `Local value =` plus `    Something.Long(argumentsss)`: the break after '=' is chosen over the opening bracket. layout_code promotes a depth-0 tier-2 '=' or '=>' break to tier 1 when the rest fits (the `whole` flag), and format_tests.cpp:269 tests that behaviour. tooling.md's Long lines bullet still says "after = or => last". The docs and the code disagree.

Fix: Update the Long lines bullet in docs/tooling.md.

Status: fixed: tooling.md's Long lines rule says a break after '=' or '=>' comes before an opening bracket's when all that follows fits, and that the next best break is tried

## layout-1 (low): A multi-line block comment that ends on an #If's line is kept twice, so fb fmt refuses a valid file

compiler/src/lexer.cpp:694. lex_directive starts an outermost #If's conditional span at line_start(begin), the start of the #If's physical line. When a block comment that began on an earlier line ends on that line, that start falls inside the comment. The comment begins before the span, so finish_trivia's erase_if (line 217) keeps it in trivia.comments. The formatter then writes the whole comment and afterwards copies the conditional from the middle of the comment ('y */ #If X ...'), so its tail appears twice. The meaning check catches this and refuses the file with the 'which is a bug in it' error. The lexer accepts '#If' after a comment because at_line_start only looks at tokens, and fb run builds the file. The one-line form '/* x */ #If X' works, because there the comment starts at the line start and is erased.

Shown by: review-m8s6/layout/p1.fb:
Import std.core
Local a = 1
/* x
y */ #If X
Local b = 2
#End If
Print(a)

fb.exe run p1o.fb (the same text) prints 1 (with only the 'X is never defined' warning). fb.exe fmt p1.fb prints: p1.fb:1:1: error: fb fmt would change what this file means, which is a bug in it; the file is left as it is, and exits 1.

Expected: fb fmt formats the file, either keeping the comment and the #If lines exactly as written or treating the comment as part of the #If lines, with no duplicated text.

Actual: fb fmt refuses a file that builds, and says the refusal is a bug in fb fmt.

Skeptic: I rebuilt p1.fb ('/* x\ny */ #If X ... #End If'). fb run prints 1. fb fmt prints 'fmt_p1.fb:1:1: error: fb fmt would change what this file means, which is a bug in it; the file is left as it is' and exits 1. The one-line form '/* x */ #If X' (p1b.fb) formats with exit 0. lexer.cpp:694 sets m_conditional_begin = line_start(begin), which for this input falls inside the multi-line comment, as the finding says. The file is left untouched, and the input is unusual (spec 13 says a directive must be the first thing on its line), so I rate it low. It is still a false 'bug in it' refusal of a file that builds.

Fix: When an outermost #If's line starts inside a recorded comment (comment.begin < line_start < comment.end), start the conditional at that comment's begin, or at the start of its line, so finish_trivia drops the comment. Add a lexer test for '/* x\ny */ #If X'.

Status: fixed: an outermost #If's lines start at a block comment its line starts inside (lexer test, and format test 'block comment that ends on an #If's line')

## layout-4 (low): An #If whose branches hold part of a statement makes fb fmt report a parse error the program doesn't have

compiler/src/lexer.cpp:694. lex_layout gives no #If branch any tokens, so parse_layout sees the code with every branch removed. Spec 13 says code in an inactive branch 'does not need to parse', and the compiler accepts branches that each hold part of a statement, for example two alternative If headers sharing one body and End If. fb fmt then reports 'unexpected End If' at a line that is fine in every build, and refuses the file. tooling.md says only that a file that doesn't parse is left as it is with its errors reported, and that #If lines are left as written. It says nothing about #If regions having to hold whole statements, so the error misleads the user.

Shown by: p3.fb:
Import std.core
Local a = True
#If DEBUG
If a
#Else
If !a
#End If
    Print("x")
End If

fb.exe run p3o.fb (the same text) prints x. fb.exe fmt p3.fb prints: p3.fb:9:1: error: unexpected 'End If' ... error: couldn't format 1 file; exit 1.

Expected: Either fb fmt formats the file, or it refuses with an error that names the cause (an #If region that splits a statement, which fb fmt can't format), and tooling.md documents the limit.

Actual: fb fmt reports a parse error at a line that builds in every configuration.

Skeptic: I rebuilt p3.fb, where the two #If branches hold alternative If headers that share one body and End If. fb run prints x. fb fmt prints 'fmt_p3.fb:9:1: error: unexpected 'End If'' then 'couldn't format 1 file', exit 1. A control (p3b.fb), whose #If region holds whole statements, formats with exit 0. Spec 13:388 says inactive code 'does not need to parse'. tooling.md says only files that don't parse are refused, and that #If lines are left as written. This file parses in every configuration, so fb fmt reports a parse error the program doesn't have. The file is left untouched and the case is rare, so I rate it low.

Fix: When parse_layout fails on a file with #If regions, report that fb fmt needs each #If region to hold whole statements, pointing at the region, instead of the raw parse error. Document the limit in tooling.md's #If rule. Add a test.

Status: fixed with format-2 (format test with the two If headers)

## layout-5 (low): layout_t::breaks' comment lists fewer break points than the parser records

compiler/include/fb/parser.hpp:50. The comment says breaks holds where 'an operator, '=', ',', or "=>"' end. skip_continuation_newlines also records the ends of a range's To and Until and of its Step (parse_range), and of a constraint's '&' (parse_constraint). These are breaks the parser accepts: 'Local r = 1 To' followed by '3' on the next line parses, and fb fmt aligns it as a continuation.

Shown by: q17.fb: 'Local r = 1 To\n    3' formats to 'Local r = 1 To\n          3' (a continuation). parser.cpp's parse_range calls skip_continuation_newlines after To/Until and after Step, and parse_constraint does after '&'.

Expected: The comment names every place a break may follow: operators (including a constraint's '&'), '=', ',', '=>', and a range's To, Until, and Step.

Actual: The comment omits To, Until, and Step, so a reader would think breaks only holds operators and punctuation.

Skeptic: parser.hpp:48-49 says breaks holds the ends of 'an operator, '=', ',', or "=>"'. skip_continuation_newlines (parser.cpp:349) adds m_previous_end to breaks, and it is called after To/Until and after Step in parse_range (parser.cpp:3488, 3494), and after '&' in parse_constraint (2265). Spec 04:114 treats To/Until as separate from the binary operators. My first q17 used 'Next' and was invalid. I fixed it to 'End For': 'Local r = 1 To\n    3' runs and prints 1 2 3, and fb fmt aligns it as a continuation ('          3'). So the comment is incomplete: it is a doc-comment inaccuracy.

Fix: Reword the comment, e.g. 'Where the tokens that a line break may follow outside brackets end: an operator, '=', ',', "=>", or a range's To, Until, or Step.'

Status: fixed: the comment names a constraint's '&' and a range's To, Until, and Step (parser test 'parse_layout's breaks', which checks what the comment says; a comment has no failing test of its own)
