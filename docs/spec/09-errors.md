# 09 · Error Handling

Two tiers:

- **Errors**: expected failures (missing file, bad input). Checked, recoverable.
- **Panics**: bugs. Crash with a message and where it happened. A stack trace comes with debug
  info (see [open-questions.md](../open-questions.md)).

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
  `UNEXPECTED_END`, `INTERRUPTED`, `OTHER`, and `Path`), `ArgumentError` (`Name`),
  `ParseError` (`Line`, `Column`), `FormatError`, `NetError` (`Address`), and `DatabaseError`,
  and `TimeoutError`, which has one kind. Each takes its Kind first, then its message, its
  details, and a Cause: `IOError(IOErrorKind.NOT_FOUND, "{path} doesn't exist", path)`.
- `Throws`, after the return type, lists the Classes a function's errors extend:
  `Throws FileError, ParseError`. Each extends `Error`, and none is covered by another. A function
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
  more when the chain goes on, as one that goes in a circle does, and exits with code 1.

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
- A panic prints `panic: <message> at <file>:<line>:<column>` and exits with code 101, so
  tests and tools can tell it from a normal exit.
- `@PanicsAtCaller` on a Function, Method, Operator, New, or Property makes every panic in its
  code report where it's called, as Rust's `#[track_caller]` does, so misuse points at the
  program's line: popping an empty Stack panics where `Pop` is called. A call of another
  @PanicsAtCaller function passes that place on. A call through an Interface's object, a
  function value, or a Callback has no place to give, so there the panic reports where it is.
  An Override of a @PanicsAtCaller Method is one too, and one that isn't can't be overridden by
  one that is, since a call through the Method table gives a place or doesn't.

## Tasks

`Task.Wait()` and `Await` **re-throw** the task's error (like C#). Waiting on a task that
came from a `Throws` function therefore counts as a throwing call. The call of an Async Function
doesn't throw: its errors go in its Task, whose type lists them, like `Task<Level> Throws
FileError` (see [10](10-concurrency.md#async--await)).

## Implementation

Errors are lowered as return values (Swift-style): zero cost when nothing throws, no
unwinding tables. A function that throws takes its caller's `Error?` variable as a hidden last
parameter, which the caller checks after the call.
