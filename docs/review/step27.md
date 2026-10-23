# The review of M6 step 27 (std.algo)

14 confirmed findings: 1 high, 5 medium, 8 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S27-delaunay-1 (high): Delaunay panics inside std on points that nearly coincide

Status: open

Where: std/algo/delaunay.fb:184

InCircle is worked out in Doubles and isn't exact. When a few points lie within millionths of each other and others are hundreds of units away, InCircle can give the wrong sign. Bowyer-Watson then takes a cavity that isn't star-shaped from the new point, so Insert's Starts linking leaves edges unlinked or linked wrong, and a later Insert indexes with -1 or past outsideEdge. The panic is std's own, at an internal line rather than at the caller, and it is the same in debug and release. The lane's call says nearly cocircular points take the Double's answer. Here the inexact answer does more than pick a different valid triangulation: it corrupts the mesh. The tests have no near-degenerate float inputs. In a scan with 5 points spread over ±500 and 4 within ±5e-6 of the origin, roughly 1 run in 20 panicked. Found by random scans, with every result checked by an exact rational brute-force Delaunay in Python.

```
Import std.core
Import std.math
Import std.algo

// Two points far off, and four within a few millionths of the origin.
Local points = [Vec2(408.96606, -216.43066), Vec2(262.08496, 219.48242),
                Vec2(-3.2101439e-06, -1.6921997e-06), Vec2(-2.2320555e-06, 2.8988648e-06),
                Vec2(1.8127442e-07, 3.326416e-06), Vec2(-2.0388793e-06, 8.404541e-07)]
Print(Delaunay(points).Count)
```

Expected: 6. The exact Delaunay triangulation is (0, 1, 4), (0, 4, 2), (1, 3, 4), (2, 4, 5), (2, 5, 3), (3, 5, 4).

Actual: panic: index 3 is out of range for 3 elements at std/algo/delaunay.fb:184:43, in both debug and --release. Adding (-304.5349, -479.4922), (317.65747, -491.60767), and (350.09766, 460.51025) to the list panics instead with "index -1 is out of range for 19 elements at std/algo/delaunay.fb:157:20".

Suggested fix: Make the predicates robust: port Shewchuk's adaptive orient2d and incircle, with Fma for the exact two-product, so each sign is exact. If the predicates stay as they are, keep the cavity star-shaped: before making the new triangles, check that every boundary edge (a, b) has Orient(a, b, p) > 0, and where one doesn't, take the triangle across it into the cavity and check again. Add this program and the S27-delaunay-2 and S27-delaunay-3 programs to tests/programs/algo_delaunay.fb.

Skeptic: Ran the program at C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s27/del/d1.fb. Debug and --release both print "panic: index 3 is out of range for 3 elements at std/algo/delaunay.fb:184:43". With the three extra points (d1b.fb), both modes print "panic: index -1 is out of range for 19 elements at std/algo/delaunay.fb:157:20". I rounded the inputs to Float and ran an exact rational brute force in Python (py/exact.py). The six points are distinct and finite, and their Delaunay triangulation is (0,1,4), (0,4,2), (1,3,4), (2,5,3), (2,4,5), (3,5,4), as the finding says. The code fits the explanation: line 184 indexes outsideEdge[i], which is short when the j loop at lines 167-172 finds no back-link, and line 157 indexes Marks with an Across of -1. Both are a corrupted mesh. The panic is inside std, on valid input, and not at the caller. That breaks the milestone's done-when ('triangulations match reference results') and goes beyond the lane's call that nearly cocircular points take the Double's answer.

## S27-delaunay-2 (medium): Delaunay gives a clockwise triangle, and drops another, for points a few Float steps apart

Status: open

Where: std/algo/delaunay.fb:158

This has the same root cause as S27-delaunay-1, here without a crash. With three points a few ulps apart and two points far away, the inexact InCircle grows the cavity across an edge the new point can't see. Insert then makes a triangle (a, b, p) with p on the wrong side, so the result has a clockwise triangle, which docs/stdlib.md and the doc comment rule out ('the triangles, counterclockwise'). It also loses the small triangle (0, 1, 2), and the triangles overlap. Orient's sign is exact for these inputs: every difference and product fits a Double. Near-duplicate points from float noise are common in real data. In clustered scans (clusters of spread 1e-4 to 1e-5 in a 1000-unit square), about 1 trial in 60 to 1 in 1200 had clockwise or overlapping triangles.

