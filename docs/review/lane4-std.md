# Lane 4: std

The whole-codebase review's findings for A11, A12, from a review of master
at ba70e73 (2026-10-22). Only A3's were checked by a skeptic, which confirmed all 12;
reproduce every other one before fixing it. Files this lane owns: std/.

23 findings: 0 high, 11 medium, 12 low. Fix the highs, then the mediums, then the lows.

Each finding's Status line is one of: open; fixed in <commit> (with its test);
dropped, with why (it doesn't reproduce, or the spec says otherwise).

## A11-1 (medium): Queue keeps dequeued values alive until it compacts

Status: open

Where: std/collections/queue.fb:37

Dequeue only moves Head forward. The value stays in Items[Head - 1] until Head * 2 >= Items.Count, when the array gets sliced. So a dequeued object is still referenced by the Queue after the caller drops it, and its Destroy is late. With a backlog of n values, up to about n dequeued values stay alive. This goes against the language's rule that refcounting runs Destroy right away (stdlib.md: resources close in Destroy). A Queue<File> or a queue of jobs holding buffers keeps finished ones open or in memory.

Reproduced by the reviewer:

```
q1.fb: Class Thing with Destroy printing 'destroy {Name}'. Enqueue a, b, c, d; `Local t = q.Dequeue()`; `t = Thing("x")`; Print("after drop"). Output: dequeue / got a / after drop / 3 / destroy x / destroy a / destroy b ... So 'a' isn't destroyed when the last user reference goes, only at program end. Stack.Pop, Deque, PriorityQueue.Pop, Map.Remove, and Pool.Remove all release at once.
```

Suggested fix: Release the slot when it's taken. Hold Items as T?[] and set Items[Head] = Null in Dequeue (TryDequeue goes through it), then compact as now. Or use a ring buffer. Add a test with a Destroy-printing Class that checks the dequeued value goes when its last variable does.

Skeptic: confirmed=True, medium. Reproduced with exp-all/skeptic11/q1.fb (Thing with New(name) and a Destroy that prints). Queue: enqueue a..d, `Local t = q.Dequeue()`, `t = Thing("x")`. Output is 'got a / after drop / 3', and 'destroy a' only comes at program end, after 'destroy x'. Stack in the same program: `u = s.Pop(); u = Thing("y")` prints 'destroy s2' right away. The cause is queue.fb:35-44: Dequeue only does `Head += 1` and slices Items only when Head * 2 >= Items.Count, so Items[Head - 1] still holds the value.

## A11-2 (medium): Map.Reserve above 2^30 overflows inside Grow; a Map can't grow past 2^30 buckets

Status: open

Where: std/collections/map.fb:364

Grow doubles an Int `buckets` until it reaches count. Any count above 1073741824 makes `buckets *= 2` overflow. Reserve(count) passes any non-negative count to Grow, so a valid argument panics with an internal 'integer overflow' at std's own line instead of at the caller with a clear message. Append's Grow(Buckets.Count + 1) hits the same overflow when a Map gets past 2^30 entries.

Reproduced by the reviewer:

```
big.fb: `Local m = Map<Int, Int>()` then `m.Reserve(1073741825)` (2000000000 gives the same) -> `panic: integer overflow in '*' at std/collections/map.fb:364:13`. Reserve is @PanicsAtCaller, but the overflow happens in Grow, which isn't, so it points into std.
```

Suggested fix: Cap the buckets at 2^30 (letting chains get longer past that), or work the size out in Int64 and stop at the largest power of two an Int holds. Make Grow @PanicsAtCaller, or check in Reserve, so any count that really can't be held panics at the caller with a message of its own. Add a test.

Skeptic: confirmed=True, medium. Reproduced with exp-all/skeptic11/big.fb: `Local m = Map<Int, Int>()` then `m.Reserve(1073741825)`. It gives 'panic: integer overflow in '*' at std/collections/map.fb:364:13' (exit 101). Entries.Reserve(count) on line 173 succeeds first, so the count itself is accepted. The overflow is Grow's `buckets *= 2` on an Int32, and Grow isn't @PanicsAtCaller, so the panic points into std instead of at the caller.

## A11-3 (medium): SortUnstable (and PriorityQueue's SiftDown) overflow working out a child index on arrays over 2^30 elements

Status: open

Where: std/collections/array.fb:318

SiftDown computes `left = 2 * at + 1` before checking it against count. Once `at` reaches 2^30 (an array of about 1.07e9 elements or more, about 1 GB of Bytes), the Int multiply overflows. Debug builds panic with 'integer overflow'. Release builds wrap to a negative index that passes `left < count` and then panics as out of range. priority_queue.fb's SiftDown (line 121, `parent * 2 + 1`) has the same overflow for a PriorityQueue that big. The array itself is valid and Sort() handles it.

Reproduced by the reviewer:

```
bigsort.fb: `Local a: Byte[] = []`, `a.Reserve(1100000000)`, `a.Resize(1100000000, 0)`, `a[1099999999] = 5`, `a.SortUnstable()`. Debug: `panic: integer overflow in '*' at bigsort.fb:9:1`. Release: `panic: index -2094967297 is out of range for 1100000000 elements at bigsort.fb:9:1`. Each takes about 4 s.
```

Suggested fix: Stop before the multiply overflows: `If at > (count - 2) / 2 Then Return` (no left child), or work the child index out in Int64. Do the same in PriorityQueue.SiftDown.

Skeptic: confirmed=True, medium. Reproduced with exp-all/skeptic11/bigsort.fb: a 1100000000-element Byte[] with a[1099999999] = 5, then a.SortUnstable(). Debug build: 'panic: integer overflow in '*' at bigsort.fb:9:1'. Release build: 'panic: index -2094967297 is out of range for 1100000000 elements at bigsort.fb:9:1'. array.fb:318 computes `left = 2 * at + 1` before checking it against count. priority_queue.fb:120 (`child = parent * 2 + 1`) has the same pattern, found by reading.

## A11-4 (medium): Spec says T[]'s FindLast gives an Int?; it gives the element, a T?

Status: open

Where: docs/spec/14-strings-collections.md:142

Spec 14's T[] row says: "`Find(test)`, which gives a `T?`; `FindLast`, `FindIndex`, `IndexOf`, `LastIndexOf`, and `BinarySearch`, which give an `Int?`". array.fb's `Public Method FindLast(test: Func(T): Bool): T?` gives the last matching element, not its index. LinkedList's FindLast is a T? too, and spec 14 says a LinkedList's Find means what it does on an array. The code and tests agree with each other; the spec is wrong.

Reproduced by the reviewer:

```
tests/programs/array_methods.fb prints `numbers.FindLast(Function(n: Int) => n < 4) ?? -1` for [5, 3, 8, 3, 1], and array_methods.out has `1` (the element). The index the spec promises would be 4.
```

Suggested fix: Move FindLast next to Find in spec 14's T[] row ("`Find(test)` and `FindLast(test)`, which give a `T?`; `FindIndex`, `IndexOf`, `LastIndexOf`, and `BinarySearch`, which give an `Int?`").

Skeptic: confirmed=True, medium. Spec 14 line 142 lists 'FindLast, FindIndex, IndexOf, LastIndexOf, and BinarySearch, which give an Int?'. But array.fb:67 is `Public Method FindLast(test: Func(T): Bool): T?`, and linked_list.fb:256 has the same signature. tests/programs/array_methods.fb:13 prints the element, and the .out file agrees, so the code, the tests, and stdlib.md agree with each other and contradict the spec.

## A11-5 (medium): Remove, IndexOf, LastIndexOf, and every collection's Contains refuse plain Class elements that == and T[].Contains accept

Status: open

Where: std/collections/array.fb:14

A Class has == by reference identity without listing Equatable (spec 06's table), and the built-in T[].Contains takes any element type. But std's T[].Remove, IndexOf, and LastIndexOf, and Stack, Queue, Deque, PriorityQueue, and ReadOnlyArray Contains, all say `Where T: Equatable`, which a plain Class doesn't meet. So `enemies.Contains(e)` compiles and `enemies.Remove(e)` doesn't. Removing an object from a list is the commonest game-code operation. Spec 14 lists Remove(value) ('the first equal one') and every collection's Contains with no such constraint, while it does spell out Sort's 'when T is Comparable'.

Reproduced by the reviewer:

```
c1.fb: `Class Enemy` with a Field; `Local arr = [e]`; `arr.Contains(e)` and `e == e` compile, but `arr.IndexOf(e)`, `arr.Remove(e)`, and `Stack<Enemy>().Contains(e)` give 'error: Enemy doesn't implement Equatable, which 'Remove' needs for T' (and the same for 'IndexOf' and 'Contains'). The same constraint is at array.fb:87 and 96, stack.fb:68, queue.fb:70, deque.fb:122, priority_queue.fb:97, and view.fb:135.
```

Suggested fix: Make every Class Equatable by identity, as its == already is, so the constraint holds. Or give these Methods the built-in == that T[].Contains uses. Otherwise, state the Equatable requirement in spec 14 for Remove, IndexOf, LastIndexOf, and the collections' Contains.

Skeptic: confirmed=True, medium. Reproduced with exp-all/skeptic11/c1.fb (plain `Class Enemy`). `arr.Contains(e)` and `e == e` compile. `arr.IndexOf(e)`, `arr.Remove(e)`, and `Stack<Enemy>().Contains(e)` each fail with 'Enemy doesn't implement Equatable, which ... needs for T'. Spec 07 does say a Class implements Equatable only by listing it. But spec 14 promises a plain `Remove(value)` ('the first equal one') with no constraint, while it does name Sort's Comparable requirement. It says T[]'s Contains 'compares with ==', and that each name 'means the same thing wherever a collection has it'. In practice Contains works on T[] for a Class but not on Stack/Queue/etc. So the spec and the code disagree, whichever one is fixed.

## A11-6 (medium): An unmet conditional Printable on a std collection gives the advice 'implement Printable, with a ToString'

Status: open

Where: compiler/src/sema.cpp:10404

report_unshowable() special-cases T? values, tuples, and arrays. Every other type with members gets 'implement Printable, with a ToString'. Map, Set, ArrayView, ReadOnlyArray, ReadOnlyMap, and ReadOnlySet do implement Printable, but only through an Extend with Where K/V/T: Printable. When that Where fails, the program can't implement Printable itself (it would clash with std's Extend). The real reason, an element type that can't be shown, isn't given. For an array, the same case says 'since its elements can't be'.

Reproduced by the reviewer:

```
d1.fb: `Local arr: Int?[] = [1, Null]; Print(arr)` -> 'Int32?[] can't be shown by 'Print', since its elements can't be' (good). `Print(Map<String, Int?>())` -> 'Map<String, Int32?> can't be shown by 'Print'; implement Printable, with a ToString'. `Print("{Set<Int?>()}")` -> 'Set<Int32?> can't be shown in a string; implement Printable, with a ToString'. `[1, Null].AsReadOnly()` gives the same.
```

Suggested fix: When the type has a conditional Printable conformance whose Where lines fail, report that: '... can't be shown, since Int32? isn't Printable, which Map<K, V>'s ToString needs for V'. Give the 'implement' advice only when there's no conformance at all.

Skeptic: confirmed=False, low. Refuted. The messages do show as described (d1.fb gives 'Map<String, Int32?> can't be shown by 'Print'; implement Printable, with a ToString'). But the finding's key claim, that the program can't implement Printable itself because it would clash with std's Extend, is wrong. exp-all/skeptic11/d2.fb (`Extend Map<String, Int?> Implements Printable` with a ToString) compiles and prints 'a map'. d3.fb does the same for Set<Int?> and ReadOnlyArray<Int?> and prints 'a set' and 'a view'. So the advice is accurate and works. Naming the element type as the reason would only make the message nicer; the diagnostic isn't wrong.

## A11-7 (medium): No test that Map, Queue, or PriorityQueue panics when changed during a For ... In

Status: open

Where: tests/programs/panic_set_changed.fb:1

Spec 14 promises that adding or removing a value during a For ... In panics at the loop's next value, in every collection but a Pool. The program tests check this for Set, Deque, and Stack (panic_set_changed, panic_deque_changed, panic_iterable_changed, panic_chain_changed) and for the step 4 collections. No test anywhere in tests/ expects 'the Map changed', 'the Queue changed', or 'the PriorityQueue changed'. Map's own check is the one Set's goes through but under a different name, and a direct Map loop isn't covered.

Reproduced by the reviewer:

```
grep -rln over tests/ for "the Map changed\|the Queue changed\|the PriorityQueue changed" finds nothing. Run by hand, it works: f_map.fb `For k, v In m ... m.Remove("b")` -> 'panic: the Map changed while a For ... In went over it at f_map.fb:9:1'; f_queue.fb (Dequeue in the loop) -> 'the Queue changed ... at f_queue.fb:7:1'; f_pq.fb (Add in the loop) -> 'the PriorityQueue changed ... at f_pq.fb:7:1'.
```

Suggested fix: Add panic_map_changed.fb, panic_queue_changed.fb, and panic_priority_changed.fb with .out files, like panic_deque_changed.fb.

Skeptic: confirmed=True, medium. Searching tests/ for 'the Map changed', 'the Queue changed', or 'the PriorityQueue changed' finds only panic_multi_map_changed.out and panic_sorted_map_changed.out, which are other types. The panic_*changed tests cover Set, Deque, Stack, BitSet, Counter, LinkedList, LruCache, MultiMap, RingBuffer, Sorted*, QuadTree, Trie, and IntervalTree. Queue and PriorityQueue have their own iterators (queue.fb:107, priority_queue.fb) and no test. Run by hand, both work: f_queue.fb gives 'the Queue changed ... at f_queue.fb:7:1' and f_pq.fb gives 'the PriorityQueue changed ... at f_pq.fb:7:1'. A Map loop only gets the 'Map' name through the shared MapIterator, which no test covers directly.

## A12-1 (medium): Near works out each point's offset in Float, not Double, so points at the radius's edge come out wrong

Status: open

Where: std/collections/spatial.fb:92

Within() writes `Local dx: Double = held.X - x` (and the same for dy and dz). Both operands are Floats, so the subtraction runs in Float32 and only its rounded result is widened to Double; `fb check --show mir` shows `_2 = _0 - _1` (Float32) followed by `_3 = _2 As Float64`. That contradicts the file header (lines 7-8), spec 14 ('its distances and boxes are worked out in Doubles') and decisions.md. Because of the Float rounding, SpatialHash, QuadTree and Octree Near both miss points that are within the radius and find points that are outside it. A Float difference can also overflow to inf, although that never changes a result. The reference Near() in tests/programs/spatial.fb:45 does the same Float subtraction, so the random test agrees with the bug and can't catch it.

Reproduced by the reviewer:

```
Program (debug and release give the same result): `Local cx: Float = 0.82066410779953`, `cy = 0.9263164401054382`, `px = 0.2964705228805542`, `py = 0.1300487220287323`, `r = 0.9533211588859558`; a SpatialHash<String>(1) and a QuadTree<String>(0,0,1,1) each get Add("p", px, py). The exact distance in Double is <= r, but `hash.Near(cx, cy, r)` and `quad.Near(cx, cy, r)` both print []. Second case: center (0.7559108138084412, 0.9752318263053894), point (0.07207980751991272, 0.4743247330188751), r = 0.8476631045341492. The exact distance is > r, yet Near prints [q]. A separate check, `Local d: Double = a - b` with a = 3e38 and b = -3e38, prints inf.
```

Suggested fix: Widen before subtracting: `Local dx: Double = held.X` then `dx -= x` (or `(held.X As Double) - x`), and the same for dy and dz. Make the same change in the test's reference Near() (tests/programs/spatial.fb:45-47). Add the two boundary cases above to the test.

Skeptic: confirmed=True, medium. Reproduced (exp-all/skeptic-a12/a1.fb). Within()'s MIR is `_14 = _13.X - _1` then `_15 = _14 As Float64`, so the subtraction is Float32. Case 1: the exact Double distance squared is 0.908821193321292 and r² is 0.9088212319796618 (inside), yet hash.Near and quad.Near both print []. Case 2: the exact distance squared is 0.7185327612663261 and r² is 0.7185327387884719 (outside), yet both print [q]. `Local d: Double = a - b` with 3e38 and -3e38 prints inf. This contradicts spatial.fb:7-8, spec 14 line 242-243 and decisions.md:669. The test's reference Near (tests/programs/spatial.fb:45-47) does the same Float subtraction, so the random test agrees with the bug.

## A12-2 (medium): An infinite radius misses a point at the same infinity as the center, though the docs say it finds every point

Status: open

Where: std/collections/spatial.fb:95

Spec 14 says a spatial structure's 'infinite one finds every point', and the spatial.fb header says 'an infinite one finds everything'. In Within(), a point at +inf with a center at +inf (or -inf with -inf) gives dx = inf - inf = NaN, and the `<= most * most` test is false for NaN, so the point is dropped even with an infinite radius. Only a SpatialHash can hold such points, since QuadTree and Octree boxes are finite. tests/programs/spatial.fb:195 writes the wrong behavior into its expected output: spatial.out line 11 lists `[corner, home]` without `east`.

Reproduced by the reviewer:

```
`Local h = SpatialHash<String>(10)`, `h.Add("east", infinity, 0)`, `h.Add("home", 0, 0)`, `Print(h.Near(infinity, 0, infinity))` prints `[home]`, while `h.Near(0, 0, infinity)` prints `[east, home]`.
```

Suggested fix: In Within(), treat an infinite radius as taking every placed value: the box from BoxAround already covers everything, so return ValuesOf(placed) when `most` is infinite. Alternatively, count a NaN component that comes from the same infinity as 0. Then update spatial.out line 11 to `[corner, east, home]`.

Skeptic: confirmed=True, medium. Reproduced (a2.fb). With a SpatialHash holding east at (inf, 0) and home at (0, 0): Near(infinity, 0, infinity) prints [home], while Near(0, 0, infinity) and Near(-infinity, 0, infinity) print [east, home]. inf - inf gives NaN in Within(), and the `<=` test then fails. Spec 14:244 says an infinite radius 'finds every point', and spatial.fb:6 says 'finds everything'. spatial.fb:195 and spatial.out line 11 (`[home] [corner, east, home] [] [corner, home] []`) write the wrong result into the expected output.

## A12-3 (medium): A NaN key quietly corrupts a SortedMap, SortedSet, or IntervalTree, so keys already in it can't be found and get duplicated

Status: open

Where: std/collections/sorted.fb:232

Float and Double are Comparable, but NaN is unordered. Place() treats it as not below any key, and Find() compares with ==, which NaN never meets, so a NaN goes in at the front of a node. After that, binary searches for keys already in the map stop at the NaN and miss them. Contains then reports a present key as absent, and Add puts in a second copy of it. Every NaN Add also adds one more entry. IntervalTree.Add (interval_tree.fb:45) has the same problem: `high < low` is false for NaN, so it doesn't panic, and Compare() puts the interval in by its high only. The spatial structures panic on a NaN coordinate, but these sorted structures don't, and the docs say nothing about NaN for them.

Reproduced by the reviewer:

```
`SortedMap<Double, Int>` with keys 1, 2, 3, then `m.Add(nan, 0)`: prints `4 False True True [-nan, 1, 2, 3]` (Contains(1) is False). `m.Add(1, 10)` then gives Count 5 and Keys `[1, -nan, 1, 2, 3]`. `SortedSet<Double>` with 2, nan, nan prints Count 3, Contains(2) False, `{-nan, -nan, 2}`. `IntervalTree<Double, String>.Add(nan, 1, "nan")` is accepted without a panic.
```

Suggested fix: Panic at the caller for a key that isn't equal to itself (`key != key`) in SortedMap.Add/[]= and SortedSet.Add, and in IntervalTree.Add for a low or high that isn't equal to itself, as CheckPoint does for spatial coordinates. Make lookups with such a key find nothing. Document this in spec 14 and add panic tests.

Skeptic: confirmed=True, medium. Reproduced (a3.fb): a SortedMap<Double, Int> with keys 1, 2, 3 and then Add(nan, 0) prints `4 False True True [-nan, 1, 2, 3]`, so Contains(1) is False for a key that is there. Add(1, 10) then gives `5 [1, -nan, 1, 2, 3]`, a duplicate key. A SortedSet<Double> with 2, nan, nan prints `3 False {-nan, -nan, 2}`. IntervalTree<Double, String>.Add(nan, 1, "nan") is accepted, with Count 1, because `high < low` (interval_tree.fb:45) is false for NaN. Place() (sorted.fb:232) and Find()'s == can't order a NaN. Nothing in the spec allows a present key to go missing, and the spatial structures already panic on NaN. The spec says nothing about NaN for the sorted structures.

## A12-4 (medium): Grid3's panics, and a few other promised panics, have no test

Status: open

Where: std/collections/grid.fb:149

Spec 14 promises that a Grid3 position outside the grid panics, and so does a size that's negative or has more cells than an Int counts. No test covers any Grid3 panic: all of tests/programs/panic_grid_*.fb use Grid, and grids.fb only tests the zero-size case. The panics do work: (1, 1, 2) outside reports at the caller's '[' with 'is outside a Grid3 2 by 2 by 2'; 2000x2000x2000 gives 'has more cells than an Int counts'; a negative size gives 'can't be 1 by -2 by 2'; Neighbors6/26 outside panic too. Some other promised panics also have no test: the changed-while-iterating panic for SpatialHash and Octree (only QuadTree's is tested), Last() on an empty LinkedList and an empty RingBuffer (only RemoveLast's is tested), and NaN in SpatialHash.Contains/Remove and in Octree's Remove/Query/Near. All of these were run by hand and give the right messages at the right places.

Reproduced by the reviewer:

```
Ran `Grid3<Int>(2,2,2,0)[1,1,2]`, `g[0,-1,0] = 3`, `Grid3<Int>(2000,2000,2000,0)`, `Grid3<Int>(1,-2,2,0)`, `g.Neighbors26(2,0,0)`, a SpatialHash and an Octree changed inside a For ... In, `LinkedList<Int>().Last()` and `RingBuffer<Int>(2).Last()`: all panic correctly, and none has a test in tests/programs.
```

Suggested fix: Add panic_grid3_outside.fb, panic_grid3_set.fb, panic_grid3_size.fb (negative and too many cells) and panic_grid3_neighbors.fb, plus panic tests for the SpatialHash and Octree change-while-iterating panics and for empty LinkedList.Last and RingBuffer.Last.

Skeptic: confirmed=True, low. The main claim is wrong. Grid3's panics are tested by tracked files: tests/programs/panic_grid3_outside.fb ([] outside), panic_grid3_set.fb ([]= outside), panic_grid3_size.fb (1 by 1 by -2), panic_grid3_cells.fb (2000³) and panic_grid3_neighbors.fb (Neighbors26). Do not add those again. Smaller gaps are real: (1) SpatialHash's own Iterate (spatial.fb:219-220, its Changes closure and the name "SpatialHash") has no change-while-iterating test. panic_spatial_changed.fb uses a QuadTree, and Octree shares that SpaceTree.Iterate path. Running it by hand gives 'the SpatialHash changed while a For ... In went over it' at the loop. (2) LinkedList.Last() on an empty list (linked_list.fb:70-74, its own panic) has no test, since panic_linked_list_last.fb tests RemoveLast. RingBuffer.Last's panic is already reached, because panic_ring_buffer_last.fb's RemoveLast calls Last(). (3) Each spatial method runs its own CheckPoint, and those for SpatialHash.Contains and Remove and for Octree's Near, Query and Remove have no NaN test. Running Octree.Near with a NaN z by hand panics correctly at the caller.

## A11-8 (low): stdlib.md says three things std doesn't do

Status: open

Where: docs/stdlib.md:183

(1) Line 183: "A collection's own `Filter` and `Map` stay eager and give arrays". Set.Filter gives a Set<T> and Map.Filter gives a Map<K, V> (set.fb:125, map.fb:216); only T[]'s give arrays. (2) Line 197: "`SortUnstable` (in place, no allocation)". SortUnstable() allocates a closure object each call (MIR: `_2 = new Function at std/collections/array.fb:295:22<Int32>`). Spec 14's 'makes no array' is the accurate claim. (3) Line 32 names `MergeSort` among helpers 'private to their files', but iterator.fb:332 declares it `Package Function MergeSort` because array.fb uses it.

Reproduced by the reviewer:

```
Read set.fb:125 (`Public Method Filter(keep: Func(T): Bool): Set<T>`), map.fb:216, and iterator.fb:332. `fb check --show mir` on `[3, 1, 2].SortUnstable()` shows the `new Function at ...array.fb:295:22` in SortUnstable().
```

Suggested fix: Say a collection's Filter and Map are eager and give a new collection (an array for T[], a Set or Map for those). Say 'makes no array' for SortUnstable. Move MergeSort to the Package examples beside JoinedText and IterateAs.

Skeptic: confirmed=True, low. All three are confirmed by reading and MIR. (1) stdlib.md:183 says a collection's own Filter and Map 'give arrays', but set.fb:125 Filter gives Set<T> and map.fb:216 gives Map<K, V>. (2) stdlib.md:197 says SortUnstable makes 'no allocation', but `fb check --show mir` on `[3,1,2].SortUnstable()` shows `_2 = new Function at std/collections/array.fb:295:22<Int32>`. (3) stdlib.md:32 names MergeSort among helpers private to their files, but iterator.fb:332 declares `Package Function MergeSort<T>`.

## A11-9 (low): Test comment says the panic is where the chain's consumer runs, but it's in stack.fb

Status: open

Where: tests/programs/panic_chain_changed.fb:1

The comment reads: 'A collection that changes while a chain of adapters goes over it panics at its next value, where the chain's consumer runs.' The expected output is 'panic: the Stack changed while a For ... In went over it at std/collections/stack.fb:99:42', not at the ToArray call on line 13. iterator.fb's header documents the actual behavior: through an adapter or a box, a Next panics where its panic is.

Reproduced by the reviewer:

```
cat tests/programs/panic_chain_changed.fb and panic_chain_changed.out: the comment names the consumer, and the .out names stack.fb:99:42.
```

Suggested fix: Reword it: '... panics at its next value, where its iterator's Next panics, since the adapter calls through an Iterator<T> value, which has no place to give.'

Skeptic: confirmed=True, low. panic_chain_changed.fb's comment says the panic comes 'where the chain's consumer runs' (that's line 13's ToArray). panic_chain_changed.out instead has 'at std/collections/stack.fb:99:42', which matches iterator.fb's header ('through an adapter or a box, where its panic is').

## A11-10 (low): Buckets comment gets the bucket bits backwards

Status: open

Where: std/collections/map.fb:23

The comment says the buckets are 'always a power of two, whose bits a mixed hash's top Shift bits leave'. BucketOf gives `mixed >> Shift`, the top 64 - Shift bits: with 8 buckets Shift is 61 and the bucket is the top 3 bits. The low Shift bits are what get dropped. It also says buckets exist after 'the first Add or Reserve', but Reserve(0) makes none.

Reproduced by the reviewer:

```
Read map.fb:22-25 against BucketOf (map.fb:338-341) and Grow (map.fb:360-366, shift starts at 61 for 8 buckets).
```

Suggested fix: For example: 'The first entry in each bucket, or -1. There are none until the first Add, or a Reserve of 1 or more; then always a power of two, 2 to the (64 - Shift), so a mixed hash's top 64 - Shift bits pick one.'

Skeptic: confirmed=True, low. map.fb:22-23 says the buckets are 'a power of two, whose bits a mixed hash's top Shift bits leave'. But BucketOf (map.fb:339-341) returns `mixed >> Shift`, the top 64 - Shift bits, and Grow starts Shift at 61 for 8 buckets. The low Shift bits are the ones dropped. The comment also says buckets exist after 'the first Add or Reserve', but Reserve (line 174) calls Grow only when count > Buckets.Count, so Reserve(0) makes none.

## A11-11 (low): Defaultable's comment breaks mid-sentence; Sendable's leaves out Channel

Status: open

Where: std/core/interfaces.fb:50

Lines 48-51 break after a short 'collections' is empty. It's' and carry 'DefaultValue, since Default is Select's keyword.' to a line of its own, a leftover of an edit rather than a 100-column wrap. Sendable's comment (lines 56-59) lists what the compiler counts as Sendable without Channel, which spec 10 says is Sendable and the compiler accepts (`Function Send<T: Sendable>(value: T)` takes a `Channel<Int>(4)`).

Reproduced by the reviewer:

```
Read interfaces.fb:48-59. snd.fb: `Print(Send(Channel<Int>(4)))` compiles.
```

Suggested fix: Rewrap the Defaultable comment ('... and std's collections' is empty. It's DefaultValue, since Default is Select's keyword.'). Add Channels to Sendable's list (and Mutex and Atomic, which a Shared Class's Fields can be).

Skeptic: confirmed=True, low. interfaces.fb:50-51 breaks the sentence after 'It's' and puts 'DefaultValue, since Default is Select's keyword.' on a line of its own, which isn't a 100-column wrap. The Sendable comment (lines 56-59) lists what the compiler counts as Sendable but leaves out Channel. Spec 10:233 says a Channel is 'Sendable itself', and spec 10:274 lists Channel, Mutex, and Atomic. exp-all/skeptic11/snd.fb's `Send<T: Sendable>(Channel<Int>(4))` compiles and prints 1.

## A11-12 (low): Clamp's comment doesn't say it panics

Status: open

Where: std/core/values.fb:17

Clamp is @PanicsAtCaller and panics when high < low ('Clamp takes a low end that isn't above its high end'). Its comment says only 'The value, or low when it's below low, or high when it's above high.' Other std comments name their panics ('Panics when the Stack is empty', Mem's 'Panics when `bytes` is negative').

Reproduced by the reviewer:

```
Read values.fb:17-24. tests/programs/panic_clamp.fb shows the panic.
```

Suggested fix: Add 'Panics when high is below low.' to the comment.

Skeptic: confirmed=False, low. The comment is accurate for every valid call and says nothing false. Leaving out an argument-precondition panic is the common pattern in std, not a breach of a convention: map.fb:169 Reserve, pool.fb:31 Reserve, array.fb:239 Resize, and SortUnstable are all @PanicsAtCaller, and their comments don't mention their panics either. The 'Panics when ... empty' comments describe a collection state, not an argument check. The panic is tested (panic_clamp.fb). This is not a misleading comment.

## A12-5 (low): A count past Int's max panics with a wrong message in release builds

Status: open

Where: std/collections/counter.fb:38

Counts are Int (Int32), and `Self[value] = Self[value] + times` isn't checked. In a debug build the overflow panics at the caller ('integer overflow in '+''). In a release build the sum wraps negative, and the []= check then panics with "a Counter's count isn't negative, but this one is -294967296", which blames a negative count the program never gave. Total is an Int64 'which can be more than an Int holds', but a single count can't go past an Int either, and nothing says so.

Reproduced by the reviewer:

```
`c.Add("a", 2000000000)` twice. Debug: `panic: integer overflow in '+' at t3.fb:7:1`. Release (`fb run --release`): `panic: a Counter's count isn't negative, but this one is -294967296 at t3.fb:7:1`.
```

Suggested fix: Use `Self[value].CheckedAdd(times)` and panic with a clear message like 'a Counter's count can't go past what an Int holds'. Or keep counts as Int64. Document the limit.

Skeptic: confirmed=True, low. Reproduced (a5.fb), calling c.Add("a", 2000000000) twice. A debug build gives `panic: integer overflow in '+' at a5.fb:5:1`. A release build gives `panic: a Counter's count isn't negative, but this one is -294967296 at a5.fb:5:1`. counter.fb:38's `Self[value] + times` wraps in a release build, as spec 02 allows, and then []= blames a negative count that the program never gave. Nothing documents that a single count can't go past an Int.

## A12-6 (low): Counter's comments say 'in the order the values were first added', but a value whose count went to 0 and came back goes last

Status: open

Where: std/collections/counter.fb:1

A count of 0 takes the value out of the Map, so adding it again puts it at the end. The header (line 1), MostCommon's tie order (line 87), Values (line 102) and ToString (line 127) all promise the order the values were first added, which isn't true then. MostCommon's ties come out in the wrong order for such a value.

Reproduced by the reviewer:

```
`c.Add("a")`, `c.Add("b")`, `c.Remove("a")`, `c.Add("a")`, then `Print(c, c.Values(), c.MostCommon(2))` prints `{b: 1, a: 1} [b, a] [(b, 1), (a, 1)]`, although "a" was added first.
```

Suggested fix: Reword the comments to 'in the order they were added, a value that went and came back going last' (or 'since each last had no count'), the way MultiMap's comments put it.

Skeptic: confirmed=True, low. Reproduced (a6.fb): Add a, Add b, Remove a, Add a, then Print(c, c.Values(), c.MostCommon(2)) prints `{b: 1, a: 1} [b, a] [(b, 1), (a, 1)]`. counter.fb lines 1, 87, 102 and 127 say 'in the order the values were first added', but a value that went to 0 and came back goes last, as Map's order puts it.

## A12-7 (low): The spec says LinkedList's First and Last mean what they do on an array, but they don't

Status: open

Where: docs/spec/14-strings-collections.md:237

Spec 14 says a LinkedList's `First`, `Last`, `Find`, and `Remove` 'mean what they do on an array'. An array's First()/Last() give a T? that's Null when the array is empty (array.fb:139-147). LinkedList.First()/Last() give a T and panic when the list is empty, and their Try twins give the T?. The table row and the code agree with each other, but the sentence contradicts both.

Reproduced by the reviewer:

```
`Local a: Int[] = []` and `Print(a.First() ?? -1)` prints -1; `Print(LinkedList<Int>().First())` panics with 'the LinkedList is empty'.
```

Suggested fix: Reword the sentence to its point, for example: 'Its First, Last, Find, and Remove give or take values, not nodes; what gives or takes a node says so in its name.'

Skeptic: confirmed=True, medium. Spec 14:237-238 says LinkedList's 'First, Last, Find, and Remove mean what they do on an array'. array.fb:139-147's First() and Last() give a T? that's Null when the array is empty. linked_list.fb:62-74's First() and Last() give a T and panic with 'the LinkedList is empty', and the Try twins give the T?. Find and Remove do match. The spec's sentence contradicts the code and the spec's own table row, so I raised the severity to medium, the rubric's level for docs that contradict the code.

## A12-8 (low): ImmutableMap and ImmutableSet have no AddAll

Status: open

Where: std/collections/immutable.fb:327

Spec 14 lists `AddAll` among the immutable collections' changes, and decisions.md says collections' AddAll takes any Iterable<T>, all read first. Only ImmutableArray has AddAll. ImmutableMap (line 327) and ImmutableSet (line 656) have only Add and Remove, unlike std's Map and Set, which both have AddAll.

Reproduced by the reviewer:

```
`ImmutableSet<Int>().AddAll([1, 2])` gives 'ImmutableSet<Int32> has no member named 'AddAll'', and `ImmutableMap<String, Int>().AddAll({"a": 1})` gives 'ImmutableMap<String, Int32> has no member named 'AddAll''.
```

Suggested fix: Add `AddAll(values: Iterable<T>): ImmutableSet<T>` and `AddAll(entries: Iterable<(K, V)>): ImmutableMap<K, V>`, which read everything first and Add in turn, with tests. Or narrow the spec's list to the types that have each change.

Skeptic: confirmed=False, low. Spec 14:217's member list is a union across the three immutable types: `Set`, `Insert` and `RemoveAt` are ImmutableArray's only, and `Get`, `Keys()` and `ToMap()` are ImmutableMap's only. So listing `AddAll` there doesn't promise it on ImmutableMap and ImmutableSet. decisions.md:658 and :660 say what an AddAll takes and that it reads everything first, not that every collection has one. stdlib.md lists no members for immutable.fb. AddAll is indeed missing from immutable.fb:327 and :656, but no doc promises it, so this is an API choice, not a defect.

## A12-9 (low): Slot arithmetic overflows Int for capacities above 2^30

Status: open

Where: std/collections/ring_buffer.fb:42

Head is below Capacity and Size is at most Capacity, so `Head + Size` (lines 42, 71, 100) and `Head + index` (line 174) reach up to 2 * Capacity - 1 in Int32. For a capacity above 1,073,741,824, after enough RemoveFirst or wrapped Adds, the sum passes 2^31 - 1. A debug build then panics with an integer overflow, and a release build wraps it negative, which gives a negative slot and an index panic. New() accepts any capacity of 1 or more. Found by reading the code, not run, because it needs gigabytes of memory.

Reproduced by the reviewer:

```
RingBuffer<UInt8>(1500000000), which is about 3 GB of UInt8? slots. Fill it, so Size = Capacity, and RemoveFirst 700,000,000 times, so Head = 7e8. Then Add computes Head + Size = 7e8 + 8e8 = 1.5e9, which still fits; the next Adds push it past 2^31 - 1 once Head + Size > 2147483647.
```

Suggested fix: Compute the slot without the overflowing sum, for example `Local at = Head + index` in Int64, or `index < Slots.Count - Head ? Head + index : index - (Slots.Count - Head)`. Or refuse capacities above 2^30 in New with a clear panic.

Skeptic: confirmed=True, medium. Reproduced in a release build (a9.fb, about 2.2 GB): RingBuffer<UInt8>(1100000000), filled, and then 1,050,000,000 more Adds, so Head is about 1.05e9 and Size is 1.1e9. Last() then panics `index -1044967297 is out of range for 1100000000 elements at a9.fb:12:7`. ring_buffer.fb:100's `Head + Size - 1` wraps in Int32, and the same happens at :42, :71 and :174. No RemoveFirst is needed: wrapped Adds on a full buffer are enough. I raised it to medium, since it's a wrong panic on valid use in an edge case. A debug build would panic with an integer overflow instead.

## A12-10 (low): RingBuffer's Changes comment says it counts value changes, which it doesn't

Status: open

Where: std/collections/ring_buffer.fb:13

The comment says 'How many times the values changed, so a For ... In over them can tell.' But `ring[i] = value` changes a value without counting it, which is deliberate, since the spec allows it while iterating. Only Add, RemoveFirst, RemoveLast and Clear count. Every other collection's Changes comment says 'added or removed'.

Reproduced by the reviewer:

```
Reading ring_buffer.fb: Operator []= (line 125) leaves Changes alone. A For ... In that sets ring[0] goes on without a panic (ran: it prints 1, 2, 3).
```

Suggested fix: Change it to '// How many times values were added or removed, so a For ... In over them can tell.'

Skeptic: confirmed=True, low. ring_buffer.fb:13 says 'How many times the values changed', but Operator []= (lines 124-127) changes a value without counting it, and only Add, RemoveFirst, RemoveLast and Clear count. The finding's 'every other collection says added or removed' is wrong, though: deque.fb:11 has the same wording, and Deque's []= (deque.fb:111-118) doesn't count either, so it has the same inaccuracy and should get the same fix. stack.fb, queue.fb and priority_queue.fb use that wording too, but they have no setter, so it's accurate there.

## A12-11 (low): Stray blank line before End Class

Status: open

Where: std/collections/trie.fb:214

trie.fb has a blank line between Find's End Method and End Class. No other End Class, End Struct, or End Extend in std/ has one before it.

Reproduced by the reviewer:

```
Running `grep -rn -B1 '^End Class\|^End Struct\|^End Extend' std` finds an empty preceding line only at collections/trie.fb:214.
```

Suggested fix: Delete the blank line at trie.fb:214.

Skeptic: confirmed=True, low. `grep -rn -B1 '^End Class|^End Struct|^End Extend|^End Interface' std` finds a blank line before an End line only at std/collections/trie.fb:214, between Find's End Method and End Class.
