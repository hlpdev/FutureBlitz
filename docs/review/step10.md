# The review of M6 step 10 (Math II), all but Rand, RandInt, RandFloat, SeedRandom, and arrays' Shuffle() and Sample(), which wait on the compiler's ThreadLocal Global

15 confirmed findings: 0 high, 4 medium, 11 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S10-geometry-1 (medium): Spline curves measure their length with 256 samples in total, however many segments they have, so Length, PointAtDistance and Sample are wrong for splines through many points

Status: open

Where: std/math/curves.fb:181

Curve2.CatmullRom, Curve2.BSpline and Curve2.Bezier, and the Curve3 versions at lines 273, 287 and 303, all build their curve as Curve2(Function ...) or Curve3(Function ...). That uses the constructor's default of 256 samples (line 168/260) for the whole curve, not 256 per segment, and the static Methods give the caller no way to pass a sample count. Once a spline has more than a few dozen segments, the length table is made of chords that skip whole segments, so it falls short. On a zigzag through 300 points, Length is 343.65. The true length is 435.68, and even the polyline through the points is 422.85, so the measured curve comes out shorter than the straight lines between its own points. PointAtDistance(100) lands 18 points too far along, and Sample's posts are not evenly spaced. A more realistic winding road with 500 waypoints 1 apart measures 489.13 against a true 501.39 (2.4% short); 200 waypoints are 0.6% short and 100 zigzag points 3% short. This breaks the class comment's promise ('more samples follow its bends closer') and stdlib.md's 'points evenly apart along the curve' for exactly the long paths (race lines, patrol routes) that arc-length sampling is for. Debug and release print the same. No test makes a spline through more than 4 points, so the tests don't catch it.

```
Import std.core
Import std.math

// A zigzag through 300 points: the spline through them is longer than the 422.85 of straight lines
// between them, but its Length, from 256 samples over 299 segments, comes out shorter.
Local zigzag: Vec2[] = []
For i = 0 Until 300
    zigzag.Add(Vec2(i As Float, (i % 2) As Float))
End For
Local spline = Curve2.CatmullRom(zigzag)
// The same curve, measured with 100 samples a segment.
Local fine = Curve2(Function(t: Float) => spline.PointAt(t), 29900)
Print(spline.Length > 422.85, spline.Length, fine.Length)
// 100 along the curve.
Print(spline.PointAtDistance(100), fine.PointAtDistance(100))
```

Expected: True, with spline.Length about 435.68 (equal to fine.Length), and spline.PointAtDistance(100) near (68.6, 0.66) like the finely measured curve:
True 435.677 435.67697
(68.6..., 0.66...) (68.60955, 0.66169626)

Actual: False 343.65488 435.67697
(87.0178, 0.9990608) (68.60955, 0.66169626)

Suggested fix: Scale the sample count used by CatmullRom, BSpline and Bezier with the number of segments, for example Max(256, 32 * count), and/or give them an optional samples parameter as the Function constructor has. Add a test with a spline through many points whose Length is checked against a finely measured one.

Skeptic: Ran the finding's program (plus a polyline sum and a 500-waypoint winding road) at C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10/geom-refute/curve1.fb. Debug and release both printed: 'False 343.65488 435.67697' / '(87.0178, 0.9990608) (68.60955, 0.66169626)' / polyline 422.8498479127884 / road '491.49838 501.44073' (2% short) / Sample(50)[10] '(61.14556, 0.94260126)' vs finely measured '(61.03774, 0.9958348)'. So a Catmull-Rom spline through 300 points measures shorter than the straight lines between its own points, and PointAtDistance(100) lands about 18 units too far along. The code agrees: in std/math/curves.fb, Curve2.CatmullRom, BSpline and Bezier (lines ~176-215) and the Curve3 versions all return Curve2(Function ...) / Curve3(Function ...) with no samples argument, so they get the constructor's default samples = 256 for the whole curve whatever the segment count. The static Methods take no sample count, so the caller can't fix it there. That contradicts the class comment ('more samples follow its bends closer') and stdlib.md's 'points evenly apart along the curve' for long paths. tests/programs/math_curves.fb has no spline through many points.

## S10-noise-1 (medium): Fixed64 Sin and Cos are not correctly rounded near a reduced angle of pi/4

Status: open

Where: std/math/fixed.fb:116

