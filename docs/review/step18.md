# The review of M6 step 18's library half: std.concurrent's locks, ConcurrentMap, ConcurrentQueue, TaskSource, WhenAll, WhenAny, and std.events' Event and Subscription (the parallel algorithms, Lazy, Subscribe(owner, handler), Select's timeout, Cancellation, std.log, and Task<T> Throws E as a type aren't in this change)

9 confirmed findings: 2 high, 4 medium, 3 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S18-tasks-events-1 (high): Cancelling a Subscription whose handler holds the only reference to an object that cancels it again in its Destroy is a use-after-free

Status: fixed in ee9b809: Drop keeps the handler in a Local until its Event has counted it off, so a Destroy that its going runs finds it off, cancelling it again does nothing, and the Event's Count is right; events.fb has a Widget that cancels its own Subscription as it goes, through Cancel and through Clear. That doesn't need S18-tasks-events-2's fix, lane 2's 1e83d59, which fixes the root cause

Where: std/events/event.fb:30

EventHandler.Drop runs `Handler = Null`. The compiler's store (see S18-tasks-events-2) releases the old value before it assigns the new one. So the closure is destroyed while Handler still points at it, and the Widget it captured goes too. The Widget's Destroy calls Mine?.Cancel(), which reaches Drop again. Drop sees Handler as non-null (it points at the freed closure), and its `Handler = Null` releases freed memory a second time. This is a common pattern: an object keeps its own Subscription and cancels it as it goes, while its handler captures it. Clear() goes through the same Drop and crashes the same way. A variant that doesn't crash shows the dangling field: during the Destroy, `Mine?.IsActive` gives True. Over 20 runs the program below crashed every time in both debug and release. selfcancel.fb, a lighter version, crashed 8 of 20 runs in debug and 5 of 20 in release. events.fb has no case where a handler's object cancels in Destroy.

```
Import std.core
Import std.events

// A Widget cancels its own Subscription as it goes, and only its handler holds it.
Class Widget
    Field Name: String
    Field Mine: Subscription?

    New(name: String)
        Name = name
    End New

    Method Destroy()
        Local note = "{Name} goes, active {Mine?.IsActive ?? False}"
        Mine?.Cancel()
        If Name == "w1" Then Print(note)
    End Method
End Class

Function Attach(event: Event<Int>, name: String): Subscription
    Const widget = Widget(name)
    Local subscription = event.Subscribe(Function(n: Int) => Print("{widget.Name} hears {n}"))
    widget.Mine = subscription
    Return subscription
End Function

Const ticks = Event<Int>()
For i = 1 To 100
    Attach(ticks, "w{i}").Cancel()
End For

Print("cancelled", ticks.Count)
```

Expected: w1 goes, active False
cancelled 0

Actual: error: uaf.exe exited with code 0xC0000005 (exit 139, nothing printed). This happens in both debug and release, in 20 of 20 runs.

Suggested fix: In EventHandler.Drop, hold the handler in a Local before clearing the Field, so the closure goes only after Handler is Null: `If Handler == Null Then Return` / `Local handler = Handler` / `Handler = Null` / `Owner?.Dropped()`. I tested this fix in a scratch copy of event.fb: the program then printed the expected output in 20 of 20 runs in both builds, and Destroy saw IsActive False. Also add this case to events.fb, both through Cancel and through Clear. Fix the compiler's store order too (S18-tasks-events-2).

Skeptic: I ran the program as given (s18/te/uaf.fb). It crashed 3 of 3 times in debug and 3 of 3 in release: 'uaf.exe exited with code 0xC0000005', exit 139, nothing printed. A variant whose Widget.Destroy only prints `Mine?.IsActive` and doesn't cancel (uaf_clear.fb) printed 'a goes, active True' through Cancel and 'b goes, active True' through Clear. So the Destroy sees the handler as still on its Event while it's being cancelled. event.fb:28-32 shows the cause: Drop runs `Handler = Null` while the closure it releases is still in Handler. I copied event.fb into the program under renamed classes with the suggested fix (`Local handler = Handler` before `Handler = Null`), in uaf_fixed.fb. That printed 'w1 goes, active False / cancelled 0 / attached 5 / cleared 0' with exit 0 in both builds, through Cancel and through Clear.

## S18-tasks-events-2 (high): Assigning a Field releases the old value before storing the new one, so a Destroy that the release runs sees a freed object in that Field (root cause of S18-tasks-events-1)

