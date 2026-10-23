# The review of M6 step 9 (Math I)

25 confirmed findings: 2 high, 9 medium, 14 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S9-color-geo-1 (high): Sphere.Raycast loses the hit to Float cancellation from a few thousand units away

Status: fixed in fccb4d5 (math_geometry.fb): Raycast works in Doubles from where the ray comes nearest the center, as Ray Tracing Gems' chapter 7 has it, and takes the nearer root as the outside over the other root's numerator, so rays from 3000 and 10000 away, a sphere of radius 0.1, and a Direction of 1e-25 hit or miss as AABB's do.

Where: std/math/geometry.fb:222

Sphere.Raycast does its quadratic in Floats: `outside = Dot(offset, offset) - Radius * Radius` and `discriminant = along * along - speed * outside`. From a few thousand units away, Dot(offset, offset) is about 1e7, where Float steps are 1 or more. The radius term, and the ray's sideways offset, round away. The discriminant then comes out 0, so a ray that misses is reported as a hit, and a head-on hit is reported at the sphere's center, a full radius too far. With a unit sphere this starts at about 3000 units: a ray passing 1.2 from the center hits at 3000. AABB.Raycast at the same distance is correct (9999). Separately, `speed = Dot(Direction, Direction)` underflows to 0 for a very short Direction, so a ray that does hit is reported as a miss, while AABB.Raycast gives the hit. docs/stdlib.md says std works lengths and distances out in Doubles, but this method doesn't. Debug and release builds agree. The tests only cast rays from 10 units away.

```
Import std.core
Import std.math

// A unit sphere at the origin, and rays along -Z from 3000 and 10000 away.
Local ball = Sphere(Vec3.Zero(), 1)
// Passing 1.2 from the center, which misses a sphere of radius 1.
Print(ball.Raycast(Ray(Vec3(1.2, 0, 3000), Vec3.Forward)) ?? -1)
Print(ball.Raycast(Ray(Vec3(1.5, 0, 10000), Vec3.Forward)) ?? -1)
// Head on: the surface is at 9999, as the box of the same size finds it.
Print(ball.Raycast(Ray(Vec3(0, 0, 10000), Vec3.Forward)) ?? -1,
      AABB(-Vec3.One(), Vec3.One()).Raycast(Ray(Vec3(0, 0, 10000), Vec3.Forward)) ?? -1)
// A short Direction: its square underflows, so the ray that hits misses.
Print(ball.Raycast(Ray(Vec3(0, 0, 10), Vec3(0, 0, -1e-25))) ?? -1,
      AABB(-Vec3.One(), Vec3.One()).Raycast(Ray(Vec3(0, 0, 10), Vec3(0, 0, -1e-25))) ?? -1)
```

Expected: -1
-1
9999 9999
about 9e+25 8.9999995e+25 (both shapes hit)

Actual: 3000
10000
10000 9999
-1 8.9999995e+25
(the same in debug and release)

Suggested fix: Do the quadratic in Doubles (offset, outside, along, speed, and the discriminant), or use the stable form from Ray Tracing Gems (ch. 7): find the point of closest approach first, `l = offset - (along / speed) * Direction`, then `discriminant = speed * (Radius² - Dot(l, l))`. Then add a far-ray case and a short-Direction case to math_geometry.fb.

Skeptic: Ran the finding's program (colorgeo/f1.fb) in debug and release; both print 3000 / 10000 / "10000 9999" / "-1 8.9999995e+25". A ray passing 1.2 from a unit sphere's center is reported as a hit at 3000, and a head-on ray from 10000 hits at 10000 (the center) where AABB gives 9999. It is worse for small spheres (f1b.fb): Sphere(0, 0.1) with a ray passing 0.12 from its center reports a hit at 500 and 1000. Head-on from 1000 it gives 1000 instead of 999.9, and from 100 it gives 99.90118. geometry.fb:222-232 does `outside = Dot(offset, offset) - Radius * Radius` and `along*along - speed*outside` in Floats, so r^2 cancels away once d^2 is around 2^23 times larger. The Float underflow of `speed` with Direction 1e-25 also reproduces, but that subcase is contrived. The main bug (silent false hits at ordinary game distances) is real and untested; the tests cast only from 10 units.

## S9-scalar-1 (high): PingPong and Wrap subtract below 0 on unsigned types: a panic in debug builds, wrong results in release

Status: fixed in 1da3da0 (math_scalars.fb): integer Wrap and PingPong work their distances out as UInt64s through LowBits (Above in scalar.fb), PingPong by whole lengths and their parity, so UInt8 PingPong(1, 2) is 1 and Wrap(3, 5, 10) is 8.

Where: std/math/scalar.fb:178

Integer PingPong works out `Abs(Mod(value, length + length) - length)` in T. For an unsigned T, `Mod(...) - length` goes below 0 whenever the value is on its way up, which is half of every cycle. Integer Wrap works out `value - low` in T, which goes below 0 for an unsigned value under low. Both Functions take any Integer, but no test runs either one on an unsigned type. In debug builds both panic with an integer overflow. Wrap's panic points at std/math/scalar.fb:160:22, since Wrap isn't @PanicsAtCaller. In release builds the arithmetic wraps and they give wrong numbers: PingPong(1, 2) is 3, and Wrap(3, 5, 10) is 9.

```
// PingPong and Wrap on unsigned types: PingPong(1, 2) is 1 and Wrap(3, 5, 10) is 8.
Import std.core
Import std.math

Local one: UInt8 = 1
Local length: UInt8 = 2
Print(PingPong(one, length))
Local three: UInt8 = 3
Local low: UInt8 = 5
Local high: UInt8 = 10
Print(Wrap(three, low, high))
```

Expected: 1
8

Actual: debug: panic: integer overflow in '-' at f_unsigned.fb:7:7 (exit 101); release: 3 then 9

Suggested fix: In PingPong, compare before subtracting: with r = Mod(value, 2 * length), give `r >= length ? length - (r - length) : r`. In Wrap, branch on value < low. When it isn't, the offset is value - low. When it is, take r = (low - value) mod span and give `r == 0 ? low : high - r`, so nothing goes below 0. Add unsigned cases to math_scalars.fb.

Skeptic: I ran f_unsigned.fb (UInt8 PingPong(1, 2) and Wrap(3, 5, 10)). Debug: 'panic: integer overflow in '-' at f_unsigned.fb:7:7' (exit 101). Release printed 3 and 9, where the answers are 1 and 8. A second program with only the Wrap call panicked in debug at std/math/scalar.fb:160:22, which is `value - low` in Wrap, and printed 9 in release. In scalar.fb:178, `Mod(value, length + length) - length` goes below 0 in T whenever the remainder is under length. Line 160's `value - low` does the same for an unsigned value under low. stdlib.md says Integer is all eight integer types, so the unsigned ones count. math_scalars.fb:69-73 runs Wrap and PingPong on signed values only. On any unsigned type, PingPong is wrong for half of every cycle.

## S9-color-geo-2 (medium): Frustum.FromMatrix gives a NaN far plane for a large far/near ratio, so everything is culled

Status: fixed in 32e0192 (math_geometry.fb): PlaneOf gives a row of no x, y, or z a plane with no normal at 0, which holds every point, where its w isn't below 0, and at Inf, which holds none, where it is. Perspective(Pi / 2, 1, 0.01, 1000000)'s frustum holds what's in front of the camera.

Where: std/math/geometry.fb:351