```
Import std.core
Import std.math
Import std.algo

// Three points a few Float steps apart, and two far off.
Local points = [Vec2(27.051820755004883, 103.13211822509766),
                Vec2(27.051820755004883, 103.13212585449219),
                Vec2(27.051815032958984, 103.13212585449219),
                Vec2(287.8077087402344, 913.6482543945312),
                Vec2(645.5819702148438, 841.5291137695312)]
For t In Delaunay(points)
    Local a = points[t.0]
    Local b = points[t.1]
    Local c = points[t.2]
    // Twice the area, exact here: the differences and products fit a Double.
    Local twice = ((b.x As Double) - a.x) * ((c.y As Double) - a.y) -
                  ((b.y As Double) - a.y) * ((c.x As Double) - a.x)
    Print(t, twice > 0)
End For
```

Expected: Four lines, each ending in True. The exact triangulation is (0, 1, 2), (0, 4, 1), (1, 3, 2), (1, 4, 3).

Actual: (2, 1, 3) True
(3, 1, 4) True
(2, 0, 4) True
(1, 2, 4) False
(the same in --release)

Suggested fix: Same fix as S27-delaunay-1: exact or adaptive incircle and orient2d, or a star-shaped check on the cavity's boundary edges before the new triangles are made. Test that every triangle is counterclockwise on near-duplicate inputs like this one.

Skeptic: d2.fb prints (2, 1, 3) True, (3, 1, 4) True, (2, 0, 4) True, (1, 2, 4) False, the same in debug and --release. With the inputs rounded to Float, exact rational Orient also says (1,2,4) is clockwise. The exact triangulation is (0,1,2), (0,4,1), (1,3,2), (1,4,3), so the result has a clockwise triangle and is missing the small one. That contradicts the doc comment and stdlib.md ('the triangles, counterclockwise'). It has the same root cause as delaunay-1 (Insert at lines 140-196 trusts the InCircle cavity), here without a crash.

## S27-delaunay-3 (medium): Delaunay silently drops a distinct point, and Voronoi then gives that point the whole bounds

Status: open

Where: std/algo/delaunay.fb:142

When no triangle conflicts with a point numerically, Locate's fallback scan gives -1 and Insert returns ('If first < 0 Then Return'). The point is left out of the triangulation without a word, though in exact arithmetic every distinct point is a corner. Voronoi builds each point's neighbors only from the triangles, so a dropped point has no neighbors, and its cell is all of bounds, overlapping every other cell. In a scan with 4 points within ±5e-6 of the origin and 5 within ±500, about 1% of trials dropped a point. With ±5e-8, about 4% did.

```
Import std.core
Import std.math
Import std.algo

// A point far off, and three a few millionths from the origin, the second inside the triangle
// of the others.
Local points = [Vec2(271.026611328125, 234.06982421875),
                Vec2(2.930908067355631e-06, -1.3082885743642692e-06),
                Vec2(3.992309302702779e-06, -2.3941038307384588e-06),
                Vec2(2.241821221105056e-06, -8.782958502706606e-07)]
Print(Delaunay(points))
Local cells = Voronoi(points, Rect(-500, -500, 1000, 1000))
Print(cells[1])
```

Expected: Three triangles, which by exact arithmetic are (0, 1, 2), (0, 3, 1), and (1, 3, 2), in some order. Cell 1 is a small polygon near the origin, not the whole bounds.

Actual: [(3, 2, 0)]
[(-500, -500), (500, -500), (500, 500), (-500, 500)]
(the same in --release)

Suggested fix: Use robust predicates, as in S27-delaunay-1. Beyond that, never drop a point: when nothing conflicts numerically, start the cavity at the triangle that contains the point by Orient, or at the infinite triangle whose hull edge it is on or past. Voronoi could also guard by giving any distinct point with no Delaunay neighbors every other point as neighbors.

Skeptic: d3.fb prints [(3, 2, 0)] and then [(-500, -500), (500, -500), (500, 500), (-500, 500)] for cell 1, in debug and --release. Exact rational brute force gives (0,1,2), (0,3,1), (1,3,2), so distinct point 1 is dropped. The code shows how: Locate returns -1 when nothing conflicts numerically (line 136), and Insert then returns at line 142 ('If first < 0 Then Return'). Voronoi takes neighbors only from the triangles (lines 341-346), so point 1 has none and gets the whole bounds. That breaks the doc's promise that only repeats aren't corners, and gives a cell that overlaps every other cell.

## S27-delaunay-4 (medium): Voronoi and LloydRelax don't check bounds: a negative or NaN size gives clockwise or degenerate cells instead of a panic

Status: open

Where: std/algo/delaunay.fb:348

Rect is documented as 'of a Width and Height that aren't negative', and std's convention is that a bug such as a negative size panics at the caller. Voronoi checks that the points are finite but never checks bounds. With a negative Width, every cell comes out clockwise, against the doc's 'counterclockwise'. With a NaN Width, one cell is a two-corner 'polygon' and the other is empty. LloydRelax quietly goes on with such bounds.