Status: fixed in 1e83d59 (a store puts the new value in before it lets go of the old one, so a destroy finds the new one), on lane 2's dev/m6-p1-lowering, with tests/programs/store_destroy.fb and a case in tests/compiler/mir_tests.cpp; a Channel's TryReceive into its ByRef variable gets the same order

Where: compiler/src/lower.cpp:2879

This bug was there before this change and isn't in step 18's files; it is the root cause of S18-tasks-events-1. lower.cpp's store() emits release_t on the target and then assign_t. If releasing the old value runs a Destroy, that Destroy reads the Field and finds the object being destroyed still there. If the Destroy assigns the Field again, the same object is released and destroyed again, without end, until the stack overflows. When the release is deferred, as in S18-tasks-events-1, the Field holds a pointer to freed memory instead.

```
Import std.core

Class Thing
    Field Name: String

    New(name: String)
        Name = name
    End New

    Method Destroy()
        Print("{Name} goes; the holder has {holder.Item?.Name ?? "nothing"}")
        holder.Item = Null
        Print("{Name} gone")
    End Method
End Class

Class Holder
    Field Item: Thing?
End Class

Global holder = Holder()
holder.Item = Thing("a")
holder.Item = Null
Print("done")
```

Expected: a goes; the holder has nothing
a gone
done

Actual: "a goes; the holder has a" about 1500 times, then the program dies with exit code 127 (stack overflow) and never prints "a gone" or "done". This happens in both debug and release.

Suggested fix: In store(), copy the old value into a temporary, assign the new value, and only then release the temporary. A Destroy that the release runs then sees the new value. Add a program test like this one.

Skeptic: store.fb printed 'a goes; the holder has a' over and over: about 1800 times in release. It then died with 0xC00000FD (stack overflow, exit 127) in both debug and release, and never printed 'a gone' or 'done'. compiler/src/lower.cpp store() (around lines 2878-2893) emits `release_t{ .target = target }` and then `assign_t`. A Destroy that the release runs therefore finds the dying object still in the Field, and assigning that Field again releases it a second time. Spec 08 says Destroy runs when the refcount reaches 0, so a Field the program can still reach must not keep pointing at the object. This bug is older than step 18, but it is the root cause of finding 1.

## S18-collections-1 (medium): ConcurrentQueue.Count goes below 0 while threads use it

Status: fixed in 9cd993c: Enqueue adds to Size inside its Lock; concurrent_queue.fb's Counted has four threads read Count while one enqueues 100000 values, which read below 0 hundreds of thousands of times before

Where: std/concurrent/concurrent_queue.fb:45

Enqueue adds 1 to Size after End Lock, but TryDequeue subtracts 1 inside the lock. A consumer can take a value in that window, before the producer has counted it, so Count reads -1. More producers can push it lower, and Clear has the same race, since it subtracts values.Count inside the lock. A count of values is never negative, and code that sizes something from Count (Reserve, For i = 0 Until queue.Count, and so on) gets a wrong number. The step's queue test only reads Count after every thread has joined, so it never sees this. Reproduces 5 times out of 5 in debug and 5 out of 5 in release.

```
// A ConcurrentQueue's Count is how many values it holds, so it's never below 0.
Import std.core
Import std.concurrent

Shared Class Tally
    Field Lowest: Atomic<Int>
    Field Producing: Atomic<Int>
End Class

Const queue = ConcurrentQueue<Int>()
Const tally = Tally()
tally.Producing.Store(1)
Local threads: Thread[] = []
threads.Add(Thread.Start(Function()
    For i = 0 Until 300000
        queue.Enqueue(i)
    End For
    tally.Producing.Subtract(1)
End Function))
For t = 1 To 4
    threads.Add(Thread.Start(Function()
        Loop
            Local value = queue.TryDequeue()
            Local count = queue.Count
            If count < tally.Lowest.Load() Then tally.Lowest.Store(count)
            If value == Null && tally.Producing.Load() == 0 && queue.Count <= 0 Then Break
        End Loop
    End Function))
End For
For thread In threads
    thread.Join()
End For
Print("never below 0: {tally.Lowest.Load() >= 0}")
```

Expected: never below 0: True

Actual: never below 0: False (every run, in debug and in release)

Suggested fix: In Enqueue, move Size.Add(1) inside the Lock block, next to values.Count += 1, so Size changes only under the lock, as it already does in TryDequeue and Clear. Add a threaded test that checks Count is never below 0, like the program here.