The series are cut off too early for 62-bit accuracy. CosineTerms stops at 1/14!, so the x^16/16! term it drops is about 1e-15 at |x| = pi/4. That is about 4600 units of 2^-62; in a Python model of SineQ62 that matches every Sin and Cos bit in math_fixed.out, the error is -4573 units at x = 0.785. SineTerms stops at 1/15!, with an error of about 210 units there. Fixed64 rounds the 62-bit result to 32 bits, so any input whose true value is within about 4e-6 of a Fixed64 step's halfway point rounds the wrong way. Near pi/4 that is roughly 1 input in 75,000. Cos uses the cosine series for reduced angles near +-pi/4, and so does Sin for angles near 3pi/4. The results are still deterministic. But docs/stdlib.md, the file header and math_fixed.fb all say Sqrt, Sin and Cos are correctly rounded, and the done-when asks for the reference's bits. The test's 13 angles happen to round correctly. I checked the true values two ways: with a 240-bit integer Taylor series and with Python's decimal module at 80 digits. Fixed32 is not affected: an exhaustive check of raw 0 to 2^19 for both Sin and Cos matched.

```
Import std.core
Import std.math

// cos(3373209229 / 2^32) * 2^32 is 3037035994.50000379..., so the nearest Fixed64 is 3037035995.
Print(Cos(Fixed64.FromRaw(3373209229)).Raw)
// sin(10119632474 / 2^32) * 2^32 is 3037103597.50000320..., so the nearest is 3037103598.
Print(Sin(Fixed64.FromRaw(10119632474)).Raw)
```

Expected: 3037035995
3037103598

Actual: 3037035994
3037103597 (the same in debug and release)

Suggested fix: Add series terms. Give the cosine 1/16! and 1/18! (and so on) and the sine 1/17! and 1/19!, each 2^62 / n! rounded. In the Python model, cosine terms through 1/20! and sine terms through 1/21! bring the 62-bit error down to about 1 unit and fix all five cases found (3373209229, 3373087991 and 3373032308 for Cos; 10119632474 and 10119479451 for Sin). Even then, 30 guard bits cannot guarantee correct rounding for every one of the 2^64 Fixed64 inputs. So either recompute in more bits when the result lands within a few units of a halfway point, or reword the Fixed64 docs as 'within 1 ulp' or 'faithfully rounded'. Add these angles to math_fixed.fb.

Skeptic: Ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10/noise-v/n1.fb in debug and release. Both printed Cos(FromRaw(3373209229)).Raw = 3037035994, Sin(FromRaw(10119632474)).Raw = 3037103597, Cos(3373087991) = 3037121720, Cos(3373032308) = 3037161092 and Sin(10119479451) = 3037211795. I computed the true values in Python with 90-digit decimal Taylor series: 3037035994.500003794, 3037103597.500003198, 3037121720.500002792, 3037161092.500001697 and 3037211795.500000419. Each is just above a halfway point, so the correctly rounded results are one higher than fb's in all five cases. In the code, fixed.fb:116-119, CosineTerms runs 1/2! to 1/14! and SineTerms (111-114) runs 1/3! to 1/15!, so the series drop x^16/16! (about 4556 units of 2^-62 at pi/4) and x^17/17! (about 210 units). That fits the reviewer's model. stdlib.md:411-412 says Sqrt, Sin and Cos 'are correctly rounded', the file header says the same, and the milestones row asks for the reference's bits. The results are still deterministic, and each wrong one is off by 1 ulp.

## S10-random-1 (medium): Percentile gives NaN, or a value outside the data, at huge or infinite values; Percentile(x, 0) isn't the least and Percentile(x, 50) isn't the Median

Status: open

Where: std/math/stats.fb:117

Percentile always goes through Lerp(a, b, t) = a + (b - a) * t, even when rank is a whole number (t = 0). When b - a overflows or is Inf - Inf, that gives NaN or Inf. So Percentile([-1e308, 1e308], 0) is NaN, not the least value, and Percentile(..., 25) is inf, which lies outside the data. Percentile([1, Inf, Inf], 50) is NaN while Median gives inf. The doc comment promises "0 for the least" and "50 is the Median", and neither holds. Python's statistics module and the doc's own promises give the expected values; NumPy also gives NaN here, but with a RuntimeWarning.

```
Import std.core
Import std.math

// Percentile 0 is the least value, 100 the largest, and 50 the Median.
Local wide: Double[] = [-1e308, 1e308]
Print(Percentile(wide, 0), Percentile(wide, 25), Percentile(wide, 100))
Local infs: Double[] = [1, Inf, Inf]
Print(Percentile(infs, 50), Median(infs))
Local low: Double[] = [-Inf, 0]
Print(Percentile(low, 0), Percentile(low, 50))
```

Expected: -1e+308 -5e+307 1e+308
inf inf
-inf -inf

Actual: -nan inf 1e+308
-nan inf
-nan -nan
(the same in debug and release)

