# 04 · Control Flow

## If

```vb
If balance <= 0 Then Close()   // single-line form: the only form that uses Then

If balance <= 0
    Close()
ElseIf balance < 20
    Warn()
Else
    Thank()
End If
```

Conditions must be `Bool`.

The single-line form is `If cond Then stmt`, optionally followed by `Else stmt` on the same
line. It has no `ElseIf`, and its statements can't be block statements (`If`, loops,
`Select`):

```vb
If balance <= 0 Then Close() Else Thank()
```

## For

```vb
For i = 0 To 9 ... End For                 // inclusive
For i = 0 Until 10 ... End For             // exclusive
For i = 0 Until 10 Step 2 ... End For
For i = 10 To 0 Step -1 ... End For

For u In users ... End For                 // values
For i, u In users ... End For              // index + value
For key, val In prices ... End For         // map entries
```

- `For` declares its variables; they exist only inside the loop. A counting loop's variable
  takes the type of its start value; a `For ... In` variable takes the element type.
- `For ... In` evaluates the array once and holds it for the loop. It reads a `T[]`'s `Count`
  before each pass, so removing elements during the loop ends it sooner. The index is an `Int`,
  and both variables are read-only copies. A `T[W, H]` goes row by row, with one index that
  counts every element.
- A literal start takes the end's type instead: `For i = 0 Until n` with `n: Long` counts with
  a `Long`.
- The start, end, and step are each evaluated once, in that order, before the first pass.
- The loop never steps past its end, so a `Byte` loop up to 255 ends instead of wrapping
  around.
- When the step's sign isn't known at compile time, it picks the direction at run time. A step
  of `0` is a compile error when known and a panic at run time.
- Assigning to a loop variable inside the loop is an error.

### Iterators

`For ... In` also goes over an Iterator, and over a value whose `Iterate()` gives one, like a
collection. An `Iterator<T>` (std/core/iterator.fb) has a `ByRef Method Next(): T?`, which gives
the next element each time, and Null once there are no more.

```vb
Struct Countdown Implements Iterator<Int>
    Field N: Int

    ByRef Method Next(): Int?
        If N == 0 Then Return Null
        N -= 1
        Return N + 1
    End Method
End Struct

For n In Countdown(3) ... End For          // 3, 2, 1
```

- The loop holds the Iterator, evaluated once: a copy of a Struct, so the variable it came from
  doesn't change, or a Class's object, which its Next changes. An `Iterate()` takes no
  arguments. One that throws needs the loop in a Try block, or written `For x In Try items`,
  which lets its errors go on like Try on a call.
- Over an Iterator, two names take each element apart, so the elements must be pairs: `For key,
  val In prices`. Only arrays give an index.
- Over values that can be Null, like an `Iterator<String?>`'s, Next gives a `String??`: its
  Null ends the loop, and a Null value is one of the values, which the loop goes over. What
  gives one of them, like First, Last, Find, and Reduce, gives a `String??` too.
- An `Iterable<T>` (std/core/iterator.fb) is what has an `Iterate(): Iterator<T>`, for code that
  takes any collection: std's collections are. A collection's own Iterate gives a Struct of its
  own, which an `Iterable<T>`'s value, or generic code, gets in a box, an `Iterator<T>` object;
  a For loop over the collection itself makes no box.
- An Iterator's adapters are lazy, each giving an `Iterator<T>` that asks the one before it for a
  value only when it's asked for one: `Map`, `Filter`, `Take`, `Skip`, `TakeWhile`,
  `SkipWhile`, `Enumerate`, `Zip`, `Chain`, `Flatten`, `StepBy`, `Chunks`, `Windows`, `OrderBy`,
  and `OrderByDescending`. Its consumers go over the values once: `ToArray`, `Count`, `Any`,
  `All`, `Find`, `First`, `Last`, `Fold`, `Reduce`, `ForEach`, `Sum` and `Product` where T is
  Numeric, `Min` and `Max` where it's Comparable, `MinBy`, and `MaxBy`. `Repeated(value, count)`
  and `Empty<T>()` give values of their own. They're Final defaults, so what a type has, and a
  call through an `Iterator<T>` value runs them as written (see
  [06-user-types.md](06-user-types.md)). On a Struct, each goes over a copy of it, so the Struct
  doesn't move, while a Class's object is used up, as a For loop over it is.