Skeptic: Ran the finding's program as C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18/cmq-verify/q_count.fb, 3 times in debug and 3 in release. Every run printed "never below 0: False" and "lowest: -1, final 0". The code matches the description. In concurrent_queue.fb:37-45, Enqueue raises values.Count inside the Lock but calls Size.Add(1) only after End Lock (line 45). TryDequeue calls Size.Subtract(1) inside the Lock (line 67). So a consumer can take the value and subtract before the producer has counted it, and Count reads -1. ConcurrentMap doesn't have this race: its Add increments inside the lock and its Remove decrements after. A negative Count also breaks callers: `a.Reserve(-1)` panics ("Reserve takes a capacity that isn't negative, not -1"), which I checked in reserve.fb. Fix: move Size.Add(1) inside the Lock.

## S18-collections-2 (medium): Values are destroyed while the map's part lock or the queue's lock is held, so a Destroy that uses the same map or queue panics

Status: fixed in 0824491: Add, Update, AddOrUpdate, Remove, and Clear keep the entries they replace in a Local made before the Lock, and ConcurrentQueue.Clear its values, so what goes goes after End Lock, and RwLock.Write, which let go of its old value under its locks too, does the same; concurrent_map.fb (a value whose Destroy calls Keys, which locks every part, through each change), concurrent_queue.fb, and concurrent_rwlock.fb have the cases

Where: std/concurrent/concurrent_map.fb:175

Remove, Add when it replaces a value, Update, AddOrUpdate and Clear all overwrite `entries` inside `Lock part.Entries`. That lets go of the old ImmutableMap, and with it the replaced or removed MapCell. If nothing else holds the value, its Destroy() runs while the part's Mutex is still locked. If that Destroy touches the same ConcurrentMap (for example, a node that removes its child's entry), the result depends on the keys' hash bits. When both keys are in the same one of the 16 parts, the Lock re-locks a Mutex the thread already holds, which panics. When they are in different parts it works. It can also deadlock across threads, if two threads each hold one part and want the other's. Users can't see or avoid the hidden lock. ConcurrentQueue.Clear (concurrent_queue.fb:116) does the same thing, `values = QueueValues<T>()` inside the Lock. This program panics too: `Shared Class Job` with `ReadOnly Field Queue: Weak<ConcurrentQueue<Job?>>` whose `Method Destroy()` does `Local queue = Queue` / `If queue != Null Then queue.Enqueue(Null)`, followed by `queue.Enqueue(Job(queue))` and `queue.Clear()`. It panics with "this thread holds the Mutex already" in both debug and release.

```
// A value whose Destroy removes another key from the same ConcurrentMap: removing it works
// whichever of the 16 parts the two keys fall in.
Import std.core
Import std.concurrent

Shared Class Node
    ReadOnly Field Registry: Weak<ConcurrentMap<Int, Node>>
    ReadOnly Field Child: Int

    New(registry: ConcurrentMap<Int, Node>, child: Int)
        Registry = registry
        Child = child
    End New

    Method Destroy()
        Local registry = Registry
        If Child >= 0 && registry != Null
            Print("removed child {Child}: {registry.Remove(Child)}")
        End If
    End Method
End Class

// Keys 1 and 2 hash into different parts, and keys 4 and 5 into the same one.
Print(1.Hash() >> 60, 2.Hash() >> 60, 4.Hash() >> 60, 5.Hash() >> 60)
Const registry = ConcurrentMap<Int, Node>()
registry.Add(2, Node(registry, -1))
registry.Add(1, Node(registry, 2))
registry.Add(5, Node(registry, -1))
registry.Add(4, Node(registry, 5))
registry.Remove(1)
registry.Remove(4)
Print(registry.Count)
```

Expected: 5 13 11 11
removed child 2: True
removed child 5: True
0

Actual: 5 13 11 11
removed child 2: True
panic: this thread holds the Mutex already, so Lock would wait for itself forever at std/concurrent/concurrent_map.fb:173:14
(exit code 101, in debug and in release)

Suggested fix: Let go of what a change replaces only after End Lock. In each changing method, declare a Local before the Lock, for example `Local old = ImmutableMap<K, MapCell<V>>()`, and set `old = entries` inside the block before overwriting `entries`, so the old nodes and cells are released when the method returns, after the lock. In ConcurrentQueue.Clear, move the QueueValues into a Local inside the Lock and let it go after End Lock. Add the program here, and the queue variant, as tests.