```
Import std.core
Import std.math
Import std.algo

Local points = [Vec2(-1, 1), Vec2(-3, 1)]
// A Width that's negative, which Rect says it isn't.
Print(Voronoi(points, Rect(0, 0, -4, 4)))
// A NaN Width, and an infinite one.
Print(Voronoi(points, Rect(0, 0, NaN, 4)))
Print(Voronoi(points, Rect(0, 0, Inf, 4)))
Print(LloydRelax(points, Rect(0, 0, NaN, 4)))
```

Expected: A panic at the first Voronoi call's line, saying bounds has a size that isn't negative. A NaN or infinite bounds panics the same way, as a NaN or infinite point already does.

Actual: [[(0, 0), (-2, 0), (-2, 4), (0, 4)], [(-2, 0), (-4, 0), (-4, 4), (-2, 4)]]   (both clockwise)
[[(0, 0), (0, 4)], []]
[[(0, 0), (inf, 0), (inf, 4), (0, 4)], []]
[(-1, 1), (-3, 1)]

Suggested fix: In Voronoi, which LloydRelax goes through, panic (it is already @PanicsAtCaller) when bounds.X, Y, Width, or Height isn't finite, or when Width or Height is negative. Document this beside the points' check, and add panic programs for it.

Skeptic: d4.fb (debug) gives these results:
- Rect(0,0,-4,4): [[(0, 0), (-2, 0), (-2, 4), (0, 4)], [(-2, 0), (-4, 0), (-4, 4), (-2, 4)]], both cells clockwise.
- NaN Width: [[(0, 0), (0, 4)], []].
- Inf Width: [[(0, 0), (inf, 0), (inf, 4), (0, 4)], []].
- Rect(NaN, 0, 4, 4): [[], []].
- LloydRelax with a NaN Width: [(-1, 1), (-3, 1)], quietly.
Voronoi (lines 348-351) uses bounds unchecked. Its own doc says 'A coordinate that's infinite or NaN panics', but a NaN bounds.X doesn't panic. stdlib.md's rule says a bug such as a negative size panics at the caller, and std's QuadTree/Octree New (std/collections/spatial.fb:338-345) does panic on a non-finite or inverted box. Medium because only invalid bounds reach it, but it gives silent garbage and clockwise cells against the doc.

## S27-order-1 (medium): Permutations and Combinations with a count above the array's length fill an array of count Ints before giving nothing

Status: open

Where: std/algo/combinatorics.fb:24

PermutationIterator.New sets Done = count > items.Count but still runs `For i = 0 Until count: Turns.Add(items.Count - i)`. CombinationIterator.New does the same with `For i = 0 Until count: Indices.Add(i)` (lines 91-94). The docs say a count above n gives none, so the answer is known without any work. Instead the iterator allocates and fills count Ints that it never reads. A count from user data, such as a choice size larger than the pool, can cost seconds and gigabytes, or exhaust memory near Int's maximum. The tests only use counts of 3 and 5 over two elements, so they don't catch it.

```
Import std.core
Import std.algo

// A count above the array's length gives no orders and no choices, which should take no time or
// memory to find out.
Print(Permutations([1, 2, 3], 500000000).Count(), Combinations([1, 2, 3], 500000000).Count())
```

Expected: Prints `0 0` at once, in a few MB of memory.

Actual: Prints `0 0` only after about 5.0 s (debug) and 4.4 s (release), with a peak working set of about 3960 MB in both. Counts of 2,000,000,000 took about 21 s.

Suggested fix: Fill Turns and Indices only when count <= items.Count: in both News, return right after setting Done when it's true (`If Done Then Return`). Alternatively, loop Until Min(count, items.Count). Add the program above, with a smaller count, to algo_combinatorics.fb.

Skeptic: I built the finding's program (Permutations([1, 2, 3], 500000000).Count() and Combinations([1, 2, 3], 500000000).Count()) in debug and release at C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s27/order/big.fb and timed it while polling PeakWorkingSet64. Debug printed `0 0` after 5.03 s with a peak of 3961 MB. Release printed `0 0` after 4.37 s with a peak of 3960 MB. The code is as described. In std/algo/combinatorics.fb, PermutationIterator.New sets `Done = count > items.Count` (line 24) and still runs `For i = 0 Until count: Turns.Add(items.Count - i)` (lines 28-30). CombinationIterator.New sets Done at line 91 and still runs `For i = 0 Until count: Indices.Add(i)` (lines 92-94). Next() returns Null at once when Done is set, so these arrays are never read. The docs (lines 139-140 and 147-149, and stdlib.md) say a count above n gives none. That is a valid input, yet it costs O(count) time and memory and can run out of memory for large counts. The fix is an early return when Done, or bounding the loops by items.Count. I kept it at medium because a valid, documented input exhausts resources.

