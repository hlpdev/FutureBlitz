# The review of M6 step 10's language part, `ThreadLocal Global`, and what it made possible: Rand, RandInt, RandFloat, SeedRandom, and arrays' Shuffle and Sample (the rest of step 10 was reviewed and fixed already; review only this part)

8 confirmed findings: 1 high, 5 medium, 2 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S10TL-runtime-1 (high): A thread C started that makes a ThreadLocal and then lets an object with object fields die corrupts the heap as it exits

Status: open

Where: runtime/src/thread_local.cpp:44

A thread the runtime didn't start (one C started, which calls a Callback) releases its ThreadLocals only from `t_locals`'s destructor (thread_locals_t::~thread_locals_t, line 22). C++ destroys a thread's thread_local objects in the reverse order they were first used. object.cpp's `thread_local letting_go_t t_letting_go` (object.cpp:65) holds a std::vector, and every let_go/release_fields uses it. It is first used the first time an object dies on that thread. When the thread made a ThreadLocal (fb_thread_local_added, the first use of t_locals) before any object died there, t_letting_go is registered after t_locals, so it is destroyed first. Then t_locals' destructor runs the releasers, and let_go pushes the ThreadLocal's object fields into the destroyed `waiting` vector. The vector's storage is already freed, so the push writes into freed memory, and a push past its old capacity frees it a second time. Pool workers, threads of their own, and main are not affected, because they call release_thread_locals() explicitly while t_letting_go is still alive. Only the C-thread path, which spec 02 promises ("A thread C started lets go of its own as it exits"), uses the destructor, and no test covers it: no program in tests/programs starts a C thread. Controls: with `Local pair = Pair()` left out, the program prints "joined". With the Pair made and dropped in a function called before the ThreadLocal's first use, so an object dies on the thread first, it also prints "joined". With the two lines of Run swapped, it still crashes, since the Pair dies at Run's end, after the ThreadLocal is made. Windows only: the program starts the thread with CreateThread. The same order applies wherever thread_local destructors run LIFO.

```
// A thread C starts makes a ThreadLocal, then lets go of an object with object fields. As the
// thread exits, the runtime releases the ThreadLocal.
Import std.core

Extern "C"
    Function CreateThread(attributes: Ptr<Void>, stack: UInt64, start: Callback(Ptr<Void>): UInt32,
                          parameter: Ptr<Void>, flags: UInt32, id: Ptr<Void>): Ptr<Void>
    Function WaitForSingleObject(handle: Ptr<Void>, milliseconds: UInt32): UInt32
    Function CloseHandle(handle: Ptr<Void>): Int32
End Extern

Class Leaf
End Class

Class Pair
    Field A: Leaf
    Field B: Leaf

    New()
        A = Leaf()
        B = Leaf()
    End New
End Class

Class Triple
    Field A: Leaf
    Field B: Leaf
    Field C: Leaf

    New()
        A = Leaf()
        B = Leaf()
        C = Leaf()
    End New
End Class

ThreadLocal Global cache = Triple()

Function Run(parameter: Ptr<Void>): UInt32
    Local held = cache
    Local pair = Pair()
    Return 0
End Function

Unsafe
    Local handle = CreateThread(Null, 0, Run, Null, 0, Null)
    WaitForSingleObject(handle, 4294967295)
    CloseHandle(handle)
End Unsafe
Print("joined")
```

Expected: joined

Actual: error: o_c_thread_crash.exe exited with code 0xC0000374 (STATUS_HEAP_CORRUPTION). This happens on every run, in debug and in release, before "joined" prints.