Skeptic: Ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18/cmq-verify/map_destroy.fb, the finding's program. Debug and release both printed "5 13 11 11" and "removed child 2: True" (keys in different parts work). Both then panicked with "this thread holds the Mutex already, so Lock would wait for itself forever at std/concurrent/concurrent_map.fb:173:14" and exit 101 (keys 4 and 5 share part 11). The queue variant, queue_destroy.fb (Job whose Destroy enqueues, then queue.Clear()), panics the same way in debug and release. The cause is concurrent_map.fb:175 (`entries = entries.Remove(key)` inside `Lock part.Entries`) and concurrent_queue.fb:116 (`values = QueueValues<T>()` inside the Lock): the old ImmutableMap or ring is released while the hidden lock is held. Whether a Destroy may touch the map then depends on the keys' hash bits, which users can't see, and stdlib.md gives no warning. Two threads each holding one part could also deadlock. Related but older: a plain std.collections Map in the same pattern (plain_map_destroy.fb) also panics, with "index -1 is out of range for 4 elements at std/collections/map.fb:352:15". That's a separate step-3 bug in Unlink, not part of this change.

## S18-collections-4 (medium): A copy of a ConcurrentMapIterator shares its position, so a consumer moves the original

Status: fixed in f5678b0: ImmutableArrayIterator's and ImmutableMapIterator's paths are linked lists of ArrayPath and HashPath nodes, which Next replaces rather than changes; immutable.fb has ImmutableArray, ImmutableMap, and ImmutableSet, and concurrent_map.fb ConcurrentMap, each partly used, under Count twice and a For ... In. SortedMap's and SortedSet's, Trie's, and IntervalTree's iterators keep arrays too, and a copy moves them the same way (Next, then Count twice, gives 49 0); they're step 3's and step 4's, and aren't fixed here

Where: std/concurrent/concurrent_map.fb:283

docs/spec/04-control-flow.md says a For loop or consumer on a Struct Iterator goes over a copy, so the Struct doesn't move. ConcurrentMapIterator keeps its current part's ImmutableMapIterator, and that iterator keeps its path in a `HashNode<K, V>[] Nodes` array, which a copy shares by reference. So it.Count(), or a For loop over `it`, drains the original's path stack, and the next use of `it` skips entries. The root cause is in ImmutableMapIterator (std/collections/immutable.fb:597, from step 4). ImmutableArrayIterator has the same problem: `it.Next()` followed by `Print(it.Count(), it.Count())` prints `49 0` for both. ConcurrentMap's new public iterator inherits it. Map's own MapIterator behaves correctly. Same in debug and release.

```
// A consumer on a Struct Iterator goes over a copy of it, so the Iterator doesn't move.
Import std.core
Import std.concurrent

Const map = ConcurrentMap<Int, Int>()
For i = 0 Until 50
    map.Add(i, i)
End For

Local it = map.Iterate()
it.Next()
Print(it.Count(), it.Count())
```

Expected: 49 49

Actual: 49 45

Suggested fix: Make ImmutableMapIterator's (and ImmutableArrayIterator's) path a persistent stack that is never changed in place, such as a Shared Class linked list of nodes that Next replaces instead of popping, so copying the Struct copies its position. ConcurrentMapIterator then needs no change. Add a test that runs a consumer on a partly used iterator for each of the three.

Skeptic: Ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18/cmq-verify/itcopy.fb in debug and release. For a ConcurrentMap of 50 entries, `it.Next(); Print(it.Count(), it.Count())` printed "49 45". A For loop over a partly used iterator, then Count, also printed "49 45". Map's MapIterator printed "49 49", and ImmutableMap's iterator printed "49 0". Spec 04-control-flow.md:75 and :96 say a For loop or consumer on a Struct goes over a copy of it, "so the Struct doesn't move". The cause: ConcurrentMapIterator (concurrent_map.fb:267) holds an ImmutableMapIterator, whose `Nodes: HashNode<K, V>[]` (immutable.fb:597) a copy shares by reference. A copy's Next pops the original's path, so the original skips the rest of its current part. The root cause predates this change (ImmutableMapIterator, step 4), but this step's new public iterator breaks the spec rule.

## S18-tasks-events-3 (medium): A TaskSource's Task isn't done once Finish gives the result; IsDone() stays False until the main thread ticks

Status: fixed in 2f57da0, by the docs: a Task read once Finish has given the result is done at once, since Finished awaits only a signal that isn't given yet, and stdlib.md and the Task property's comment say one read before is done at the main thread's first tick after Finish, as an Async Function's Task is once what it awaits is done; concurrent_tasks.fb has the case. Finishing every Task at Finish needs the runtime to write a T into a Task<T>, which an Extern, having no type parameters, can't; that can come with a TaskSource over throwing Tasks

