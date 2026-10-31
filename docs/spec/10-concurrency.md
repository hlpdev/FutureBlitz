# 10 · Concurrency

## The model

Programs need concurrency for three different jobs. Each gets its own tool:

| Job | Example | Tool | Runs on |
|---|---|---|---|
| Do heavy work faster | resizing images, parsing, compressing | `Spawn`, `Parallel For` | worker threads, in parallel |
| Wait without freezing | reading a file, a network request | `Async` / `Await` | main thread; waiting costs nothing |
| Do things over time | show a message for 2s, then hide it; retry after a delay | `Async` coroutines | main thread, resumed by the main scheduler |

Jobs 2 and 3 are the same mechanism: a coroutine is an `Async` function that awaits time
instead of I/O.

## Thread layout

```
MAIN THREAD     top-level code, and the loop it runs, if any: a UI's event loop, a terminal
                UI's, a server's; Async code and coroutines run here

WORKER POOL     [task][task][parallel-for chunk][task]...      one thread per core, minus 1
                (pure computation, no main-thread APIs)

DEDICATED       Thread.Start(...) for long-running blocking loops (e.g. a network listener)
THREADS         so they don't tie up a pool worker
```

Most code never leaves the main thread.

- The pool starts its workers the first time it's used, so a program that never uses it has
  only the main thread. It has one per core but one, and at least one.
- A worker that waits for the pool's work, like a task's `Wait()` or a `Parallel For`, runs the
  work it started itself meanwhile, so a task that waits for tasks of its own doesn't leave the
  pool a worker short. It runs no one else's, which could wait for it in turn. A thread waiting
  on a Channel runs nothing, so a task that waits on one holds its worker: work that mostly waits
  belongs on a Thread.
- When top-level code ends, the program waits for the pool's work to finish, then releases the
  Globals and ends.