Suggested fix: Make sure t_letting_go outlives t_locals on every thread. For example, have fb_thread_local_added touch t_letting_go (through an internal hook in object.cpp) before its first t_locals.add, so t_letting_go's destructor is registered first and runs last. Another way is to give letting_go_t storage that t_locals' destructor frees only after it has released the values. Add a test program in which a C thread (CreateThread under #If PLATFORM_WINDOWS, pthread_create elsewhere) makes a ThreadLocal and then lets an object with object fields die, and which checks that the Destroys ran.

Skeptic: I wrote the finding's program as C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10tl/refute/c_thread.fb, unchanged. It ran 3 times in debug and 2 in release, and every run ended with `error: c_thread.exe exited with code 0xC0000374` (STATUS_HEAP_CORRUPTION) before "joined" printed. I ran three controls, and each printed "joined": c_ctrl_nopair.fb (no Pair), c_ctrl_warm.fb (a Pair made and dropped in Warm() before the ThreadLocal's first use) and c_ctrl_notl.fb (a Pair but no ThreadLocal use). So the crash depends on t_locals being constructed before t_letting_go on that thread. The code shows why. thread_local.cpp:44 `thread_local thread_locals_t t_locals` has a destructor (line 22) that calls release(), which runs the releasers. Those releasers call let_go, and object.cpp:173 `letting_go_t& state = t_letting_go;` then reaches `state.waiting.push_back(object)` on the thread_local at object.cpp:65, which C++ has already destroyed (thread_locals are destroyed in reverse order of construction). Pool workers (thread.cpp:243), threads of their own (thread.cpp:116) and main (fb_finish) call release explicitly, so only the C-thread path that spec 02 promises ("A thread C started lets go of its own as it exits") goes through the destructor. No program in tests/programs uses CreateThread or pthread_create.

## S10TL-back-1 (medium): Rand, RandInt, RandFloat, Shuffle() and Sample() panic inside a Destroy that runs after the thread has released its ThreadLocals, but only if the thread drew a number earlier

Status: open

Where: compiler/src/codegen.cpp:1136

The main thread releases its ThreadLocals (generate_main, codegen.cpp:2081) before the Globals. Each releaser sets the state to `released` (codegen.cpp:1136), and after that any use panics. std's default generator is a ThreadLocal Random, and Random is a Class (random.fb:297), so it has a releaser. A Global whose Destroy calls RandInt therefore panics at program end if the main thread drew a number earlier. If the main thread never drew, the same Destroy works, because the generator is made fresh and fb_finish releases it. The panic names an internal of std ('std.math.random.generator') and points at std/math/random.fb:309:12, not at the caller, even though RandInt is @PanicsAtCaller. stdlib.md lists no such panic for Rand, RandInt, RandFloat, Shuffle() or Sample(). The same thing happens on any thread because of the 'last made first' order. In this program a thread sets `spark = Particle()` and then calls RandInt; at the thread's end Particle's Destroy calls RandInt and panics the same way, because the generator was made later and so was released first. As a result, std's Rand can't be used in a Destroy that runs at a thread's end or at program end, depending on whether a number was drawn earlier.

```
Import std.core
Import std.math

Class Enemy
    Method Destroy()
        Print("enemy goes, dropping loot", RandInt(1, 6) >= 1)
    End Method
End Class

Global boss = Enemy()

Print(RandInt(1, 6) >= 1)
Print("end")
```

Expected: True
end
enemy goes, dropping loot True
(exit 0; this is what it prints when the first Print(RandInt(1, 6) >= 1) is removed)

Actual: True
end
panic: 'std.math.random.generator' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at std/math/random.fb:309:12
(exit 101, in debug and release)

Suggested fix: Option 1: keep std's default generator in a ThreadLocal that holds no references, such as a Struct with the PCG state and its stream. Such a ThreadLocal is never let go, so no use of it can come after a release. Option 2: let a use after release make a new value. The releaser would set the state back to 0 once its let_go is done, and the release loop or fb_finish's second fb_thread_locals_release would let go of the new value. The panic would stay only for uses while that value's own Destroy runs. In either case, add a test for a Global's Destroy and a ThreadLocal value's Destroy that draw after an earlier draw. If the panic is kept, list it in stdlib.md and raise it at the caller.