Where: std/concurrent/tasks.fb:29

docs/stdlib.md (line 465) and the Task property's comment both say the Task is done once Finish gives the result. But the Task that `Task` gives is the Task of the Async Function Finished, which Awaits the signal's Task. An Await always suspends until a later tick. So after Finish, every Task read from the source reports IsDone() False until the main scheduler ticks, and that includes a Task read after Finish. Before Flip exists, the main scheduler ticks only at Wait, an Await, or the end of top-level code. So main-thread code that polls `task.IsDone()` while a Thread finishes the source never sees True and loops forever. A Spawn's Task in the same loop would see True.

```
Import std.core
Import std.concurrent

Const source = TaskSource<Int>()
Local before = source.Task
source.Finish(5)
Local after = source.Task
Print(source.IsDone, before.IsDone(), after.IsDone())
Print(after.Wait(), before.IsDone(), after.IsDone())
```

Expected: True True True
5 True True

Actual: True False False
5 True True
(the same in debug and release)

Suggested fix: Either make the Task the signal's own Task, so Finish completes it directly (the runtime would write the result into it, as fb_io_input_async does), or change the docs and the comment to say what happens: the Task is done at the main thread's next tick after Finish, as an Async Function's is, so IsDone() before then is False. Add the IsDone-after-Finish case to concurrent_tasks.fb.

Skeptic: tsdone.fb printed 'True False False' and then '5 True True', the same in debug and release. So after Finish(5), even a Task read after the Finish has IsDone() False. A polling variant (tspoll.fb, release) has a Spawn call Finish(7) while the main thread loops on task.IsDone(). It printed 'still waiting; source done True' three times (300M iterations), then broke out with 'task done False 7'. The same loop over a Spawn's Task ends at once (spawndone.fb). This contradicts stdlib.md line 465 ('its Task is done once Finish(value) ... gives the result') and the Task property's comment in tasks.fb:27. The cause is that `Task` gives Finished(...), an Async Function that Awaits the signal's Task, so it is done only at a later tick.

## S18-collections-3 (low): PartOf takes the hash's raw top 4 bits, so keys with an Id-style Hash all share part 0 and one Mutex

Status: fixed in 74c1dee: PartIndex mixes the hash as Map's BucketOf does before it takes the top bits; std_tests.cpp's "a ConcurrentMap's keys spread over its parts" puts 64 hashes that differ in their low, middle, or high bits in all 16 parts, where they went in 1, 1, and 16 before, and Iterate's comment and stdlib.md say it goes part by part, and by hash in a part. concurrent_map.fb's race of adds and removes had a thread's TryAdd come after another's Remove, which this showed, so its threads now wait at a Barrier between the two (e35b4f2)

Where: std/concurrent/concurrent_map.fb:230

PartOf is `(key.Hash() >> 60) As Int`, with no mixing. Map.BucketOf mixes the hash first, `(hash ^ (hash >> 32)).WrappingMultiply(0x9E3779B97F4A7C15)`, and its comment gives the reason: "so hashes that differ only in their high bits, or only in their low ones, spread out alike". A user type whose own Hash is a small number, like `Method Hash(): UInt64 => Id As UInt64` (a common way to write one), has 0 in its top 4 bits for every key below 2^60. Every entry then lands in part 0, behind a single Mutex. That defeats the design the file and docs/stdlib.md describe, where threads that change different parts don't wait for each other. Measured with release builds: eight threads updating disjoint keys took 17.2 s and 19.2 s with the raw Id hash. The same program with the hash mixed as Map mixes it took 2.5 s and 3.9 s. Results are correct either way; only the parallelism is lost.

```
// Eight threads each change keys of their own in one ConcurrentMap. A key's Hash is its Id, as
// user types' often are. Time it against the same program with
// `Method Hash(): UInt64 => (Id As UInt64).WrappingMultiply(0x9E3779B97F4A7C15)`.
Import std.core
Import std.concurrent

Struct Id Implements Hashable
    Field Id: Int

    Method Hash(): UInt64 => Id As UInt64
End Struct

Const map = ConcurrentMap<Id, Int>()
Local threads: Thread[] = []
For t = 0 Until 8
    threads.Add(Thread.Start(Function()
        For i = 0 Until 200000
            Local key = Id(t * 1000 + i % 1000)
            map.AddOrUpdate(key, 1, Function(n: Int) => n + 1)
        End For
    End Function))
End For
For thread In threads
    thread.Join()
End For
Print(map.Count, map[Id(0)])
```