## S27-search-1 (medium): AStar can give a dearer path when the guess is admissible but not consistent, though its doc says admissible is enough

Status: open

Where: std/algo/search.fb:255

AStar's doc (search.fb:312-315) gives the condition for the cheapest path as 'the guess can't be more than what's left', which is admissibility. Search.New never reopens a node once it's settled: at line 255 it skips any cheaper cost found later for a done node. Without reopening, A* only finds the cheapest path when the guess is consistent (h(n) <= w(n,m) + h(m)). In the program below every guess is no more than what's left (S 7, A 6, B 8, C 5, G 0), but A's guess of 6 is more than the step A->C (1) plus C's guess (0). C gets settled through B at 4, and the cheaper way through A at 2 is then thrown away. Dijkstra on the same graph finds the right path, and so does the random cross-check with consistent guesses (Bellman-Ford, 300 graphs, 0 mismatches). The bug only shows up with guesses that are admissible but not consistent, and the doc says those are fine.

```
// A* with a guess that's never more than what's left (admissible), but not consistent.
Import std.core
Import std.collections
Import std.algo

Local edges = Map<String, (String, Int)[]>()
edges["S"] = [("A", 1), ("B", 1)]
edges["A"] = [("C", 1)]
edges["B"] = [("C", 3)]
edges["C"] = [("G", 5)]
edges["G"] = []
// What's really left: S 7, A 6, B 8, C 5, G 0. Each guess is no more than that.
Local guesses = Map<String, Int>()
guesses["S"] = 0
guesses["A"] = 6
guesses["B"] = 0
guesses["C"] = 0
guesses["G"] = 0
Local star = AStar("S", Function(n) => edges[n], Function(n) => guesses[n], Function(n) => n == "G")
Local none: String[] = []
Print(star?.Nodes ?? none, star?.Cost ?? -1)
Local plain = Dijkstra("S", Function(n) => edges[n], Function(n) => n == "G")
Print(plain?.Nodes ?? none, plain?.Cost ?? -1)
```

Expected: [S, A, C, G] 7
[S, A, C, G] 7

Actual: [S, B, C, G] 9
[S, A, C, G] 7   (the same in debug and release)

Suggested fix: Either reopen a settled node when A* finds it cheaper. In AStar's mode, when done[to] and cost < Costs[to], set done[to] = False, update Parents/Costs, and push it again. This is correct for any admissible guess and never happens with Dijkstra's guess of 0, so DijkstraAll's Settled list is unaffected. Or say in the doc comment (and stdlib.md) that the guess must be consistent: at a node, no more than a step's weight plus the guess where the step ends, which the straight-line, octile, and Manhattan distances are. Either way, add this graph to algo_search.fb.

Skeptic: Ran the reporter's program unchanged (rev-scratch/s27/search-v/astar_inconsistent.fb). Debug and release both print "[S, B, C, G] 9" for AStar and "[S, A, C, G] 7" for Dijkstra. Every guess is admissible: the costs left are S 7, A 6, B 8, C 5, G 0, and the guesses are 0, 6, 0, 0, 0. The guess at A isn't consistent, since 6 > w(A,C) 1 + h(C) 0. Trace: S pops, then B (total 1), which pushes C at 4. C pops and settles at 4. A pops at total 7, finds C at 2, and search.fb:255 (`ElseIf done[to] || !(cost < Costs[to])` then Continue) throws that away because C is done. AStar's doc comment (search.fb:312-315) says the path is the cheapest when 'the guess can't be more than what's left', which is admissibility, but without reopening the code needs consistency. The documented guarantee is broken and the path is silently wrong. GridPath's octile and Manhattan guesses are consistent, so GridPath isn't affected.

## S27-delaunay-5 (low): Voronoi clips by each Delaunay neighbor twice, which leaves stray corners partway along edges

Status: open

Where: std/algo/delaunay.fb:343

neighbors[a].AddAll([b, c]) runs once per triangle, so every interior Delaunay neighbor is in the list twice. The second clip by the same bisector sees the corners on that edge at sides of about +-1e-16 and can add an intersection partway along the edge. That gives a cell an extra collinear corner whose position depends on scale. The test's six points scaled by 10^30 (or 10^-30) give cell 4 six corners instead of five. In a copy of delaunay.fb that lists each neighbor once, the stray corner goes away at both scales. It also doubles the clipping work.

