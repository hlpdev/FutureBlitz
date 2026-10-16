# 10 · Concurrency

## The model

Games need concurrency for three different jobs. Each gets its own tool:

| Job | Example | Tool | Runs on |
|---|---|---|---|
| Do heavy work faster | pathfinding, particles, mesh building | `Spawn`, `Parallel For` | worker threads, in parallel |
| Wait without freezing | loading a file, network request | `Async` / `Await` | main thread; waiting costs nothing |
| Do things over time | door opens, waits 2s, closes; cutscenes | `Async` coroutines | main thread, resumed each frame |

Jobs 2 and 3 are the same mechanism: a coroutine is an `Async` function that awaits time
instead of I/O.

## Thread layout

```
MAIN THREAD     user's loop: input → update → UpdateWorld → RenderWorld → Flip → resume coroutines
                (owns the engine: entities, rendering, audio)

WORKER POOL     [task][task][parallel-for chunk][task]...      one thread per core, minus 1
                (pure computation, no engine calls)

DEDICATED       Thread.Start(...) for long-running blocking loops (e.g. a network listener)
THREADS         so they don't tie up a pool worker
```

Most game code never leaves the main thread.

## Tasks and parallel loops

```vb
Local t: Task<Mesh> = Spawn(Function() => BuildNavMesh(level))
...
Local nav = t.Wait()                 // blocks; re-throws the task's error

Parallel For i = 0 Until particles.Count
    particles[i].Step(dt)            // iterations must be independent
End For
```

- Work-stealing thread pool in the runtime.
- `Thread.Start(fn)` for long-lived blocking work.

## Async / Await

Stackless coroutines on the same scheduler.

```vb
Async Function LoadLevel(path: String): Level Throws FileError
    Const bytes = Await File.ReadAsync(path)                   // game keeps running
    Local nav = Await Spawn(Function() => BuildNavMesh(bytes)) // heavy part on a worker
    Return Level(bytes, nav)                           // back on the main thread
End Function
```

## Coroutines

```vb
Async Function DoorSequence(door: Door)
    door.Open()
    Await Seconds(2)
    door.Close()
End Function

DoorSequence(door)    // calling without Await starts it on the main scheduler
```

### Scheduling (Proposed: recommended, not explicitly confirmed)

The user owns the main loop, so the frame boundary is `Flip()`.

**Main scheduler, ticked by `Flip()`.** After presenting, `Flip()` resumes ready
coroutines, so they run at the start of the next frame before update code. Nothing extra
goes in the loop:

```vb
Graphics3D(1280, 720)
DoorSequence(door)

While !KeyHit(KEY_ESCAPE)
    UpdateGame()
    UpdateWorld()
    RenderWorld()
    Flip()                      // coroutines resume here
End While
```

**Custom schedulers, ticked manually**, for pausing and time scaling:

```vb
Global gameplay = Scheduler()
gameplay.Run(DoorSequence(door))

While running
    If !paused Then gameplay.Tick(dt * timeScale)
    UpdateWorld()
    RenderWorld()
    Flip()                      // main scheduler (UI coroutines) still runs
End While
```

Rules:

- `Await NextFrame()` resumes at the next tick of the owning scheduler.
- `Await Seconds(n)` uses the scheduler's time (respects pause and time scale).
  `Await RealSeconds(n)` ignores both.
- An I/O await resumes on the scheduler that started it, at its next tick. Coroutine code
  never runs on a background thread.
- `Await MainThread()` hops back to the main thread.

## Communication

| Tool | Use |
|---|---|
| `Channel<T>(capacity)` | one thread sends, another receives |
| `Mutex<T>` | shared data behind a lock |
| `Atomic<T>` | shared counters and flags |

```vb
Const ch = Channel<Packet>(64)
Spawn(Function() => NetLoop(ch))
While ch.TryReceive(pkt)
    Handle(pkt)
End While

Global score = Mutex<Int>(0)
Lock score As s                 // guard released at End Lock
    s += 10
End Lock

Global frames = Atomic<Int>(0)
frames.Add(1)
```

## Safety: data races are compile errors

**Rule: an object belongs to one thread unless its type says otherwise.**

| Kind of data | Thread rule | Refcount |
|---|---|---|
| Normal classes | stay on one thread | non-atomic (fast) |
| `Sendable` data | may cross threads | n/a |
| `Shared Class` | may cross threads; fields must be `Atomic`, `Mutex`, or `ReadOnly` | atomic |

`Sendable` covers: primitives, `Struct`s (copied; see the open question below), `ReadOnly`/
frozen data, `Channel`, `Mutex`, `Atomic`, and `Shared Class` instances.

The compiler checks every `Spawn` capture, `Parallel For` body, and channel send.

**Captures across threads (Proposed: recommended, not explicitly confirmed).** Closures
capture by reference ([05](05-functions.md#closures)), so sharing a mutable `Local` with another
thread would be a data race. A closure passed to `Spawn`, or the body of a `Parallel For`, can
only capture `Const`s holding `Sendable` values.

```vb
Const enemy = Enemy("grunt")
Spawn(Function() => enemy.Think())
// ✗ compile error: Enemy is not Sendable

Const pos = enemy.pos                   // a read-only Vec3
Spawn(Function() => ComputePath(pos))   // ✓

Local target = enemy.pos
Spawn(Function() => ComputePath(target))
// ✗ compile error: a Local can't be captured by another thread; make it a Const
```

**Open:** is a `Struct` that holds a class reference `Sendable`? Recommended: a struct is
`Sendable` only when all its fields are, so it isn't (see
[open-questions.md](../open-questions.md)).

### Main-thread rules

- Engine APIs (rendering, audio, entities) are marked `@MainThread`. Calling them from a
  worker is a compile error.
- Mutable `Global`s are main-thread-only unless they are `Atomic` or `Mutex`.

```vb
Spawn(Function() => PositionEntity(cam, 0, 0, 0))   // ✗ compile error: @MainThread
```

## Scope for v1

- **Ship:** `Spawn`, `Task`, `Parallel For`, `Async`/`Await` + coroutines, `Scheduler`,
  `Channel`, `Mutex`, `Atomic`, `Thread.Start`, Sendable checking.
- **Defer:** lock-free collections, actors, GPU compute from the language. These can be
  added as libraries later without language changes.