Expected: 8000 200, in about the time the mixed-hash version takes (2.5 s to 3.9 s with --release here), since the threads' keys are spread over 16 parts

Actual: 8000 200, but 17.2 s to 19.2 s with --release: every key is in part 0, so all eight threads wait on one Mutex

Suggested fix: Mix the hash before taking the part, as Map.BucketOf does, for example `Local mixed = (hash ^ (hash >> 32)).WrappingMultiply(0x9E3779B97F4A7C15)` and then `(mixed >> PART_SHIFT) As Int`. Iteration then goes part by part, and by hash within a part, so update the Iterate comment and docs/stdlib.md's "by hash" wording to match.

Skeptic: concurrent_map.fb:230 is `(key.Hash() >> PART_SHIFT) As Int` with no mixing, while Map.BucketOf (map.fb:338) mixes the hash first. Spec 07's Hashable only asks for a UInt64, so `Method Hash(): UInt64 => Id As UInt64` is valid, and every such key goes to part 0. Built-in hashes are already mixed: Int, Bool, Enum, UInt8, Float, String, and auto Struct hashes spread over the parts (hashes.fb). I built both versions in release (parts_raw.fb, parts_mixed.fb) and timed 8 threads: the raw hash took 14.0 s and 11.4 s, the mixed one 4.1 s and 3.0 s. Single-threaded, the same 1.6M operations took about 10 s with either hash (one_raw 10.3 s and 10.3 s, one_mixed 8.7 s and 10.0 s). So raw-hash keys get no parallel speedup at all: one Mutex serializes every thread. Results were correct every time (8000 200). That defeats the design's stated aim that threads changing different parts don't wait for each other. It's low because only performance suffers, and stdlib.md literally says "by the top bits of their hashes".

## S18-tasks-events-4 (low): WhenAny's tie rule doesn't hold: when two tasks are done in the same tick, a later one can win

Status: fixed in 566f2cb, by the docs: WhenAny looks at its tasks in their order at a tick, so of tasks done before it looks the first given wins, and one done later in the tick doesn't count. Deciding after the tick can't keep the rule either, since a decider that resumes at the next tick would count tasks done in that tick before it. concurrent_tasks.fb has the finding's case, which gives 1

Where: std/concurrent/tasks.fb:126

The docs (stdlib.md line 469) and WhenAny's comment say: of tasks done by the same tick of the scheduler, the first in the array wins. But each FirstOf waiter calls TryFinish as soon as the scheduler resumes it, and resumption goes in m_waiting order. A task can be finished by a coroutine that comes after WhenAny's waiters in m_waiting, for example one that started waiting again after WhenAny was called. That task is done later in the same tick, after its waiter was already checked. Meanwhile a later task, finished by a coroutine earlier in the order, has already won. In the program below, both `slow` (index 0) and `fast` (index 1) are done by tick 2, and WhenAny gives 1. concurrent_tasks.fb tests ties only in the order where the docs' rule happens to hold.

```
Import std.core
Import std.concurrent

Global slow: Task<Int>? = Null
Global fast: Task<Int>? = Null
Global first: Task<Int>? = Null

Async Function Later(frames: Int, value: Int): Int
    For i = 1 To frames
        Await NextFrame()
    End For

    Return value
End Function

Async Function Run()
    Await NextFrame()
    fast = Later(1, 1)
    first = WhenAny([slow.Expect("slow is set"), fast.Expect("fast is set")])
End Function

Const s = Scheduler()
Local run = Run()
slow = Later(2, 0)
s.Run(run)
s.Run(slow.Expect("slow is set"))
For tick = 1 To 4
    s.Tick(0)
    Local slowDone = slow.Expect("slow is set").IsDone()
    Local fastDone = fast?.IsDone() ?? False
    Local anyDone = first?.IsDone() ?? False
    Print("after tick {tick}: slow {slowDone}, fast {fastDone}, WhenAny {anyDone}")
End For

Print("WhenAny gives", first.Expect("first is set").Wait())
```

Expected: after tick 1: slow False, fast False, WhenAny False
after tick 2: slow True, fast True, WhenAny True
after tick 3: slow True, fast True, WhenAny True
after tick 4: slow True, fast True, WhenAny True
WhenAny gives 0

Actual: after tick 1: slow False, fast False, WhenAny False
after tick 2: slow True, fast True, WhenAny True
after tick 3: slow True, fast True, WhenAny True
after tick 4: slow True, fast True, WhenAny True
WhenAny gives 1
(the same in debug and release)