```
Import std.core
Import std.collections
Import std.math
Import std.algo

// The test's six points and bounds, and the same scaled by 10^30: the cells have the same shape.
Local six = [Vec2(0, 0), Vec2(4, 0), Vec2(0, 3), Vec2(5, 4), Vec2(2, 1.5), Vec2(1, 3.7)]
Local big = six.Map(Function(p: Vec2) => p * 1e30)
Print(Voronoi(six, Rect(-1, -1, 7, 6))[4].Count, Voronoi(big, Rect(-1e30, -1e30, 7e30, 6e30))[4].Count)
Print(Voronoi(big, Rect(-1e30, -1e30, 7e30, 6e30))[4])
```

Expected: 5 5. The scaled cell has the five corners of the unscaled one, times 10^30.

Actual: 5 6
[(1.13965505e+30, 2.4362069e+30), (4.3749994e+29, 1.5000001e+30), (1.5625e+30, 3.0223146e+22), (2e+30, -5.8333323e+29), (4.026316e+30, 2.1184211e+30), (3.041209e+30, 3.3005494e+30)]
(1.5625e30, ~0) lies on the edge from (0.4375e30, 1.5e30) to (2e30, -0.5833e30).

Suggested fix: Add each neighbor once, for example only when it isn't already in the list, or by collecting them in a Set.

Skeptic: d5.fb prints '5 6' in debug and --release. Scaled by 1e30, cell 4 has the extra corner (1.5625e+30, 3.0223146e+22). Worked out, that corner lies on the edge from (4.375e29, 1.5e30) to (2e30, -5.833e29): t = 0.72 gives y = 0. The unscaled cell has the five corners. The code adds each neighbor twice: line 343's neighbors[a].AddAll([b, c]) runs once per triangle, and every interior edge has two triangles. So each bisector clips twice, which doubles the work and lets rounding add a collinear corner. Low: it's cosmetic and scale-dependent.

## S27-delaunay-6 (low): Voronoi cells can be degenerate: one or two corners, or a corner twice in a row

Status: open

Where: std/algo/delaunay.fb:369

The doc says each cell is 'the polygon, counterclockwise', and that a cell is empty for a point whose cell is outside bounds. A point outside bounds whose cell only touches them gets a one-corner or two-corner list instead, or a zero-area list with a repeated corner. Random scans with bounds offset from integer points hit this in about 20% of trials. Separately, where two bisectors meet on the bounds' edge, the two near-equal Double corners round to the same Float and appear twice in a row in a cell that has area. LloydRelax copes, since a zero area gives Null, but code that draws or walks the edges gets zero-length edges and non-polygons.

```
Import std.core
Import std.math
Import std.algo

Local square = Rect(0, 0, 4, 4)
// Cells of points outside bounds that only touch them, at a corner and along an edge.
Print(Voronoi([Vec2(0, 0), Vec2(8, 8)], square)[1])
Print(Voronoi([Vec2(2, 2), Vec2(2, -2)], square)[1])
// A cell with one corner twice in a row, where two bisectors meet on the bounds' edge.
Print(Voronoi([Vec2(3, 2), Vec2(6, 1), Vec2(1, 2), Vec2(6, 3)], Rect(0, 2, 11, 10))[0])
```

Expected: []
[]
[(2, 2), (4.6666665, 2), (2, 10)]
(or, if touching cells are meant to be kept, a doc that says a cell can be a point or a segment)

Actual: [(4, 4)]
[(0, 0), (4, 0)]
[(2, 2), (4.6666665, 2), (4.6666665, 2), (2, 10)]

Suggested fix: After converting a cell's corners to Float, drop any corner equal to the one before it (wrapping around). Make the cell empty when fewer than three corners remain or its area is 0, and say so in the doc.

Skeptic: d6.fb prints [(4, 4)], then [(0, 0), (4, 0)], then [(2, 2), (4.6666665, 2), (4.6666665, 2), (2, 10)]. Worked out by hand, in the third case the bisectors y=3x-12 and y=-3x+16 both meet the bounds' edge y=2 at x=14/3. The exact cell is the triangle (2,2), (14/3,2), (2,10), but the output has 14/3 twice in a row after the Float conversion at lines 369-372, a zero-length edge. The first two results are a point and a segment, not a polygon or empty as the doc describes. Low: LloydRelax copes, and the touching cells are degenerate rather than wrong regions, but the repeated corner is a defect.

## S27-delaunay-7 (low): Voronoi of collinear points is quadratic: 20000 points on a line take about a minute

Status: open

Where: std/algo/delaunay.fb:333

When there are no triangles, every distinct point is given every other distinct point as a neighbor, so n collinear points take n^2 KeepNearer clips, each of which allocates an array. In release, 5000 points on a line take about 4.5 s against about 1.0 s when every other point is moved off the line. 20000 points on a line take 57 s. Points along a line, such as a path or a row, are a plausible input. Sorted by x and then y, collinear points are in line order, so only the points before and after each one matter.

