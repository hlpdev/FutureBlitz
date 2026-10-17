# 09 · Error Handling

Two tiers:

- **Errors**: expected failures (missing file, bad input). Checked, recoverable.
- **Panics**: bugs. Crash with a message and stack trace.

## Errors

```vb
Class FileError Extends Error
End Class

Function LoadLevel(path: String): Level Throws FileError
    If !File.Exists(path) Then Throw FileError("missing: {path}")
    ...
End Function
```

- `Error` is a Class from the standard library (std/core/error.fb) with a `Message: String`
  field. `Error("text")` makes one, and a Class that extends it without a `New` of its own takes
  the same message (see [06](06-user-types.md#inheritance)).
- `Throws`, after the return type, lists the Classes a function's errors extend:
  `Throws FileError, ParseError`. Each extends `Error`, and none is covered by another. A function
  that can throw **must** declare it (checked; no surprise exceptions).
- `Throw error` ends the code there with an object of a Class that extends `Error`.
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
  `uncaught <Class>: <Message> at <file>:<line>:<column>` and exits with code 1.

## Panics

- `Panic("msg")` for unrecoverable bugs. Code after it doesn't run.
- `Assert(condition)` and `Assert(condition, "msg")` panic when the condition is False, in every
  build, and the message is only made then. Code after an Assert counts on its condition, so
  `Assert(e != Null)` narrows `e`.
- Also triggered by: out-of-bounds indexing, failed `Assert`, integer overflow in debug
  builds, integer division by zero, use of a freed engine resource (see
  [02](02-types.md#integer-overflow) for the full list of arithmetic checks).
- Panics are not catchable, and don't run Defers or Finallys.
- A panic prints `panic: <message> at <file>:<line>:<column>` and exits with code 101, so
  tests and tools can tell it from a normal exit.

## Tasks

`Task.Wait()` and `Await` **re-throw** the task's error (like C#). Waiting on a task that
came from a `Throws` function therefore counts as a throwing call.

## Implementation

Errors are lowered as return values (Swift-style): zero cost when nothing throws, no
unwinding tables. A function that throws takes its caller's `Error?` variable as a hidden last
parameter, which the caller checks after the call.