Mat4.Perspective accepts near 0.01 and far 1000000. In Floats, its depth term far / (near - far) then rounds to exactly -1. That matrix renders everything past near, like an infinite far plane. But in FromMatrix, `fourth - third` for the far plane has a zero xyz. PlaneOf divides by Length(row.xyz) = 0, which makes the Far plane's Normal NaN and its Distance -Inf. Every SignedDistance against it is NaN, so `>= 0` fails. As a result, Frustum.Contains and both Intersects overloads return False for everything, and culling with this frustum hides the whole scene. NaN's sign also differs between builds: the plane prints as -nan in debug and nan in release.

```
Import std.core
Import std.math

// A camera at the origin looking down -Z, near 0.01 and far 1000000, which Perspective takes.
Local view = Mat4.LookAt(Vec3.Zero(), Vec3.Forward, Vec3.Up)
Local frustum = Frustum.FromMatrix(Mat4.Perspective(Pi / 2, 1, 0.01, 1000000) * view)
// A point, a sphere, and a box 10 in front of the camera are all in view.
Print(frustum.Contains(Vec3(0, 0, -10)), frustum.Intersects(Sphere(Vec3(0, 0, -10), 1)),
      frustum.Intersects(AABB(Vec3(-1, -1, -11), Vec3(1, 1, -9))))
Print(frustum.Far)
```

Expected: True True True
a Far plane that every point in view is in front of (for example Plane((0, 0, 1), d) with a finite d, or a plane that holds everything)

Actual: False False False
Plane((-nan, -nan, -nan), -inf)   (release prints Plane((nan, nan, nan), -inf))

Suggested fix: In PlaneOf, handle a row whose xyz is zero. If its w is 0 or more, it holds everywhere (for example Plane(Vec3.Zero(), 0), whose SignedDistance is always 0). If its w is below 0, it holds nowhere. Never divide by a zero length. Add a FromMatrix test with a far/near ratio this large.

Skeptic: Ran f2.fb. With Perspective(Pi/2, 1, 0.01, 1000000) * LookAt, the program prints "False False False" and Far = Plane((-nan, -nan, -nan), -inf). Sweeping far with near 0.01: up to 2e5 is fine, and 5e5 gives the NaN plane with Contains False. With near 0.1, far 1e7 gives the NaN plane too. The projection prints its depth row as (0, 0, -1, -1), so fourth - third has a zero xyz. PlaneOf (geometry.fb:351-354) divides by Length(row.xyz) = 0 with no guard, so the frustum culls everything for a matrix that Perspective accepts. One detail is wrong: release also printed -nan here, not nan. That doesn't change the finding.

## S9-color-geo-3 (medium): Grow by a negative amount past half the size makes an inverted Rect or AABB that still has area and overlaps

Status: fixed in c3ed663 (math_geometry.fb): Rect.Grow and AABB.Grow make an axis shrunk past nothing 0 long, at the center, so it has no area and meets only what holds that point.

Where: std/math/geometry.fb:84

Rect's doc comment says its Width and Height aren't negative. But Rect.Grow(amount), which shrinks for a negative amount, doesn't stop at nothing. Rect(0, 0, 10, 5).Grow(-10) is Rect(10, 10, -10, -15). Its Area is 150, it Intersects a Rect around it, and its Intersection with that Rect is a 10 by 15 Rect. Only Contains(point) correctly says False. AABB.Grow (line 178) does the same: AABB(-1, 1).Grow(-2) has Min above Max and a negative Size, and still Intersects a box around it. Shrinking a small UI rect by its padding is enough to hit this.

```
Import std.core
Import std.math

// A 10 by 5 Rect shrunk by 10 on each side is nothing, yet it has an area and meets a Rect around it.
Local gone = Rect(0, 0, 10, 5).Grow(-10)
Local around = Rect(-50, -50, 100, 100)
Print(gone, gone.Area, gone.Intersects(around), gone.Intersection(around) ?? Rect())
// The same for a box shrunk by 2 on each side.
Local box = AABB(-Vec3.One(), Vec3.One()).Grow(-2)
Print(box, box.Size, box.Intersects(AABB(Vec3(-5, -5, -5), Vec3(5, 5, 5))))
```

Expected: A shape with no negative size, for example Rect(5, 2.5, 0, 0) 0 ... and AABB((0, 0, 0), (0, 0, 0)) (0, 0, 0) ..., with no Area or Intersection of 150. Alternatively, the docs say what a shrink past nothing gives.

Actual: Rect(10, 10, -10, -15) 150 True Rect(0, -5, 10, 15)
AABB((1, 1, 1), (-1, -1, -1)) (-2, -2, -2) True

Suggested fix: In Rect.Grow and AABB.Grow, keep each size at 0 or more: a shrink past nothing collapses to the center on that axis. Otherwise, document that a negative size is the caller's bug, and have Area and Intersects treat a negative size as empty. Add a test for a shrink past nothing.

Skeptic: Ran f3.fb: `Rect(10, 10, -10, -15) 150 True Rect(0, -5, 10, 15)` and `AABB((1, 1, 1), (-1, -1, -1)) (-2, -2, -2) True`. A milder shrink also breaks it: Rect(0,0,10,5).Grow(-3) is Rect(3, 3, 4, -1), with Area -4, Intersects True, and Contains(center) False. Rect's own doc comment (geometry.fb:13-14) says Width and Height aren't negative. Grow (line 84-86), which documents shrinking for a negative amount, breaks that invariant from a valid input, and Area, Intersects, and Intersection then give wrong answers. AABB.Grow (178-181) does the same. Nothing documents what happens past nothing.

## S9-linear-1 (medium): Release builds don't vectorize Vec4's Operators or Mat4's *, despite stdlib.md and the milestone row

