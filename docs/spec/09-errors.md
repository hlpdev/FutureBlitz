# 09 · Error Handling

Two tiers:

- **Errors**: expected failures (missing file, bad input). Checked, recoverable.
- **Panics**: bugs. Crash with a message, where it happened, and a stack trace.

## Errors

```vb
Class FileError Extends Error
End Class

Function LoadLevel(path: String): Level Throws FileError
    If !File.Exists(path) Then Throw FileError("missing: {path}")
    ...
End Function
```

- `Error` is a Class from the standard library (std/core/error.fb, `Import std.core`) with a
  `Message: String`, a `Cause: Error?`, the error it comes from, and a ReadOnly `Place`, where
  it was first thrown. `Error("text")` and `Error("text", cause)` make one, and a
  Class that extends it without a `New` of its own takes the same ones (see
  [06](06-user-types.md#inheritance)).
- `Place` is a Struct of the `File`, `Line`, and `Column` (from 1) that a panic shows, and shows
  as `file:line:column`. A Place whose Line is 0 is nowhere yet.
- Each area of std has one Error, whose `Kind` Enum says what went wrong, with its details:
  `IOError` (`IOErrorKind`: `NOT_FOUND`, `PERMISSION_DENIED`, `ALREADY_EXISTS`,
  `UNEXPECTED_END`, `INVALID_DATA`, `INTERRUPTED`, `OTHER`, and `Path`), `ArgumentError` (`Name`),
  `ParseError` (`Line`, `Column`, and Load's `Path`), `FormatError`, `RegexError` (`Offset`),
  `NetError` (`Address`), and `DatabaseError`, and `TimeoutError`, which has one kind. Each takes
  its Kind first, then its message, its details, and a Cause:
  `IOError(IOErrorKind.NOT_FOUND, "{path} doesn't exist", path)`.
- `Throws`, after the return type, lists the Classes a function's errors extend:
  `Throws FileError, ParseError`. Each extends `Error`, or is a type parameter that a constraint
  says does, `<E: Error>` (see [07](07-generics.md#class-and-error)), and none is covered by
  another. A function
  that can throw **must** declare it (checked; no surprise exceptions).
- `Throw error` ends the code there with an object of a Class that extends `Error`, and sets
  its Place to the Throw's, unless an earlier Throw did: an error thrown again from a Catch
  keeps the place it was first thrown from.
- A `New` can throw. Its object is then let go without its `Destroy()`, which could read fields
  that `New` didn't set, and its fields are released. A `New` that throws in the Class extended
  is called `Try Super.New(...)`, and a Class that takes it throws the same.
- `Destroy` can't throw: it runs whenever the last reference goes, with no caller to give the
  error to. An `Override` throws only what the Method it overrides can; it may throw less.

## Handling

```vb
Try
    Local lvl = LoadLevel("lvl1.dat")
Catch e As FileError
    Log(e.Message)
Catch e As Error
    ...
Finally
    ...
End Try
```

- An error in the body goes to the first `Catch` whose Class it's of, or extends. `Catch e`
  without `As` catches every Error. A Catch that an earlier one covers, or one that nothing in
  the body throws, is an error.
- What no Catch takes goes on out of the Try, so an outer Try or the function's `Throws` must
  cover it.
- `Finally` runs however the Try ends: after the body or a Catch, and through `Return`, `Break`,
  `Continue`, or an error. It can't throw, or leave with `Return`, `Break`, or `Continue`.
- The error can come from any part of the body, so a Catch starts from what was true before the
  Try, and so does the Finally. What the Finally assigns is assigned after the Try.

## Propagation and conversion

```vb
Local lvl = Try LoadLevel(p)      // propagate: enclosing function must be Throws
Local lvl = Try? LoadLevel(p)     // convert: Level?, error discarded
```

A call to a throwing function must be inside a `Try` block or prefixed with `Try`/`Try?`.

- `Try` and `Try?` bind like a prefix operator: `Try? Parse(s) ?? 0` is `(Try? Parse(s)) ?? 0`,
  and `Try a.B().C()` covers each call in the chain.
- `Try x` lets x's errors go on, to the Catches of a Try block around it, or out of the function,
  whose `Throws` must cover them.
- `Try? x` is a `T?`: Null when anything in x throws, and the error is dropped. A `T?` stays one.
- A Defer's statement can't throw, so it uses `Try?`.
- Top-level code can let any error out. Its Defers and Finallys run on the way, then it prints
  `uncaught <Class>: <Message> at <file>:<line>:<column>`, at the error's Place, then a
  `caused by <Class>: <Message> at ...` line for each Cause, up to 100, then says there are
  more when the chain goes on, as one that goes in a circle does, then the stack trace of where
  the error was first thrown (see [Stack traces](#stack-traces)), and exits with code 1.

## Panics

- `Panic("msg")` for unrecoverable bugs. Code after it doesn't run.
- `Assert(condition)` and `Assert(condition, "msg")` panic when the condition is False, in every
  build, and the message is only made then. Code after an Assert counts on its condition, so
  `Assert(e != Null)` narrows `e`.
- `DebugAssert(condition)` and `DebugAssert(condition, "msg")` are an Assert that release builds
  leave out, condition and message too, so code after one can't count on its condition.
- `Unreachable()` and `Todo()` panic, saying the code wasn't meant to be reached, or isn't done
  yet, then the message they're given, if any. Like Panic, they end the code's path, so a
  Function that gives a value can end with one.
- std.core's `AssertEqual(actual, expected)` and `AssertEqual(actual, expected, "msg")` panic
  when the two aren't equal, saying what each is, where AssertEqual is called. They're
  Equatable and Printable.
- Also triggered by: out-of-bounds indexing, failed `Assert`, integer overflow in debug
  builds, integer division by zero, use of a freed engine resource (see
  [02](02-types.md#integer-overflow) for the full list of arithmetic checks).
- Panics are not catchable, and don't run Defers or Finallys.
- A panic on any thread ends the whole program. When two threads panic at once, only the first
  one's message shows.
- A panic prints `panic: <message> at <file>:<line>:<column>`, then its stack trace, and exits
  with code 101, so tests and tools can tell it from a normal exit.
- `@PanicsAtCaller` on a Function, Method, Operator, New, or Property makes every panic in its
  code report where it's called, as Rust's `#[track_caller]` does, so misuse points at the
  program's line: popping an empty Stack panics where `Pop` is called. A call of another
  @PanicsAtCaller function passes that place on. A call through an Interface's object, a
  function value, or a Callback has no place to give, so there the panic reports where it is,
  and a @PanicsAtCaller function called that way passes none on either. An Interface's Final,
  generic, or conditional default can be @PanicsAtCaller, since a call through the Interface's
  value runs it directly, giving its place, as `Take(-1)` on an `Iterator<T>` value does.
  An Override of a @PanicsAtCaller Method is one too, and one that isn't can't be overridden by
  one that is, since a call through the Method table gives a place or doesn't.

## Stack traces

```text
panic: index 5 is out of range for 3 elements at game.fb:12:17
    at Counter.Take (game.fb:12)
    at Function at game.fb:22:20 (game.fb:22)
    at Apply<Counter> (game.fb:17)
    at Run (game.fb:22)
    at top-level code (game.fb:25)
```

- A panic's trace, and an uncaught error's, has a line for each call of the program's code that
  it's in, the innermost first: `    at <function> (<file>:<line>)`, with the file named as the
  panic names it. A Method or Property is `Class.Method`, a generic Function's instance names its
  type arguments, a function value's code and a Parallel For's body are named by where they're
  written, as `Parallel For at game.fb:9:5`, a function of another module than the program's
  main one has its module first, and a module's top-level code is `top-level code`. A call whose
  line a release build loses, as when it merges calls on several lines into one, is
  `    at <function> (<file>)`. The runtime's frames, the C code's, and those of the code the
  compiler makes itself are left out.
- It shows the innermost 100 places, then `    and more calls, which aren't shown` when the
  stack goes on.
- An uncaught error's trace is where it was first thrown, the Throw that set its Place, though a
  Catch threw it again further out.
- Every build has them, debug and release, with no debug files: the compiler writes a table into
  each binary, a library beside the executable's too, of where each call is in the source, which
  the runtime reads as it walks the stack. A call that a release build inlines still has its line
  of its own, after the call it's in, and no call is made a tail call, which would take its
  caller out of the trace.
- Debug builds have full debug info, DWARF and on Windows CodeView too, so a debugger stops at
  lines and shows the Locals, each in the block it's declared in; release builds have the line
  tables. On macOS a build's debug info is in the .dSYM beside it. tools/lldb/futureblitz.py,
  which VS Code's debugger loads, shows a String, an array, a Map, a `T?`, and an object as its
  value (see [tooling.md](../tooling.md)).

## Tests

```vb
Import std.test

@Test
Function ParsesALevel() Throws IOError, ParseError
    Local level = Try LoadLevel("one.dat")
    Expect(level.Rooms.Count == 12)
    Expect(ExpectThrows<ParseError>(LoadLevel("broken.dat")) != Null)
End Function
```

- `@Test` goes before a top-level Function that takes nothing and gives nothing back, though it
  can throw, anywhere in a package: a test, which `fb test` runs (see
  [tooling.md](../tooling.md#testing-step-7)). It isn't generic, Async, Const, Unsafe, or Extern,
  it takes no visibility, and no code calls it, or uses it as a value. It sees its file's Private
  names, as the file's other code does. `fb build` and `fb run` leave tests out, and the code only
  they reach; a library's binary and interface file leave the tests out.
- Each example in the doc comments of a package's `.fb` files, a code block under `# Examples`
  (see [01](01-lexical.md#doc-comments)), is a test of its own, named after its declaration, as
  `game.shapes.Circle.Grow (example)`, and its second `(example 2)`. It's a module of a package
  of its own, `<name>.doctests`, which depends on the package and what it depends on, so it uses
  the package through its Public names, as a reader's code would. It has `Import std.core`,
  `Import std.test`, and an Import of its declaration's module, unless it writes them itself, and
  writes any other Import at its top, as a file does; its declarations, like a Struct that
  implements an Interface, go outside its test, and its other code is the test's body. Its
  failures, panics, and stack traces show its lines in the doc comment. `fb test std` runs the
  standard library's examples. For a `.fb` file outside a package, `fb test` runs that file's
  examples, each a module beside it that imports it by its path, as `Import "./game"`, so it
  sees the file's Public and Package declarations, as the program's other files do.
- The `.fb` files of a package's tests/ folder are a package of their own, `<name>.tests`, which
  depends on the package and on what it depends on, so their tests use it from outside, through
  its Public names, as another package would; they import each other by their paths. A package
  builds without its tests/ folder, so its own files don't import them. A tests/ folder with a
  project.toml of its own is a package of its own, which `fb test` leaves alone.
- A test build has each of the package's `.fb` files, and runs the tests in place of the main
  file's top-level code, once the Globals are set; another main file's top-level code, as a
  tool's in the package can have, is left out too. The tests run in parallel, on the runtime's
  pool, but for those whose code uses what only the main thread can (see
  [10](10-concurrency.md#main-thread-rules)), like a Global or a `@MainThread` Function, which
  run on the main thread, one at a time. Work a test starts, with Spawn, a Thread, or a Parallel
  For, is the test's wherever it runs: its failures, panics, and leaks are the test's.
- A test passes unless it fails: by a failed Expect, ExpectThrows, or Fail, which it goes on
  after, by an error it lets out, by a panic, a failed Assert's too, which ends its program, or
  by a leak, which a debug build finds as it does a program's, once the Globals are let go of.
  `fb test` starts the program again after a panic, for the tests that didn't finish, and in a
  debug build, for the ones that finished in that run too, since their leaks weren't checked. A
  panic outside the tests, as in a Global's initializer or Destroy, and a leak no test made, fail
  the run, as a failed test does. Ctrl+C stops `fb test`, which then exits as the tests' program
  did.
- std.test's built-ins are only a file's that imports std.test, with `Import std.test` alone,
  not From or As, since they're the compiler's; and a file's own Function of their names and
  types hides them, as it does any built-in:
  - `Expect(condition)` and `Expect(condition, message)` are an Assert whose test goes on: a
    False condition fails it, saying "expect failed: ", the condition as it's written, and where,
    then for a comparison (`==`, `!=`, `<`, `<=`, `>`, `>=`), each side's debug form, as
    `"{x:?}"` shows it, worked out once, before the comparison, and the message, which is only
    made then. It doesn't narrow, since the code after it runs either way.
  - `ExpectThrows(x)` and `ExpectThrows<E>(x)` take any expression, whose errors they catch, as
    Try? does, so nothing in it needs Try, and give the error, an `Error?` or an `E?`, where E is
    a Class that extends Error. When x throws nothing, or an error that isn't an E, the test
    fails, saying so, and it gives Null. An x that can't throw, or can't throw an E, is an error.
  - `Fail()` and `Fail(message)` fail the test, which goes on.
  - `Skip()` and `Skip(reason)` skip the test, unless it failed already, and end it, as a
    Return would, so Skip is only in a @Test Function's own code, not a lambda's, a Defer's, or
    a Finally's.
  - In code that's no test's, as in a program `fb build` makes, a failure of Expect,
    ExpectThrows, or Fail panics, saying what it would have reported, and so does Skip.

## Tasks

`Task.Wait()` and `Await` **re-throw** the task's error (like C#). Waiting on a task that
came from a `Throws` function therefore counts as a throwing call. The call of an Async Function
doesn't throw: its errors go in its Task, whose type lists them, like `Task<Level> Throws
FileError` (see [10](10-concurrency.md#async--await)).

## Implementation

Errors are lowered as return values (Swift-style): zero cost when nothing throws, no
unwinding tables. A function that throws takes its caller's `Error?` variable as a hidden last
parameter, which the caller checks after the call.
