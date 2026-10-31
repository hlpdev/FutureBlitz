# FutureBlitz for VS Code

FutureBlitz (.fb) in VS Code: highlighting, the language server's errors as you type and
formatting on save, tasks for `fb build`, `fb run`, `fb test`, and `fb fmt`, and debugging through
CodeLLDB. The design is in [docs/tooling.md](https://github.com/hlpdev/FutureBlitz/blob/HEAD/docs/tooling.md#the-vs-code-extension-step-5).

## What it needs

- **fb**: `futureblitz.path`, else `fb` on PATH, else `~/.futureblitz/bin`.
- **The language server**, `fb-lsp`: `futureblitz.lspPath`, else beside fb, else on PATH. Without
  it, highlighting, tasks, and debugging still work.
- **CodeLLDB** (`vadimcn.vscode-lldb`), which VS Code installs with the extension, for debugging
  on every OS.

## Using it

- **Completion** knows where it is: what the code wants there first (a call's argument's type,
  an initializer's, a Return's, an Enum's members where one is wanted), the parameters' names a
  call doesn't give yet, the keywords that go there, and the names of modules the file doesn't
  import, which add the Import when chosen. After a value's `.`, `.if`, `.ifnull`, `.for`,
  `.not`, `.try`, and `.local` write it into what they say, and in a Class's body, the Methods
  it can override or implement are written out. VS Code's own word suggestions are off for .fb
  files, so the language server's are what's shown.
- **Reading code**: inlay hints show the types of Locals, For variables, and lambda parameters
  where none is written, and the parameters' names at a call's arguments (each a setting). A
  name's other uses in its file are highlighted, reads and writes apart; blocks, each If branch,
  Select Case, and Catch, runs of comments, and Import lines fold; Expand Selection grows by
  expressions and statements. Go to Type Definition, Go to Implementations (an Interface's
  Classes and Structs, a Class's subclasses, a Method's overrides), Show Call Hierarchy, and Show
  Type Hierarchy work across the package, and a count of references sits above each type and
  function.
- **Tests**: the Testing panel lists each file's `@Test` Functions, and runs or debugs a file's or
  one test's, with their buttons in the gutter too: a run is `fb test --filter`, and a debug run
  builds the tests' executable with `fb test -o` and runs it under CodeLLDB. A program's main file
  has Run and Debug above its first line.
- **Run** and **Debug** are in the editor's title bar on a .fb file, and in the Command Palette as
  FutureBlitz: Run, FutureBlitz: Debug, and FutureBlitz: Run Tests. Each takes the open file's
  package, the folder with the project.toml above it, or the file itself without one.
- **Tasks**: Terminal > Run Task > fb lists build, run, test, fmt, and check for the workspace
  folder's package and for the open file. Their errors, and a program's panics and failed
  Expects, go to Problems. In tasks.json:

  ```json
  {
    "type": "futureblitz",
    "command": "run",
    "target": "game",
    "release": true,
    "programArgs": ["--level", "2"]
  }
  ```

- **Debugging**: F5 debugs the open file's package, or the file. A launch.json configuration:

  ```json
  {
    "type": "futureblitz",
    "request": "launch",
    "name": "Debug",
    "program": "${workspaceFolder}",
    "args": [],
    "stopOnEntry": false
  }
  ```

  It builds the program in debug, then CodeLLDB runs it with fb's lldb script, which shows
  Strings, arrays, Maps, nullables, and objects as their values. CodeLLDB's own settings, like
  `env` or `preRunCommands`, go in the same configuration.

## Settings

| Setting | |
|---|---|
| `futureblitz.path` | fb; `${workspaceFolder}` and `~` are expanded |
| `futureblitz.lspPath` | the language server |
| `futureblitz.lsp.enabled` | start the language server; on by default |
| `futureblitz.inlayHints.types` | show the types the code doesn't write; on by default |
| `futureblitz.inlayHints.parameterNames` | show the parameters' names at calls; on by default |
| `futureblitz.trace.server` | trace its messages in the FutureBlitz output |
| `futureblitz.debug.lldbScript` | fb's `futureblitz.py`; beside fb, else the extension's copy |

In the FutureBlitz repo itself, fb from the CMake build folder is
`"futureblitz.path": "${workspaceFolder}/cmake-build-debug/cli/fb.exe"`.

## Building and installing

With Node 22 or newer:

```sh
cd editors/vscode
npm ci
npm run package          # futureblitz.vsix
code --install-extension futureblitz.vsix
```

`npm run compile` builds into out/ and copies tools/lldb/futureblitz.py and the license in. To
work on the extension, open editors/vscode in VS Code and run it with F5's Extension Development
Host, or `npm run watch`.

## Tests

```sh
npm test                 # compile, the unit tests, then the extension tests
npm run test:unit        # the grammar, language configuration, snippets, and helpers, in Node
npm run test:extension   # the extension in VS Code, through @vscode/test-electron
```

- `FUTUREBLITZ_FB=<path to fb>` runs the tests that build programs, and
  `FUTUREBLITZ_LSP=<path to fb-lsp>` those of the real server's diagnostics and formatting;
  without them they're skipped.
- The extension tests download VS Code, the oldest version package.json takes, into
  .vscode-test/; `FB_VSCODE_VERSION=stable` picks another, and `FB_VSCODE_EXECUTABLE` runs an
  installed one. They run twice, on a workspace of three folders and then with no folder open. A
  stand-in for CodeLLDB records each launch, and a stand-in language server formats.
- The grammar's tests are vscode-tmgrammar-test's assertion files, test/grammar/*.fb, and its
  snapshots of real std and tests/programs code, test/grammar/snap/*.fb.snap.
  `UPDATE_SNAPSHOTS=1 npm run test:unit` writes the snapshots again; read what changed before
  committing them. `node scripts/tokens.mjs <file.fb>` prints how the grammar scopes a file.
- `FUTUREBLITZ_LLDB` names an lldb with Python, for the test that it loads fb's script.

## Publishing

To the VS Code Marketplace and Open VSX, under the `futureblitz` publisher, once its accounts
exist: `npx vsce publish` and `npx ovsx publish futureblitz.vsix`.
