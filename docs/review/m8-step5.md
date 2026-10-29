# The review of M8 step 5 (the VS Code extension)

23 confirmed findings, each with its Status.

## client-1 (medium): Two server starts that overlap leave an orphaned language server running

editors/vscode/src/client.ts:60. ServerClient.start() clears m_client in stop() and only sets it again after `await this.stop()`, with no serialization. A second start() that begins while the first is still in its stop() (same tick, or during the up to 2 s client.dispose of a running server: the Restart button clicked twice, the Settings UI writing futureblitz.path as you type, or the onDidChangeConfiguration start racing an explicit one, as the extension test 'missing' does) sees m_client undefined, makes its own LanguageClient, and then the first one overwrites m_client. The overwritten client keeps running with nothing holding it. Restart and stop never dispose of it, so it lives as long as the extension host, and two servers serve the same documents.

Shown by: In a scratch copy of editors/vscode, I added test/fixtures/probe-lsp.js, which logs 'start <pid>' and 'exit <pid>' to a file and then requires fake-lsp.js. I also added a suite test that calls `api.server.start({command: node, args: [probe, 'x', log]})` twice without awaiting, then `await Promise.all`, then start() once more, then stop(). I ran it with FB_VSCODE_EXECUTABLE set to the installed VS Code 1.143.1 and `node out/test/run.js`. The log showed: start 31140, start 36684, exit 31140 0, start 26252, exit 26252 0. The test printed 'PROBE twice running: started=2 live=[31140,36684]', then after the restart 'live=[36684,26252]', and after stop() 'live=[36684]'.

Expected: Only one server process at a time. A later start() supersedes and disposes of any client an earlier start() made, and stop() leaves no server running.

Actual: Two fake servers run at once. One of them (pid 36684) survives a restart and stop() and is never disposed of.

Skeptic: Read src/client.ts. start() runs `await this.stop()` first, and stop() sets m_client to undefined before its dispose(2000). So a second start() that begins during the first one's stop sees no client and makes its own LanguageClient. The two then each assign m_client in turn, and the client overwritten first is never disposed. Nothing serializes this. The overlap happens in practice: extension.ts:88 restarts on every change to futureblitz.path or lspPath (each keystroke in the Settings UI), extension.ts:93 starts on activation, and Restart (extension.ts:68 and the status item) can be clicked again while the first dispose is still waiting up to 2 s. The code alone confirms the reviewer's VS Code probe, which showed a survivor pid after restart and stop. I didn't rerun it in VS Code.

Fix: Serialize start/stop. Chain each start onto a promise of the previous one, or keep a generation counter and dispose of a client whose start finished after a newer start() began. Add an extension test that runs two starts at once and checks that only one server process stays alive.

Status: fixed: each start and stop takes a generation, and one that's superseded disposes of its client and changes nothing; tested by three starts at once leaving one stand-in server and a stop leaving none

## client-2 (medium): $fb-runtime misses an uncaught error that has no message