Suggested fix: When rank is a whole number, or the two neighbors are equal, return sorted[below] directly. Otherwise interpolate as sorted[below] * (1 - t) + sorted[below + 1] * t, which can't overflow between finite ends and keeps an infinite end. Add these cases to math_stats.fb.

Skeptic: Ran the finding's program (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10/random-verify/f1.fb) in debug and release. Both print "-nan inf 1e+308", "-nan inf", "-nan -nan". stats.fb:117 always returns Lerp(sorted[below], sorted[below + 1], rank - below). scalar.fb:140 Lerp special-cases only t == 1, so at t = 0 it computes a + (b - a) * 0. That is NaN when b - a is inf (an overflow, or inf - -inf) and also when it is inf - inf. The comment at stats.fb:103-104 promises "0 for the least" and "50 is the Median". Percentile(wide, 0) is NaN instead of -1e308, and Percentile([1, Inf, Inf], 50) is NaN while Median gives inf, so the code contradicts its own documented contract. Medium because an infinity in the data (say, unreachable = Inf) is a plausible input, and the result is a silent NaN.

## S10-random-3 (medium): Histogram counts a value exactly on a bin's lower edge in the bin below

Status: open

Where: std/math/stats.fb:137

The bin is Floor((x/2 - low/2) / width), with width = (high/2 - low/2) / bins already rounded. When width isn't exact, a value exactly on an edge divides to k - 1 ulp and floors into bin k - 1. With 22 ranges from 0 to 100, 50 is exactly where bin 11 starts, but it lands in bin 10. The same happens for 9 in 14 ranges over 0..18, 72 in 35 ranges over 0..360, and 127.5 in 26 ranges over 0..255. A search over whole-number ranges found 525 such cases. Histogram(values, bins) goes through the same path. NumPy's histogram puts every one of these values in the upper bin.

```
Import std.core
Import std.math

// 22 equal ranges from 0 to 100: index 11 is [50, 54.54...), so 50 is in it.
Print(Histogram([50], 22, 0, 100))
// 14 equal ranges from 0 to 18, 9/7 wide: index 7 starts at 9 exactly.
Print(Histogram([9], 14, 0, 18))
// 35 ranges from 0 to 360: index 7 starts at 72.
Print(Histogram([72], 35, 0, 360))
// Without a range, from the least to the largest, 0 to 18.
Print(Histogram([0, 9, 18], 14))
```

Expected: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
[0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]
(35 counts with the 1 at index 7)
[1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1]

Actual: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
[0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]
(35 counts with the 1 at index 6)
[1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]
(the same in debug and release)

Suggested fix: After computing the index, check it against the bin's own edges, as NumPy's histogram does: step down while x is below edge(i) and up while x is at or above edge(i + 1). Compute the edges in halves the same way, so wide ranges still can't overflow. For finite products, multiplying before dividing, (x/2 - low/2) * bins / (high/2 - low/2), already fixes every whole-number case found. Add an on-edge case to math_stats.fb.

Skeptic: Ran f3.fb in debug and release. Both put 50 (22 bins over 0..100) at index 10, 9 (14 bins over 0..18) at index 6, and 72 (35 bins over 0..360) at index 6. The auto-ranged Histogram([0, 9, 18], 14) gives [1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1]. The comment at stats.fb:120-121 says "bins equal ranges from low to high, the last range holding high too", which makes every other range half-open, [edge k, edge k+1). 50 = 11 * 100/22 is exactly where bin 11 starts. At stats.fb:133/137 the width (high/2 - low/2)/bins is already rounded, so (x/2 - low/2)/width lands 1 ulp below k, and Floor puts the value in bin k - 1. That is a wrong count for an on-edge value, and on-edge values are common with whole-number data.

## S10-geometry-2 (low): Union, Intersection, Difference and Offset silently drop features smaller than 1e-9 of the farthest coordinate from the origin

Status: open

Where: std/math/polygon.fb:747

Boundary decides whether each split edge is on the boundary by testing the winding a fixed step to either side of its midpoint. That step is SizeOf(regions) * 1e-9 (line 747), and SizeOf (line 626) is how far any corner of either region lies from the origin, not the size of the edge or of the feature it belongs to. SplitEdges' snap tolerance (line 595) is scaled the same way, at 1e-11. When one region reaches far from the origin, every edge of a small shape near the origin is probed at points outside it: both sides read 'not filled', so the edge is dropped and the shape disappears from the result without any panic. A 1 mm square unioned with itself is kept. Unioned with a 1 m square 1000 km away, it vanishes. Intersection([tiny, far], [tiny]) returns no rings, and Offset([tiny, far], 0), which goes through the same Boundary to clean the region, loses the tiny ring too. A unit square unioned with a square at 1e20 is likewise lost. Float coordinates near the origin can hold features that small, so the cutoff is set by the other operand rather than by precision. Debug and release agree.