- Lines that `Print` writes from different threads don't mix, and a panic on any thread ends
  the whole program at once (see [09](09-errors.md#panics)).

## Tasks and parallel loops

```vb
Local t: Task<Index> = Spawn(Function() => BuildIndex(files))
...
Local index = t.Wait()               // blocks; re-throws the task's error

Parallel For i = 0 Until items.Count
    scores[i] = Score(items[i])      // iterations must be independent
End For
```

- The runtime has a work-stealing thread pool.
- `Spawn(work)` runs the work on the pool and gives its `Task<T>`, where T is what the work
  gives; work that gives nothing gives a `Task`. `t.Wait()` returns once it's done, with its
  result, and `t.IsDone()` says whether it is.
- `Thread.Start(work)`, for long-lived blocking work, runs the work on a thread of its own and
  gives a `Thread`, with `Join()` and `IsDone()`.
- The work is a lambda or a Function's name, which takes no arguments, or a Sendable Func (see
  [05](05-functions.md#sendable-func)), since no other function value can go to the other
  thread. A `Task<T>` expected says what the lambda gives, as in `Local t: Task<Int64> =
  Spawn(Function() => 5)`, so neither takes type arguments.
- What the work makes is its own: it reaches only Sendable values from outside it, so its result
  moves safely to the thread that waits for it, even an object or an array.
- A Task or Thread stays on the thread that started it, so it isn't Sendable, and only that
  thread waits for it.
- The program waits for every Task and Thread when its top-level code ends. A panic in one ends
  the whole program.
- The work's lambda can't throw, like any function value's. A Task whose Wait throws comes from
  an Async Function with a Throws (see [Async / Await](#async--await)).
- `Parallel For i = a Until b`, or `To b`, runs its passes on the pool at once, in chunks, with
  the calling thread taking some, and the next statement waits for them all. It counts integers
  up by 1, with no Step, through its type's last value too, as `For` does. The calling thread
  waits only for the chunks being run: a worker busy elsewhere joins in only if chunks are left.
- What the body uses from outside it is a copy, made once, which it can't change, and which is
  Sendable. An array from outside whose elements are Sendable is used only through its elements
  and its Count: each pass changes an element only at its own index, `a[i]`, so no two passes
  change the same element, and a body that changes an array's elements reads that array only
  there too, since another pass may be changing any other element at once; it reads any element
  of an array it doesn't change. A ByRef Method can change `a[i]` there, since the array can't
  grow meanwhile, and a lambda in the body can't take the array.
- Continue goes on to the next index; Break and Return can't leave the body, and neither can an
  error. The body is work on other threads, so the main-thread rules hold for it.

## Async / Await

Async Functions are stackless coroutines, which the main thread's scheduler runs.

```vb
Async Function ReadReport(path: String): Report Throws IOError
    Const text = Try Await File.ReadTextAsync(path)       // the program keeps running
    Local stats = Await Spawn(Function() => Summarize(text)) // heavy part on a worker
    Return Report(path, stats)                            // back on the main thread
End Function
```

- Calling an `Async Function` starts it and gives its `Task<T>`, where T is its return type, or
  a `Task` when it returns nothing. It runs on the main thread until it first Awaits, then the
  call returns. `a?.M()` on an Async Method gives a `Task<T>?`, which is `Null` when `a` is.
- `Await t` in an Async Function waits for a Task, a Spawn's or another Async Function's, and
  gives its result. Meanwhile the main thread goes on; the Function's Locals and parameters wait
  with it.
- The main thread runs the waiting Async Functions when the main scheduler runs (see
  [Scheduling](#scheduling)): once top-level code ends, until none is left; whenever it waits for a
  Task that isn't done, with `Wait()`, or with an Await in top-level code, which waits as `Wait()`
  does; and at each `Scheduler.Main.Step()`. A wait, and the result it gives, are the Task's that
  the expression gave when the wait began, even if what runs meanwhile changes the variable it came
  from. Their code only ever goes on on the main thread: the runtime panics where it would go on
  anywhere else. Their code never runs on another thread, so work on another thread can't call one,
  and an Async lambda isn't work for `Spawn` or `Thread.Start`.
- Await goes only in the code of an Async Function, Method, or lambda, or in top-level code,
  not in a lambda inside one, and not in a Lock block, which would hold its Mutex while
  it waits.
- An Async Function's errors go in its Task, so its call doesn't throw, but `Await t` and
  `t.Wait()` re-throw them, each time, and need Try, Try?, or a Try block, as a call that throws
  does. The Task's type says so: `Task<Report> Throws IOError`, which can be written as a type.
  After a function's result, Throws is the function's, so a Task's type there goes in
  parentheses, `Function Start(): (Task<Report> Throws IOError)`, and so it does at the end of
  a Func that's the result: `Function Make(): Func(): Task<Int> Throws IOError` can throw
  itself. A Func type's result is the same, so a Func value that gives a throwing Task is
  `Func(): (Task<Int> Throws IOError)`. Its Throws lists Classes as a function's does, so it
  goes in parentheses before a `?` or `[]` too, and where a `,` goes on to anything but a
  parameter, like a tuple's next element or the next type argument; in a Func type's or a
  lambda's parameters, a `,` always goes on to the next parameter.
- Its error type can be a type parameter that a constraint says is an error, `E: Error` (see
  [07](07-generics.md#class-and-error)), which each call or instance has its own of: `Async
  Function WhenAll<T, E: Error>(tasks: (Task<T> Throws E)[]): T[] Throws E` passes the Tasks'
  errors on, and a generic Class's Async Method can throw its own E.
- A Task that throws converts to one of the same result that throws what covers its errors, as
  an Override's Throws is covered by its Method's: a `Task<Int> Throws IOError` is a
  `Task<Int> Throws Error`. A Task that throws and one that doesn't don't convert, either way,
  since only one of them keeps an error.
- A Method can be Async, but not a ByRef Method or Destroy. An Override of an Async Method is
  Async, and an Async Method overrides only an Async one. An Interface's Method can be Async, a
  required one or a default, and what implements an Async one is Async too, while an Async Method
  implements only an Async one: a call through the Interface's value, or generic code, starts it
  as any Async Method's call does, and gives its Task, so `Await reader.ReadAsync(buffer)` waits
  for whatever Class the Reader is. The Task is the caller's, so what implements one gives just
  the type the Interface's gives, not a Struct or Class implementing the Iterator<T> it gives,
  which a plain Method's result can be. A Static one can be Async too, which generic code calls
  on its type. An Interface that redeclares one of an Interface it extends is Async where that
  one is, and only there.
- `Async Function(x) => ...` is an Async lambda, whose call gives a Task, through a function
  value too: `Func(Int): Task<Int>`. Making one is main-thread code, since its code runs there.
- An Async Function's parameters can't be ByRef, and it isn't a function value; a lambda can
  call it.

## Coroutines

```vb
Async Function ShowToast(toast: Toast, text: String)
    toast.Show(text)
    Await Wait(2.Seconds)
    toast.Hide()
End Function

ShowToast(toast, "Saved")    // calling without Await starts it on the main scheduler
```

### Scheduling

A coroutine waits on a scheduler, which resumes it once what it waits for is done. The main
thread's scheduler, the main scheduler, runs by itself, so a program never has to tick it:

- at an `Await` in top-level code, while the Task it waits for isn't done;
- at a Task's `Wait()` on the main thread, while the Task isn't done;
- once top-level code ends: the runtime then runs it until no coroutine waits on it, as Node
  does.

Nothing else runs it. An Await in an Async Function doesn't: the coroutine stops there, and the code
that called it, or the scheduler that resumed it, goes on. A `Wait()` or a top-level Await of a Task
that's done gives its result at once, without running the scheduler. A blocking call, like reading a
file, doesn't run it either, so no other code runs in the middle of one. Main-thread code stays
single-threaded across an Await, as in C#'s UI apps and JavaScript; CPU-heavy work goes to Spawn and
Parallel For, on every core.

**A loop of the program's own steps the main scheduler.** Top-level code that runs a loop, like
a UI's event loop, a terminal UI's redraws, or a server's poll, resumes the coroutines between
its passes with `Scheduler.Main.Step()`, or with `Await NextTick()`. A library that owns such a
loop, like a UI toolkit's event loop, a terminal UI's loop, or a package's own loop, calls Step
for its users, so a program built on one never does:

```vb
ShowToast(toast, "Saved")

While window.IsOpen()
    window.HandleEvents()
    window.Draw()
    Scheduler.Main.Step()          // coroutines resume here
End While
```

**Schedulers of a program's own, ticked by hand**, for pausing and scaling time:

```vb
Global playback = Scheduler()
playback.Run(Animate(chart))

While window.IsOpen()
    If !paused Then playback.Tick(dt * speed)
    window.Draw()
    Scheduler.Main.Step()          // the main scheduler still runs the rest
End While
```

Rules:

- The main scheduler runs once top-level code ends, until no coroutine waits on it, whenever
  the main thread waits for a Task that isn't done with `Wait()` or a top-level Await, and at
  each `Scheduler.Main.Step()`. Its time is the real clock's.
- `Scheduler.Main` is the main scheduler, as a `Scheduler`. The runtime keeps it for the whole
  program, so it's never destroyed, and it can't be assigned. Its coroutines run on the main
  thread, so only the main thread uses it: work on another thread that uses it is a compile
  error.
- `s.Step()` resumes the coroutines that can go on, without moving the Scheduler's time, and
  returns at once when none can; it never waits. On `Scheduler.Main` it finishes the timers that
  the real clock has reached first.
- The main scheduler's time is the real clock's, which a Tick can't move, so
  `Scheduler.Main.Tick(seconds)` is a compile error, and a Tick of the main scheduler through a
  variable or parameter panics.
- Once top-level code ends, a coroutine that can never go on, since it waits for a Task on a
  Scheduler that nothing ticks any more, or for one that waits for such a Task, is destroyed before
  the Globals are released, as when its Scheduler goes. So is one that a Destroy starts while the
  Globals are released, at its first Await, since the program has ended, even when that Destroy
  steps the main scheduler or waits, and a Wait there for the Task of a coroutine destroyed that way
  panics, since it's never done. A Task from NextTick, Wait, or RealWait that nothing awaits then
  doesn't keep the program going, and is never done.
- `Scheduler()` makes one, only on the main thread, since its coroutines run there, and it isn't
  Sendable. `s.Tick(seconds)` moves its time on by that much and resumes what can go on, `s.Step()`
  is a Tick of no time, and `s.IsIdle()` says whether no coroutine waits on it.
- An Async call starts on the scheduler running the code: the main scheduler, or a Scheduler
  while it's ticked or stepped. `s.Run(task)` moves its coroutine to `s`, from the scheduler
  running the code, with the coroutines and timers it waits for. A Task that's done, or a
  Spawn's, has nothing to move.
- `Await NextTick()` resumes at the next tick of the scheduler running the coroutine: for the main
  scheduler, its next Step, or the main thread's next `Wait()` or top-level Await that has to wait;
  for a Scheduler, its next Tick or Step. In top-level code it waits as `Wait()` does, so it runs
  the main scheduler once.
- `Await Wait(time)` uses the scheduler's time (respects pause and time scale), and
  `Await RealWait(time)` the real clock's, ignoring both. A time is std.time's `Duration`, like
  `2.Seconds` or `500.Milliseconds`, so either one needs `Import std.time`. NextTick, Wait, and
  RealWait give a Task that the scheduler running the code finishes, so only the main thread
  calls them.
- An I/O await resumes on the scheduler that started it, at its next tick. Coroutine code
  never runs on a background thread. A coroutine that starts waiting during a tick waits for the
  next one, even for a Task that's done.
- `Await MainThread()` hops back to the main thread. Coroutine code is always there already, so
  `MainThread()` gives a Task that's done, and the Await goes on at the next tick.
- A coroutine still waiting on a Scheduler when the Scheduler goes is destroyed: it lets go of
  what it holds, without running its Defers or Finally blocks, and its Task is never done.

## Communication

| Tool | Use |
|---|---|
| `Channel<T>(capacity)` | one thread sends, another receives |
| `Mutex<T>` | shared data behind a lock |
| `Atomic<T>` | shared counters and flags |

```vb
Const ch = Channel<Packet>(64)
Spawn(Function() => NetLoop(ch))
While ch.TryReceive(ByRef pkt)
    Handle(pkt)
End While

Global total = Mutex<Int>(0)
Lock total As s                 // guard released at End Lock
    s += 10
End Lock

Global requests = Atomic<Int>(0)
requests.Add(1)
```

### Channel

- `Channel<T>(capacity)` makes a Channel of a Sendable T with room for that many, 1 or more. It's
  Sendable itself, so tasks share one, through a Const or a parameter.
- `ch.Send(value)` waits while it's full, and panics once it's closed. `ch.TrySend(value)` gives
  False instead of waiting, when it's full or closed.
- `ch.Receive()` waits while it's empty, and gives a `T?`, which is Null once it's closed and
  empty. `ch.TryReceive(ByRef value)` gives False instead of waiting, and sets the variable when
  it gives True; `ch.TryReceive()` gives a `T?`, Null when there's nothing, as Receive gives.
- `ch.Close()` lets the receivers take what's left. `For x In ch` receives until the Channel is
  closed and empty. `ch.IsClosed()` says whether it's closed, though it may have values left, so
  code that waits on several Channels at once, with TryReceive, knows when one won't get more.
- Values come out in the order they went in, and what's left when the Channel goes is released.

### Atomic and Mutex

- An `Atomic<T>` holds an integer or a Bool. `Load()`, `Store(value)`, `Exchange(value)`, and
  `CompareExchange(expected, desired)`, which says whether it held `expected` and now holds
  `desired`, work on it at once, in one order every thread sees. An integer's also has
  `Add(amount)` and `Subtract(amount)`, which wrap around. Exchange, Add, and Subtract give what
  it held before.
- A `Mutex<T>` holds a Sendable T. Its value is used only in `Lock m As value ... End Lock`,
  where `value` is the Mutex's own, like a ByRef parameter, which a lambda can't capture. Another
  thread's Lock of it waits until the block is done, however it ends: at End Lock, or by Return,
  Break, Continue, or an error. A thread that Locks a Mutex it holds already panics, since it
  would wait for itself forever.
- Both stay where they're made, so every thread shares the one: a Global, a Static Field, or a
  Class's Field. A Struct is copied, so it can't hold one. Code can pass one ByRef, but can't
  copy, assign, or give one back, and neither can be part of another type, like an array.
- `Atomic<Int>(0)` and `Mutex<Stats>(Stats())` make one, as what a Global or Field starts with.
  Without a type argument, the T is the value's type: `Mutex(Stats())`. A Field of one starts at
  its zero, or an Atomic's at a value known at compile time; New can change a Mutex Field's value
  in a Lock block.
- Both are Sendable. A For Field loop leaves them out, since `f.Get(value)` would copy one.

## Safety: data races are compile errors

**Rule: an object belongs to one thread unless its type says otherwise.**

| Kind of data | Thread rule | Refcount |
|---|---|---|
| Normal classes | stay on one thread | non-atomic (fast) |
| `Sendable` data | may cross threads | n/a |
| `Shared Class` | may cross threads; fields must be `Atomic`, `Mutex`, or `ReadOnly` | atomic |

`Sendable` covers: primitives, `String`s (immutable, with atomic refcounts), `Struct`s (copied)
whose fields are all Sendable, `ReadOnly`/frozen data, `Channel`, `Mutex`, `Atomic`, `Callback`,
and `Shared Class` instances.

```vb
Shared Class Hub
    ReadOnly Field Title: String
    Field Visits: Atomic<Int>
    Field Log: Mutex<String>
End Class
```

- A `Shared Class`'s own Fields are Atomic, Mutex, or ReadOnly, and a ReadOnly one is Sendable,
  so no two threads change one at once. Its Methods can run on any thread.
- It extends only a Shared Class, and only a Shared Class extends it, so a variable of one always
  holds a Shared Class's object. A generic one says its type parameters are Sendable, as in
  `Shared Class Box<T: Sendable>`, for a ReadOnly Field of type T.
- Its objects count their owners atomically, Weak references too, and whichever thread lets go
  last runs its Destroy(). An Interface's value can hold either kind of object, so the runtime
  tells them apart by their Class.

The compiler checks every `Spawn` capture, `Parallel For` body, and channel send.

- `Sendable` is an Interface in std/core/interfaces.fb that the compiler decides from a type's
  parts: numbers, Bools, Strings, Chars, Enums, Callbacks, Sendable Funcs, Channels, Mutexes,
  Atomics, and a Shared Class's objects are Sendable, and so is a `Weak<T>` of a Shared Class,
  whose objects count Weak references atomically too, or of a type parameter that's `Class &
  Sendable`, which stands for one, and so are the Structs, tuples, and fixed arrays whose fields
  and elements all are, and a `T?` of any of them. A Ptr or CString isn't, since nothing keeps
  what it points at for another thread.
- Classes, arrays, `Object`, other function values, and other `Weak<T>`s aren't, since an
  object's refcount isn't atomic: an object stays on the thread that made it. Neither is a Struct that
  holds one.
- Generic code says a type parameter is Sendable with a constraint, `<T: Sendable>`, and an
  instance like `Box<Int>` is Sendable when its fields are.
- No type has to list it. One that does, `Struct Packet Implements Sendable`, or that lists an
  Interface extending it, has the compiler check that its fields are; a generic one needs
  `<T: Sendable>` for a field of type T.
- It's only a constraint, so it isn't a type of its own. An Interface that extends it can be
  one.
- An error says which part isn't: "Holder isn't Sendable, which 'Send' needs for T: its field
  'Target' is Session?, and a Class's objects stay on the thread that made them".

**Captures across threads.** Closures capture by reference ([05](05-functions.md#closures)), so
sharing a mutable `Local` with another thread would be a data race. A closure passed to `Spawn`
or `Thread.Start`, the body of a `Parallel For`, or a lambda that's a Sendable Func, can only
capture what can't change, holding a `Sendable` value: a `Const`, a parameter, a loop variable,
or a Self that's Sendable.

```vb
Const session = Session("ada")
Spawn(Function() => session.Refresh())
// ✗ compile error: Session is not Sendable

Const id = session.id                   // a String
Spawn(Function() => FetchProfile(id))   // ✓

Local user = session.id
Spawn(Function() => FetchProfile(user))
// ✗ compile error: a Local can't be captured by another thread; make it a Const
```

### Main-thread rules

- APIs bound to the main thread, like a UI toolkit's windows and widgets, are marked
  `@MainThread`. Calling one from work on another thread is a compile error.
- Mutable `Global`s are main-thread-only unless they are `Atomic`, `Mutex`, or `ThreadLocal`, each
  thread's own (see [02](02-types.md#threadlocal-globals)).

```vb
@MainThread Function SetTitle(text: String)
    ...
End Function

Spawn(Function() => SetTitle("done"))   // ✗ compile error: @MainThread
```

- Work on another thread is a Spawn's or Thread.Start's, a Sendable Func's, or a Callback's
  code, which C can call on any thread (see [12](12-ffi.md#callbacks)), or a ThreadLocal
  Global's initializer, which runs on each thread that uses it (see
  [02](02-types.md#threadlocal-globals)), with everything it can run: what it calls, its lambdas,
  what overrides the Methods it calls, and what implements the Interface Methods it calls, with
  each New's Class's Destroy(). A Shared Class's Destroy() is too, since whichever thread lets go
  last runs it.
- That code can't use a Global or Static Field unless it's an Atomic, a Mutex, a ThreadLocal
  Global, or a Static ReadOnly Field of a Sendable type, which keeps the value it starts with, or
  a top-level Const that isn't Sendable, or call a `@MainThread` Function or Method.
  Async code, the timers, Schedulers, and `Scheduler.Main` are the main thread's too. A
  top-level Const that's Sendable is fine, and so is all of it in top-level code, which runs on
  the main thread.
- The error says how the work gets there: "this work runs on another thread, but it calls
  'Bump', which uses the Global 'total', which only the main thread can, unless it's an Atomic or
  a Mutex".

## Beyond the language

Lock-free collections, actors, and GPU compute aren't part of the language: libraries can
provide them with what this chapter describes.