Status: fixed in c220e41: it reproduces (the release IR of the finding's program has 84 `fmul float` and no `<4 x float>`), and stdlib.md now says Vec4's Operators and Mat4's `*` are scalar code until codegen runs LLVM's SLP vectorizer. Real SIMD needs the compiler: optimize() at codegen.cpp:415 would set LLVMPassBuilderOptionsSetSLPVectorization (and SetLoopVectorization) for release builds, the compiler lane's item 13. milestones.md's "with SIMD" is the coordinator's. No test, since it's the docs.

Where: compiler/src/codegen.cpp:415

stdlib.md (line 338) says "Release builds vectorize them as they stand: Vec4's Operators and Mat4's `*` come out as SIMD instructions", and milestone 9's scope says "with SIMD". The lane relied on the release SLP vectorizer, but it never runs. codegen.cpp's optimize() passes a fresh LLVMCreatePassBuilderOptions() to "default<O2>", and LLVM's PipelineTuningOptions leaves SLPVectorization off unless it is set. Nothing in compiler/src mentions SLP or vectorization. A release build of Mat4 * Mat4, Mat4 * Vec4, and Vec4 + and * is all scalar code.

```
Import std.core
Import std.math

Extern "C"
    Function rand(): Int32
End Extern

Function R(): Float => rand() As Float

Function V(): Vec4 => Vec4(R(), R(), R(), R())

Local a = Mat4(V(), V(), V(), V())
Local b = Mat4(V(), V(), V(), V())
Local c = a * b
Local d = V() + V()
Local e = V() * V()
Print(c, d, e, a * V())
```

Expected: `fb build --release --show llvm` shows <4 x float> fmul and fadd (packed mulps/addps in the executable's main) for the Mat4 and Vec4 arithmetic.

Actual: The optimized release IR has 84 `fmul float` and 64 `fadd float` and no `<4 x float>` anywhere. objdump of the release main shows 148 mulss/addss and 0 mulps/addps.

Suggested fix: In optimize(), for release builds call LLVMPassBuilderOptionsSetSLPVectorization(options, 1) (and SetLoopVectorization explicitly), as clang does at -O2. Add a codegen test that the release IR of Mat4 * Mat4 holds <4 x float>. Otherwise drop the SIMD claim from stdlib.md and the milestone row.

Skeptic: I ran the finding's program (refute-linear/../linear/f1.fb) with `fb build --release --show llvm`. The optimized IR (inlined down to 3 defines) has 84 `fmul float`, 64 `fadd float`, and no `x float>` at all. objdump of the release f1.exe shows 84 mulss, 64 addss, 0 mulps, and 0 addps. codegen.cpp:415 optimize() passes a fresh LLVMCreatePassBuilderOptions() to "default<O2>" (line 161). LLVM's default PipelineTuningOptions has SLPVectorization = false, and nothing in compiler/src turns it on. So stdlib.md:338-339 ("Vec4's Operators and Mat4's `*` come out as SIMD instructions") and the milestone 9 row ("with SIMD") are false as built. The lane's premise that the SLP vectorizer already does this is wrong.

## S9-linear-2 (medium): ToEuler snaps any pitch within about 0.26 degrees of a quarter turn to exactly 90, giving another rotation

Status: fixed in 51162da (math_quat.fb): ToEuler works the pitch out as Atan2(sine, cosine), with the cosine from m.x.y and m.y.y, and folds y and z into one only where that cosine is below 0.00035, about 0.02 degrees from the turn, where a Float can't tell y from z; pitches of 89.8 to 89.99 degrees round-trip within 0.02 degrees.

Where: std/math/quat.fb:140

ToEuler takes the gimbal branch whenever Abs(sine) >= 0.99999, which is any pitch from about 89.74 to 90 degrees. That branch sets x to exactly Pi / 2 and z to 0, so a camera pitched 89.8 degrees comes back as a different rotation, 0.2 degrees off, and FromEuler(ToEuler(q)) isn't q. The doc comment says this happens only "where x is a quarter turn". FPS cameras often clamp pitch to 89.9 degrees, which lands in this range.

```
Import std.core
Import std.math

// A camera pitched 89.8 degrees up, turned 10 degrees, and rolled 20.
Local q = Quat.FromEuler(Deg(89.8), Deg(10.0), Deg(20.0))
Local angles = q.ToEuler()
Print(q.ToEulerDegrees())
Local back = Quat.FromEuler(angles.x, angles.y, angles.z)
Print(IsClose(back, q), ToDegrees(Angle(back, q)))
```

Expected: (89.8, 10, 20) (or angles that make the same rotation)
True 0 (within rounding)

Actual: (90, -9.999889, 0)
False 0.19999722

Suggested fix: Work the pitch out with Atan2(sine, Sqrt(m.x.y * m.x.y + m.y.y * m.y.y)), which stays accurate near the pole. Take the gimbal branch only when that cosine is effectively 0 (say below 1e-6). Add a round-trip test at 89.8 degrees.

Skeptic: Ran f2.fb in debug and release with the same output. FromEuler(Deg(89.8), Deg(10), Deg(20)).ToEulerDegrees() gives (90, -9.999889, 0), and the round trip gives IsClose False with Angle 0.19999722 degrees. A sweep shows the cutoff: 89.0, 89.5, and 89.7 round-trip within 3e-4 degrees, while 89.75 is off by 0.25 degrees and 89.9 by 0.10. quat.fb:140 takes the gimbal branch at Abs(sine) >= 0.99999, which is pitches from about 89.74 degrees up. The non-gimbal branch is already accurate at 89.7, so the threshold is too loose. The doc comment's "where x is a quarter turn" doesn't hold.

## S9-linear-5 (medium): Vec2i and Vec3i division by zero and overflow panic inside std/math/int_vector.fb, not at the caller

Status: fixed in 0d1af08 (panic_math_vec2i_divide.fb, panic_math_vec3i_divide.fb, panic_math_vec2i_overflow.fb, panic_math_vec3i_distance.fb): Vec2i's and Vec3i's Operators, Dot, LengthSquared, ManhattanDistance, ChebyshevDistance, and Abs are @PanicsAtCaller. The two overflow programs are in verify.py's RELEASE_DIFFERS.

Where: std/math/int_vector.fb:34

std's convention is that a bug panics at the caller's line (@PanicsAtCaller). Vec2i's and Vec3i's Operators (+, -, *, /, unary -), Dot, LengthSquared, ManhattanDistance, ChebyshevDistance, and Abs have no @PanicsAtCaller. A division by zero or a debug-build overflow is reported at std/math/int_vector.fb, for example 31:52 for +, 84:67 for ManhattanDistance, and 34:52 for /. The user's own line isn't named. @PanicsAtCaller on an Operator moves the panic to the caller: a local Struct's `/` with it reports the program's own line.

```
Import std.core
Import std.math

Local step = Vec2i(4, 4)
Local cells = Vec2i(0, 1)
Print(step / cells)
```

Expected: panic: division by zero at <program>.fb:6:7

Actual: panic: division by zero at std/math/int_vector.fb:34:52

Suggested fix: Mark Vec2i's and Vec3i's Operators and the Functions over them @PanicsAtCaller. Add panic programs for division by zero and for overflow.

Skeptic: Debug runs gave these panics. Vec2i(4, 4) / Vec2i(0, 1) panics with "division by zero at std/math/int_vector.fb:34:52". Vec2i(2147483647, 0) + Vec2i(1, 0) panics at int_vector.fb:31:52. ManhattanDistance(Vec2i(-2147483647, 0), Vec2i(2147483647, 0)) panics at int_vector.fb:84:67. For comparison, a local Struct's @PanicsAtCaller Operator / reports f5d.fb:11:8, the program's own line, and a plain Int division reports f5e.fb:5:7. stdlib.md:151 says a bug panics at the caller's line (@PanicsAtCaller), and std already marks Iterator's Sum and Product @PanicsAtCaller for exactly this. int_vector.fb's Operators and Functions have no such mark. (scalar.fb's integer Mod and Rem have the same gap: Mod(5, 0) panics at std/math/scalar.fb:48:16.)

## S9-scalar-2 (medium): Integer Wrap and PingPong overflow when the range is wider than half the type

Status: fixed in 1da3da0 with S9-scalar-1 (math_scalars.fb): Int8 Wrap(120, -100, 100) is -80 and PingPong(10, 100) is 10, and Int's and Long's whole ranges and UInt64 lengths past 2^63 work.

Where: std/math/scalar.fb:160

Integer Wrap works out `value - low` and `high - low` in T, and integer PingPong works out `length + length` in T. Each of these overflows once the range is wider than about half of T, even though the result always fits: Wrap's result lies between low and high, and PingPong's between 0 and length. An Int8 Wrap over -100 to 100, or an Int8 PingPong with a length of 100, can't be worked out. The same happens for Int over MinValue to MaxValue. Debug builds panic, Wrap's inside std/math/scalar.fb. Release builds give wrong values: Wrap gives back 120, outside the range, and PingPong gives -10, a negative result.

```
// Wrap and PingPong over a range wider than half the type: Wrap(120, -100, 100) is -80 and
// PingPong(10, 100) is 10.
Import std.core
Import std.math

Local value: Int8 = 120
Local low: Int8 = -100
Local high: Int8 = 100
Print(Wrap(value, low, high))
Local ten: Int8 = 10
Local length: Int8 = 100
Print(PingPong(ten, length))
```

Expected: -80
10

Actual: debug: panic: integer overflow in '-' at std/math/scalar.fb:160:22 (exit 101); release: 120 then -10

Suggested fix: Work out the distances as unsigned values in a UInt64: T.LowBits(value) - T.LowBits(low), masked to BitWidth bits, with the sign decided by comparing value and low. Take the remainder there, then add it back with T.FromLowBits(T.LowBits(low) + r). For PingPong, reduce by 2 * length in the UInt64; for a UInt64 length above 2^63, which doesn't double, reduce by length and use the quotient's parity instead.

Skeptic: f_wide.fb (Int8 Wrap(120, -100, 100) and PingPong(10, 100)) panicked in debug with 'integer overflow in '-' at std/math/scalar.fb:160:22'. In release it printed 120, which is outside the range, and -10, which is negative. I also ran Wrap(5, MinValue<Int>(), MaxValue<Int>()): release gave -2147483648. PingPong(10 As Int8, 100) panics in debug at the caller on `length + length`. Lines 160 and 178 work out `value - low`, `high - low`, and `length + length` in T, though the result always fits. Neither comment warns about it, unlike Gcd and Lcm, whose comments say they overflow.

## S9-scalar-3 (medium): Float Wrap gives high itself, and float Mod gives b, for a small negative value

Status: fixed in b629641 (math_scalars.fb): float Mod gives 0 with b's sign where the sum would round to b, and for a 0 remainder, and float Wrap gives low where its sum reaches high, so Wrap(-0.00001, 0.0, 360.0) is 0. Their comments say so.

Where: std/math/scalar.fb:168

Float Mod adds b to fmod's negative remainder (line 64). For a remainder smaller than half of b's rounding gap, `rest + b` rounds to b exactly, so Mod(-0.00001, 360.0) is 360. Wrap is `low + Mod(value - low, high - low)`, so it gives high, while its comment and stdlib.md say high is left out. This shows up in normal angle code: an angle a hair below 0 wraps to exactly 360 or Tau. Code that relies on the result being below high, such as `Floor(Wrap(a, 0, 360) / 45)` as an index into 8 sectors, then goes out of range. Debug and release builds give the same results.

```
Import std.core
Import std.math

Local angle: Float = -0.00001
Local wrapped = Wrap(angle, 0.0, 360.0)
Print(wrapped, wrapped < 360.0)
Local turn = Wrap(-0.0000001 As Float, 0.0, Tau)
Print(turn, turn < Tau)
Print(Mod(-0.00001 As Float, 360.0))
```

Expected: a value below 360 (0 or 359.99997) and True; a value below Tau and True; a Mod below 360

Actual: 360 False
6.2831855 False
360

Suggested fix: In float Mod, when `rest + b` rounds to b, give 0 with b's sign: a remainder that small is as near 0 as it is to b. Or have Wrap give low when the sum reaches high. Either way, add the case to math_scalars.fb. If it stays as it is, Mod's and Wrap's comments and stdlib.md should say it can happen, as Fract's comment already does.

Skeptic: f_float_wrap.fb printed the same in debug and release: '360 False', '6.2831855 False', and '360'. A Double Wrap(-1e-20, 0.0, 360.0) gave '360 False' too. Float Mod (scalar.fb:64) adds b to fmod's tiny negative remainder, and the sum rounds to b. So Wrap (line 168) gives high, and Wrap's comment says high is left out. Fract's comment notes this kind of rounding, but Mod's and Wrap's comments don't, and math_scalars.fb doesn't test it. Code that indexes by Wrap's result, such as sector counts, goes past the end.

## S9-scalar-4 (medium): Lerp, and Remap through it, doesn't give b at t = 1 as its comment says

Status: fixed in 5e1d9f1 (math_scalars.fb): Lerp is `t == 1 ? b : a + (b - a) * t`, which keeps Lerp(a, a, t) exact too; Remap goes through it.

Where: std/math/scalar.fb:128

Lerp's comment says it gives "a at 0, b at 1". It's worked out as `a + (b - a) * t`. When b - a rounds, adding a back doesn't give b. In Float, Lerp(1.0, 0.1, 1.0) is 0.100000024, not 0.1, so an animation that ends at t = 1 never lands on its end value, and `== finish` is False. With values of very different sizes the error is large: Lerp(100000000.0, 3.0, 1.0) and Remap(10.0, 0.0, 10.0, 100000000.0, 3.0) both give 0 instead of 3. The test checks only endpoints whose differences are exact.

```
// Lerp's comment says it gives b at t = 1.
Import std.core
Import std.math

Local done: Float = 1.0
Local finish: Float = 0.1
Local at = Lerp(1.0, finish, done)
Print(at, at == finish)
Local far: Float = 100000000.0
Print(Lerp(far, 3.0, done), Remap(10.0, 0.0, 10.0, far, 3.0))
```

Expected: 0.1 True
3 3

Actual: 0.100000024 False
0 0 (debug and release)

Suggested fix: Use `T.Fma(t, b, T.Fma(-t, a, a))`, which is exact at t = 0 and at t = 1. Or use `t == T.One() ? b : a + (b - a) * t`. Or change the comment and stdlib.md so they don't promise b at 1. Either way, add Lerp(1.0, 0.1, 1.0) to math_scalars.fb.

Skeptic: f_lerp.fb printed '0.100000024 False' and '0 0' in debug and release, where the answers are 0.1, True, 3, and 3. Lerp (scalar.fb:128) is `a + (b - a) * t`, and its comment (line 125) says 'a at 0, b at 1'. When b - a rounds, t = 1 doesn't give b back. Remap goes through Lerp, so it's wrong the same way. math_scalars.fb:63 tests only endpoints whose differences are exact.

## S9-scalar-5 (medium): Mod and Rem by 0 panic inside std/math/scalar.fb, not at the caller's line; Abs, Gcd, and Lcm overflows do too

Status: fixed in eead6df (panic_math_mod.fb, panic_math_rem.fb, panic_math_abs.fb, panic_math_gcd.fb, panic_math_lcm.fb): Abs, Mod, Rem, Gcd, Lcm, and IntegerRem are @PanicsAtCaller. The abs, gcd, and lcm programs are in verify.py's RELEASE_DIFFERS.

Where: std/math/scalar.fb:48

Integer Mod's and Rem's comments say "A b of 0 panics", and stdlib.md's convention is that a bug panics at the caller's line, through @PanicsAtCaller. Neither Function has that attribute. The division in IntegerRem panics, and the panic points at std/math/scalar.fb:48:16 in every build, so the user can't tell which call failed. No panic program tests it. Abs, Gcd, and Lcm, whose comments say a result too large for T overflows, aren't @PanicsAtCaller either, so their debug overflow points into std as well. For example, Gcd(-128 As Int8, 0) panics at std/math/scalar.fb:13:32. PingPong is @PanicsAtCaller, and its overflow is reported at the caller's line, so the attribute is the whole fix. Integer Wrap has the same problem through Mod.

```
// Mod's and Rem's documented panic for a b of 0 should be at the caller's line, as std's
// panics are.
Import std.core
Import std.math

Local zero = 0
Print(Mod(7, zero))
```

Expected: panic: division by zero at f_mod_zero.fb:7:7 (or a Mod message at that line)

Actual: panic: division by zero at std/math/scalar.fb:48:16 (debug and release)

Suggested fix: Mark integer Mod, Rem, Gcd, Lcm, Wrap, and Abs @PanicsAtCaller, since IntegerRem's division and Abs's negation run inside them. Optionally, panic with a message of their own on b == 0. Add panic programs such as panic_math_mod.fb and panic_math_rem.fb.

Skeptic: Mod(7, zero) and Rem(7, zero) both panicked with 'division by zero at std/math/scalar.fb:48:16' in debug and in release. Gcd(-128 As Int8, 0) and Abs(-128 As Int8) panicked in debug at std/math/scalar.fb:13:32, and gave -128 in release. A PingPong overflow, by contrast, was reported at the caller's line (f_pp.fb:6:7), because PingPong is @PanicsAtCaller. Mod's and Rem's comments say 'A b of 0 panics', and stdlib.md's Conventions say a bug panics 'at the caller's line (@PanicsAtCaller)'. Mod, Rem, Gcd, Lcm, integer Wrap, and Abs don't have the attribute, and there is no panic_math_mod or panic_math_rem program.

## S9-color-geo-4 (low): Sphere.Contains and Sphere.Intersects overflow Floats for huge values

Status: fixed in a2a5578 (math_geometry.fb).

Where: std/math/geometry.fb:208

Sphere.Contains compares DistanceSquared(Center, point) <= Radius * Radius in Floats, and Sphere.Intersects does the same with the sum of the radii. Past about 1.8e19, both squares overflow to Inf, and Inf <= Inf is True. So a point outside the sphere is held, and spheres far apart meet. Distance(), which works in Doubles, gives the right 4e+19 for the same points. docs/stdlib.md says lengths and distances are worked out in Doubles so that no Float overflows on the way. AABB.Intersects(Sphere) goes through Contains, so it has the same problem.

```
Import std.core
Import std.math

// A sphere of radius 3e19 doesn't hold a point 4e19 from its center, and two of radius 1e19,
// 1e20 apart, don't meet; the squares overflow a Float, and Inf <= Inf.
Print(Sphere(Vec3.Zero(), 3e19).Contains(Vec3(4e19, 0, 0)),
      Sphere(Vec3.Zero(), 1e19).Intersects(Sphere(Vec3(1e20, 0, 0), 1e19)))
Print(Distance(Vec3.Zero(), Vec3(4e19, 0, 0)))
```

Expected: False False
4e+19

Actual: True True
4e+19

Suggested fix: Compare the squares in Doubles: widen the difference's components and the radius before squaring, as Distance does.

Skeptic: Ran f4.fb: "True True", and Distance gives 4e+19 while DistanceSquared gives inf. AABB(4e19..5e19).Intersects(Sphere(0, 3e19)) is also wrongly True. Sphere.Contains and Intersects (geometry.fb:208-216) compare Float squares that overflow to Inf, and Inf <= Inf is True. vector.fb documents DistanceSquared as "in Floats", so the code doesn't plainly contradict the Doubles rule, which covers Length and Distance. But the answers are wrong, and the fix is cheap. Low, since Float positions near 1e19 are far outside practical use.

## S9-color-geo-5 (low): Plane.Raycast gives -0 for a ray that starts on the plane and goes to its back

Status: fixed in a5509cf (math_geometry.fb): Raycast gives along + 0.

Where: std/math/geometry.fb:282

For a ray that starts on the plane, `(Distance - Dot(Normal, ray.Origin)) / facing` is 0 / negative = -0 when facing is negative. The check `!(along >= 0)` lets it through, so Raycast returns -0, which prints as "-0". The same ray going the other way gives 0. The docs promise 0 for a ray that starts on a shape, and math_geometry.fb's own Shown() adds 0 to hide -0. AABB.Raycast doesn't have this problem, because its near starts at 0.

```
Import std.core
Import std.math

// A ray that starts on the plane meets it at 0, going either way.
Local floor = Plane(Vec3.Up, 2)
Print(floor.Raycast(Ray(Vec3(0, 2, 0), Vec3.Up)) ?? -1, floor.Raycast(Ray(Vec3(0, 2, 0), Vec3.Down)) ?? -1)
```

Expected: 0 0

Actual: 0 -0

Suggested fix: Return `along + 0`, or return 0 when the numerator is 0. Add the downward case to math_geometry.fb.

Skeptic: Ran f5.fb in debug and release: "0 -0", and the downward hit's ToBits is 2147483648 (-0.0); it still == 0. Sphere and AABB give +0 from their surfaces. geometry.fb:282-284 computes 0 / negative facing, and `!(along >= 0)` lets -0 through. The value is numerically 0, so only printing and things like 1/x or CopySign see it. The test's Shown() adds 0 to hide -0, so the lane treats -0 as unwanted output, and the result differs by ray direction. Low.

## S9-color-geo-6 (low): Color32.Green and Color.Green are CSS's lime, not CSS's green, though the docs say the named colors are CSS's values

Status: fixed in 52926af (math_color.fb): Green is CSS's #008000, and the new Lime is #00ff00, so every named color is CSS's, as the docs say, and stdlib.md names both.

Where: std/math/color.fb:37

docs/stdlib.md says the named colors are "CSS's sRGB values", and math_color.fb's comment says the same. The other ten match CSS: transparent, black, white, gray #808080, red, blue, yellow, cyan (aqua), magenta (fuchsia), and orange #ffa500. Green is #00ff00, which is CSS's `lime`; CSS's `green` is #008000. Someone who reads the docs and expects CSS green gets a different color.

```
Import std.core
Import std.math

// CSS's green is #008000; #00ff00 is CSS's lime.
Print(Color32.Green.ToHex(), Color.Green.ToHex(), Color32.Hex("#008000"))
```

Expected: #008000 #008000 (0, 128, 0, 255)   (per the docs' claim)

Actual: #00ff00 #00ff00 (0, 128, 0, 255)

Suggested fix: Either make Green #008000 as CSS has it (and perhaps add Lime), or change the docs and the test comment to say Green is full green (CSS's lime) and the rest are CSS's values. This is the owner's call.

Skeptic: Ran f6.fb: `#00ff00 #00ff00 (0, 128, 0, 255)`. color.fb:37-39 makes Green Color32(0, 255, 0), which is CSS's `lime`; CSS's `green` is #008000. stdlib.md line 335 says the named colors are "CSS's sRGB values", and math_color.fb:51 says "The named colors, the same in both: CSS's sRGB values." The other ten match CSS. The docs and code plainly disagree, so one of them has to change; the owner decides which.

## S9-color-geo-7 (low): Color.Gray and Color.Orange don't equal their own Color32 or hex converted, because ToLinear rounds its input to a Float first

Status: fixed in a366b8e (math_color.fb): ToLinear takes a Double, so each byte's linear value is rounded once, and Orange's constant is 0.37626213; each named Color equals its Color32's ToColor and its Hex.

Where: std/math/color.fb:291

ToLinear(channel: Float) takes a Float. So Color32.ToColor's `r / 255.0` is rounded to a Float before the gamma curve is worked out in Doubles. The result can be 1 or 2 ulps off the correctly rounded linear value: #808080 gives bits 1046284939, and the correctly rounded value is 1046284937. Color.Gray's constant, 0.2158605, is the correctly rounded value. Color.Orange's, 0.3762621, is one ulp low (correct is 0.37626213, bits 1052812654). As a result, `Color.Gray == Color32.Gray.ToColor()` and `Color.Gray == Color.Hex("#808080")` are False, and so are both for Orange, although the docs call the named colors the same in both. The test checks only the ToColor32 direction, so it doesn't catch this. Debug and release agree.

```
Import std.core
Import std.math

// The named Colors are the linear values of the same named Color32s.
Print(Color.Gray == Color32.Gray.ToColor(), Color.Gray == Color.Hex("#808080"))
Print(Color.Orange == Color32.Orange.ToColor(), Color.Orange == Color.Hex("#ffa500"))
Print(Color.Gray.r.ToBits(), Color32.Gray.ToColor().r.ToBits())
Print(Color.Orange.g.ToBits(), Color32.Orange.ToColor().g.ToBits())
```

Expected: True True
True True
1046284937 1046284937
1052812654 1052812654

Actual: False False
False False
1046284937 1046284939
1052812653 1052812655

Suggested fix: Have ToLinear take a Double, and pass `r / 255.0` from ToColor as a Double, so each byte's linear value is correctly rounded. Write Orange's constant as 0.37626213. Add `Color.X == Color32.X.ToColor()` checks to math_color.fb.

Skeptic: Ran f7.fb in debug and release: "False False" for Gray and for Orange. Gray's r bits are 1046284937 (constant) vs 1046284939 (Color32.Gray.ToColor()), and Orange's g bits are 1052812653 vs 1052812655. The correctly rounded values, worked out in Doubles and cast, are 1046284937 and 1052812654. So ToColor is 2 ulps off because ToLinear(channel: Float) (color.fb:291) rounds r/255.0 to a Float before the 'worked out in Doubles' gamma, and Orange's constant 0.3762621 is 1 ulp below the correctly rounded value. Red, White, and Green compare equal. So `Color.Hex("#808080") == Color.Gray` is False, though the test says the named colors are the same in both. It's a Float-equality precision issue, so low.

## S9-linear-10 (low): Spec 06's `Extend Vec3 Implements Printable` example no longer compiles

Status: fixed in ebac864: spec 06's first Extend example makes a program's own Point Printable, and a second adds Flattened to std.math's Vec3; both compile. No test, since it's the spec.

Where: docs/spec/06-user-types.md:339

The Extend section's first example adds Printable and a ToString to Vec3. std's Vec3 now implements Printable with that same ToString, so the example is an error: Printable is listed twice and ToString is already declared.

```
Import std.core
Import std.math

Extend Vec3 Implements Printable
    Method ToString(): String => "({x}, {y}, {z})"
End Extend

Print(Vec3(1, 2, 3))
```

Expected: (1, 2, 3), as the spec's example implies

Actual: p16.fb:5:12: error: 'ToString' is already declared with the same parameter types (std/math/vector.fb:89:19: note: first declared here), plus an error on Printable listed again; 2 errors

Suggested fix: Change the example to extend a type of the program's own, or to add a Method std's Vec3 doesn't have.

Skeptic: spec 02-types.md:13 says Vec3 comes from std.math. spec 06-user-types.md:339's example `Extend Vec3 Implements Printable` with ToString, compiled as f10.fb with Import std.math, fails with 2 errors: "Vec3 implements Printable already" (note: vector.fb:47:50) and "'ToString' is already declared with the same parameter types" (note: vector.fb:89:19). The step changed only spec 07, so the example is stale. This is a docs fix.

## S9-linear-11 (low): Perspective's panic for a non-finite far or aspect says 0 < near < far was broken when it wasn't

Status: fixed in 3699695 (panic_math_perspective_far.fb, panic_math_perspective_aspect.fb): the message says a finite aspect above 0 and 0 < near < far with a finite far.

Where: std/math/matrix.fb:153

Perspective also panics for a non-finite aspect or far, but the message names only the field of view range, an aspect above 0, and 0 < near < far. An infinite far plane, which reversed-depth renderers use, gives 0.1 and inf, which meet every rule the message names.

```
Import std.core
Import std.math

// An infinite far plane, as reversed-depth renderers use.
Print(Mat4.Perspective(Deg(60), 1.5, 0.1, Inf))
```

Expected: A panic that says the aspect and far have to be finite, as the doc comment does

Actual: panic: Perspective takes a field of view between 0 and Pi, an aspect above 0, and 0 < near < far, not 1.0471976, 1.5, 0.1, and inf at p20.fb:5:7

Suggested fix: Say "a finite aspect above 0, and 0 < near < far with a finite far" in the message.

Skeptic: Perspective(Deg(60), 1.5, 0.1, Inf) panics with "...an aspect above 0, and 0 < near < far, not 1.0471976, 1.5, 0.1, and inf", and Perspective(Deg(60), Inf, 0.1, 100) gives the same wording with aspect inf. Both inputs meet every rule the message names. The panic comes from the IsFinite checks at matrix.fb:151-152, which the message at :153-154 leaves out (the doc comment covers far's finiteness, not aspect's). The message misleads, so the severity is low.

## S9-linear-3 (low): Quat.FromTo returns Identity for turns under about 0.04 degrees

Status: fixed in 2339731 (math_quat.fb): FromTo has no near-parallel shortcut; the general formula gives Identity for equal directions.

Where: std/math/quat.fb:82

FromTo returns Identity whenever cosine > 1 - 2 * Epsilon, which is any angle under about 6.9e-4 radians. The general branch handles those angles exactly: the cross product is accurate and w is about 2, and for equal directions it already gives Identity. So the shortcut only throws accuracy away. Something aimed with FromTo at a target 1000 units away and 0.5 units off stays pointing straight ahead.

```
Import std.core
Import std.math

// A turn of 0.0005 radians, about 0.03 degrees.
Local target = Normalize(Vec3(1, 0.0005, 0))
Local turn = Quat.FromTo(Vec3.Right, target)
Print(turn, turn * Vec3.Right, target)
Print(Angle(Vec3.Right, target), Angle(turn * Vec3.Right, target))
// Aimed 1000 units away, the miss in units.
Print(Distance(turn * Vec3.Right * 1000, target * 1000))
```

Expected: (0, 0, 0.00025, 1) (0.9999999, 0.0005, 0) (0.9999999, 0.00049999997, 0)
0.00049999997 0
0 (within rounding)

Actual: (0, 0, 0, 1) (1, 0, 0) (0.9999999, 0.00049999997, 0)
0.00049999997 0.00049999997
0.5

Suggested fix: Drop the near-parallel shortcut and keep only the near-opposite one. Normalize(Quat(Cross(start, finish), 1 + cosine)) is exact for small angles and Identity for equal directions. Add a test with a 0.0005-radian turn.

Skeptic: Ran f3.fb in debug and release. FromTo(Right, Normalize(Vec3(1, 0.0005, 0))) gives (0, 0, 0, 1), turn * Right is (1, 0, 0), the angle left over is 0.0005 rad, and the miss at 1000 units is 0.5. 1 - 2 * Epsilon is 0.99999976 and the Dot is 0.9999999, so the shortcut at quat.fb:82 fires. The general formula on the same inputs gives (0, 0, 0.00025, 1), with a leftover angle of 5.8e-11, and it gives Identity for equal directions, so the shortcut only loses accuracy. Severity is low: a 600-frame turret-tracking test turned 9.98 of 10 degrees, so the error stays under about 0.04 degrees and doesn't build up. glm has the same kind of shortcut, at half this threshold.

## S9-linear-4 (low): The vectors' Lerp doesn't give b at t = 1, as its comment and stdlib.md say

Status: fixed in fa59123 (math_vectors.fb): as the scalar Lerp does, for S9-scalar-4.

Where: std/math/vector.fb:232

Lerp(a, b, t) is a + (b - a) * t, which rounds b - a and so often misses b at t = 1. For random Floats in -100 to 100 it misses about a third of the time. The comment says "a at 0, b at 1". Code that lerps to a target and then checks `pos == target` never arrives. The scalar Lerp in scalar.fb has the same formula and the same comment.

```
Import std.core
Import std.math

Local start = Vec2(3.3, 1)
Local finish = Vec2(0.2, 5)
Local arrived = Lerp(start, finish, 1)
Print(arrived, finish, arrived == finish)
Print(Lerp(Vec3(3.3, 0, 0), Vec3(0.2, 0, 0), 1) == Vec3(0.2, 0, 0))
```

Expected: (0.2, 5) (0.2, 5) True
True

Actual: (0.20000005, 5) (0.2, 5) False
False

Suggested fix: Use a * (1 - t) + b * t per component, which is exact at both ends (in the scalar Lerp too), or return b when t == 1. Add t = 1 cases with values like these to the tests.

Skeptic: Ran f4.fb in debug and release. Lerp(Vec2(3.3, 1), Vec2(0.2, 5), 1) gives (0.20000005, 5), which is not == (0.2, 5). The Vec3 case is also False, and the scalar Lerp(3.3, 0.2, 1) gives 0.20000005. vector.fb:232-238 and scalar.fb:126-128 compute a + (b - a) * t, and their comments say "b at 1". The comment is plainly wrong, by 1 ulp. Severity is low because this is the common Unity/Godot formula and the error is one rounding.

## S9-linear-6 (low): Inverse and Decompose break when the Float determinant over- or underflows

Status: fixed in b8720ce (math_matrices.fb): TryInverse, Determinant, and Decompose work in Doubles, which hold any four Floats' products, the Mat4's by Geometric Tools' 2 by 2 minors, each cofactor divided by the determinant, so each element is within about an ulp of the correctly rounded inverse. The camera matrix's round trip is held to 0.0001: its condition, about 500, leaves even the correctly rounded inverse's product 3e-5 off, which the old Float inverse's errors happened to cancel.

Where: std/math/matrix.fb:327

TryInverse returns inverse * (1 / determinant). 1 / determinant is Inf for a determinant under about 2.9e-39 and 0 for an infinite one, so an invertible matrix inverts to Inf and NaN, or to zeros and NaN, with no panic. Mat3's TryInverse (line 279) does the same. Decompose panics on `Determinant(Self) == 0` (line 205) when a tiny scale's determinant underflows to 0, though it uses the determinant only for its sign and the lengths are worked out in Doubles.

```
Import std.core
Import std.math

Print(Inverse(Mat4.Scale(Vec3(1e-13, 1e-13, 1e-13))))
Print(Inverse(Mat4.Scale(Vec3(1e13, 1e13, 1e13))))
Print(Mat4.Compose(Vec3.One(), Quat.Identity, Vec3(1e-16, 1e-16, 1e-16)).Decompose())
```

Expected: ((1e+13, 0, 0, 0), (0, 1e+13, 0, 0), (0, 0, 1e+13, 0), (0, 0, 0, 1))
((1e-13, 0, 0, 0), (0, 1e-13, 0, 0), (0, 0, 1e-13, 0), (0, 0, 0, 1))
((1, 1, 1), (0, 0, 0, 1), (1e-16, 1e-16, 1e-16))

Actual: ((inf, -nan, -nan, -nan), (-nan, inf, -nan, -nan), (-nan, -nan, inf, -nan), (-nan, -nan, -nan, inf))
((0, -0, 0, -0), (-0, 0, -0, 0), (0, -0, 0, -0), (-0, 0, -0, -nan))
panic: Decompose takes a matrix that has an inverse at f6.fb:6:7

Suggested fix: Divide the cofactors by the determinant (or work the determinant and the scaling in Doubles) instead of multiplying by its reciprocal. In Decompose, panic on a zero column length, or work the determinant in Doubles.

Skeptic: Ran f6.fb. Determinant(Mat4.Scale(1e-13 each)) is 1e-39. Inverse of that Mat4 prints ((inf, -nan, -nan, -nan), ...), and the Mat3 version does the same. Inverse(Mat4.Scale(1e13 each)) gives zeros and a -nan. Compose(One, Identity, 1e-16 each).Decompose() panics with "Decompose takes a matrix that has an inverse", though the matrix has one that Floats hold. Scales of 1e-6 and 1e-3 invert correctly. matrix.fb:279 and :327 multiply by 1 / determinant, and :205 checks the underflowed determinant against 0. These are extreme scales and glm uses the same reciprocal, so the severity is low.

## S9-linear-7 (low): Project, Angle, and MoveTowards overflow or underflow in Floats where Length and Distance don't

Status: fixed in 1610821 (math_vectors.fb).

Where: std/math/vector.fb:309

stdlib.md says lengths and distances are worked out in Doubles so no Float overflows, and Length(Vec3(3e20, 4e20, 0)) and Length(Vec3(3e-30, 4e-30, 0)) are right. Project divides by LengthSquared in Floats, so a long or short onto gives the zero vector. Angle takes Atan2 of a Float Cross and Dot, giving 0 for short vectors and NaN for long ones. MoveTowards casts the Double distance to Float, which is Inf for points 6e38 apart, so Inf * 0 gives NaN.

```
Import std.core
Import std.math

Local short = Vec3(3e-30, 4e-30, 0)
Local long = Vec3(3e20, 4e20, 0)
Print(Length(short), Normalize(short), Length(long), Normalize(long))
Print(Project(Vec3(5, 0, 0), short), Project(Vec3(5, 0, 0), long))
Print(Angle(short, Vec3(0, 0, 4e-30)), Angle(long, Vec3(3e20, 4e20, 1e19)))
Print(MoveTowards(Vec2(-3e38, 0), Vec2(3e38, 0), 1))
```

Expected: 5e-30 (0.6, 0.8, 0) 5e+20 (0.6, 0.8, 0)
(1.8, 2.4, 0) (1.8, 2.4, 0)
1.5707964 0.019997334
(-3e+38, 0)

Actual: 5e-30 (0.6, 0.8, 0) 5e+20 (0.6, 0.8, 0)
(0, 0, 0) (0, 0, 0)
0 -nan
(-nan, 0)

Suggested fix: Work Project's and Angle's products in Doubles, as Magnitude does, or scale by the larger component first. In MoveTowards, keep the distance and the ratio in Doubles.

Skeptic: Ran f7.fb and got the finding's exact output. Length and Normalize are right. Project onto the short or long vector gives (0, 0, 0) instead of (1.8, 2.4, 0). Angle(short, Vec3(0, 0, 4e-30)) is 0 for two perpendicular non-zero vectors, and Angle on the long ones is -nan, though the comment promises 0 to Pi, and 0 only for a zero vector. MoveTowards(Vec2(-3e38, 0), Vec2(3e38, 0), 1) gives (-nan, 0) because Distance is inf. The cause is Float LengthSquared, Cross, and Dot at vector.fb:308-322, and Inf * 0 at :338. The comments say otherwise, but only for absurd magnitudes, so the severity is low.

## S9-linear-8 (low): Normalize(Quat) works in Floats and gives a zero quaternion for a long one

Status: fixed in 26e056d (math_quat.fb): they go through Vec4's.

Where: std/math/quat.fb:177

Length(q) is Sqrt(Dot(q, q)) in Floats, unlike the vectors' Length, which uses Doubles. Normalize(Quat(0, 0, 0, 1e20)) divides by an infinite length and gives (0, 0, 0, 0), which isn't a rotation. A short quaternion's square underflows, so Normalize returns Identity rather than its direction. The Vec4 Normalize handles both inputs.

```
Import std.core
Import std.math

Print(Normalize(Vec4(0, 0, 0, 1e20)), Normalize(Quat(0, 0, 0, 1e20)))
Print(Normalize(Vec4(0, 0, 1e-25, 1e-25)), Normalize(Quat(0, 0, 1e-25, 1e-25)))
Print(Length(Vec4(0, 0, 0, 1e20)), Length(Quat(0, 0, 0, 1e20)))
```

Expected: (0, 0, 0, 1) (0, 0, 0, 1)
(0, 0, 0.70710677, 0.70710677) (0, 0, 0.70710677, 0.70710677)
1e+20 1e+20

Actual: (0, 0, 0, 1) (0, 0, 0, 0)
(0, 0, 0.70710677, 0.70710677) (0, 0, 0, 1)
1e+20 inf

Suggested fix: Work Length(Quat) and Normalize(Quat) in Doubles, as vector.fb's Magnitude does (it could be made Public or Package for quat.fb).

Skeptic: Ran f8.fb. Normalize(Quat(0, 0, 0, 1e20)) is (0, 0, 0, 0), which isn't a rotation, while the Vec4 version gives (0, 0, 0, 1). Normalize(Quat(0, 0, 1e-25, 1e-25)) gives Identity instead of (0, 0, 0.707, 0.707). Length(Quat(0, 0, 0, 1e20)) is inf, while the Vec4's is 1e+20. quat.fb:177 is Sqrt(Dot(q, q)) in Floats. The comment says "scaled to length 1", which is wrong for these inputs. Quaternions of such sizes are rare, so the severity is low.

## S9-linear-9 (low): Mat3, Mat4, and Quat don't scale from the left, though stdlib.md says a scalar scales from either side

Status: fixed in 821a193 (math_matrices.fb): Mat3 and Mat4 have scale * m and m / scale, and stdlib.md says the vectors' Operators work component by component, as the matrices' + and - do, and a scalar scales and divides a vector or a matrix, which leaves Quat out.

Where: std/math/matrix.fb:76

stdlib.md's linear algebra entry lists Vec2 through Mat4, then says "Their Operators work component by component, and a scalar scales from either side". Mat3 and Mat4 have only `m * scale`, with no `scale * m` and no `m / scale`. Quat has no scalar Operators at all. Either the Operators are missing or the sentence should name the vectors.

```
Import std.core
Import std.math

Print(Mat3.Identity * 2)
Print(2 * Mat3.Identity)
```

Expected: ((2, 0, 0), (0, 2, 0), (0, 0, 2))
((2, 0, 0), (0, 2, 0), (0, 0, 2))

Actual: f9.fb:5:7: error: no Operator * takes (Int32, Mat3)

Suggested fix: Add Operator *(scale: Float, m: Mat3) and Mat4's (and / if wanted), or reword stdlib.md so the scalar sentence covers only the vectors.

Skeptic: Mat3.Identity * 2 compiles, but 2 * Mat3.Identity is "no Operator * takes (Int32, Mat3)". Mat4.Identity / 2 is "'/' can't combine Mat4 and Int32", and Quat.Identity * 2 is "no Operator * takes (Quat, Int32)". The Vec2i scalar forms work from both sides. stdlib.md:314-316 lists Mat3 and Mat4 among the types whose "scalar scales from either side", and matrix.fb:76 and :234 have only m * scale. Quat's Operators aren't component-wise, so that sentence plainly can't cover it. Even so, the matrices contradict the doc. It's a doc-or-API mismatch, so the severity is low.

## S9-scalar-6 (low): ToDegrees has no Int overload, though stdlib.md says each angle Function takes an Int

Status: fixed in 66e2f03 (math_scalars.fb).

Where: std/math/scalar.fb:328

stdlib.md lists Deg, ToRadians, ToDegrees, and Rad, then says "Each takes an Int too, since degrees are often whole, and gives a Float." Deg, Rad, and ToRadians have `(x: Int): Float` overloads, but ToDegrees has only the generic FloatingPoint one. So ToDegrees(1) and ToDegrees(someInt) are compile errors.

```
Import std.core
Import std.math

Local r: Int = 2
Print(ToDegrees(1))
Print(ToDegrees(r))
```

Expected: 57.29578
114.59156

Actual: error: Int32 doesn't implement FloatingPoint, which 'ToDegrees' needs for T (once at each call)

Suggested fix: Add `Public Function ToDegrees(radians: Int): Float => (radians * (180 / Pi64)) As Float`, or change stdlib.md to name the three Functions that take an Int.

Skeptic: ToDegrees(1) and ToDegrees(r) with r: Int each fail with 'error: Int32 doesn't implement FloatingPoint, which 'ToDegrees' needs for T'. Deg(r), Rad(r), and ToRadians(r) compile and print 0.034906585, 2, and 0.034906585. stdlib.md's Angles item covers Deg, ToRadians, ToDegrees, and Rad and says 'Each takes an Int too ... and gives a Float'. scalar.fb:328 has only the generic ToDegrees.

## S9-scalar-7 (low): The math types are still in the Deferred work, and spec 06's Extend Vec3 example no longer compiles

Status: fixed in ebac864 for the spec: spec 02's table drops the math types, with a line saying they're std.math's Structs, which a program imports, and spec 06's example is S9-linear-10's. milestones.md's Deferred work row is the coordinator's to drop. No test, since it's the docs.

Where: docs/milestones.md:270

Step 9's done-when says "the math types leave the Deferred work". sema.cpp's later names did drop them, but docs/milestones.md's Deferred work still has the row "The math types, Vec2 through Color | | M6 step 9", and the step doesn't touch milestones.md. Spec 07 was updated, but two other spec pages still describe the math types as they were before this step. docs/spec/06-user-types.md:339 shows `Extend Vec3 Implements Printable` with a ToString, which now fails with "Vec3 implements Printable already" and "'ToString' is already declared", since std.math's Vec3 is Printable. docs/spec/02-types.md:13 still lists Vec2 through Color among the built-in types, without Vec2i, Vec3i, or Color32, though they are std Structs that need an Import.

```
Import std.core
Import std.math

Extend Vec3 Implements Printable
    Method ToString(): String => "({x}, {y}, {z})"
End Extend

Print(Vec3(1, 2, 3))
```

Expected: (1, 2, 3), as spec 06's example implies

Actual: spec06.fb:4:24: error: Vec3 implements Printable already
spec06.fb:5:12: error: 'ToString' is already declared with the same parameter types

Suggested fix: Remove the math types' row from milestones.md's Deferred work. Give spec 06's Extend example a type that isn't already Printable, such as a program's own Struct, or an Interface Vec3 doesn't implement. Change spec 02's table row so it says these are std.math's Structs, which need an Import, rather than built-ins.

Skeptic: In the review checkout, docs/milestones.md:270 still has '| The math types, `Vec2` through `Color` | | M6 step 9 |' in the Deferred work. Step 9's done-when (line 177) says 'the math types leave the Deferred work', and the step's diff changes only spec 07 and stdlib.md under docs/. I compiled spec 06's `Extend Vec3 Implements Printable` example (06-user-types.md:339) and it failed with 'Vec3 implements Printable already' (std/math/vector.fb:47) and ''ToString' is already declared with the same parameter types' (vector.fb:89). spec 02-types.md:13 still lists Vec2 through Color in the Built-in types table, with no Vec2i, Vec3i, or Color32.