Skeptic: I ran the finding's program as back-verify/f1.fb. In debug and in release it printed "True", "end", then "panic: 'std.math.random.generator' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at std/math/random.fb:309:12" and exited 101. The control f1b.fb is the same program without the earlier draw; in both builds it printed "end" and "enemy goes, dropping loot True" and exited 0. I also ran a thread version, f1t.fb: a thread sets ThreadLocal `spark = Particle()`, calls RandInt, and Particle's Destroy calls RandInt. It panicked the same way in both builds, because the generator was made after spark and so was released first. The cause is in the code: generate_main calls fb_pool_finish and fb_thread_locals_release before the Globals' loop; the releaser stores `released` (codegen.cpp generate_thread_local_functions); make_thread_local panics on that state; and random.fb's default generator is a `ThreadLocal Global generator = Random(...)`, a Class, so it has a releaser. The spec's use-after-release panic is a lane decision for a program's own ThreadLocals. The problem is that std's RandInt/Rand/RandFloat/Shuffle()/Sample() inherit it on valid arguments, and whether they do depends on whether the thread drew earlier. stdlib.md's Random bullet lists no such panic. The panic names a std internal and points into std, not at the caller, against stdlib.md's convention that a panic for a bug is reported at the caller's line. The cause is that the panic is raised in Generator(), which is not @PanicsAtCaller.

## S10TL-back-2 (medium): A job that a Global's Destroy starts runs on the main thread after the main thread released its ThreadLocals, so it panics on a ThreadLocal, or not, depending on which thread runs it

Status: open

Where: compiler/src/codegen.cpp:2081

generate_main calls fb_pool_finish and fb_thread_locals_release, then releases the Globals. A Global's Destroy that Spawns a job starts the pool again. fb_finish (object.cpp:529) then calls fb_pool_finish again, and pool_t::finish() runs the pending jobs on the calling thread with run_one(). The calling thread is the main thread, whose ThreadLocals are already in the `released` state. The job is pool work, and on a worker it would make its own value of the ThreadLocal and release it when the pool stops. On the main thread it panics with 'used after it's released' instead. With one such job, main always took it (40 of 40 runs panicked, in debug and in release). With four jobs that each did `tally.Count += 1` on a ThreadLocal Tally, the run panicked or finished cleanly depending on whether main or a new worker took a job first: 29 of 30 runs panicked and 1 exited 0, in both builds. That is nondeterministic behavior caused by the order in which the main thread's ThreadLocals are released.

```
Import std.core

ThreadLocal Global label = "main's"

Class Saver
    Method Destroy()
        Spawn(Function(): Int
            label = label + ", saved"
            Return 0
        End Function)
    End Method
End Class

Global saver = Saver()

Print(label)
```

Expected: main's
(exit 0: the job makes and releases its own label, as it would on a worker)

Actual: main's
panic: 'label' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at t15.fb:8:13
(exit 101 on 40 of 40 runs, in debug and release; with four such jobs, 29 of 30 runs panic and 1 exits 0)

Suggested fix: After the main thread has released its ThreadLocals, fb_finish's fb_pool_finish should not run jobs on the main thread. It can wait for the workers that submit() started instead. Alternatively, make a use after release create a new value (see S10TL-back-1), which fb_finish's later fb_thread_locals_release lets go. Add a program test with a Global whose Destroy Spawns jobs that use a ThreadLocal.

Skeptic: I ran the finding's program (f2.fb). With fb run, debug exited 0 and release panicked with "'label' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at f2.fb:8:13" (exit 101). I then built both builds and ran each 30 times: debug panicked 26 times and exited 0 4 times; release panicked 24 times and exited 0 6 times. The same program has nondeterministic outcomes. The code shows why. generate_main calls fb_pool_finish and then fb_thread_locals_release before the Globals let go. Saver's Destroy Spawns, which calls submit() and starts the workers again. fb_finish (object.cpp) then calls fb_pool_finish again. pool_t::finish() (thread.cpp:193-205) runs pending jobs on the calling thread with run_one(), and that thread is main, whose ThreadLocal state is already `released`. If a new worker takes the job instead, the worker makes its own label and releases it when the pool stops (thread.cpp:240-244). spec 02 says main releases its ThreadLocals 'once the top-level code and the jobs are done'. It does not cover jobs that a Global's Destroy starts, and no reading of the spec makes this program's outcome depend on which thread wins the job.