```
Import std.core
Import std.math
Import std.algo

Local points: Vec2[] = []
For i = 0 Until 20000
    points.Add(Vec2(i As Float, 0))
End For
Local cells = Voronoi(points, Rect(-1, -1, 20001, 2))
Print(cells.Count, cells[10])
```

Expected: 20000 [(9.5, -1), (10.5, -1), (10.5, 1), (9.5, 1)] in about a second, as for points not on a line

Actual: The right output, but `fb run --release` takes 57 s of wall time.

Suggested fix: In the no-triangles case with three or more distinct points, which are then all on one line, give each point only the points before and after it in `order` as neighbors.

Skeptic: I built d7.fb and d7b.fb in release and timed them. 20000 points on a line take real 0m56.467s and print the right '20000 [(9.5, -1), (10.5, -1), (10.5, 1), (9.5, 1)]'. The same count with every other point off the line (y = 0.5) takes real 0m0.103s. Lines 333-339 give every distinct point every other one as a neighbor when there are no triangles, so the clipping is n^2. The output is correct, so this is low, but it's a 500x quadratic cliff on plausible input.

## S27-delaunay-9 (low): Mesh.InfiniteAt's comment says it gives the first real corner past the point at infinity, but it gives the point at infinity's own position

Status: open

Where: std/algo/delaunay.fb:74

The comment reads 'The triangle's first real corner past the point at infinity, or -1 for a real triangle'. The method returns the k where Corners[k] < 0, which is where the point at infinity is, and every caller adds 1 and 2 to reach the real corners ((infinite + 1) % 3 for u). The program copies the method word for word.

```
Import std.core
Import std.collections

// Mesh.InfiniteAt from std/algo/delaunay.fb, as written, on the triangle (5, 7, infinity).
Class Mesh
    Field Corners: Int[] = []

    Method InfiniteAt(triangle: Int): Int
        For k = 0 Until 3
            If Corners[triangle * 3 + k] < 0 Then Return k
        End For
        Return -1
    End Method
End Class

Local mesh = Mesh()
mesh.Corners.AddAll([5, 7, -1])
Local k = mesh.InfiniteAt(0)
Print(k, mesh.Corners[k])
```

Expected: Per the comment, 0 5: the first real corner past the point at infinity.

Actual: 2 -1: the point at infinity's own position.

Suggested fix: Reword the comment to say it gives where the point at infinity is among the triangle's corners, or -1 for a real triangle.

Skeptic: d9.fb, a word-for-word copy of InfiniteAt, prints '2 -1'. The method returns the k where Corners[k] < 0, the point at infinity's own position. Every caller (lines 95-96, 116) adds 1 and 2 to reach the real corners. The comment at line 74 ('The triangle's first real corner past the point at infinity') contradicts the code. It's a comment-only fix.

## S27-order-2 (low): TopologicalSort's docs say unordered nodes keep their given order, which the code doesn't do and no order always can

Status: open

Where: std/algo/topological.fb:37

topological.fb's comment and docs/stdlib.md's std.algo entry both claim this: by taking the earliest ready node first, "nodes with no order between them keep the one they were given in". Kahn's algorithm with a min-queue gives the lexicographically least order by index, and that doesn't keep the relative order of every unordered pair. Take nodes given as [a, b, c] with one edge c -> a. Nothing orders a against b, and a is given first, yet the result puts b first. No order can meet the claim in this case: c must come before a, and the claim would put a before b and b before c. So the wording is wrong, not the algorithm. What does hold is that at each step the earliest ready node goes first, and that nodes nothing leads to keep their given order among themselves. The test's cases all happen to keep the claim.

```
Import std.core
Import std.algo

// a and b have no order between them; a is given first.
Local next = Function(n: String): String[] => n == "c" ? ["a"] : []
Local none: String[] = []
Print(TopologicalSort(["a", "b", "c"], next) ?? none)
```

Expected: By the docs, a comes before b, since nothing orders them and a is given first. The claim can't be met here (c must precede a, and b must precede c), so the docs should describe the order the code actually gives: [b, c, a].

Actual: [b, c, a]: b comes before a.

Suggested fix: Reword topological.fb lines 37-39 and stdlib.md's TopologicalSort entry to say what holds. Of the nodes that could come next, the earliest given goes first, which gives the least order by position. Nodes that nothing leads to keep the order they were given in.