Suggested fix: Either decide at the end of the tick (the first waiter to wake only signals; a decider that runs after the tick picks the lowest index whose task IsDone()), or change the docs and the comment to the rule the code follows: of tasks whose waiters the scheduler finds done in the same pass, the first in the array wins, which is the array order only for tasks done before the tick began. Add this case to concurrent_tasks.fb.

Skeptic: I ran tie.fb as given in debug and release. After tick 2, slow (index 0) and fast (index 1) are both done and WhenAny is done, yet 'WhenAny gives 1'. stdlib.md line 469 ('of those done by the same tick the first given') and WhenAny's comment in tasks.fb:100 promise index 0. FirstOf (tasks.fb:126-134) calls TryFinish as soon as its waiter is resumed, in m_waiting order. Slow's coroutine re-waited after the FirstOfs were made, so it finished after FirstOf0 was checked in the same tick. The existing test in concurrent_tasks.fb (line 69) only covers tasks whose coroutines come before the FirstOfs. I rate it low: it changes only which index a tie gives, and the index given did finish first in execution order.

## S18-tasks-events-6 (low): Event.Clear() takes quadratic time: each Drop compacts the whole handler array

Status: fixed in b677828: Clear counts as an Emit while it drops the handlers, and takes them out once at the end; program_tests.cpp's "an Event's Clear takes out all its handlers at once" clears 100000 in a debug build in under 10 seconds, which took 0.3 seconds, and 146 before

Where: std/events/event.fb:78

Clear() calls Drop on each handler. Outside an Emit, each Drop calls Dropped, which calls TakeOutDropped, and that runs RemoveAll over the whole Handlers array. So Clear costs O(n²). Measured in release: subscribing 40000 handlers takes 0.1 s, but Clear takes 12.9 s with 40000 handlers and 55.7 s with 80000, which is quadratic. Inside an Emit, the same drops are batched into one TakeOutDropped.

```
Import std.core
Import std.events

Const count = 40000
Const event = Event<Int>()
Local subscriptions: Subscription[] = []
For i = 1 To count
    subscriptions.Add(event.Subscribe(Function(n: Int) => Print(n)))
End For

Print("subscribed", event.Count)
event.Clear()
Print("cleared", event.Count)
```

Expected: subscribed 40000
cleared 0
in well under a second, linear in the count

Actual: subscribed 40000
cleared 0
after 12.9 s (release; with count = 80000, 55.7 s)

Suggested fix: Batch the take-out in Clear, as Emit does: `Emitting += 1`, drop each handler of the clone, `Emitting -= 1`, then `If Emitting == 0 && Stale > 0 Then TakeOutDropped()`.

Skeptic: In release, Clear took 0.68s with 10000 handlers, 2.44s with 20000, and 13.0s with 40000, about 4-5x per doubling, which is quadratic. The same 40000-handler program without event.Clear() ran in 0.10s (noclear40000.fb). event.fb:78-93 shows why: Clear calls Drop on each handler, and with Emitting == 0 each Drop's Dropped runs TakeOutDropped, a RemoveAll over the whole Handlers array. Emit already batches these removals, so Clear could too.

## S18-new-1 (medium): SortedMap's and SortedSet's, Trie's, and IntervalTree's iterators keep their stacks in arrays, so a copy moves the original

Status: fixed in fde965a, 4db33ad, and d131c8a: SortedMapIterator (which SortedSetIterator wraps), TrieIterator, and IntervalTreeIterator keep the nodes above the one they're at in linked lists of plain Class nodes, SortedPath, TriePath, and IntervalPath, which a step replaces rather than changes, as S18-collections-4's fix did for the immutable ones; the node it's at and its place are the Struct's own fields, so a copy keeps its place. sorted_collections.fb (SortedMap: Next, then Count twice and Next; SortedSet: a For ... In, then Count and Next), trie.fb, and interval_tree.fb (Next, then Count twice and Next, then a For ... In and Count) gave 49 0 before, and 49 49, then the second entry, now

Where: std/collections/sorted.fb:440, std/collections/trie.fb:218, std/collections/interval_tree.fb:277

Found while fixing S18-collections-4, which is the same bug in ImmutableArray's and ImmutableMap's iterators: they're step 3's and step 4's. Each iterator is a Struct whose stack of nodes is an array field (Nodes, and Places for SortedMap and Trie). Copying the Struct copies the array reference, not the array, so a consumer like Count, or a For ... In, that takes a copy pops the original's stack as it goes. An iterator that has given one entry then reads as empty.