```
Import std.core
Import std.math

// A 1 mm square at the origin and a 1 m one 1000 km away: each alone, and the two together.
Local tiny = [Vec2(0, 0), Vec2(0.001, 0), Vec2(0.001, 0.001), Vec2(0, 0.001)]
Local far = [Vec2(1000000, 0), Vec2(1000001, 0), Vec2(1000001, 1), Vec2(1000000, 1)]
Print(Union([tiny], [tiny]).Count, Union([tiny], [far]).Count)
Print(Intersection([tiny, far], [tiny]).Count, Offset([tiny, far], 0).Count)
```

Expected: 1 2
1 2

Actual: 1 1
0 1

Suggested fix: Make the side probe local, not global: for example, step a small fraction of the part's own length (and of the distance to its nearest other edge) to each side. Better still, classify each part exactly: work out the winding at its midpoint from the other edges, then add or subtract the part's own contribution and that of edges lying along it, as Greiner-Hormann and Martinez-style overlays do. Scale the snap tolerance to the points being snapped, not to the farthest corner. Add a test that mixes a small shape with a far one.

Skeptic: Ran the finding's program, extended, at C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s10/geom-refute/poly2.fb. Debug and release both printed '1 1' / '0 1'. So Union([tiny],[far]) keeps only the far square (its one ring has 4 corners and area 1), and Intersection([tiny, far],[tiny]) returns no rings, though tiny is in both. Offset([tiny, far], 0) also drops tiny. A unit square unioned with a square at 1e20 gives 1 ring, and a 1 cm square with a far square 100 km away keeps both (2). The code agrees: Boundary in std/math/polygon.fb probes step = SizeOf(regions) * 1e-9 (line 747) to each side of every part's midpoint. SizeOf (line 626) is the largest |x| or |y| of any corner, so with a corner at 1e6 the step is 1e-3, and both probes of the 1 mm square's edges land outside it. Both sides then read as unfilled, and the edge is dropped silently. The snap tolerance (line 595) is scaled the same way, and neither polygon.fb nor stdlib.md documents the limit. I rated it low, not medium: it takes a 1e9 ratio between the farthest coordinate and the feature's size in one call. That ratio is beyond Float32's precision across a whole scene, and is representable only because the small shape sits at the origin.

## S10-noise-2 (low): Worley noise searches only the 3x3 cells and misses nearer points, so it jumps at cell edges

Status: open

Where: std/math/noise.fb:230

Worley(x, y) takes the nearest point among the 3x3 cells around the place. A cell's point can be up to sqrt(2) away, though, while a point two cells over can be just over 1 away. When that point is nearer, the result is not 'the distance to the nearest of a point in each whole cell'. A distance to a set of points can never change faster than the place moves, but here the value jumps when the place crosses into the next cell column and the far cell enters the search. With Perlin's table, the 2D search over 256x256 cells found such places near (216.97, 193.2 to 193.3). The table repeats every 256 cells, so the seam repeats too. 60,000 random 3D samples found no case for the 3x3x3 search, but 3D has the same structural gap.

```
Import std.core
Import std.math

// The nearest point to (216.96875, 193.28125) is in the cell two to the right, 1.0467875 away,
// past the 3 by 3 cells Worley looks in, whose nearest is 1.0673788 away.
Local noise = Noise()
Print(noise.Worley(216.96875, 193.28125))
// So Worley noise jumps as a place crosses x = 217, though a distance can't move faster than
// the place: both should be about 1.0160.
Print(noise.Worley(216.999999, 193.28125), noise.Worley(217.000001, 193.28125))
```

Expected: 1.0467875
1.0160165 1.0160146

Actual: 1.0673788
1.0395699 1.0160146 (the same in debug and release)

Suggested fix: Search out to two cells: 5x5 in 2D, and 5x5x5 in 3D or a pruned version of it. Skip any cell whose nearest edge is already farther than the best distance so far, so the usual case stays near 9 or 27 lookups. Add a test at (216.96875, 193.28125).

Skeptic: Ran n2.fb in debug and release. Both printed 1.0673788, then '1.0395699 1.0160146'. The value moves by 0.0236 when x moves by 2e-6 across x = 217. A distance to a set of points can't change faster than the place moves, so the result is not the nearest distance that noise.fb:223 promises. I also modeled noise.fb's Hash and points in Python with Perlin's table read from the file. The 3x3 search reproduces fb's three values exactly. A 5x5 search gives 1.0467875 at (216.96875, 193.28125) and 1.0160165 at 216.999999, as the reviewer says. The cause is the loops at noise.fb:230-231 (and 248-250 in 3D), which cover only -1..1. A 1/16-step scan of a whole 256x256 tile found 2 wrong samples out of 16.7M, all in cell (216, 193). So the seam is real but rare, and the jump is small. That's why I rate it low, not medium.