Skeptic: I ran the finding's program (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s27/order/topo.fb: nodes ["a", "b", "c"], one edge c -> a) in debug, and it printed `[b, c, a]`. Nothing orders a against b and a is given first, yet b comes first. That contradicts std/algo/topological.fb lines 37-39 ("so nodes with no order between them keep the one they were given in") and docs/stdlib.md line 540-541, which says the same. No order can meet the claim here: it would need a<b and b<c (both pairs unordered, given in that order), while the edge needs c<a, which is a cycle. So the claim, which the lane decision also states, can't hold in general. The algorithm, Kahn's with a min-queue by given position, is fine and gives the least order by position. Only the wording in the comment and stdlib.md is false. It's a doc defect, so low.

## S27-search-2 (low): A NaN guess from AStar's heuristic isn't checked and silently gives a dearer path

Status: open

Where: std/algo/search.fb:263

Weights and FlowField costs panic at the caller when they're NaN, which is std's rule for NaN inputs. The guess heuristic gives isn't checked at all (lines 224 and 263). A NaN guess makes that node's Total NaN, so Before() can't order it against anything: neither a<b nor b<a. It stays at the top of the heap and comes out first, so its node is settled at a cost that isn't its cheapest. A cheaper way found later is then thrown away because the node is done. The result is a wrong path with no panic. Users can easily hit a NaN guess from a distance function on bad coordinates, e.g. Sqrt of a negative number.

```
// A* with a guess that's NaN at one node.
Import std.core
Import std.collections
Import std.math
Import std.algo

Local edges = Map<String, (String, Double)[]>()
edges["S"] = [("X", 5), ("A", 1)]
edges["A"] = [("X", 1)]
edges["X"] = [("G", 1)]
edges["G"] = []
Local guess = Function(n: String): Double => n == "X" ? NaN : 0
Local star = AStar("S", Function(n) => edges[n], guess, Function(n) => n == "G")
Local none: String[] = []
Print(star?.Nodes ?? none, star?.Cost ?? -1)
```

Expected: A panic at the AStar call (line 13), like a NaN weight's, e.g. "panic: a guess can't be NaN ... at <file>:13:12"

Actual: [S, X, G] 6   (debug and release; the cheapest path is [S, A, X, G] at 3)

Suggested fix: In Search.New, check each guess where it's made (the start's and each successor's): If left != left Then Panic("a guess can't be NaN"). C is Comparable, so it's Equatable, and NaN != NaN. Add a panic_algo_astar_guess program, and mention it in AStar's comment and stdlib.md.

Skeptic: Ran the reporter's program (astar_nan.fb). Debug and release both print "[S, X, G] 6", with no panic; the cheapest path is S-A-X-G at 3. Search.New never checks heuristic's result (search.fb:224, 263), while a NaN weight (line 243) and a NaN FlowField cost do panic. The NaN Total goes into the PriorityQueue's order. decisions.md's NaN row says a NaN panics where 'an order can't place it' (SortedMap, IntervalTree, and spatial all check), so this is a gap in std's NaN rule. I set it low rather than medium for two reasons. First, AStar's doc already says a guess that breaks its condition can give a dearer path. Second, the damage stays local: in 300 random graphs where only a dead end's guess was NaN (astar_nan_deadend.fb), AStar and Dijkstra gave the same cost every time ('wrong 0'), so the heap didn't misorder the other nodes.

## S27-search-3 (low): GridNeighbors doesn't panic for a cell off the grid, as stdlib.md says; at the Int edge, debug panics inside std/math while release gives []

Status: open

Where: std/algo/grid.fb:45

stdlib.md's grid bullet says '... a cell off the grid, or a cost that's negative or NaN panics'. GridNeighbors never checks its cell. It gives neighbors for a cell off the grid: (-1, 0) on a 3x3 grid gives [(0, 0)], and algo_grid.fb relies on (9, 9) giving []. With a cell at Int's limit, `cell + step` in Steps' loop overflows inside Vec2i's Operator +. Debug builds then panic at std/math/int_vector.fb, not at the caller, even though GridNeighbors is @PanicsAtCaller. Release builds wrap and give []. So the doc and the code disagree, and debug and release give different results.

```
// GridNeighbors of cells off the grid.
Import std.core
Import std.math
Import std.algo

Print(GridNeighbors(3, 3, Vec2i(-1, 0)))
Print(GridNeighbors(3, 3, Vec2i(2147483647, 0)))
```

Expected: As stdlib.md says: "panic: (-1, 0) is off a grid 3 by 3 at <file>:6:7". Failing that, the same result in debug and release for the second line.

Actual: debug: [(0, 0)] then "panic: integer overflow in '+' at std/math/int_vector.fb:31:52"; release: [(0, 0)] then []