### Ranges

```vb
For i In 1 To 3 ... End For                // 1, 2, 3
Local evens = 0 Until 10 Step 2            // 0, 2, 4, 6, 8
Print(evens.Sum(), (1 To 5).Product())
```

- `a To b` and `a Until b`, with `Step s` or without, are values of std.core's `Range<T>`, a
  Struct that implements `Iterator<T>` and gives what `For i = a To b Step s` counts. T is the
  start's type, or the end's when the start is a literal, as a For loop's is, or the T of the
  `Range<T>` expected. A range counts with integers: a Float range is an error, though a For
  loop counts Floats.
- The start, end, and step are evaluated once, in that order, when the range is made. A Step of
  0 is an error when it's known, and a panic when the program runs otherwise.
- `To` and `Until` bind looser than every binary operator but `? :`, so each end is a whole
  expression: `0 Until n - 1`, `1 To count ?? 0`, and the Step takes a whole expression, `? :`
  too, as a For loop's does. A Method of the range needs parentheses: `(1 To 5).Sum()`. In a For
  loop's bounds and a Case's values, To and Until belong to the loop or the Case. A slice is
  `a[from..to]`.
- Ranges are `==` when they give the same values, so every range that gives none is `==` to
  every other, one that Next has gone to the end of too, and an empty range is a Range's zero.
  That `==` is an Operator, which a Const Function doesn't run. A range is a value, copied like
  any Struct, so a loop over a stored range goes over a copy of it.

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
    Case State.Idle, State.Connecting
        Reconnect()
    Case 1 To 5                    // inclusive range
        ...
    Case u As Admin                // type match + bind
        u.Grant()
    Default
        ...
End Select
```

- No fallthrough.
- `Select` on an enum must be **exhaustive** or include `Default`. An exhaustive one leaves no
  path past it, so a function can return from every Case. A `@Flags` Enum's values combine, and
  an `@Open` Enum holds others, so a `Select` on one always needs a `Default`.
- The subject is an integer, `Bool`, `String`, `Char`, or enum value, unless every `Case` is a
  type match like `Case u As Admin`.
- A type match runs for an object of that Class, or of one extending it, with `e` naming it as
  one. A `Case` after one for a Class it extends never matches, so it's an error. `Null` goes to
  `Default`. `Case x As T` always matches a Class; a value cast in a `Case` needs parentheses.
- `Case` values are of the subject's type and must be known at compile time. `To` ranges work
  on integers and Chars, like `Case 'a' To 'z'`. The same value can't appear in two cases.

## Defer

Runs a statement when the enclosing block exits, however it exits: at its end, or through
`Return`, `Break`, `Continue`, or a thrown error.

```vb
Local f = File.Open(path)
Defer f.Close()
```

- The statement is one call, assignment, `++` or `--`, or a one-line `If` of those. It can't
  leave the block or declare anything.
- A block's Defers run the last first, after those of the blocks inside it. A loop's body is a
  block, so each pass runs its own. Variables declared before a Defer are alive when it runs.
- Its expressions are evaluated when it runs: `Defer Print("{n}")` shows `n` as it is at the
  exit. `Return` computes its value first, so a Defer can't change what's returned.
- It's checked where it's written, as if it ran there. A null check above it still holds unless
  the rest of the block changes the variable, and what it assigns isn't assigned after it.
- A panic ends the program without running Defers.

## Jumps

There is no `Goto` or `Gosub`, and no line labels other than loop labels.