editors/vscode/package.json:260. The runtime prints an uncaught error with an empty Message as 'uncaught <Class> at <file>:<line>:<col>', with no ': ' after the class name (runtime.cpp fb_error_uncaught writes ': ' only when the message isn't empty). The matcher's 'uncaught [^:]+: .*' requires ': ', so this line, which is the common one for a custom Error class thrown as `Throw Oops()`, never reaches Problems. On Windows an absolute path doesn't help either: 'C:/' has ':' followed by '/'. tooling.md and the unit test cover only 'uncaught Type: message at'.

Shown by: C:/.../scratchpad/review-m8s5/client/w/u/uncaught.fb, containing `Import std.core / Class Oops Extends Error / End Class / Throw Oops()`. Running `fb run --color never uncaught.fb` prints 'uncaught Oops at uncaught.fb:6:1'. Running that line through package.json's three regexps in node (match.js) gives NO MATCH. In VS Code 1.143.1, a probe ran `api.tasks.make({command:'run', target:'uncaught.fb'}, files)` through runTask and then called getDiagnostics(uncaught.fb). The probe printed 'PROBE uncaught: exit=1 diags=[]'.

Expected: An error at uncaught.fb line 6, column 1, in Problems.

Actual: No problem is reported. A message-less uncaught error is invisible to the matcher.

Skeptic: I built scratchpad/review-m8s5/refute-client/uncaught.fb (Class Oops Extends Error; Throw Oops()). `fb run --color never uncaught.fb` prints 'uncaught Oops at uncaught.fb:6:1'. match.js runs every regexp in package.json's problemMatchers over that output and matches nothing, because $fb-runtime's `uncaught [^:]+: .*` needs a ': ' after the class name. A message-less custom Error, the common case, never reaches Problems.

Fix: Make the ': message' part optional, for example '^((?:panic: .*|uncaught [^:\s]+(?:: .*)?|expect failed: .*)) at (.+?):(\d+):(\d+)$'. Add 'uncaught Oops at main.fb:6:1' to problems.test.ts and run a real message-less Throw through fb.

Status: fixed: $fb-runtime's `uncaught Type` takes its `: message` or none; tested on the line and on fb run's message-less Throw

## client-3 (medium): fb test's Fail() and thrown-error failures don't reach Problems

editors/vscode/package.json:260. fb test reports a test that called Fail(message) as 'failed: <message> at <file>:<line>:<col>', and a test that let an error out as 'threw <Class>[: message] at <file>:<line>:<col>' (runtime/src/test.cpp fb_test_threw, fb_test_threw_other). $fb-runtime matches only 'panic', 'uncaught', and 'expect failed', so these test failures, both with a location, get no problem. A Run Tests task can fail with nothing in Problems.

Shown by: A scratch package, w/t, with a @Test that calls Expect(Add(1, 2) == 4) and then Fail("by hand"), and w/u/throws.fb, whose @Test does `Try Boom()` where Boom throws Oops. `fb test --color never t` prints 'failed: by hand at t/src/math.fb:10:5'. `fb test --color never throws.fb` prints 'threw Oops at throws.fb:8:5'. Both lines give NO MATCH against package.json's regexps in node. In VS Code 1.143.1, a probe ran a futureblitz 'test' task on files/pkg, whose tests are Fail("by hand") and an out-of-range index. getDiagnostics(pkg/src/math.fb) returned only the panic: [[0,'panic: index 3 is out of range for 1 element',11,10]].

Expected: Each failed test's 'failed: ... at' and 'threw ... at' line is a problem at its place, as 'expect failed' is.

Actual: Only panics and failed Expects show. Fail() and thrown errors don't.

Skeptic: scratchpad/review-m8s5/refute-client/throws.fb has three @Tests: one lets an Oops out through Try Boom() with Throws Oops, one calls Fail("by hand"), one calls Expect(1 == 2). `fb test --color never throws.fb` prints 'threw Oops at throws.fb:8:5', 'failed: by hand at throws.fb:18:5', and 'expect failed: 1 == 2 at throws.fb:23:5'. match.js matches only the expect-failed line. The other two failures have locations but get no problem. tooling.md lists only panic, uncaught, and expect failed, but it promises Problems for failed Expects, so leaving out Fail() and thrown errors, which fb test reports in the same format, is an inconsistent gap in 'runs tests'.

Fix: Add 'failed' and 'threw [^ ]+(?:: .*)?' to the $fb-runtime alternation, or add a pattern for each. Cover both in problems.test.ts against real fb test output, and update tooling.md's list.

Status: fixed: $fb-runtime takes `failed[: message] at` (Fail, with a message or without) and `threw Type[: message] at`; tested on the lines and on fb test's output, and tooling.md lists them

## debug-1 (medium): With no folder open, F5 or FutureBlitz: Debug on a lone .fb file hangs and never builds

editors/vscode/src/debug.ts:120. When a lone .fb file is open in a window with no workspace folder, resolveDebugConfigurationWithSubstitutedVariables passes folder undefined to tasks.make. That makes a TaskScope.Workspace ProcessExecution with no cwd, and VS Code defaults that cwd to ${workspaceFolder}, which it can't resolve. The build task never starts, runTask never settles, and the debug start hangs with no FutureBlitz message. If the build did run, the program's cwd would fall back to process.cwd(), which is VS Code's install folder (C:\Users\Hunter\AppData\Local\Programs\Microsoft VS Code). README and tooling.md both say F5 debugs 'the file'.

Shown by: Probe: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/debug/probe/run.js and suite.js (mode 'lone'). Run as `node run.js <scratch copy>/repo/editors/vscode lone`, which uses the installed VS Code 1.143 with no folder, the CodeLLDB stand-in, and FUTUREBLITZ_FB set to cmake-build-debug/cli/fb.exe. It opens probe/ws/lone/hello.fb, then: workbench.action.debug.start; the futureblitz.debug command; api.tasks.make({command:'build', target:file, args:['-o',...]}, undefined) through runTask; and debug.resolveDebugConfigurationWithSubstitutedVariables(undefined, cfg).

Expected: The file builds beside itself and CodeLLDB launches hello.exe, with cwd set to the file's folder. If a folder really is needed, FutureBlitz says so instead of hanging.

Actual: Every path logs 'Variable workspaceFolder can not be resolved. Please open a folder.' runTask and the resolver are still pending after 30 s (race timed out), and futureblitz.debug had not returned more than 200 s later. No launch is recorded and hello.exe is never written. The extension host's process.cwd() is C:\Users\Hunter\AppData\Local\Programs\Microsoft VS Code.

Skeptic: I copied editors/vscode at a222429b to refute-debug/ext, then ran npm ci and compile. My own suite, refute-debug/probe/lone.js, ran in VS Code 1.143 with no folder open, the CodeLLDB stand-in, and futureblitz.path set to cmake-build-debug's fb.exe. workspaceFolders was undefined. I opened lone/hello.fb and ran workbench.action.debug.start. VS Code logged 'Variable workspaceFolder can not be resolved. Please open a folder.' After 60 s there were no launches and no task had started (onDidStartTask never fired). The command was still pending and hello.exe didn't exist. Running futureblitz.debug gave the same error, and after 30 s it had 0 launches and was still pending. process.cwd() was C:\Users\Hunter\AppData\Local\Programs\Microsoft VS Code. The code plainly means to support no folder: debug.ts:120 falls back to process.cwd() and :144 passes an absolute target when folder is undefined. But tasks.ts:57 gives the ProcessExecution cwd folder?.uri.fsPath, which is undefined here, and runTask has no failure path, so it hangs with no message. That breaks tooling.md's 'F5 ... debugs the open file's package, or the file'.

Fix: In tasks.make, give the ProcessExecution an explicit cwd: folder?.uri.fsPath ?? path.dirname(target), or the package root. In debug.ts, set base to the program's folder (or its package root) when there's no workspace folder, not process.cwd(). Make runTask reject or time out if executeTask fails, and add an extension test that runs without a folder.

Status: fixed: a task with no folder runs in its target's folder, the problem matchers take a relative path from the task's folder (`${cwd}`, not `${workspaceFolder}`, which was what VS Code couldn't resolve), the launch runs in the program's folder, and runTask gives up on a task that doesn't start in 30 seconds, which the debugger says; tested by a second extension test session with no folder open: F5's configuration, a debug launch, FutureBlitz: Debug, and a lone file's errors in Problems

## debug-3 (medium): In a multi-root workspace, F5 on a file in another folder runs it with the first folder as cwd

editors/vscode/src/debug.ts:120. base, which becomes the launch's default cwd, is the folder VS Code passes in. With no launch.json in a multi-root workspace, that's the first folder, not the open file's. defaultProgram already finds the file's own folder (owner), but resolveDebugConfigurationWithSubstitutedVariables doesn't use it, so the program runs in another workspace folder. Relative file loads, such as assets beside the program, then break. package.json documents cwd as 'Without one, the workspace folder.'

Shown by: Probe mode 'mr' opens a copy of test/fixtures/workspace/fixtures.code-workspace (folders app and files) in VS Code 1.143, opens each file, and runs workbench.action.debug.start.

Expected: F5 on files/hello.fb launches files\hello.exe with cwd ...\mr\files, the program's own workspace folder.

Actual: F5 on app/main.fb gives program ...\mr\app\build\app.exe with cwd ...\mr\app, which is right. F5 on files/hello.fb gives program ...\mr\files\hello.exe with cwd ...\mr\app.

Skeptic: Mode mr of refute-debug/probe/multi.js opened a fresh copy of test/fixtures/workspace/fixtures.code-workspace (folders app and files). F5 on app/main.fb launched app\build\app.exe with cwd ...\mr\app, which is right. F5 on files/hello.fb launched files\hello.exe with cwd ...\mr\app, the other folder's. The futureblitz.debug command on the same file gave cwd ...\mr\files, so F5 and the command disagree. debug.ts:120 uses the folder VS Code passes as base and cwd, and doesn't use the file's own folder, which defaultProgram already finds. package.json's cwd says 'Without one, the workspace folder', but the program runs in a folder it isn't in, so relative asset loads break.

Fix: Take the folder from the program itself: vscode.workspace.getWorkspaceFolder(vscode.Uri.file(program)) ?? folder, with the program's folder as the last fallback. Use it for both the task (scope, cwd, relativeTarget) and the launch's default cwd. Add an extension test that debugs files/hello.fb in the two-folder fixture.

Status: fixed: the build and the launch's cwd take the program's own workspace folder, else the one passed; tested by debugging files/hello.fb with app's folder passed

## grammar-1 (medium): A comparison written without spaces opens type arguments that color the rest of the file

editors/vscode/syntaxes/futureblitz.tmLanguage.json:984. type-arguments starts at any `<` right after a word character when a `>` comes later on the line, and its lookahead allows `&`, `(`, `)` and spaces, so `x<0 && (y>max)` counts as type arguments. Inside them, #type opens a tuple at `(`, and #type has no pattern for `>`, so the `>` inside the parentheses is skipped. The tuple closes at `)`, and after that type-arguments waits for a `>` that never comes, because its end has no `$`. From there to the end of the file, Return, End Function, Print and every string are colored as entity.name.type. The repo sweep doesn't catch this because fb fmt puts spaces around `<`, but code looks like this while it's being typed, and format on save can't fix it until the step-4 server exists. The design's rule that a single-line construct left open must not color the rest of the file applies here too. Type arguments can't span lines (`<` isn't a bracket that continues a statement), so ending them at the line end is safe.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/run.fb:
Import std.core
Function Check(x: Int, y: Int, max: Int): Bool
    If x<0 && (y>max) Then Return True
    Return False
End Function
Print("{Check(-1, 5, 3)}")

fb.exe run probe/run.fb prints True, so the program is valid. In a scratch copy of editors/vscode (C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/repo/editors/vscode): node scripts/tokens.mjs ../../../probe/run.fb

Expected: `<` and `>` are keyword.operator.comparison, and `Return False`, `End Function` and the Print line keep their normal scopes.

Actual: `x` is entity.name.type and `<` is punctuation.definition.typeparameters.begin. Every later token in the file, including `Return`, `False`, `End`, `Function`, `Print` and the string `"{`, sits inside meta.type.parameters as entity.name.type.

Skeptic: Rebuilt probe p/run.fb in my scratch (review-m8s5/verify-grammar); fb run prints True. node scripts/tokens.mjs on a copy of editors/vscode: `x` is entity.name.type, `<` is punctuation.definition.typeparameters.begin, `&&` keyword.operator.type, `(y>max)` a meta.type.tuple with `>` unscoped, and every later token (Then, Return, False, End, Function, Print, the string) stays inside meta.type.parameters as entity.name.type up to the end of the file. type-arguments (line 984) has end `>` only, so it never closes at the line end. One open construct coloring the rest of the file goes against the design's rule for single-line constructs and its test that leaves nothing open.

Fix: End type-arguments at `>|$` (it also needs no `(?=...)` around `$`). Also keep `&&`/`||` out of the lookahead in type-arguments and generic-name, or require that the `>` isn't inside parentheses. Add an assertion line like `If x<0 && (y>max) Then` followed by a line that must stay `- meta.type.parameters`.

Status: fixed: type arguments end at their `>` or their line, and `&&` can't be in them, in type-arguments and generic-name; tested by contexts.fb's `x<0 && (y>max)` and `x<(y>max)` and the lines after them

## grammar-2 (medium): A default value containing parentheses ends the parameter list early

editors/vscode/syntaxes/futureblitz.tmLanguage.json:518. parameters is `\(` ... `\)`. Inside it, parameter-list's #expression consumes a nested `(` as plain punctuation instead of opening a nested group, so the first `)` after a call or parenthesized default closes meta.parameters. Every parameter after that default loses its scopes: its name gets no scope, its `:` becomes keyword.operator.ternary, and its type gets no scope. The spec allows defaults that are compile-time calls and parenthesized expressions (05-functions.md: 'Default values must be known at compile time'), and fb accepts them. The lambda rule has the same flaw, since it also ends at the first `\)`.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/defaults.fb:
Import std.core
Const Function Twice(x: Int): Int => x * 2
Function Place(scale: Int = Twice(3), offset: Int = (1 + 2) * 4): Int
    Return scale + offset
End Function
Print("{Place()}")

fb.exe run prints 18. node scripts/tokens.mjs probe/defaults.fb

Expected: `offset` is variable.parameter, its `:` is punctuation.separator.type, and `Int` is storage.type.primitive inside meta.parameters, as `scale` is.

Actual: `Twice(3`'s `)` gets punctuation.definition.parameters.end. After that, `offset` has only meta.function, `:` is keyword.operator.ternary, and `Int ` has no scope. For `(1 + 2)`, the `(` opens a second meta.parameters, the function's own closing `)` is plain punctuation.parenthesis, and only the return type comes out right.

Skeptic: p/defaults.fb runs and prints 18. Tokens: the `)` of `Twice(3` gets punctuation.definition.parameters.end. After it, `offset` has only meta.function, its `:` is keyword.operator.ternary, and ` Int ` has no scope. `(1 + 2)` opens a second meta.parameters, and the function's real closing `)` is plain punctuation.parenthesis. The parameters rule is `\(`..`\)` and has no nested group. The spec allows compile-time defaults, and the design says parameters and types are scoped.

Fix: In parameter-list (or in #expression for these contexts), add a begin/end group for `(`...`)` that includes #expression, like interpolation-groups, so nested parentheses balance before parameters' `\)` can end.

Status: fixed: parentheses and brackets balance in a parameter list (a new groups rule), so a default's call or parenthesized expression doesn't end it, in a lambda too; tested by contexts.fb's Place and pick

## grammar-3 (medium): A Method named Set called at a line's start is scoped as a Property accessor and indents the next line

editors/vscode/language-configuration.json:84. increaseIndentPattern has `Set\b(?!.*=>)` with no Property context, so any line starting with `Set` indents the next one. The grammar's accessor rule `^\s*(Set)\s*(\()\s*([A-Za-z_]\w*)\s*(\))` likewise colors a one-argument call as an accessor declaration with a parameter. Calling your own Set Method from inside a Class is ordinary code: std/io/preferences.fb lines 127, 133 and 138 do `Set(key, "{value}")`, and std/collections/pool.fb and immutable.fb declare `Method Set(...)`. The spec makes Set a keyword only 'at the start of a line inside Property ... End Property'.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/setcall.fb (a Class Box with Method Set(value: Int), and a Method Reset that runs Set(0), Set(V), Print("{V}")). fb.exe run prints 0. node scripts/tokens.mjs probe/setcall.fb; then in node: new RegExp(require('./language-configuration.json').indentationRules.increaseIndentPattern).test('        Set(key, "{value}")')

Expected: `Set(V)` is entity.name.function.call with its argument as an ordinary variable, as `Set(0)` is, and pressing Enter after `Set(key, "{value}")` keeps the indentation.

Actual: `Set(V)` is scoped storage.type.function.accessor, punctuation.definition.parameters, and variable.parameter. The increase pattern returns true for `Set(0)`, `Set(V)`, and `Set(key, "{value}")`, so the next line indents one level too deep.

Skeptic: p/setcall.fb (I changed `New Box()` to `Box()`, since fb rejects New there) prints 0. Tokens: `Set(0)` is entity.name.function.call, but `Set(V)` is storage.type.function.accessor with `V` as variable.parameter. In node, increaseIndentPattern returns true for `        Set(key, "{value}")`, `Set(0)`, and `Set(V)`. std/io/preferences.fb lines 127, 133, and 138 have exactly `Set(key, "{value}")`. Spec 01 line 170 and the design make Set a keyword only at a Property's lines.

Fix: Have the indent rule match only an accessor's shape: `Set\s*\(\s*\w+\s*\)\s*(?://.*)?$` with no other text. Keep the grammar's single-identifier accessor match, but don't let one call argument count as a parameter, for example by requiring end of line or `//` after the `)`. Add `Set(key, value)` and `Set(x)` calls to the 'a line that's whole doesn't' test.

Status: fixed: Set(value) is an accessor only inside a Property, a region that goes to its End Property or the next member, and the indent rule takes only an accessor's shape, `Set(name)` alone on its line; tested by contexts.fb's Box (Set(0), Set(V), a Property's Set, and a call after a Property with no body) and by `Set(key, "{value}")` and `Set(a, b)` not indenting. A one-line rule can't tell `Set(x)` alone on a line from an accessor, so that call still indents the next line, which tooling.md says

## grammar-4 (medium): Step is colored as a keyword wherever it's used as an identifier

editors/vscode/syntaxes/futureblitz.tmLanguage.json:1057. Spec 01's contextual table says Step is a keyword only 'after the bound in a For header, or a range's end', and the design repeats 'Step after a bound'. The grammar instead marks every Step as keyword.control.loop unless `( < . : =` or the end of the line follows it. So a variable named Step, valid because Step isn't reserved, is a loop keyword in `Step * 2`, `Count(Step, 1)`, `Step += 1`, `"{Step}"`, and so on.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/step.fb:
Import std.core
Function Count(a: Int, b: Int): Int => a + b
Local Step = 3
Local total = Step * 2 + Count(Step, 1)
Print("{total}")

fb.exe run prints 10. node scripts/tokens.mjs probe/step.fb

Expected: Both uses of Step on the `Local total` line are ordinary identifiers, as the declaration's Step is (variable.other.readwrite).

Actual: Both are keyword.control.loop.futureblitz.

Skeptic: p/step.fb prints 10. Tokens: both Steps in `Local total = Step * 2 + Count(Step, 1)` are keyword.control.loop. The rule at line 1057 is `(?<![.\w])Step\b(?!\s*[(<.:=]|\s*$)`, a general match. Spec 01 line 171 says 'after the bound in a For header, or a range's end', and tooling.md says 'Step after a bound'.

Fix: Match Step only after a bound: a lookbehind is impractical, so use a pattern like `\b(To|Until)\b[^\n]*?\s(Step)\b` in keywords, or a For-header/range rule, and drop the general Step match. Add an assertion line with Step as a variable.

Status: fixed: Step is a keyword only in a range or For header's region from its To or Until, with brackets balanced, and the general match is gone; tested by contexts.fb's Step as a Local, in a For header after an index, in a range, and a Case's ranges

## client-4 (low): Problem columns are off after characters outside the BMP: fb counts code points, VS Code UTF-16 units

editors/vscode/package.json:222. diagnostic_render.cpp prints columns in code points (column_unit_t::CODEPOINT). A VS Code problem matcher's column is a UTF-16 character offset. On a line with an emoji or another non-BMP character before the error, the problem lands one column left per such character. The squiggle and the Problems entry then point at the wrong token.

Shown by: files/emoji.fb, containing `Import std.core` and then `Local s = "😀😀" + y` (two U+1F600). `fb build --color never emoji.fb` prints 'emoji.fb:2:18: error: 'y' isn't declared'. node gives y's UTF-16 column as 20 and its code-point column as 18. In VS Code 1.143.1, a probe ran a futureblitz 'build' task on emoji.fb. The diagnostic was at line 1, character 17, and the text there is '+'; 'y' is at character 19.

Expected: The error underlines 'y'.

Actual: It underlines '+', two characters left of 'y'.

Skeptic: scratchpad/review-m8s5/refute-client/emoji.fb has `Local s = "😀😀" + y`. `fb build --color never emoji.fb` prints 'emoji.fb:2:18: error: 'y' isn't declared'. In node, y's UTF-16 column is 20 and its code-point column is 18. diagnostic_render.cpp:50 uses column_unit_t::CODEPOINT, while VS Code document positions, and so problem-matcher columns, count UTF-16 units. That puts the problem 2 characters left, on '+'. source.hpp already has a UTF16 unit for the LSP. I rate it low because it only affects lines with characters outside the BMP before the error. I didn't rerun the VS Code probe myself.

Fix: A problem matcher can't convert units. One option is for fb to print UTF-16 columns when asked: an option or environment variable the extension's tasks set, which the LSP will need anyway. The other is to document the limitation. Add a non-BMP case to the extension tests.

Status: fixed: FB_COLUMNS=utf-16 makes fb count its diagnostics' columns in UTF-16 code units (render_options_t's columns, which cli/src/terminal.cpp's diagnostic_columns reads), code points without it, and the extension's tasks set it; tested by a render test, a cli test of an error after two U+1F600s in both units, the problem-matcher test over fb build's output in both, and an extension test whose problem is at y's column. A running program's places, which the compiler writes in code points, are as they were, which tooling.md says

## client-5 (low): A server that never answers initialize leaves the client 'starting' forever, with no message

editors/vscode/src/client.ts:106. start() awaits client.start() with no timeout. vscode-languageclient waits for the initialize response indefinitely, so a server that hangs (deadlocks, or waits on something) leaves m_state 'starting'. The status item says 'Language server starting' with no command, and no warning or Restart prompt appears. tooling.md promises that a server that fails to start 'is said once, with Restart and Show Output'. A hung one is never said. The tests cover only a server that exits or crashes.

Shown by: test/fixtures/hang-lsp.js reads stdin and never replies. A probe ran `void api.server.start({command: node, args: [hangServer]})`, waited 5 s, and printed 'PROBE hang: stateAfter5s=starting'. Restart does recover it: a following start() reached 'running' and the hung process was killed.

Expected: After a bounded wait, for example 10 to 30 s, the client reports the server failed to start, with Show Output and Restart, as it does for one that exits.

Actual: The status stays 'Language server starting' indefinitely, with no message and no Restart on the status item.

Skeptic: vscode-languageclient 11.0.0's client.js initialize() has no timeout; its only timeouts are in stop and shutdown. client.ts:106 awaits client.start() with no race, so a server that never answers initialize stays 'starting'. In that state show() sets status.command to undefined, so the status item has no Restart, and no warning ever appears. tooling.md says a server that fails to start is reported once, with Restart and Show Output, and a hung server is a failure to start that's never reported. I rate it low because Restart from the command palette still recovers, and a deadlocked initialize is an edge case.

Fix: Race client.start() against a timeout. On expiry, dispose of the client and call reportFailed(command, 'it didn't answer in N seconds'). Add a hanging stand-in server to the extension tests.

Status: fixed: a start that initialize doesn't answer in 20 seconds disposes of its client, which ends the process, and says so with Restart and Show Output, and the starting status has Restart; tested with a stand-in that never answers, given 1 second, whose process goes

## client-6 (low): Without fb, the task's message says to set futureblitz.path even when it's set to a missing file

editors/vscode/src/tasks.ts:108. findFb reports missingSetting when futureblitz.path names a file that isn't there, but locateFb keeps only .path, and missingFb() always prints 'set futureblitz.path to it, put it on PATH, or install it'. A user whose setting has a typo, or a Windows path without .exe, is told to set the setting they already set, and isn't told which path was tried. The language server's message does name a bad lspPath. The debug provider's message has the same problem.

Shown by: Code reading, plus the existing extension test 'without fb, a task says how to get it', which sets futureblitz.path to files/no_such_fb. The task's terminal text is the fixed string at tasks.ts:108, which doesn't mention no_such_fb.

Expected: Something like 'futureblitz.path is <path>, which isn't there' when the setting names a missing file.

Actual: The generic install advice, with no mention of the bad setting.

Skeptic: paths.ts fromSetting returns missingSetting for a futureblitz.path that names a missing file. But extension.ts:37-38 locateFb keeps only .path, and tasks.ts missingFb() always writes the fixed text 'set futureblitz.path to it, put it on PATH, or install it'. debug.ts:113-116 likewise says 'set futureblitz.path'. A user whose setting points at a file that isn't there is told to set it, without the tried path, while client.ts reportMissing does name a bad lspPath ('futureblitz.lspPath is X, which isn't there'). This is a small inconsistency in the messages.

Fix: Pass findFb's Found, or its missingSetting, through the locator to missingFb() and to the debug provider's message, and name the path when the setting is wrong.

Status: fixed: the locator passes on what findFb found, and the task's and the debugger's messages name a futureblitz.path that names nothing (missingFbMessage); tested by its unit test

## client-7 (low): The Operator snippet's defaults are invalid in a Struct or Class

editors/vscode/snippets/futureblitz.json:166. The snippet, described as 'An Operator on a Struct or Class', defaults to `Operator +(a: Self, b: Self): Self`. Per spec 07-generics and the compiler, Self is a type only in an Interface. In the Class or Struct the snippet is for, the defaults produce four errors. The snippet test only runs `fb fmt --check`, which parses and doesn't type-check, so it doesn't catch this.

Shown by: w/snip/s.fb, a Class Name with the snippet's defaults pasted in (`Operator +(a: Self, b: Self): Self / Return a / End Operator`). `fb check --color never s.fb` gives 'an Operator of Name needs a parameter of that type' and three times 'Self is a type only in an Interface, where it's the type implementing it'.

Expected: The snippet's defaults compile in a Class or Struct, for example with the enclosing type's name as a placeholder.

Actual: It errors until every Self is replaced.

Skeptic: I pasted the snippet's defaults into a Class in scratchpad/review-m8s5/refute-client/s.fb: `Operator +(a: Self, b: Self): Self / Return a / End Operator`. `fb check --color never s.fb` gives 4 errors: 'an Operator of Name needs a parameter of that type' and three times 'Self is a type only in an Interface...'. Spec 07 (line 209) shows that form only inside an Interface, but the snippet's description says 'An Operator on a Struct or Class', where its defaults don't compile.

Fix: Use one linked placeholder for the type, `Operator ${1:+}(a: ${2:Name}, b: $2): $2`, or another default that names a real type. Consider running `fb check` on the snippets placed in a Class, rather than only fmt.

Status: fixed: the Operator snippet's types are one linked placeholder, Name, and its defaults compile in a Struct Name; tested by fb check on them, and the snippet test's expansion fills a repeated tab stop

## debug-2 (low): A package's file debugged as a file is built beside its source, not where fb puts it

editors/vscode/src/debug.ts:139. When program is a .fb file, name is undefined and debugExecutable puts the executable beside the source, even when the file is in a package. Without -o, fb puts a package's main file at <root>/build/<name> and another file at <root>/build/<stem>. provideDebugConfigurations writes program '${file}' for any workspace folder whose root has no project.toml, which is the usual layout for a repo with packages in subfolders, and the 'Debug the open file' snippet does the same. Debugging pkg/main.fb or pkg/src/tool.fb therefore drops main.exe/main.pdb and tool.exe/tool.pdb into the package's source folders, outside build/, where a package's .gitignore won't cover them. tooling.md says the build goes 'where fb puts it anyway (a package's build/<name>, a file's beside it)'.

Shown by: probe/ws/multi is a folder whose pkg/ subfolder has project.toml name="pkg", plus main.fb and src/tool.fb. In the probe (mode 'multi'), provideDebugConfigurations(folder) gives program '${file}'. startDebugging with program '${file}' launches ...\multi\pkg\main.exe on main.fb and ...\multi\pkg\src\tool.exe on tool.fb. For comparison, `fb.exe build pkg/src/tool.fb` and `fb.exe build pkg/main.fb` write pkg/build/tool.exe and pkg/build/pkg.exe.

Expected: A file inside a package is built where fb would put it: pkg/build/pkg.exe for the main file and pkg/build/tool.exe for src/tool.fb.

Actual: The build writes pkg/main.exe and pkg/main.pdb, and pkg/src/tool.exe and pkg/src/tool.pdb, beside the sources.

Skeptic: Without -o, fb.exe build pkg/src/tool.fb (run from the parent folder, and again from pkg/src) writes pkg/build/tool.exe and .pdb. fb.exe build pkg/main.fb writes pkg/build/pkg.exe. A lone file builds beside itself (lone/hello.exe). In VS Code, refute-debug/probe/multi.js mode multi showed provideDebugConfigurations giving program '${file}' for a folder whose package is in pkg/. startDebugging with '${file}' launched ...\multi\pkg\main.exe and ...\multi\pkg\src\tool.exe. Afterwards pkg/ held main.exe, main.pdb, src/tool.exe and src/tool.pdb beside the sources. That contradicts tooling.md's 'fb build -o where fb puts it anyway (a package's build/<name> ...)'. The cause is debugExecutable in fb.ts:111-124, which puts any file's build beside it. I rated it low: the program still builds and debugs correctly, and only where the output files land is wrong.

Fix: For a file program, run findPackageRoot on it. If there's a package, use <root>/build/<package name> when the file is the manifest's main (main.fb by default), else <root>/build/<file stem>, plus .exe on Windows, and pass that as -o. Add a unit test for debugExecutable on a package's file and an extension test for the ${file} case.

Status: fixed: debugExecutable takes the file's package, from findPackageRoot and projectOf (name, type, and main), and puts its main file at build/<name> and another at build/<file>, as driver.cpp names them; tested by its unit tests and by debugging app/main.fb, which launches app/build/app.exe and writes nothing beside it

## debug-4 (low): F5 in a library package says the build's errors are in Problems, but Problems is empty

editors/vscode/src/debug.ts:152. The debug provider builds any folder whose project.toml has a name, library packages included. fb refuses a library with a plain 'error: lib1 is a library, whose build goes in its build/ folder; '-o' names an executable' (exit 2). That line has no path:line:col, so $fb doesn't match it. The extension still says 'the build failed, so there's nothing to debug; its errors are in Problems.' Pressing F5 while editing a library's file (defaultProgram picks the library's root) hits this every time.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/debug/lib1 holds project.toml ([project] name="lib1", type="library") and f.fb with `Public Function F(): Int32`. `fb.exe build --color never -o build/lib1.exe .` prints the library error and exits 2. Probe mode 'lib' opens lib1 in VS Code, opens f.fb, and calls startDebugging(folder, {type:'futureblitz', request:'launch', name:'lib'}).

Expected: FutureBlitz says the package is a library, which has no program to debug, before building. At least the message shouldn't point to Problems when nothing is there.

Actual: startDebugging returns false with 0 launches, and vscode.languages.getDiagnostics() is empty, so the message sends the user to an empty Problems panel.

Skeptic: fb.exe build --color never -o build/lib1.exe . in lib1 (type = "library") prints 'error: lib1 is a library, whose build goes in its build/ folder; '-o' names an executable' and exits 2. That line has no path:line:col, so the $fb regexp '^(.+?):(\d+):(\d+): (error|warning)...' can't match it. In VS Code, mode lib's startDebugging(folder, {type:'futureblitz', request:'launch'}) on f.fb returned false with 0 launches, and getDiagnostics() was empty. With a non-zero code, debug.ts:151-154 always shows 'its errors are in Problems', so the message sends the user to an empty Problems panel. I rated it low: debugging a library is the user's mistake, nothing launches, the real error is in the task's terminal, and only the message is wrong.

Fix: Read the [project] type next to the name (as packageName does) and, for type = "library", show 'FutureBlitz: <name> is a library; it has no program to debug.' Also make the build-failed message say to look in the task's terminal, not only Problems.

Status: fixed: a package whose type is library says it has no program to debug before any build, and a failed build's message says Problems and the build's terminal; tested by debugging a library folder, which starts no task

## debug-5 (low): launch.json flags CodeLLDB settings the README says to add, including initCommands, which the code merges

editors/vscode/package.json:275. VS Code sets additionalProperties=false on each debugger's configurationAttributes (workbench getSchemaAttributes: o.additionalProperties=!1). The futureblitz launch schema lists only program, args, cwd, env, release, stopOnEntry and terminal. README.md:48-49 says 'CodeLLDB's own settings, like env or preRunCommands, go in the same configuration', and debug.ts:166 explicitly merges a user's initCommands. Users who follow the README get warnings on those lines.

Shown by: probe/ws/multi/.vscode/launch.json holds a futureblitz configuration with preRunCommands and initCommands. Probe mode 'schema' opens it in VS Code 1.143 and reads its diagnostics.

Expected: No warnings for the CodeLLDB properties the README tells users to use.

Actual: Line 10: 'Property preRunCommands is not allowed.' Line 11: 'Property initCommands is not allowed.'

Skeptic: Mode schema opened a launch.json whose futureblitz configuration has preRunCommands and initCommands. VS Code's diagnostics were: line 10 'Property preRunCommands is not allowed.' and line 11 'Property initCommands is not allowed.' package.json's configurationAttributes.launch lists only program, args, cwd, env, release, stopOnEntry and terminal. README.md:48-49 tells users 'CodeLLDB's own settings, like env or preRunCommands, go in the same configuration', and debug.ts:166 merges initCommands. They still work at run time, so the only harm is the editor's warnings.

Fix: Add the CodeLLDB pass-through properties to configurationAttributes.launch: initCommands, preRunCommands, postRunCommands, exitCommands, sourceMap, expressions, and the others the README points to. Otherwise, drop the README's claim.

Status: fixed: the launch schema lists CodeLLDB's initCommands, preRunCommands, postRunCommands, preTerminateCommands, exitCommands, envFile, stdio, sourceMap, and expressions; tested by every property the README's Debugging names being in it

## debug-6 (low): The README's design link is broken in the vsix and on the Marketplace

editors/vscode/README.md:5. vsce rewrites relative links against the repository URL and ignores repository.directory. The link ../../docs/tooling.md#the-vs-code-extension-step-5 therefore becomes https://github.com/hlpdev/FutureBlitz/blob/HEAD/../../docs/tooling.md#..., which resolves to /hlpdev/FutureBlitz/docs/tooling.md, outside blob/, and that 404s.

Shown by: In the scratch copy, `npx vsce package --no-dependencies --out nodeps.vsix`, then `unzip -p nodeps.vsix extension/readme.md | grep tooling`. The worktree's own futureblitz.vsix shows the same.

Expected: The packaged README links to https://github.com/hlpdev/FutureBlitz/blob/HEAD/docs/tooling.md#the-vs-code-extension-step-5.

Actual: [docs/tooling.md](https://github.com/hlpdev/FutureBlitz/blob/HEAD/../../docs/tooling.md#the-vs-code-extension-step-5)

Skeptic: I ran npx vsce package --no-dependencies --out refute-debug/nodeps.vsix. unzip -p nodeps.vsix extension/readme.md gives line 5: '[docs/tooling.md](https://github.com/hlpdev/FutureBlitz/blob/HEAD/../../docs/tooling.md#the-vs-code-extension-step-5)'. node's new URL(...) resolves that to https://github.com/hlpdev/FutureBlitz/docs/tooling.md#x, which is outside blob/HEAD and isn't a valid GitHub file URL. The source README.md:5 uses the relative ../../docs/tooling.md.

Fix: Use an absolute GitHub URL in README.md, or pass --baseContentUrl https://github.com/hlpdev/FutureBlitz/blob/HEAD/editors/vscode (and --baseImagesUrl) in the package script.

Status: fixed: the README's design link is absolute; tested by no README link leaving the extension's folder

## debug-7 (low): tooling.md's release layout has no lldb/, which the extension looks in for the script

docs/tooling.md:43. The extension, its package.json setting description, and tooling.md's VS Code section (line 145) look for a release's script at <root>/lldb/futureblitz.py, beside bin/. The toolchain section's release folder list ('bin/ ..., lib/<target>/ ..., and std/') doesn't include lldb/, so step 2 (releases), built from that list, has no reason to ship it. The lookup would then always fall back to the extension's bundled copy, which may not match the installed fb.

Shown by: `grep -n lldb docs/tooling.md` in the worktree: the only lldb/ paths are in the VS Code section (lines 144-147). Line 43's release folder list names bin/, lib/<target>/ and std/ only.

Expected: The release layout lists lldb/futureblitz.py, matching where the extension looks.

Actual: The release layout doesn't mention lldb/.

Skeptic: In a222429b's docs/tooling.md, line 44 lists a release's folder as `bin/` ..., `lib/<target>/` ..., and `std/` and nothing else. The VS Code section this change adds (lines 145-146) says the script is looked for as 'a release's lldb/futureblitz.py', and the extension's lldbScript lookup does the same. The Debug info section says only 'a Python script fb ships'. So the release layout that step 2 builds from doesn't include the folder the extension looks in. This is a doc inconsistency the change introduced, with no runtime effect today: the extension falls back to its own copy.

Fix: Add 'lldb/ (futureblitz.py, the debugger's script)' to the release folder line in 'The toolchain (steps 0 and 1)'.

Status: fixed: the release lane ships the script at <root>/lldb/futureblitz.py and adds it to the release layout in tooling.md; the VS Code section says releases ship it there. No test: it's the docs

## grammar-5 (low): Attribute declarations aren't recognized

editors/vscode/syntaxes/futureblitz.tmLanguage.json:412. Spec 13 has `Attribute Name(parameters)` declarations (`Attribute Range(Low: Int = 0, High: Int = 100)`), and std/core/load.fb declares four (Rename, Skip, Flatten, Renamed). The design says the grammar scopes 'declarations' names, parameters, and types', but no rule matches Attribute. The word itself gets no scope, the name is scoped as a function call, the parameters get no scope, every `:` is keyword.operator.ternary, and the types get no scope.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/attr.fb, line 1: `Public Attribute Range(Low: Int = 0, High: Int = 100)`. node scripts/tokens.mjs probe/attr.fb

Expected: `Attribute` is a declaration keyword, `Range` is entity.name.type (or entity.name.function.decorator), and `Low` and `High` are variable.parameter with typed annotations, as in a Function's signature.

Actual: `Attribute` has no scope, `Range` is entity.name.function.call, `Low` and `High` have no scope, `:` is keyword.operator.ternary, and `Int` has no scope.

Skeptic: Tokenized `Public Attribute Range(Low: Int = 0, High: Int = 100)` (spec 13 line 113; std/core/load.fb declares Rename, Skip, Flatten, and Renamed). `Attribute` has no scope, `Range` is entity.name.function.call, `Low` and `High` have no scope, `:` is keyword.operator.ternary, and `Int` has no scope. The design says declarations' names, parameters, and types are scoped.

Fix: Add a rule `^\s*(?:(Public|Private|Package)\s+)?(Attribute)\s+([A-Za-z_]\w*)` with `end: $` and #signature or #parameters as its patterns, plus an assertion line.

Status: fixed: `[Public|Private|Package] Attribute Name(parameters)` is a declaration with a signature; tested by contexts.fb's Range

## grammar-6 (low): A lambda's tuple or Func return type, and a generic Operator's type parameters, aren't scoped as types

editors/vscode/syntaxes/futureblitz.tmLanguage.json:608. The lambda rule's end regex captures a return type only when it's a dotted name with optional `<...>`, `?` and `[]`. A tuple return type, as in std/algo/grid.fb:154 `Function(cell: Vec2i): (Vec2i, Double)[]`, or a Func type falls out of the lambda: its `:` is keyword.operator.ternary and the types get no scope. Likewise, operator-declaration doesn't allow type parameters after the operator, though spec 06 shows `Operator *<Const L2: Int>(a: Quantity<L>, b: Quantity<L2>): Quantity<L + L2>`. There, type-arguments needs a word character before `<`, so `<` and `>` come out as comparisons, `Const` as storage.type, `L2` as a constant, and `:` as the ternary.

Shown by: probe/attr.fb lines 2 and 3 (`Local backward = Function(cell: Vec2i): (Vec2i, Double)[]` and the `Int[]` version for contrast), and probe/p1.fb's `Operator *<Const L2: Int>(...)` line inside a Struct. node scripts/tokens.mjs on each.

Expected: `: (Vec2i, Double)[]` is a return type (punctuation.separator.type, then entity.name.type inside meta.type.tuple). `<Const L2: Int>` is meta.type.parameters, as `Function Sum<T: Numeric>` is.

Actual: In the lambda, `:` is keyword.operator.ternary and `Vec2i` and `Double` have no scope, while `Int[]` comes out right. In the Operator, `<` and `>` are keyword.operator.comparison, `L2` is variable.other.constant, and `:` is keyword.operator.ternary.

Skeptic: Tokenized the line `Function(cell: Vec2i): (Vec2i, Double)[] =>` (the shape of std/algo/grid.fb:154). After `)` the `:` is keyword.operator.ternary, and `Vec2i` and `Double` have no scope. The `Int[]` version is scoped right. `Operator *<Const L2: Int>(...)` (spec 06 line 310) inside a Struct: `<` and `>` are keyword.operator.comparison, `Const` is storage.type, `L2` is variable.other.constant, `:` is ternary, and `Int` has no scope.

Fix: After the lambda's `)`, open a begin/end return-type region (like return-type) that includes #type, rather than capturing the type in the end regex. In operator-declaration, allow `(?:\s*(?=<))` and include a type-arguments variant without the `(?<=\w)` requirement right after the operator symbol.

Status: fixed: a lambda is a region of its parameters and a return-type region of any type, a tuple or a Func too, and an Operator's type parameters right after its symbol are type arguments; tested by contexts.fb's pick, check, and Quantity's Operator *

## grammar-7 (low): The `:` of named arguments, Map literals, and `For Method m: M` is scoped as the ternary operator

editors/vscode/syntaxes/futureblitz.tmLanguage.json:1241. Outside type-annotation contexts, every `:` falls through to operators' `\?|:` and becomes keyword.operator.ternary. That covers named arguments (`Money(cents: a.cents + b.cents)`, std/core/load.fb:93 `path: "")`), Map literals (`{"apple": 3}` in tests/programs/map_methods.fb:46), and typed For Method loops (`For Method m: M In T`, 14 times in std/net/http/routes.fb). A scan of std and tests/programs found 670 such colons on lines with no `?`.

Shown by: A scan script, C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/repo/editors/vscode/scan.mjs, run as `node scan.mjs ../../std ../../tests/programs` in the scratch copy, reports each `:` token scoped keyword.operator.ternary on a line without `?`. Count: 670.

Expected: A named argument's or Map entry's `:` is punctuation (for example punctuation.separator.key-value or punctuation.separator.argument), and `For Method m: M` gives `M` a type scope.

Actual: keyword.operator.ternary.futureblitz, so themes color these colons as operators.

Skeptic: Tokenized `Money(cents: a.cents + b.cents)`, `{"apple": 3}` (tests/programs/map_methods.fb:46), and `For Method m: M In T` (14 of these in std/net/http/routes.fb). Each `:` is keyword.operator.ternary.futureblitz, and `M` has no type scope. A real ternary, `ok ? 1 : 2`, gets the same scope. These colons aren't the ternary operator, and the design scopes types after a declaration's `:`. I didn't recount the 670 total.

Fix: Scope `:` as keyword.operator.ternary only after a `?` on the line, for example with a ternary begin/end that starts at ` ? `. Elsewhere use a punctuation.separator scope, and handle `For (Method|Field|Property) \w+(:)\s*(\w+)`.

Status: fixed: a colon is the ternary's only at the end of a region from a spaced `?`, or at the start of a line that goes on from one, and punctuation.separator.key-value elsewhere, and a typed For Method/Field/Property loop's variable and type are scoped; tested by contexts.fb's named argument, Map, nested ternary, continued ternary, and `For Method m: M`

## grammar-8 (low): The indentation rules read `Then` and `=>` inside comments and strings

editors/vscode/language-configuration.json:84. `If\b(?!.*\bThen\b)` and `(?:Function|Method|Operator|New)\b(?!.*=>)` look at the whole line. A block If whose trailing comment or string has `Then`, or a Function with `=>` in its trailing comment, is taken for a one-liner, so Enter doesn't indent the body.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s5/grammar/probe/indent.js, run as `node probe/indent.js repo/editors/vscode/language-configuration.json`. It tests the increase pattern on `If ready // Then go`, `If name == "Then"`, and `Function Foo() // => comment`.

Expected: All three open a block, so increaseIndentPattern matches them.

Actual: None of the three match. The same script shows that `Public Async Function Load()` and `If x` do match.

Skeptic: A node script on language-configuration.json's increaseIndentPattern: `If ready // Then go` false, `If name == "Then"` false, `Function Foo() // => comment` false, while `Public Async Function Load()` and `If x` are true. The lookaheads `(?!.*\bThen\b)` and `(?!.*=>)` read comments and strings. That isn't among the one-line limitations the design lists.

Fix: Strip a trailing `//...` before testing: change `.*` in the two lookaheads to `(?:(?!//)[^"])*`, or similar. Add these lines to 'a block's first line indents the next'.

Status: fixed: the indent rules look for Then and => outside strings and comments, a lambda's => too; tested by `If ready // Then go`, `If name == "Then"`, `Function Foo() // => comment`, and `Print("Function(")`

## grammar-9 (low): wordPattern splits hex, binary, octal, and exponent literals

editors/vscode/language-configuration.json:82. wordPattern's number alternative is only decimal digits with an optional fraction. In `0xFF_00`, double-clicking FF selects `xFF_00`, and word-based completion offers `xFF_00` and `b1010` as words. `1.5e-3` splits into `1.5`, `e`, and `3`.

Shown by: probe/indent.js, run as `node probe/indent.js repo/editors/vscode/language-configuration.json`, which also prints String.match(new RegExp(wordPattern, 'g')) for `0xFF_00`, `1.5e-3`, and `0b1010`.

Expected: Each literal is one word, as the lexer reads it: ["0xFF_00"], ["1.5e-3"], ["0b1010"].

Actual: ["0","xFF_00"], ["1.5","e","3"], ["0","b1010"].

Skeptic: In node, the matches of the wordPattern regex: `0xFF_00` gives ["0","xFF_00"], `1.5e-3` gives ["1.5","e","3"], and `0b1010` gives ["0","b1010"]. Spec 01 lines 79 to 83 make 0xFF00, 0b1010, 0o755, and 1.5e-3 single number literals.

Fix: Use `0[xbo][0-9A-Fa-f_]+|[0-9][0-9_]*(?:\.[0-9][0-9_]*)?(?:[eE][+-]?[0-9_]+)?|[A-Za-z_][A-Za-z0-9_]*` and test it in language.test.ts.

Status: fixed: wordPattern takes hex, binary, and octal literals, and a decimal's exponent; tested by language.test.ts's words