Suggested fix: Either call CheckCell(width, height, cell) in GridNeighbors (and change algo_grid.fb's (9, 9) case into a panic program), or keep the lenient behavior. In that case, return [] for a cell that isn't on the grid or next to it, before adding steps, so nothing overflows. Then make stdlib.md and the doc comment say what GridNeighbors does with an off-grid cell.

Skeptic: Ran grid_off.fb. Debug prints "[(0, 0)]" and then "panic: integer overflow in '+' at std/math/int_vector.fb:31:52" (exit 101). Release prints "[(0, 0)]" and then "[]". stdlib.md's grid bullet (line 535-536) ends with 'A size that's negative or past what an Int counts, a cell off the grid, or a cost that's negative or NaN panics'. The size clause applies to GridNeighbors, which calls CheckSize, so the cell clause reads as applying too. GridNeighbors (grid.fb:45-53) never calls CheckCell, and tests/programs/algo_grid.fb:24 relies on GridNeighbors(4, 2, Vec2i(9, 9)) giving []. Doc and code contradict each other. At Int's limit, the @PanicsAtCaller function panics inside std/math, not at the caller's line as stdlib.md's convention (line 151) asks. The debug/release split there is the language's overflow rule (spec 02-types: release wraps), so that part alone isn't a std bug. I set it low: the lenient answer for an off-grid cell is harmless, and the overflow needs a coordinate at Int's limit. It's still a plain stdlib.md mismatch.

## S27-search-4 (low): Dijkstra, DijkstraAll, and AStar also need the weight type to be Printable, which the docs don't say

Status: open

Where: std/algo/search.fb:204

stdlib.md says the weights are 'of any Numeric and Comparable type', and search.fb's header says 'of any Numeric type'. Search, Dijkstra, DijkstraAll, and AStar also require C: Printable (lines 204, 283, 292, 320), only so the negative-weight panic can show the weight. Numeric doesn't extend Printable, so a program's own fixed-point weight type that implements Numeric and Comparable is refused at compile time.

```
// A weight of the program's own Numeric and Comparable type, as stdlib.md allows.
Import std.core
Import std.algo

Struct Fixed Implements Numeric, Comparable
    Field Units: Int

    Static Method Zero(): Fixed => Fixed(0)
    Static Method One(): Fixed => Fixed(100)
    Operator +(a: Fixed, b: Fixed): Fixed => Fixed(a.Units + b.Units)
    Operator -(a: Fixed, b: Fixed): Fixed => Fixed(a.Units - b.Units)
    Operator *(a: Fixed, b: Fixed): Fixed => Fixed(a.Units * b.Units / 100)
    Operator /(a: Fixed, b: Fixed): Fixed => Fixed(a.Units * 100 / b.Units)
    Operator <(a: Fixed, b: Fixed): Bool => a.Units < b.Units
End Struct

Local steps = Function(n: Int): (Int, Fixed)[] => n < 3 ? [(n + 1, Fixed(150))] : []
Local path = Dijkstra(0, steps, Function(n) => n == 3)
Print(path?.Cost?.Units ?? -1)
Local all = DijkstraAll(0, steps)
Print(all.Nodes)
```

Expected: 450
[0, 1, 2, 3]

Actual: own_weight.fb:18:14: error: Fixed doesn't implement Printable, which 'Dijkstra' needs for C
own_weight.fb:20:13: error: Fixed doesn't implement Printable, which 'DijkstraAll' needs for C

Suggested fix: Drop Printable from the bounds and make the panic not show the weight ("a step's weight can't be negative or NaN"), or show it only through a conditional overload. Otherwise, say in stdlib.md and search.fb's header that the weight is Numeric, Comparable, and Printable.

Skeptic: Ran own_weight.fb as given. It fails to compile: "own_weight.fb:18:14: error: Fixed doesn't implement Printable, which 'Dijkstra' needs for C" and the same error for DijkstraAll at 20:13. Those are its only errors, so Fixed otherwise satisfies Numeric and Comparable. stdlib.md:521 says the weight is 'of any Numeric and Comparable type', and search.fb's header says 'any Numeric type'. Search, Dijkstra, DijkstraAll, and AStar (search.fb:204, 283, 292, 320) also require Printable, only for the panic text at line 244. This is a plain doc/code mismatch that shows up only at compile time, so it's low.

## Refuted by the skeptics (no action)

- S27-delaunay-8: LloydRelax's doc says it spreads points evenly, but equal points never separate. d8.fb prints [(0.81384444, 1.6290865), (0.81384444, 1.6290865), (2.3659847, 3.18715), (2.9314003, 1.070192)], so the equal points do stay stacked. That follows from documented behavior, though. Voronoi's doc says 'Points that are equal share a cell', and LloydRelax moves each point to its cell's centroid (both doc comment and stdlib.md), so equal points get the same centroid. That's standard Lloyd relaxation. 'Spreads them evenly' describes distinct points, and nothing in the spec, stdlib.md, or milestones row is plainly contradicted. At most a doc clarification.