```
Import std.core
Import std.collections

Local map = SortedMap<Int, Int>()
For i = 0 Until 50
    map.Add(i, i)
End For

Local entries = map.Iterate()
entries.Next()
Print(entries.Count(), entries.Count())
```

Expected: 49 49

Actual: 49 0 (and the same for a SortedSet, a Trie, and an IntervalTree of 50)

## Refuted by the skeptics (no action)

- S18-locks-1: A Read nested under a Read through the job pool panics or not depending on which thread runs it. I reproduced what the finding saw, but the behaviour follows from the spec and stdlib.md. It isn't a defect in the RwLock.

What I ran, from C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18/locks1:
- The finding's p12.fb, built with fb build, 20 runs per build. Debug finished 8 times and panicked 12 times; release finished 7 times and panicked 13. The panic was "this thread holds the RwLock already, so a Read could wait for itself forever at p12.fb:7:54".
- pf.fb, a reader whose Parallel For body calls rw.Read. Debug printed "1 4" in 2 of 20 runs and panicked in the rest; release panicked in 20 of 20.

Why it isn't a bug:
- docs/spec/10-concurrency.md says "A worker that waits for the pool's work, like a task's Wait() or a Parallel For, runs the work it started itself meanwhile".
- The same section says Parallel For runs "with the calling thread taking some" of the chunks.
- The spec ties lock ownership to the thread: "A thread that Locks a Mutex it holds already panics".
- stdlib.md says the same for the std locks: "a thread that takes a lock it holds already ... panics". The lane's decision is "any second Read or Write of an RwLock on one thread".
- So when the inner work runs on the thread that holds the read, the panic is exactly what the docs specify. Which thread runs it is the pool's documented freedom (thread.cpp: run_own in wait_for, take_chunks in fb_parallel_for). The RwLock code does what the docs say.

The outcome the finding calls the correct one isn't safe either:
- The RwLock is writer-preferring, as documented (fb_rwlock_read waits while writers_waiting != 0).
- So a reader that waits on other work that reads the same lock is the classic recursive-read deadlock. Go's RWMutex forbids it for this reason.
- My wr.fb shows it. On the main thread the inner task always runs on another worker, so this is the finding's "success" path. The outer reader starts a Thread that sets rw.Value, spins so that writer is queued, then Spawns the inner Read and waits for it. Under `timeout 15` it hung all 3 runs (exit 124).
- So the run-to-run difference comes from a program that breaks the lock's documented contract, and its "success" depends on timing anyway.
- The proposed fix (swap t_held out while a job runs inline) would make the inline case block on itself whenever a writer waits, which is the self-wait the panic exists to prevent.

The done-when row ("each concurrent type runs under many threads, many times over, with the same result") is about using the types correctly, and this pattern is incorrect use. At most, stdlib.md could say that a reader shouldn't wait for other work that takes the same RwLock. That is a documentation nicety, not a contradiction of the docs.
- S18-collections-5: The queue's threaded test runs only Enqueue and TryDequeue, and never reads Count while threads run. Ran the finding's program as C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s18/cmq-verify/q_mixed.fb. Debug and release both printed "taken 80000, sum 12799960000, out of order 0, 0 left", which is correct. The finding itself says there's no wrong result. tests/programs/concurrent_queue.fb does run the queue on 8 threads (4 producers, 4 consumers, 2000 values each), so the done-when ("each concurrent type runs under many threads...") isn't plainly unmet: it names types, not each Method. TryPeek, ToArray and Clear take the same single Mutex as Enqueue and TryDequeue, and running them across threads showed no fault. The one real gap, Count read while threads run, is already covered: under CLAUDE.md, S18-collections-1's fix has to come with a test, the never-below-0 program. Refuted as a separate finding. Adding the program as an extra test is optional.
- S18-tasks-events-5: WhenAny keeps waiting on its losing tasks, so a timeout given to it keeps the program from ending. The behavior reproduces: timeout1.fb printed 'work done / first 0 / end of top-level code' and took real 5.086s, while timeout2.fb (a Seconds(5) nothing awaits) took 0.116s. But spec 10-concurrency.md says that once top-level code ends 'the runtime then runs until nothing waits, as Node does', and that a Seconds Task 'that nothing awaits is never done'. WhenAny's FirstOf does await the timer, so the program waiting for it is what the spec prescribes, just as Node's Promise.race with a setTimeout keeps the process alive. stdlib.md promises nothing about the losers, and Cancellation is planned and outside this change. Nothing in the spec, stdlib.md, or the milestones row is contradicted; at most a note in the docs would help.