## S10-noise-3 (low): Simplex overflows Int64 in i + j (i + j + k in 3D) for coordinates noise.fb says it takes; debug panics and release returns a value

Status: open

Where: std/math/noise.fb:135

noise.fb says only a place too far out for an Int64, or NaN, panics, with the message 'noise takes coordinates...'. Simplex adds the skewed cells as Int64s, `((i + j) As Double) * Unskew2` and `(i + j + k)` at line 159. Both cells can be in range while their sum is not: x = y = 4e18 is below 2^63 (about 9.22e18), and so is each skewed cell (about 6.9e18), but i + j (about 1.39e19) is not. A debug build stops with an internal 'integer overflow in +' at the caller. A release build wraps and prints 0, so the two builds differ. Simplex(6e18, 0) and Simplex(3e18, 3e18, 3e18) do the same. Perlin, Value and Worley are fine at these places.

```
Import std.core
Import std.math

// 4e18 is nearer 0 than 2^63 (about 9.2e18), so noise takes it.
Local noise = Noise()
Print(noise.Perlin(4e18, 4e18))
Print(noise.Simplex(4e18, 4e18))
Print(noise.Simplex(3e18, 3e18, 3e18))
```

Expected: The same in debug and release: 0, 0, 0 (or, if such places are out of range, the documented noise panic in both builds).

Actual: Debug: 0, then 'panic: integer overflow in '+' at simplex_big.fb:7:7'. Release: 0, 0, 0.

Suggested fix: Add the cells as Doubles: `((i As Double) + (j As Double)) * Unskew2`, and the same for i, j and k in 3D. That is the same value wherever i + j fits in 53 bits. Alternatively, check the sum in Cell's range and raise the noise panic. Add a panic or value test at 4e18.

Skeptic: Ran n3.fb. Debug printed 0, then 'panic: integer overflow in '+' at n3.fb:6:7' and exited with code 101. Release printed 0, 0, 0. The noise.fb header (line 7) says only a place too far out for an Int64, or NaN, panics, and Cell's own message says 'nearer 0 than 2^63'. 4e18 is within both, and each skewed cell (about 6.93e18) fits. The overflow is the Int64 sum (i + j) at noise.fb:135, and i + j + k at line 159. So debug and release differ, and the debug panic isn't the documented one. I rate it low because it only happens at coordinates above about 2.7e18, where a Double has no fractional detail left.

## S10-noise-4 (low): Fixed64.ToFloat rounds twice (Int64 to Double to Float), so it is not 'rounded to the nearest' above 2^21

Status: open

Where: std/math/fixed.fb:335

ToFloat is `ToDouble() As Float`. For |value| at or above 2^21, Bits As Double already rounds. That first rounding can land exactly on a halfway point between two Floats, and ties-to-even then rounds the wrong way. The doc comment says 'The value as a Float, rounded to the nearest.' Fixed32.ToFloat is exact through the Double, so it is fine.

```
Import std.core
Import std.math

// 2^30 + 192 - 2^-32 is below the halfway point between the Floats 2^30 + 128 and 2^30 + 256,
// so the nearest Float is 1073741952.
Print(Fixed64.FromRaw(4611686843061108735).ToFloat() As Double)
```

Expected: 1073741952

Actual: 1073742080 (the same in debug and release)

Suggested fix: Round once. For example, convert the whole part and the fraction separately, or round Bits to 24 significant bits in integers (round to nearest, using the bits shifted out as a sticky bit) before the single As Double and As Float. Or change the doc to say the result can be off by one Float step for values past 2^21. Add this value to math_fixed.fb.

Skeptic: Ran n4.fb in debug and release. Both printed 1073742080. The exact value is 1073742015.99999999976716935... (Python decimal). Floats there are 128 apart, between 1073741952 and 1073742080, and the midpoint is 1073742016. The value is below the midpoint, so the nearest Float is 1073741952. fixed.fb:335 is 'ToDouble() As Float'. Bits As Double first rounds up to exactly the midpoint, and ties-to-even then picks 1073742080. The doc comment on line 334 says 'rounded to the nearest', which this contradicts. Fixed32 isn't affected, because its value is exact in a Double.

## S10-noise-5 (low): Fixed64 prints through a Double, so MaxValue prints 2147483648, a value it can't hold

Status: open

Where: std/math/fixed.fb:345

