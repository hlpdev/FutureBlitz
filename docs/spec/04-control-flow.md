# 04 · Control Flow

## If

```vb
If hp <= 0 Then Die()          // single-line form: the only form that uses Then

If hp <= 0
    Die()
ElseIf hp < 20
    Flash()
Else
    Regen()
End If
```

Conditions must be `Bool`.

The single-line form is `If cond Then stmt`, optionally followed by `Else stmt` on the same
line. It has no `ElseIf`, and its statements can't be block statements (`If`, loops,
`Select`):

```vb
If hp <= 0 Then Die() Else Regen()
```

## For

```vb
For i = 0 To 9 ... End For                 // inclusive
For i = 0 Until 10 ... End For             // exclusive
For i = 0 Until 10 Step 2 ... End For
For i = 10 To 0 Step -1 ... End For

For e In enemies ... End For               // values
For i, e In enemies ... End For            // index + value
For key, val In scores ... End For         // map entries
```

- `For` declares its variables; they exist only inside the loop. A counting loop's variable
  takes the type of its start value; a `For ... In` variable takes the element type.
- A literal start takes the end's type instead: `For i = 0 Until n` with `n: Long` counts with
  a `Long`.
- The start, end, and step are each evaluated once, in that order, before the first pass.
- The loop never steps past its end, so a `Byte` loop up to 255 ends instead of wrapping
  around.
- When the step's sign isn't known at compile time, it picks the direction at run time. A step
  of `0` is a compile error when known and a panic at run time.
- Assigning to a loop variable inside the loop is an error.

`Parallel For` is covered in [10-concurrency.md](10-concurrency.md).

## While, Repeat, Loop

```vb
While running
    ...
End While

Repeat                                     // body runs at least once
    ...
Until done

Loop                                       // infinite
    ...
End Loop
```

- `Until` is outside the `Repeat` body, so it can't see `Local`s declared inside it.
- `While True`, `Repeat ... Until False`, and `Loop` only end through `Break` (or `Return`), so
  code after one without a `Break` is never reached.

## Break and Continue

```vb
Outer: For y = 0 Until h
    For x = 0 Until w
        If grid[x, y] == 0 Then Continue
        If grid[x, y] < 0 Then Break Outer   // labeled break
    End For
End For
```

A label (`Outer:`) must come right before a loop.

## Select

```vb
Select state
    Case State.Idle, State.Walk
        Animate()
    Case 1 To 5                    // inclusive range
        ...
    Case e As Boss                 // type match + bind
        e.Enrage()
    Default
        ...
End Select
```

- No fallthrough.
- `Select` on an enum must be **exhaustive** or include `Default`.
- The subject is an integer, `Bool`, `String`, or enum value, unless every `Case` is a type
  match like `Case e As Boss`.
- `Case` values are of the subject's type and must be known at compile time. `To` ranges work
  on integers. The same value can't appear in two cases.

## Defer

Runs a statement when the enclosing scope exits, in reverse order of declaration,
including when exiting via `Return`, `Break`, or a thrown error.

```vb
Local f = File.Open(path)
Defer f.Close()
```

## Removed

- No `Goto`, no `Gosub`, no line labels other than loop labels.