## S10TL-back-3 (medium): A ThreadLocal's initializer that waits on a pool worker runs that worker's older queued jobs inside it, and those jobs falsely panic with 'its initializer reaches it'

Status: open

Where: compiler/src/codegen.cpp:2564

The maker sets the thread's state to `making` while the initializer runs (codegen.cpp:1122), and any use in that state panics with 'its initializer reaches it' (codegen.cpp:2564). On a pool worker, a wait (Task.Wait, Thread.Join, a Parallel For's wait) goes through wait_running_own (thread.cpp:148). While it waits, it runs the worker's own queued jobs, newest first, including jobs started before the initializer began. In this program a job queues 200 jobs that read `setting` and then reads `setting` itself. The initializer Joins a thread of its own. While it waits, the worker runs the queued jobs inline; they read `setting` while it is `making` and panic. The initializer never reaches its Global. The same initializer run on the main thread, or on a worker whose queue was already stolen empty, finishes fine, so the outcome changes from run to run. An instrumented copy, which counted the queued jobs that ran while the worker was making the value, counted 0, 3, 20, 34, or 60 of them depending on the run.

```
Import std.core

// Loads a setting on a thread of its own, as blocking work does, and waits for it.
Function Load(): Int
    Local loader = Thread.Start(Function()
        Local total = 0
        For i = 1 To 30000000
            total += i % 3
        End For
    End Function)
    loader.Join()
    Return 5
End Function

ThreadLocal Global setting = Load()

// A job that starts jobs of its own, which each read their thread's setting, then reads its own.
Local outer = Spawn(Function(): Int
    Local inner: Task<Int>[] = []
    For i = 1 To 200
        inner.Add(Spawn(Function(): Int => setting))
    End For
    Local sum = setting
    For task In inner
        sum += task.Wait()
    End For
    Return sum
End Function)
Print(outer.Wait())
```

Expected: 1005
(on every run)

Actual: Nondeterministic: in debug 3 of 12 runs, and in release 5 of 12 runs, print
panic: 'setting' is used before its initializer has run, since its initializer reaches it at t20.fb:21:44
and exit 101. The other runs print 1005. An earlier debug build with the same loop gave 7 panics in 10 runs.

Suggested fix: While a thread is making a ThreadLocal, its waits must not run jobs that were queued before the making began. For example, the maker could tell the runtime when it starts and finishes, and run_own() would then take only jobs started after that point, or no jobs at all while any making is in progress. The `making` panic would then fire only when the initializer itself reaches the Global. Add a test for an initializer that waits on a worker whose queue holds jobs that use the Global.

Skeptic: I built the finding's program (f3.fb) in both builds and ran each 20 times. Debug panicked 5 times and printed 1005 15 times; release panicked 7 times and printed 1005 13 times. Each panic was "'setting' is used before its initializer has run, since its initializer reaches it at f3.fb:21:44". The initializer Load() never reaches `setting`. It only starts a thread and Joins it, and spec 02 says only an initializer that 'reaches its own Global, through what it calls' panics. So this panic and its message contradict the spec. In the code, the maker stores `making` before it calls the initializer (codegen.cpp generate_thread_local_functions), and make_thread_local panics on `making`. Thread.Join/Task.Wait on a worker go through pool().wait_for/wait_running_own (thread.cpp:137-157, 780, 815), whose run_own() (thread.cpp:256-272) pops and runs the worker's own queued jobs, newest first. Those include the 200 inner jobs queued before the outer job's first read of `setting` began making it. Those jobs read `setting` on the same thread while it is `making`. Whether any are still on the worker's queue, and not stolen by other workers, varies from run to run, which explains the nondeterminism.