Fixed64.ToString is "{ToDouble()}", and ToDouble rounds once Bits needs more than 53 bits. MaxValue<Fixed64>() is 2147483647.9999999998 but prints 2147483648. That is above the type's own maximum: Fixed64.FromDouble(2147483648) panics, and the panic message itself says the range ends at 2147483647.9999999998. In general a Fixed64 past 2^21 prints rounded. For example, FromRaw(4294967296 * 3000000 + 1) prints 3e+06 although it is not equal to FromInt(3000000). The step's docs only say Fixed64 is Printable.

```
Import std.core
Import std.math

// A Fixed64 holds values up to 2147483647.9999999998.
Local most = MaxValue<Fixed64>()
Print(most)
Print(Fixed64.FromDouble(2147483647.9999998).Raw)
```

Expected: A value no higher than 2147483647.9999999998, such as 2147483647.9999999998.
9223372036854774784

Actual: 2147483648
9223372036854774784

Suggested fix: Print Fixed64 exactly, from its integers: the whole part from Bits >> 32 with the sign handled, then the fraction as decimal digits from the low 32 bits, trimmed of trailing zeros. A shortest round-trip form is fine too, if it is computed from the bits rather than a Double. Or document that values past 2^21 print rounded. Add MaxValue to math_fixed.fb.

Skeptic: Ran n5.fb in debug and release. Both printed '2147483648' for MaxValue<Fixed64>(). Fixed64.FromRaw(4294967296 * 3000000 + 1) printed '3e+06', yet comparing it with FromInt(3000000) gave False. fixed.fb:345 builds ToString from ToDouble(), which rounds past 2^21 (its own comment on line 331 says so). The type's maximum is 2147483647.9999999998, and FromDouble(2147483648) panics, so the printed form doesn't read back as the value. That goes against decisions.md:162 and spec 14 ('Numbers print in the shortest form that reads back as the same value'), and ToString's caveat isn't documented. Low, because only printing is affected and the values themselves are right.

## S10-random-2 (low): Mean (and an even count's Median) overflow to inf for finite values a Double holds

Status: open

Where: std/math/stats.fb:41

Mean adds every value into one Double sum before dividing, so values near a Double's top overflow even though the mean is representable. Mean([1e308, 1e308]) is inf, not 1e308. Mean([1e308, 1e308, -1e308]) is inf, not 3.33e307; Python's statistics.mean, which the test file says it matches, gives 1e308 and 3.333333333333333e+307. Median of an even count computes (a + b) / 2, so Median([1e308, 1e308]) is inf even though both middle values are 1e308. Histogram in the same file deliberately works in halves so a Double's whole range can't overflow, but Mean and Median don't.

```
Import std.core
Import std.math

// The mean and median of values a Double holds, which are values a Double holds too.
Local big: Double[] = [1e308, 1e308]
Print(Mean(big), Median(big))
Local mixed: Double[] = [1e308, 1e308, -1e308]
Print(Mean(mixed))
```

Expected: 1e+308 1e+308
3.333333333333333e+307

Actual: inf inf
inf
(the same in debug and release)

Suggested fix: In Mean, when the plain sum isn't finite but every value is, sum value / count (or halves) instead. In Median, use a / 2 + b / 2 (or a + (b - a) / 2 when the signs match) when a + b overflows. Add both cases to math_stats.fb.

Skeptic: Ran f2.fb in debug and release. Both print "inf inf" and "inf". Mean (stats.fb:37-41) adds every value into one Double before dividing, so the mean of finite values a Double holds comes out inf. The test file math_stats.fb's header says it matches Python's statistics module, and statistics.mean gives 1e308 and 3.333333333333333e+307 for these inputs. Within the same step the lane fixed this same class of bug in Float(low, high) (commit c935c2c) and designed Histogram to work in halves, and Spread's Welford mean already gives 1e308 for [1e308, 1e308]. So by the project's own standard this is a defect. The Median half is weaker: Python's statistics.median also computes (a + b) / 2 and gives inf. Even so, the comment calls it "the mean of the two middle ones", and that mean is 1e308. Low because only values near a Double's top trigger it.

## S10-random-4 (low): Histogram panics with an internal conversion error for a valid range of subnormal Doubles

Status: open

Where: std/math/stats.fb:133

For a range of the smallest Doubles, (high/2 - low/2) / bins underflows to 0. Then (x/2 - low/2) / width is 0/0 = NaN, and Floor(NaN) As Int panics with "the value doesn't fit in Int32". The range is finite with low below high, which Histogram accepts by its own checks, so the panic message doesn't describe the caller's mistake. Histogram(values, bins) over such values hits the same panic. Only subnormal ranges trigger it.

```
Import std.core
Import std.math

// A finite low below a finite high, both among the smallest Doubles.
Local tiny: Double[] = [0, 5e-324, 1e-323]
Print(Histogram(tiny, 2, 0, 1e-323))
```

