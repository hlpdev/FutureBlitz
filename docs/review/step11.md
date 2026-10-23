# The review of M6 step 11's std.math.big (BigInt over libtommath, Decimal over mpdecimal, Rational, Complex; the typed units wait on the compiler's computed Const type arguments and aren't in this change). A known macOS build break (decimal.cpp's from_chars) is already reported; don't report it

15 confirmed findings: 0 high, 5 medium, 10 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S11-bigint-1 (medium): A BigInt past 2^31-1 bits is made without a panic, then BitLength, ToInt64, ToDouble, >> and ToString go wrong

Status: open

Where: std/math/big/bigint.fb:237

In libtommath 1.3.0, mp_count_bits returns an int computed as (used - 1) * 60 + top bits, and nothing checks it for overflow. The guard on << in bigint.fb only panics for a count above 2147483647, and ** only panics for an exponent above 2147483647. Neither looks at how many bits the result will have. So BigInt(1) << 2147483647 is accepted, as the doc comment says it should be, but it builds a number of 2147483648 bits, and mp_count_bits wraps to -2147483648 for it. After that, everything that uses the bit count is wrong. BitLength is negative. fb_bigint_fits_int64 sees bits <= 63, so ToInt64 gives 0 instead of Null. fb_bigint_to_double takes the bits <= 63 path and gives 0.0 instead of infinity. fb_bigint_shift_right sees count >= bits and gives 0. ToString sizes its buffer from the negative count: in another program, (BigInt(1) << 2147483647).ToString(32) ended with "terminate called after throwing an instance of 'std::length_error'" and exit code 0xC0000409, although the runtime is meant never to throw. * and ** with an exponent of 2147483647 or less can build such numbers the same way. Debug and release give the same results.

```
Import std.core
Import std.math.big

// The doc comment on << says only a shift past 2147483647 bits is too large.
Local x = BigInt(1) << 2147483647
Print(x.BitLength)
Print(x.ToInt64() == Null)
Print(x.ToDouble())
Print(x >> 2147483600)
```

Expected: 2147483648
True
inf
140737488355328
(or a panic at the shift, saying the result is too large)

Actual: -2147483648
False
0
0

Suggested fix: Limit a BigInt's size to what libtommath's int counts can hold, for example at most INT_MAX - MP_DIGIT_BIT bits. In bigint.fb, panic in << when a.BitLength + b would pass that limit, and in ** when a.BitLength * b would, rather than checking only the count or exponent. In bigint.cpp, check the size of the results of mp_mul, mp_expt_n and mp_mul_2d (result->used against INT_MAX / MP_DIGIT_BIT) and panic with "a BigInt got too large". Update the doc comments on << and ** to give the real limit, and add a panic test at the boundary, such as BigInt(1) << 2147483647.

Skeptic: I ran the finding's program (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s11/bigint-verify/f1.fb) in debug and release. Both printed -2147483648, False, 0, 0, where 2147483648, True, inf and 140737488355328 (or a panic) were expected. A second program, f1b.fb, just calls x.ToString(32) on the same number. It died with "terminate called after throwing an instance of 'std::length_error'" and exit code 0xC0000409, an uncaught C++ exception rather than a panic. The code confirms why. bigint.fb:237 panics on << only when b > 2147483647, and :217 does the same for ** with the exponent, so neither guard looks at the result's size. The doc comment on << (232-234) promises a shift of 2147483647 is fine. In runtime/src/bigint.cpp, BitLength (line 232), fits_int64 (183), to_double (194), shift_right (298) and to_string (161) all trust mp_count_bits, and for a number of 2^31 bits that int has wrapped negative. The results are silently wrong, and the runtime throws when it is meant never to. It only bites on a 256 MB number, which is why it's medium.

## S11-decimal-1 (medium): FromDouble of a large whole Double gives its exact binary value, not the shortest Decimal

Status: open

Where: runtime/src/decimal.cpp:137