## S10TL-runtime-2 (medium): Rand, RandInt, RandFloat, Shuffle() and Sample() panic in a Global's Destroy, or a ThreadLocal's, with std's private name and std's line

Status: open

Where: std/math/random.fb:297

The default generator is `ThreadLocal Global generator`, a Random object. The main thread releases its ThreadLocals after fb_pool_finish and before the Globals. Any later draw from the default generator therefore panics, and that includes any Global's Destroy at program end. The message names std's private Global, 'std.math.random.generator', and points into std (std/math/random.fb:309:12, Generator's `Return generator`), not the caller's line. That breaks std's @PanicsAtCaller convention, and stdlib.md says nowhere that Rand can't be used there. The same thing happens on any thread whose own ThreadLocal was made before its first draw and whose Destroy draws. Releasing last-made first frees the generator first, so with `ThreadLocal Global emitter = Emitter()` used before RandInt on a Thread.Start thread, Emitter.Destroy's RandInt panics with the same message as the thread ends. Debug and release give the same output.

```
// A Global whose Destroy draws a random number as the program ends.
Import std.core
Import std.math

Class Loot
    Method Destroy()
        Print("loot drops {RandInt(1, 6) >= 1}")
    End Method
End Class

Global loot = Loot()

SeedRandom(1)
Print(RandInt(1, 6))
```

Expected: 4
loot drops True

Actual: 4
panic: 'std.math.random.generator' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at std/math/random.fb:309:12
(exit code 101, in debug and in release)

Suggested fix: Keep the default generator's state in a ThreadLocal that holds no references, such as a (UInt64, UInt64) PCG state or a Struct that Generator steps. Such a ThreadLocal has no releaser and stays usable in any Destroy. If the panic is kept instead, make it a std panic at the caller's line that says the default generator is gone, and document it in stdlib.md's default-generator bullet. Either way, add a test that draws in a Global's Destroy.

Skeptic: I wrote loot.fb, the finding's program. In debug and in release it printed `4` and then `panic: 'std.math.random.generator' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at std/math/random.fb:309:12`, exit 101. I also checked the thread variant in emitter.fb: a Thread.Start thread uses `ThreadLocal Global emitter = Emitter()`, then RandInt, and Emitter.Destroy calls RandInt. It printed `sparks`, `True`, then the same panic at std/math/random.fb:309:12. Line 309 is Generator()'s `Return generator`. Generator() isn't @PanicsAtCaller, so even RandInt, which is, reports std's line and std's private name. Spec 02's release order does make a user ThreadLocal panic in a Global's Destroy; my control loot_own.fb panics at the user's own line 12:22, as specified. Rand's case still contradicts stdlib.md's conventions either way. If drawing there counts as misuse, stdlib.md:169 says a bug "panics, at the caller's line (@PanicsAtCaller)", and this one points into std. If it isn't misuse, nothing documents the panic: the default-generator bullet at stdlib.md:395-400 doesn't mention it, and a user can't see or control when std's generator is first made, so whether their own ThreadLocal's Destroy can draw depends on a hidden first-use order.

## S10TL-runtime-3 (medium): A job started after the main thread released its ThreadLocals runs on the main thread, so its ThreadLocal use panics

Status: open

Where: runtime/src/object.cpp:529

Generated main calls fb_pool_finish, then fb_thread_locals_release, then releases the Globals. A Global's Destroy, or a main-thread ThreadLocal's Destroy, can Spawn a job without waiting for it. fb_finish's fb_pool_finish (object.cpp:529) then runs that job, and pool_t::finish() takes jobs with run_one() on the calling thread, which here is the main thread (thread.cpp:196). The main thread has already released its ThreadLocals, so a job that uses any ThreadLocal panics with "used after it's released". On a worker it would simply get that worker's own value. The same Destroy with `.Wait()` on the task prints "saved", because a worker runs it. So whether a job may use a ThreadLocal depends on whether its starter waited for it, which the spec doesn't allow for: a Spawned job is pool work with its thread's own ThreadLocals. The same happens with Rand in such a job, through std's generator. It panicked on all 12 runs in debug and in release.

```
// A Global's Destroy starts a job that uses a ThreadLocal, and doesn't wait for it: the program
// runs it before it ends, on whichever thread gets to it.
Import std.core

Class Counter
    Field Count = 0
End Class

ThreadLocal Global counter = Counter()

Class Saver
    Method Destroy()
        Spawn(Function(): Int
            counter.Count += 1
            Print("saved")
            Return counter.Count
        End Function)
    End Method
End Class

Global saver = Saver()

counter.Count = 5
Print(counter.Count)
```

Expected: 5
saved

Actual: 5
panic: 'counter' is used after it's released, since a thread releases its ThreadLocal Globals as it ends at h_late_job.fb:14:13
(exit code 101, in debug and in release)

Suggested fix: Once the main thread has released its ThreadLocals, don't run jobs on it. For example, give pool_t::finish() a mode for fb_finish in which the calling thread only waits while workers run what's left. Another way is to have generated main release the main thread's ThreadLocals only after the Globals' Destroys and fb_finish's pool_finish have run their jobs, if the spec's order allows that. Add a test in which a Global's Destroy Spawns a job, without waiting for it, that uses a ThreadLocal.

Skeptic: I wrote late_job.fb, the finding's program. Under `fb run` in debug it printed `5` and then `panic: 'counter' is used after it's released, ... at late_job.fb:14:13`, exit 101, and release did the same. Over 6 more debug runs, 5 panicked and 1 printed `saved`, so the outcome depends on scheduling. The control late_job_wait.fb is the same program with `.Wait()` on the Spawn, and it printed `5`, `saved`, exit 0. The code shows the cause. Generated main calls fb_pool_finish and then fb_thread_locals_release before the Globals' Destroys (the codegen.cpp generate_main diff). fb_finish (object.cpp:527-531) then calls fb_pool_finish again, and pool_t::finish() takes leftover jobs with run_one() on the calling main thread (thread.cpp ~194). Main's `counter` is already in the released state at that point, so a job main picks up panics, while one a restarted worker picks up gets a fresh value. Spec 10 says `Spawn(work)` runs the work on the pool and that the program waits for every Task. Spec 02 says code on any thread uses its own ThreadLocal. So whether a fire-and-forget job panics comes down to which thread wins the race, which the spec doesn't allow for.

## S10TL-front-1 (low): @MainThread on a ThreadLocal Global that has an initializer is reported twice, the second time as if the initializer were @MainThread work

Status: open

Where: compiler/src/sema.cpp:9848

check_variable_modifiers() already reports a misplaced @MainThread on a Global, but the modifier stays on the symbol. check_thread_local() then queues the Global itself as the worker, and check_worker() first checks `symbol(worker.work).modifiers.is_main_thread`. That check is meant for a Function or Method given as work, so it adds a second error: "a ThreadLocal Global's initializer runs on each thread that uses it, but 'x' is @MainThread". It also returns early, so a real main-only use in that initializer would go unreported. A plain `@MainThread Global y = 2`, and a ThreadLocal with no initializer (`@MainThread ThreadLocal Global x: Int`), each get only the one error. Neither the sema tests nor the parser tests cover an attribute on a ThreadLocal.

```
Import std.core

// @MainThread is misplaced on a Global, which is reported; a ThreadLocal one with a value is
// reported a second time, as though its initializer were @MainThread work.
@MainThread ThreadLocal Global x = 1
@MainThread Global y = 2
Print(x, y)
```

Expected: Two errors, one on each line, both "@MainThread only goes before a Function or Method" (2 errors).

Actual: Three errors. Line 5 gets both "@MainThread only goes before a Function or Method" and "a ThreadLocal Global's initializer runs on each thread that uses it, but 'x' is @MainThread" (5:36), and line 6 gets the first message only.

Suggested fix: In check_worker(), treat the work's is_main_thread as @MainThread only when the work is a FUNCTION, METHOD or CONSTRUCTOR, so a ThreadLocal Global's worker goes on to the main-only walk. Another option is to have check_thread_local() skip a Global whose @MainThread was already reported. Add `@MainThread ThreadLocal Global x = 1` to the ThreadLocal sema test, expecting only the one error.

Skeptic: Ran the finding's program (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10tl/front-verify/f1.fb) with build-rev fb.exe. It printed 3 errors: 5:32 '@MainThread only goes before a Function or Method', 5:36 'a ThreadLocal Global's initializer runs on each thread that uses it, but 'x' is @MainThread', and 6:20 the first message only. The code matches the description. check_variable_modifiers() (sema.cpp ~3402) reports the misplaced @MainThread but leaves the flag set. check_thread_local() (sema.cpp 12414) pushes a worker whose .work is the Global itself. check_worker() (sema.cpp 9848) tests symbol(worker.work).modifiers.is_main_thread before the reach walk, gives the second error, and returns. I also ran f1b.fb, where the initializer calls a @MainThread Function: `@MainThread ThreadLocal Global x = OnMain()` gets only "... but 'x' is @MainThread", while the same line without the attribute gets the real cause, "... but it calls 'OnMain', which is @MainThread" with a 'here' note. So the bogus error stands in for the true one. There is still one error on that line, so it isn't silently accepted. The program still fails to build, so this only affects diagnostic quality: low. The check is new in a4e2099..7b95611, and no sema or parser test puts an attribute on a ThreadLocal.

## S10TL-front-2 (low): `ThreadLocal @Attr Global` says ThreadLocal only goes before a Global, though it does, and hides the real error (attribute order)

Status: open

Where: compiler/src/parser.cpp:911

parse_modified() reports `ThreadLocal only goes before a Global` whenever the next token isn't Global. When the next token is an attribute, as in `ThreadLocal @Deprecated Global x = 1`, ThreadLocal is in fact before a Global; the mistake is that the attribute comes after a modifier. The `attributes go before a declaration's modifiers` error that parse_modified() would give at the AT token doesn't appear, because only one error per line is reported. So the user is told the wrong thing. `Static @Inline Function F()` gets the right message. Async and Shared behave the same as ThreadLocal, but this check is new in this step and copies their pattern. No parser test covers an attribute after ThreadLocal.

```
Import std.core

// The attribute is in the wrong place, but ThreadLocal is before a Global.
ThreadLocal @Deprecated Global x = 1
Static @Inline Function F()
End Function
Print(x)
```

Expected: 4:13: error: attributes go before a declaration's modifiers, as in @Inline Static Function (or similar), like the 5:8 error the Static line gets

Actual: 4:1: error: ThreadLocal only goes before a Global, which each thread then has its own of
5:8: error: attributes go before a declaration's modifiers, as in @Inline Static Function

Suggested fix: In parse_modified(), skip the ThreadLocal placement error when at(token_kind_t::AT), so the attribute-order error is reported instead. The same guard fits the Shared and Async checks next to it. Add `ThreadLocal @Deprecated Global x = 1` to the "ThreadLocal goes before a Global" parser test.

Skeptic: Ran f2.fb (same folder) with build-rev fb.exe. It printed '4:1: error: ThreadLocal only goes before a Global, which each thread then has its own of' for `ThreadLocal @Deprecated Global x = 1`. It also printed a 7:7 deprecated warning, which shows the attribute was in fact applied to a Global. The Static line got the correct '5:8: attributes go before a declaration's modifiers, as in @Inline Static Function'. In parser.cpp parse_modified(), line 911 checks `if (thread_local_span && !at(token_kind_t::KW_GLOBAL))` right after the modifier loop. That check fires when the next token is AT, before the AT branch (~line 975) can give the attribute-order error, and only the first error per line shows. The diff a4e2099..7b95611 adds this ThreadLocal check, copying the Shared and Async checks, which have the same flaw from before this step. No parser test covers an attribute after ThreadLocal. This is a wrong message on code that is rejected anyway: low.