Expected: [1, 2] (0 in the first range; 5e-324 is the boundary and 1e-323 is high, both in the last), or at least no internal panic

Actual: panic: the value doesn't fit in Int32 at f4.fb:6:7
(the same in debug and release)

Suggested fix: Don't let a zero width reach Floor. Scale low, high, and x up by a power of two when the halves' width underflows, or compute the index multiply-first. Add a subnormal case to math_stats.fb.

Skeptic: Ran f4.fb in debug and release. Both print "panic: the value doesn't fit in Int32 at f4.fb:6:7". With low 0 and high 1e-323, width = (5e-324 - 0) / 2 rounds to 0 (round-half-even of 2.5e-324) at stats.fb:133. For x = 0, (0 - 0)/0 is NaN, and Floor(NaN) As Int at line 137 hits the conversion panic. The inputs pass Histogram's own checks at line 126 (finite, low < high), so the internal panic doesn't describe any caller mistake. Low because only subnormal ranges trigger it.

## S10-random-5 (low): Histogram(values, bins) panics on an infinite value with a message about a low and high the caller never gave, and its doc doesn't list the panic

Status: open

Where: std/math/stats.fb:158

The auto-range Histogram's comment says only "No values, a NaN, or a count of bins that isn't above 0, panics" and that all values fall in. With an infinite value, it passes the least and largest values to the ranged overload. That panics with "Histogram takes a finite low below a finite high, not 1 and inf", but this caller gave no low or high. Neither the comment nor docs/stdlib.md mentions this panic. NumPy raises for the same input, but names an autodetected range that isn't finite.

```
Import std.core
Import std.math

// Histogram without a range: "No values, a NaN, or a count of bins that isn't above 0, panics."
Local values: Double[] = [1, 2, Inf]
Print(Histogram(values, 2))
```

Expected: A panic naming the actual problem, e.g. "Histogram takes values that are finite, not inf", and a doc comment that lists it

Actual: panic: Histogram takes a finite low below a finite high, not 1 and inf at f5.fb:6:7

Suggested fix: In Histogram(values, bins), check that sorted[0] and the last value are finite and panic with Histogram's own message. Add that to the comment and stdlib.md's stats bullet, and add a panic program.

Skeptic: Ran f5.fb in debug and release. Both print "panic: Histogram takes a finite low below a finite high, not 1 and inf at f5.fb:6:7". The caller called Histogram(values, 2) and passed no low or high. The comment at stats.fb:143-144 says the values "all do" fall in, and it lists only no values, a NaN, or bins <= 0 as panics. docs/stdlib.md's stats bullet mentions only too few values and NaN. At stats.fb:150-158 the least and largest values go straight to the ranged overload, whose check fires with that overload's message. The panic itself is reasonable, since no equal ranges reach inf, but it isn't documented and its message names arguments the caller never gave.

## S10-random-6 (low): WeightedPick panics for finite, non-negative weights whose sum overflows, which its comment doesn't list

Status: open

Where: std/math/random.fb:158

The comment lists the panics as other counts, a negative or non-finite weight, or weights that add up to 0. Weights [1e308, 1e308] are each finite and non-negative, and their relative sizes are valid. But the Double total overflows to inf, so WeightedPick panics with "sum is above 0 and finite, not inf".

```
Import std.core
Import std.math

// Each weight finite and not negative, and their sum above 0.
Local random = Random(1)
Local weights: Double[] = [1e308, 1e308]
Print(random.WeightedPick(["a", "b"], weights))
```

Expected: a or b (each as likely), or a documented panic

Actual: panic: WeightedPick takes weights whose sum is above 0 and finite, not inf at f6.fb:7:7

Suggested fix: Scale the weights so finite ones can't overflow (e.g. divide each by the largest before summing and use the scaled target), or list the overflowing sum in the comment and stdlib.md. Add a test either way.

Skeptic: Ran f6.fb in debug and release. Both print "panic: WeightedPick takes weights whose sum is above 0 and finite, not inf at f6.fb:7:7". The comment at random.fb:142-143 lists the panics as counts that differ, a weight that's negative or not finite, or weights that add up to 0. Each of [1e308, 1e308] is finite and non-negative, and their sum isn't 0. The total still overflows at line 156, and the !IsFinite(total) check at line 158 panics. That panic is real but unlisted. Low: a documentation gap (or a fixable overflow) for extreme weights.

## S10-random-7 (low): random.fb's header says nothing it makes uses C's libm, but Normal and UnitVector3 call libm's sqrt

Status: open

Where: std/math/random.fb:3