fb_decimal_from_double formats the Double with plain std::to_chars and parses the result. For whole Doubles at or above 2^53, to_chars's fixed form and the zero-padded shortest digits can be the same length. The standard breaks that tie toward the smaller difference, so to_chars prints the Double's exact integer value. FromDouble then gives 17 to 21 digits where the docs promise the shortest Decimal that reads back as the Double (decimal.fb FromDouble, big.hpp, stdlib.md, and the lane's own Java BigDecimal.valueOf rule). The result isn't == to the Decimal of its shortest digits. The randomized differential run against Python's Decimal(repr(x)) shows this for every such Double in about [9e15, 1e22) and for nothing else. The tests only check 1e16 and 12345678901234567.0, which this doesn't affect.

```
Import std.core
Import std.math.big

Local x = Decimal.FromDouble(5.83386073903097e16)
Print(x)
Print(x == (Decimal.Parse("5.83386073903097e16") ?? Decimal(0)))
Print(Decimal.FromDouble(1.2345678901234568e20))
```

Expected: 5.83386073903097E+16
True
1.2345678901234568E+20
(Python's Decimal(repr(x)) and Java's BigDecimal.valueOf give these values)

Actual: 58338607390309696
False
123456789012345683968
(same in debug and --release)

Suggested fix: Get the digits from std::to_chars(first, last, value, std::chars_format::scientific), which gives the shortest round-trip digits with no exact-value tie-break, and parse them. To keep the current forms like FromDouble(100.0) = "100", rescale the result to exponent 0 (exactly, padding with 0s) when the plain to_chars form has no exponent. Add tests for Doubles at or above 2^53, such as 5.83386073903097e16 and 1.2345678901234568e20. If matching how a Double prints is what's wanted instead, change the docs: they say shortest.

Skeptic: Ran f1.fb (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s11/dec-verify/f1.fb) in debug and --release. Both printed: FromDouble(5.83386073903097e16) = 58338607390309696, == Parse("5.83386073903097e16") is False, and FromDouble(1.2345678901234568e20) = 123456789012345683968. Python's Decimal(repr(x)) gives 5.83386073903097E+16 and 1.2345678901234568E+20. The unaffected cases matched Python: 1e16 -> 1E+16, 12345678901234567.0 -> 12345678901234568, 100.0 -> 100, 1e22 -> 1E+22. Cause: decimal.cpp:137 calls std::to_chars(first, last, value) with no format. For these Doubles the fixed form is no longer than the scientific one, and the standard's smallest-difference tie-break then prints the Double's exact integer value. That is 17 to 21 digits, not the shortest round-trip digits. This contradicts stdlib.md ("as the shortest digits that read back as it"), decimal.fb FromDouble ("The shortest Decimal that reads back as the Double") and big.hpp:84. The resulting Decimal is a different number from the documented one, so it fails == against the shortest digits.

## S11-rational-1 (medium): Complex.Sqrt overflows near the largest Double and breaks down on subnormals

Status: open

Where: std/math/big/complex.fb:70

Sqrt works out `sqrt((fabs(Real) + hypot(Real, Imaginary)) / 2)` with no scaling. Python's cmath.sqrt divides by 8 first for large values and scales subnormals up by 2^53. Without that, the sum overflows to infinity once |z| is above about 9e307, so even the real number 1e308 gets an infinite square root. For tiny inputs the halving underflows: Complex(0, 5e-324) gives a root of 0, and Imaginary / (2 * root) is then infinity. Other subnormal inputs lose about 5 digits, which is far worse than the 12 digits the tests claim. Debug and release print the same.

```
Import std.core
Import std.math.big

Print(Complex(1e308, 0).Sqrt())
Print(Complex(-1e308, 0).Sqrt())
Print(Complex(0, 5e-324).Sqrt())
Print(Complex(-1e-320, 1e-321).Sqrt())
```

Expected: 1e+154+0i
0+1e+154i
1.5717277847026288e-162+1.5717277847026285e-162i
4.983904733313116e-162+1.0012356355133176e-160i
(Python's cmath.sqrt values)

Actual: inf+0i
0+infi
0+infi
4.983938546602109e-162+1.00122884267878e-160i

Suggested fix: Follow CPython's c_sqrt. If both |parts| are below DBL_MIN, set ax = ldexp(|Real|, 53) and s = ldexp(sqrt(ax + hypot(ax, ldexp(|Imaginary|, 53))), -27). Otherwise set ax = |Real| / 8 and s = 2 * sqrt(ax + hypot(ax, |Imaginary| / 8)). Then d = |Imaginary| / (2 * s), and the result is (s, copysign(d, Imaginary)) when Real >= 0, else (d, copysign(s, Imaginary)). Add tests for 1e308, -1e308, 5e-324i, and a subnormal pair.

Skeptic: Ran f1.fb in debug and release. Both print `inf+0i`, `0+infi`, `0+infi`, and `4.983938546602109e-162+1.00122884267878e-160i`. Python 3.12's cmath.sqrt gives 1e+154, 1e+154j, 1.5717277847026288e-162+1.5717277847026285e-162j, and 4.983904733313116e-162+1.0012356355133176e-160j. complex.fb:70 computes `sqrt((fabs(Real) + hypot(Real, Imaginary)) / 2)` with no scaling, so the sum overflows near 9e307 and the halving underflows to 0 on subnormals. Line 71 then divides by 2*0 and gives an infinite part for a tiny input. The file's header says it works these out as Python's cmath does.

## S11-rational-2 (medium): Complex.Exp gives a NaN or infinite part where the result is finite or real

Status: open

Where: std/math/big/complex.fb:50

Exp multiplies exp(Real) by cos and sin of Imaginary. Once exp(Real) overflows (Real > 709.78), two things go wrong. A real input gets a NaN Imaginary part, because inf * sin(0) is NaN. Python's cmath.exp(inf+0j), like C99's cexp, gives inf+0j, and a real z should give exp(x) with Imaginary 0. Inputs whose result fits in a Double also come out infinite: exp(709.9) overflows, but exp(709.9) * cos(pi/4) is about 1.43e308. CPython avoids that by computing exp(Real - 1) * cos * e when Real is above about 708.4.

```
Import std.core
Import std.math.big

Global zero = 0.0

Print(Complex(710, 0).Exp())
Print(Complex(1.0 / zero, 0).Exp())
Print(Complex(709.9, 0.7853981633974483).Exp())
```

Expected: inf+0i
inf+0i
1.4293471013865737e+308+1.4293471013865737e+308i

Actual: inf-nani
inf-nani
inf+infi

Suggested fix: When Imaginary == 0.0, return Complex(exp(Real), Imaginary). When Real > 708.3964185322641, compute l = exp(Real - 1) and return (l * cos(Imaginary) * e, l * sin(Imaginary) * e), as CPython's cmath_exp does. Handle Real = -inf as 0 times the signs of cos and sin. Add tests for 710+0i, inf+0i, and 709.9+pi/4 i.

Skeptic: Ran f2.fb in debug and release. Both print `inf-nani`, `inf-nani`, and `inf+infi`. Python gives (inf+0j) for exp(inf+0j) and 1.4293471013865737e+308+1.4293471013865737e+308j for exp(709.9+pi/4 j). Python's exp(710) raises OverflowError, so inf+0i is the never-panics equivalent. complex.fb:50-51 multiplies exp(Real), which is infinite there, by sin(0), so a real input gets a NaN Imaginary part where Double's exp gives inf. A result that fits in a Double also overflows, because the code has no exp(Real-1)*e step.

## S11-rational-3 (medium): Rational.Parse("1e999999") takes about 21 seconds, since FromDecimal expands the exponent into a million-digit string

Status: open

Where: std/math/big/rational.fb:59

FromDecimal, which Parse uses for any text without a '/', calls value.ToPlainString(). For an 8-character input like "1e999999" or "1e-999999", that is a string of about a million digits. Fraction and DigitsOf then rebuild it with a BigInt multiply-add per 18 digits, which is quadratic. In a release build Parse("1e999999") took 21.4 s, and Parse("1e200000") took about 1.6 s. Python's Fraction('1e999999') takes 0.36 s, and BigInt(10) ** 999999 is near-instant here too. Decimal's exponent limit allows this text, so any program that parses untrusted text with Rational.Parse can be stalled by a short input.

```
Import std.core
Import std.math.big

Local r = Rational.Parse("1e999999") ?? Rational(0)
Print(r.Numerator.BitLength)
```

Expected: 3321925, printed in well under a second (Python's Fraction takes 0.36 s)

Actual: 3321925, printed after 21.4 s in a --release build (about 1.6 s for "1e200000", so the time grows quadratically)

Suggested fix: Build the fraction from the Decimal's coefficient and exponent, not its plain text. For example, split ToString()'s scientific form at 'E' (or expose the coefficient and exponent from the runtime). Then set numerator = BigInt.Parse(digits) * BigInt(10) ** max(exponent, 0) and denominator = BigInt(10) ** max(-exponent, 0). Add a test that parses "1e999999" and "1e-999999".

Skeptic: Built f3.fb (Parse("1e999999")) and f3b.fb ("1e200000") with --release. f3b prints 664386 after 1.22 s. f3 prints 3321925 after 27.3 s. A control program (v3.fb) runs Decimal.Parse("1e999999"), BigInt(10) ** 999999, and ToPlainString (1,000,000 chars) in 0.18 s in total, so the time is spent in rational.fb:59. There FromDecimal sends the plain string to Fraction/DigitsOf, whose `value = value * scale + chunk` loop (lines 221-231) is quadratic in the number of digits. An 8-character input stalls Parse.

## S11-bigint-2 (low): A literal expression that needs 64 bits beside a BigInt doesn't compile, though the same literal does

Status: open

Where: compiler/src/sema.cpp:17452

beside() quietly tries the literals at the hint, which is the default type when nothing else applies, and keeps that type whenever the check returns a type. A constant expression like (1 << 40) or (2_000_000_000 * 2) passes as Int32 there, and its range error is only reported afterwards, so beside() never moves on to Int64. As a result, big * 1_099_511_627_776 and big * 4_000_000_000 compile, but big * (1 << 40) fails with "the shift amount must be from 0 to 31", and big * (2_000_000_000 + 2_000_000_000) fails with "the result of '+' overflows Int32". The same expressions compile beside an Int64 (j * (1 << 40)) and as an Int64 argument (F(1 << 40)). bigint.fb's header promises that an Int64 on either side works, and the sema test's comment says a literal takes the type of the Operator's parameter, but neither holds for these expressions.

```
Import std.core
Import std.math.big

Local big = BigInt(10)
Local j: Int64 = 10
Print(j * (1 << 40), big * 1_099_511_627_776)
Print(big * (1 << 40))
```

Expected: 10995116277760 10995116277760
10995116277760

Actual: sema2 build error: f2.fb:7:14: error: the shift amount must be from 0 to 31 (without the last line, the program prints 10995116277760 10995116277760)

Suggested fix: In beside(), treat a type as holding the literals only when their constant value fits it too. For example, try each candidate with the constant evaluation that reports the overflow and the shift range, and count those errors as failures, so the search goes on to Int64. Alternatively, type the literal side from the matching Operator's parameter. Add a sema test such as probe_money("Local x = m * (1 << 40)") == "Money".

Skeptic: I ran the finding's program, f2.fb. The build failed with "f2.fb:7:14: error: the shift amount must be from 0 to 31". In f2b.fb, big * (2_000_000_000 + 2_000_000_000) failed with "the result of '+' overflows Int32". In f2c.fb, big * 1_099_511_627_776 and big * 4_000_000_000 compiled and printed 10995116277760 and 40000000000. In f2d.fb, the same expressions passed as an Int64 argument, set an Int64 local, and multiplied an Int64, all fine: F(1 << 40) printed 1099511627776. beside() is new in this change (compiler/src/sema.cpp:17448). It returns the hint as soon as quietly(check(literals, hint)) gives a type. The range errors for these expressions only come out later, so it never tries Int64. sema.cpp's is_flexible counts arithmetic made only of literals as a literal, and spec 06-user-types.md:306 says "a literal takes the type of the parameter it's passed for". So rejecting big * (1 << 40) breaks the spec, and the bigint.fb header's promise that an Int64 works on either side. The workaround is easy, so it's low.

## S11-bigint-3 (low): No test checks the Miller-Rabin half of Baillie-PSW: the listed strong Lucas pseudoprimes never reach it

Status: open

Where: tests/programs/bigint_number_theory.fb:130

The test's header says it checks strong pseudoprimes to base 2, Carmichael numbers and strong Lucas pseudoprimes. Every strong Lucas pseudoprime it lists (5459, 5777, 10877, 16109, 18971), every base-2 strong pseudoprime below 10000 (2047, 3277, 4033, 4681, 8321) and every Carmichael number (561, 1105, 1729, 41041, 825265) has a factor of 101 or less. fb_bigint_is_probable_prime's trial division rejects all of them before either probabilistic test runs. Four composites in the test do reach those tests: 2^67-1, 2^257-1, 3215031751 and 1427247692705959880439315947500961989719490561. Checked with an independent strong Lucas-Selfridge test in Python, all four fail the strong Lucas test by itself. So bigint_number_theory.fb would still pass if mp_prime_miller_rabin were never called, and the base-2 half of Baillie-PSW, the half that rejects strong Lucas pseudoprimes, goes unchecked. The code itself is right: 22499, 25199 and 40309 are strong Lucas pseudoprimes with no factor up to 101, and they are correctly reported as not prime.

```
Import std.core
Import std.math.big

// Strong Lucas pseudoprimes: the ones bigint_number_theory.fb lists, then three it could.
For text In ["5459", "5777", "10877", "16109", "18971", "22499", "25199", "40309"]
    Local n = BigInt.Parse(text) ?? BigInt(0)
    Local factor = 0
    For p = 2 To 101
        If factor == 0 && n % p == 0 Then factor = p
    End For
    Print("{n}: factor up to 101 {factor}, prime {n.IsProbablePrime()}")
End For
```

Expected: The test's strong Lucas pseudoprimes would have no factor up to 101 (factor 0), so that Miller-Rabin base 2 is what rejects them.

Actual: 5459: factor up to 101 53, prime False
5777: factor up to 101 53, prime False
10877: factor up to 101 73, prime False
16109: factor up to 101 89, prime False
18971: factor up to 101 61, prime False
22499: factor up to 101 0, prime False
25199: factor up to 101 0, prime False
40309: factor up to 101 0, prime False

Suggested fix: Add strong Lucas pseudoprimes with no factor up to 101 to bigint_number_theory.fb (22499 = 149*151, 25199 = 113*223, 40309 = 173*233, 58519 = 139*421), each expected False. Also add base-2 strong pseudoprimes that pass trial division, such as 42799 = 127*337 and 49141 = 157*313. Then reword the header so it says which stage each group checks.

Skeptic: I ran the finding's program, f3.fb. The test's strong Lucas pseudoprimes 5459, 5777, 10877, 16109 and 18971 have factors 53, 53, 73, 89 and 61, so the trial division by small_primes (2 to 101, runtime/src/bigint.cpp:93) rejects them. 22499, 25199 and 40309 have no factor up to 101 and are correctly reported False. Every composite below 10000 also falls to trial division, since the smallest composite without a small factor is 103^2 = 10609. That means the 1229-primes loop never reaches either probabilistic test with a composite. I checked the composites that do get past trial division and the square check in Python (scratchpad mr2.py and lucas.py). My strong Lucas-Selfridge implementation reproduces OEIS A217255's start: 5459, 5777, 10877, 16109, 18971, 22499, 24569, 25199. 2^67-1, 2^257-1 and 3215031751 pass Miller-Rabin base 2, so Lucas is what rejects them. 1427247692705959880439315947500961989719490561 fails base 2, but it also fails strong Lucas on its own. So bigint_number_theory.fb would still pass with the mp_prime_miller_rabin call (bigint.cpp:401) removed: nothing tests Baillie-PSW's base-2 half. The code itself is right, so this is a test gap, low.

## S11-bigint-4 (low): big.hpp says fb_bigint_mod_inverse takes a modulus above 1, but it handles 1 and is given 1

Status: open

Where: runtime/include/fb/big.hpp:70

The header comment for fb_bigint_mod_inverse describes the modulus as above 1. But the function's first branch deliberately handles a modulus of 1 and returns 0. BigInt.ModInverse's CheckModulus only requires a modulus above 0, so it passes 1 through, and the result matches Python's pow(5, -1, 1) == 0. The comment states a precondition the code and its caller don't keep. The neighbouring fb_bigint_mod_power comment correctly says above 0.

```
Import std.core
Import std.math.big

// big.hpp says fb_bigint_mod_inverse takes a modulus above 1; BigInt.ModInverse passes it 1.
Print(BigInt(5).ModInverse(BigInt(1)) ?? BigInt(-1))
```

Expected: 0, from a function whose comment allows a modulus of 1 (above 0)

Actual: 0, from a function whose header comment says the modulus is above 1

Suggested fix: Change the comment to say the modulus is above 0, matching fb_bigint_mod_power's: "The number from 0 to below `modulus`, above 0, that times `a` is 1 modulo it, or null when there's none."

Skeptic: runtime/include/fb/big.hpp:70-71 says "The number from 0 to below `modulus`, above 1, that times `a` is 1 modulo it". But fb_bigint_mod_inverse (runtime/src/bigint.cpp:338-342) handles a modulus of 1 on purpose and returns 0. BigInt.ModInverse's CheckModulus (bigint.fb:263-264) only requires a modulus above 0, so it passes 1 through. f4.fb, BigInt(5).ModInverse(BigInt(1)), printed 0. The comment states a precondition that neither the code nor its caller keeps, while fb_bigint_mod_power's comment two lines up correctly says above 0. It's only a doc comment, so it's low.

## S11-decimal-2 (low): Round can give a result past exponent 999999 without panicking

Status: open

Where: runtime/src/decimal.cpp:302

fb_decimal_round calls mpd_qrescale under the exact (max) context and only checks the result's digit count. Nothing checks the result's exponent. Rounding up at the top of the range therefore gives a Decimal whose adjusted exponent is past 999999. decimal.fb's class docs and stdlib.md say a result past 999999 panics, and decimal_conversions.fb says Round works as Python's quantize does, which raises InvalidOperation here. The bad value lives on, and the panic surfaces later at whatever operation next touches it, so it points at the wrong line. Zero results with out-of-range exponents (D("0").Round(2000000) is 0E-2000000, D("123").Round(-1000000) is 0E+1000000) also go through where Python's quantize raises.

```
Import std.core
Import std.math.big

Local x = Decimal.Parse("9.5E+999999") ?? Decimal(0)
Local y = x.Round(-999999)
Print(y)
Print(y * 1)
```

Expected: a panic at line 5, where Round makes 10E+999999 (adjusted exponent 1000000), as Python's quantize raises InvalidOperation

Actual: 1.0E+1000000
panic: a Decimal's exponent goes up to 999999, and this result's is past it at f2.fb:7:7

Suggested fix: In fb_decimal_round, after the rescale, return null when the result is nonzero and mpd_adjexp(result) > 999999. Like Python, also reject a target exponent (-places) outside Etiny..Emax (-1000026 to 999999). Then give Round's panic a message for an exponent past 999999 as well as for too many digits. Add a panic program for it.

Skeptic: Ran f2.fb in debug and --release. Both printed 1.0E+1000000 at line 6 and then panicked at f2.fb:7:7 (y * 1) with "a Decimal's exponent goes up to 999999, and this result's is past it". So Round at line 5 made a value whose adjusted exponent is 1000000 without panicking. Python's quantize raises InvalidOperation for the same input. fb_decimal_round (decimal.cpp:284-304) checks only `places` against MPD_MAX_EMAX and the padded digit count, then calls mpd_qrescale under fitting() (maxcontext emax). finish() sees no Overflow, so nothing bounds the result's exponent. This contradicts decimal.fb's header ("A result's exponent stays from -999999 to 999999: past that panics") and stdlib.md ("a result past that panics"). Zero results also escape: Decimal(0).Round(2000000) is 0E-2000000, D("123").Round(-1000000) is 0E+1000000, and Decimal(1).Round(-2000000000) is 0E+2000000000. Those zero values behaved harmlessly afterwards (+1, ==, ToInt64, ToDouble and ToBigInt all gave correct results with no hang). I rated it low rather than medium: only a value rounding up at the very top of the range (within 1 of 1E+1000000) can produce a nonzero out-of-range result, the value itself is the correct rounding, and the only harm is a missing panic that then fires at a later line.

## S11-decimal-3 (low): No test covers % or Round past 28 digits, which the intended rule allows

Status: open

Where: tests/programs/decimal_arithmetic.fb:1

The rule is that % allows a quotient of up to 28 digits, or the operands' own count when that's more, and that Round keeps up to 28 digits, or the value's own. Those allowances are the fitting() context in decimal.cpp, and decimal_arithmetic.fb's header claims them. No test reaches them. A script over every % pair in decimal_arithmetic.fb finds no quotient over 28 digits. panic_decimal_quotient.fb only checks 27 digits passing and 1E+40 % 7 panicking. No Round in decimal_conversions.fb gives a result over 28 digits; its longest case, 1.2345678901234567890123456789012, is only rounded to 3 places. Both paths work today, as the program shows, but a regression to a fixed precision of 28 (Python's behavior, where both of these raise) would go unnoticed.

```
Import std.core
Import std.math.big

Function D(text: String): Decimal => Decimal.Parse(text) ?? Decimal(-424242)

// A quotient of 34 digits, which the dividend's 35 allow.
Print(D("12345678901234567890123456789012345") % 7)
// Rounded results of 30 digits, which the value's 31 allow.
Print(D("123456789012345678901234567890.5").Round(0))
Print(D("123456789012345678901234567891.5").Round(0))
```

Expected: 4
123456789012345678901234567890
123456789012345678901234567892
(and checks like these in decimal_arithmetic.fb and decimal_conversions.fb)

Actual: The program prints the expected values, but no check in tests/programs/decimal_arithmetic.fb or decimal_conversions.fb uses a % quotient or a Round result over 28 digits

Suggested fix: Add these checks (values confirmed with Python at the matching precision) to decimal_arithmetic.fb's % section and decimal_conversions.fb's rounding section. Also add a % case where a long divisor sets the precision, e.g. a 35-digit b.

Skeptic: Ran f3.fb in debug and --release. It printed 4, 123456789012345678901234567890 and 123456789012345678901234567892, which match Python (int % 7 = 4; quantize at prec 31), so the behavior is correct. The gap is only in tests. I scanned all 237 `CheckDecimal(..., a % b, ...)` checks in tests/programs/decimal_arithmetic.fb (the only % uses besides the header) with a Python script that computes each exact integer quotient's digit count. The largest quotient is 28 digits (10%2, 10%5, 10%9, 10%11, 29%28) and none is over 28. decimal_conversions.fb's Round checks all give short results: the longest input, 1.2345678901234567890123456789012, is rounded to at most 3 places. panic_decimal_round.fb tests only Decimal(1).Round(27) passing and Round(28) panicking. panic_decimal_quotient.fb tests only a 27-digit quotient and 1E+40 % 7. So the "or the operands' own" / "or the value's own" allowance in fitting() (decimal.cpp:42-46) is never exercised, though decimal_arithmetic.fb's header says the test covers it. A regression to a fixed precision of 28 would go unnoticed. This is a coverage gap rather than wrong behavior, so low.

## S11-rational-10 (low): docs say std.math.big's numbers never change once made, but Complex's fields can be assigned

Status: open

Where: std/math/big/complex.fb:24

docs/stdlib.md:374 says std.math.big's numbers 'never change once made'. Complex declares `Public Field Real` and `Public Field Imaginary` without ReadOnly, while Rational's fields are ReadOnly. A program can therefore assign z.Real.

```
Import std.core
Import std.math.big

Local z = Complex(1, 2)
z.Real = 5.0
Print(z)
```

Expected: A compile error that Real is ReadOnly, as with a Rational's Numerator

Actual: 5+2i

Suggested fix: Make Real and Imaginary `Public ReadOnly Field`s (keeping Imaginary's default of 0.0), or say in the docs that Complex, as a Struct, is the exception.

Skeptic: f10.fb compiles and prints `5+2i` in debug and release after `z.Real = 5.0`. docs/stdlib.md:374 says std.math.big's numbers "never change once made". complex.fb:24-25 declares `Public Field Real` and `Public Field Imaginary` without ReadOnly, while Rational's are ReadOnly. Structs support ReadOnly fields (std/collections/pool.fb's Handle has `Public ReadOnly Field Index`), so either the code or the docs needs to change.

## S11-rational-4 (low): Rational ** with the lowest Int64 panics, with a different message in debug and in release, where 1 is expected

Status: open

Where: std/math/big/rational.fb:198

For a negative exponent, Operator ** computes `a.Denominator ** -b` and `a.Numerator ** -b`, and -b overflows when b is the lowest Int64. BigInt ** deliberately allows huge exponents for 0, 1, and -1, so Rational(1) ** lowest and Rational(-1) ** lowest should be 1, as Python's Fraction gives. Instead, a debug build panics with an integer overflow in '-'. A release build wraps -b back to a negative number and panics with BigInt's 'needs an exponent of 0 or more'. So the two builds disagree, and both are wrong. Complex's ** handles this same case explicitly; Rational's doesn't.

```
Import std.core
Import std.math.big

Local lowest: Int64 = -9223372036854775807 - 1
Print(Rational(1) ** lowest)
```

Expected: 1

Actual: Debug: panic: integer overflow in '-' at r1.fb:5:7
Release: panic: an integer power needs an exponent of 0 or more at r1.fb:5:7
(Rational(-1) ** lowest behaves the same way.)

Suggested fix: For b < 0, avoid negating b. For example, use `Local count = -(b + 1)` and return Rational(a.Denominator ** count * a.Denominator, a.Numerator ** count * a.Numerator). Add tests for Rational(1) ** lowest and Rational(-1) ** lowest.

Skeptic: Ran f4.fb (Rational(1) ** lowest Int64) and f4b.fb (Rational(-1) ** lowest). Debug panics with `integer overflow in '-' at f4.fb:5:7`. Release panics with `an integer power needs an exponent of 0 or more at f4.fb:5:7`. Python's Fraction gives 1 for both. rational.fb:198 negates b. BigInt's ** (bigint.fb:216-217) deliberately allows huge exponents when BitLength <= 1, so 1 and -1 to any power is meant to work. Complex's ** handles the lowest Int64 explicitly at complex.fb:139. This is low severity because the input is contrived, but std panicking with an integer overflow, and with a different message in debug and release, is real.

## S11-rational-5 (low): Complex.Log overflows for huge inputs and loses accuracy on subnormal ones

Status: open

Where: std/math/big/complex.fb:59

Log takes log(hypot(Real, Imaginary)) with no scaling, outside the log1p band. When hypot overflows (both parts above about 1.27e308), the Real part is inf, though the true log is about 710. When both parts are subnormal, hypot rounds to a few bits and the Real part is off by 0.35. CPython's c_log scales in both cases: log(hypot(ax/2, ay/2)) + ln 2 when ax > DBL_MAX/4, and scaling by 2^53 for subnormals. The file's header says it works these out as Python's cmath does.

```
Import std.core
Import std.math.big

Print(Complex(1.5e308, 1.5e308).Log())
Print(Complex(5e-324, 5e-324).Log())
```

Expected: 709.9482473405542+0.7853981633974483i
-744.0934983311014+0.7853981633974483i
(Python's cmath.log values)

Actual: inf+0.7853981633974483i
-744.4400719213812+0.7853981633974483i

Suggested fix: Mirror CPython's c_log. If larger > DBL_MAX / 4, real = log(hypot(larger / 2, smaller / 2)) + ln 2. Else if larger < DBL_MIN and the input is nonzero, real = log(hypot(ldexp(larger, 53), ldexp(smaller, 53))) - 53 * ln 2. Keep the log1p band as it is. Add tests for both cases.

Skeptic: Ran f5.fb in debug and release. Both print `inf+0.7853981633974483i` and `-744.4400719213812+0.7853981633974483i`. Python's cmath.log gives 709.9482473405542+0.785...j and -744.0934983311014+0.785...j. complex.fb:59-60 takes log(hypot(larger, smaller)) with no scaling, so hypot overflows when both parts are about 1.27e308 or more, and it loses bits on subnormals. It is real but narrow, since a real input like 1.5e308+0i is fine.

## S11-rational-6 (low): Sqrt, FromPolar, and Exp give NaN parts for infinite inputs that have defined values

Status: open

Where: std/math/big/complex.fb:71

The header promises NaN parts only for what has no value. But Python's cmath and C99 Annex G define these: sqrt(inf+inf i) and sqrt(1+inf i) are inf+inf i, rect(inf, 0) is inf+0i, and exp(-inf+inf i) is 0+0i. The code divides inf by inf in Sqrt (Imaginary / (2 * root)) and multiplies inf by 0, or 0 by NaN, in FromPolar and Exp, which gives NaN parts instead.

```
Import std.core
Import std.math.big

Global zero = 0.0

Local inf = 1.0 / zero
Print(Complex(inf, inf).Sqrt())
Print(Complex(1, inf).Sqrt())
Print(Complex.FromPolar(inf, 0))
Print(Complex(-inf, inf).Exp())
```

Expected: inf+infi
inf+infi
inf+0i
0+0i

Actual: inf-nani
inf-nani
inf-nani   (release: inf+nani)
-nan-nani

Suggested fix: Add cmath's special cases. In Sqrt, an infinite Imaginary gives (inf, Imaginary), and an infinite Real gives (inf, copysign(0, Imaginary)) or (0, copysign(inf, Imaginary)). In FromPolar, an angle of 0 gives (magnitude, angle * magnitude sign) as cmath.rect does. In Exp, Real = -inf gives a signed 0+0i for any Imaginary. Add tests.

Skeptic: Ran f6.fb. Debug prints `inf-nani`, `inf-nani`, `inf-nani`, `-nan-nani`. Release prints the same, except FromPolar gives `inf+nani`. Python gives (inf+infj), (inf+infj), (inf+0j), and 0j. The Sqrt result comes from Imaginary / (2*root) with both infinite (complex.fb:71). FromPolar's comes from inf*sin(0) (line 33), and Exp's from 0*cos(inf) (line 51). These values exist under cmath and C99 Annex G, but the header promises NaN parts only for what has no value.

## S11-rational-9 (low): docs/stdlib.md says std.math.big's numbers have Parse, but Complex has none

Status: open

Where: docs/stdlib.md:374

The std.math.big bullet says its numbers come 'with Operators, an Int64 (or a Double, for Complex) on either side of one too, and `Parse` and `ToString`'. Complex is named in that sentence, but complex.fb has only ToString, so Complex.Parse doesn't compile.

```
Import std.core
Import std.math.big

Print(Complex.Parse("1+2i") ?? Complex(0, 0))
```

Expected: 1+2i (or the docs say Parse is for BigInt, Decimal, and Rational only)

Actual: c3.fb:4:15: error: Complex has no member named 'Parse'

Suggested fix: Either add Complex.Parse(text): Complex?, which reads what ToString writes ("1+2i", "0-1.5i", "nan+nani"), with tests, or reword the bullet so Parse applies only to BigInt, Decimal, and Rational.

Skeptic: f9.fb fails to compile: `f9.fb:4:15: error: Complex has no member named 'Parse'`. docs/stdlib.md:374-375 says std.math.big's numbers come with Operators, "an Int64 (or a Double, for Complex) on either side of one too, and `Parse` and `ToString`". Complex is named in that sentence, and complex.fb has only ToString. The docs and the code disagree.

## Refuted by the skeptics (no action)

- S11-decimal-4: Docs say an Int64 works on either side of an operator, but Int64 ** Decimal doesn't compile. Ran f4.fb. It reports `f4.fb:5:7: error: no Operator ** takes (Int64, Decimal)` in debug and release, so the overload is indeed missing. It's a compile error, not wrong behavior, and the docs read fairly as not promising it. Decimal has no (Decimal, Decimal) ** at all. Its only ** is `Operator **(a: Decimal, b: Int64)`, documented as "To a whole power", and Pow(Decimal) covers other exponents. So the header's "An Int64 on either side of an operator works too" means that wherever an operator takes a Decimal, an Int64 can stand in for it. For ** that position is the base, which would make it Int64 ** Int64. An Int64 ** Decimal overload would need a Decimal exponent, which ** never takes. BigInt, Rational and Complex follow the same design (each defines only `Operator **(a: T, b: Int64)`), so this is the lane's consistent design, not a Decimal gap. stdlib.md's wording could be tighter ("** takes an Int64 exponent"), but under that reading it isn't plainly contradicted, so I'm refuting.
- S11-rational-7: Complex.ToString shows a NaN Imaginary part's sign bit, so the same program prints differently in debug and release. I reproduced it: f7.fb prints `inf-nani` in debug and `inf+nani` in release. A plain Double does the same. v7.fb, with Double locals a*b - a*b, zero/zero, and a*b*0.0, prints `-nan` three times in debug and `nan` three times in release. The divergence comes from Double's NaN printing showing the sign bit and from LLVM constant-folding a NaN with a clear sign. That predates this change and isn't Complex's. Complex's ToString follows Double's own convention of showing a NaN's sign, so no Complex-specific bug.
- S11-rational-8: Complex.Pow isn't exact for whole exponents, unlike Python's complex pow and Complex's own **. I reproduced the output: i.Pow(2+0i) gives -1+1.2246467991473532e-16i, and Complex(0,1) ** 2 gives -1+0i. That is rounding at the scale of 1e-16 from the polar method, not a wrong result. complex.fb's header promises the principal value, not exactness. stdlib.md says only that the functions are worked out as Python's cmath does, and cmath has no pow. tests/programs/complex.fb deliberately compares Pow to Python's values within 12 digits (the CheckClose lines, e.g. "2 pow (2+0j)" against -1.0, 0.0), since each libm differs. Pow and ** use different algorithms, so they can differ by an ulp, as pow(x, 2.0) and x*x can. This doesn't contradict the spec or the docs.
