# 09 · Error Handling

Two tiers:

- **Errors**: expected failures (missing file, bad input). Checked, recoverable.
- **Panics**: bugs. Crash with a message and stack trace.

## Errors

```vb
Class FileError Extends Error
    ...
End Class

Function LoadLevel(path: String): Level Throws FileError
    If !File.Exists(path) Then Throw FileError("missing: {path}")
    ...
End Function
```

- A function that can throw **must** declare `Throws` (checked; no surprise exceptions).
- Error types extend `Error`.

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

## Propagation and conversion

```vb
Local lvl = Try LoadLevel(p)      // propagate: enclosing function must be Throws
Local lvl = Try? LoadLevel(p)     // convert: Level?, error discarded
```

A call to a throwing function must be inside a `Try` block or prefixed with `Try`/`Try?`.

## Panics

- `Panic("msg")` for unrecoverable bugs.
- Also triggered by: out-of-bounds indexing, failed `Assert`, integer overflow in debug
  builds, integer division by zero, use of a freed engine resource (see
  [02](02-types.md#integer-overflow) for the full list of arithmetic checks).
- Panics are not catchable.
- A panic prints `panic: <message> at <file>:<line>:<column>` and exits with code 101, so
  tests and tools can tell it from a normal exit.

## Tasks

`Task.Wait()` and `Await` **re-throw** the task's error (like C#). Waiting on a task that
came from a `Throws` function therefore counts as a throwing call.

## Implementation

Errors are lowered as return values (Swift-style): zero cost when nothing throws, no
unwinding tables.