The header says "Nothing it makes uses C's libm", and math_random.fb's comment says none of them "goes through C's libm". Normal and UnitVector3 call Sqrt<Float64>, which goes to Float64.Sqrt and then to the Extern C sqrt. Normalize, through Magnitude, does the same. The results are still the same everywhere, because IEEE 754 requires sqrt to be correctly rounded; a check of 1,000,000 random Doubles found it correctly rounded and identical in debug and release. Only the comment's claim is wrong.

```
Import std.core
Import std.math

Local random = Random(1)
Print(random.Normal(), random.UnitVector3())
// Build with: fb build --show llvm libm.fb
```

Expected: No call into C's libm from Random's methods, as the header says

Actual: The IR shows Random.Normal calling fb.std.math.scalar.Sqrt<Float64>, then fb.std.math.numbers.Float64.Sqrt, then `call double @sqrt(double %2)` with `declare double @sqrt(double)`; UnitVector3 calls the same Sqrt<Float64>

Suggested fix: Reword the header and math_random.fb's comment, e.g. "only C's sqrt, which IEEE 754 rounds correctly on every OS, and a logarithm of its own".

Skeptic: Built f7.fb with `fb build --show llvm`. Random.Normal calls fb.std.math.random.Ln and then fb.std.math.scalar.Sqrt<Float64>. UnitVector3 calls Sqrt<Float64> and Normalize. Sqrt<Float64> goes to fb.std.math.numbers.Float64.Sqrt, which does `call double @sqrt(double %2)` with `declare double @sqrt(double)` (numbers.fb:240, Extern sqrt at line 43). random.fb:3 says "Nothing it makes uses C's libm", and tests/programs/math_random.fb:3 says "none of them goes through C's libm". Both claims are false. Determinism still holds because IEEE 754 requires sqrt to be correctly rounded, so only the comments are wrong.

## S10-random-8 (low): A negative stream silently gives the same numbers as a positive one

Status: open

Where: std/math/random.fb:29

Random(seed, stream) takes an Int64 stream, and Start computes (stream << 1) | 1, which drops the top bit. So stream s and stream s + 2^63 are the same generator: Random(1, -1) equals Random(1, 9223372036854775807). The comment says "one of 2^63 streams" but doesn't say that negative streams alias positive ones. A program that picks streams from hashes or chunk coordinates can get two "apart" generators that give the same numbers. Fork only ever passes non-negative streams.

```
Import std.core
Import std.math

// Two streams, -1 and 9223372036854775807, give the same numbers for the same seed.
Local a = Random(1, -1)
Local b = Random(1, 9223372036854775807)
Print(a.Int(), a.Int(), a.Int())
Print(b.Int(), b.Int(), b.Int())
Print(a.State == b.State)
```

Expected: Different numbers for different streams, or a documented rule (or panic) for negative streams

Actual: -1048575 1148350300 -875323592
-1048575 1148350300 -875323592
True

Suggested fix: Either document that a stream is taken modulo 2^63 (a negative stream is the same as stream + 2^63), in the comment and stdlib.md, or panic on a negative stream at the caller, as Int(count) does for a bad count. Add a test.

Skeptic: Ran f8.fb in debug and release. Random(1, -1) and Random(1, 9223372036854775807) both print "-1048575 1148350300 -875323592", and a.State == b.State prints True. At random.fb:37, Increment = (stream << 1) | 1 drops the top bit, so stream s and s + 2^63 (that is, a negative s and s with the top bit cleared) are the same generator. Fork deliberately passes (stream >> 1), which suggests the intended domain is 0 to 2^63-1. Yet New(seed, stream) takes any Int64. Its comment says "one of 2^63 streams, which give numbers apart from each other" and doesn't say how negative streams map, so two streams picked from signed hashes can silently match. This is inherent to PCG's pcg32_srandom_r. Only documentation (or a panic on a negative stream) is missing, hence low.

## Refuted by the skeptics (no action)

- S10-noise-6: EaseInOutSine(0) and EaseInOutBack(0) give -0, and the test hides it. Ran n6.fb in debug and release. Both printed '-0 -0 0', so the -0 is real. It comes from easing.fb:18 and line 66, which are easings.net's own formulas: -(cos(PI*x) - 1) / 2, and the Back InOut branch. JS gives -0 for them too, and stdlib.md:400-401 says the curves are 'as easings.net has them'. IEEE -0 equals 0 under ==, <, and any lerp (start + (end - start) * -0 == start), so 'from 0 at 0' still holds as a value. Only the printed sign differs, and math_easing.fb masks it on purpose ('with -0 as 0'). Neither stdlib.md nor the spec is plainly contradicted, so I'm refuting it, given the 'when unsure, refute' rule. A one-line change (1 - Cos(Pi * t)) / 2 would tidy it if wanted.
