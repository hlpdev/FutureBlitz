# The review of M6 step 16, std.time: Duration with 2.Seconds (the lexer's and sema's literal-before-a-member rule), Wait and RealWait taking a Duration, Timestamp, Date, TimeOfDay, LocalDateTime, ZonedDateTime, TimeZone over embedded IANA data and the OS's zone, Stopwatch, Measure, Sleep, formats, timers, Deadline, FixedStep, RateLimiter, and Debounce

18 confirmed findings: 0 high, 6 medium, 12 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S16-calendar-3 (medium): VV can't parse a fixed zone's name, which Format writes, because ':' isn't a zone letter

Status: open

Where: std/time/format.fb:473

A fixed TimeZone's Name is OffsetText, like "-08:00" or "+05:30", and Format's VV writes that. ReadPiece's VV only reads IsZoneLetter characters (letters, digits, '/', '_', '-', '+'), so it stops at the colon. It reads "-08", Find gives a -08:00 zone, and ":00" is left over, so the parse fails. As a result, ZonedDateTime.Parse(z.Format(p), p) returns Null for every ZonedDateTime in a fixed zone other than UTC whenever p uses VV. That covers zones from TimeZone.Fixed, from an offset-only ISO or RFC 2822 parse, and from TimeZone.System()'s fixed fallback. TimeZone.Find(zone.Name) itself works. Same in debug and release.

```
Import std.core
Import std.time

Local zoned = Timestamp.FromUnixSeconds(1792758600).InZone(TimeZone.Fixed(-(8.Hours)))
Local pattern = "yyyy-MM-dd HH:mm VV"
Local text = zoned.Format(pattern)
Print(zoned, "|", text, "|", zoned.Zone.Name)
Local back = ZonedDateTime.Parse(text, pattern)
Print(back == Null ? "Null" : "{back}")
Print(TimeZone.Find(zoned.Zone.Name) ?? TimeZone.Utc())
```

Expected: 2026-10-23T04:30:00-08:00 | 2026-10-23 04:30 -08:00 | -08:00
2026-10-23T04:30:00-08:00
-08:00

Actual: 2026-10-23T04:30:00-08:00 | 2026-10-23 04:30 -08:00 | -08:00
Null
-08:00

Suggested fix: In ReadPiece's VV branch, read with ReadOffset(chars, start, False) when the text starts with '+' or '-' and make TimeZone(0, offset), or add ':' to IsZoneLetter (no tz name contains one). Add a fixed-zone VV round trip to time_formats.fb.

Skeptic: I ran p3.fb in debug and release. Both print '2026-10-23T04:30:00-08:00 | 2026-10-23 04:30 -08:00 | -08:00', then 'Null', then '-08:00'. So Format's VV writes the fixed zone's Name '-08:00' (zone.fb:113, OffsetText), and TimeZone.Find reads it back, but ReadPiece's VV branch (format.fb:704-713) only reads IsZoneLetter characters (format.fb:473, no ':'). It stops at '-08', and the ':00' left over makes the parse Null. format.fb's pattern table marks only z as 'which only Format writes', so VV is meant to parse back. Every non-UTC fixed zone fails, including the ones ParseRfc2822 and offset-only ISO parses produce.

## S16-lang-2 (medium): 2.Seconds without Import std.time doesn't say which Import is missing

Status: open

Where: compiler/src/driver.cpp:1023

add_std_names/declared_names records only the Public Methods (function_stmt_t) of std's Extends on built-in types, not their Properties. std.time's units (Nanoseconds to Days) are Properties on Int64 and Float64. So when no file loads std.time, sema's report_hidden can't find a home for 'Seconds'. The object_hint trial then reports "neither Int32 nor Int64 has a member named 'Seconds'" with no hint. A Method gets the hint in the same situation: 5.SaturatingAdd(1) says "'SaturatingAdd' is in std.math, which this file doesn't import". When another file loads std.time, 2.Seconds also gets the right hint. Step 1's done-when says a std name used without its Import should say which Import, and forgetting Import std.time before 2.Seconds will be the most common case.

```
Import std.core

Print(2.Seconds, 1.5.Minutes)
Local n: Int64 = 5
Print(n.Milliseconds)
```

Expected: error: 'Seconds' is in std.time, which this file doesn't import
error: 'Minutes' is in std.time, which this file doesn't import
error: 'Milliseconds' is in std.time, which this file doesn't import

Actual: hint.fb:3:9: error: neither Int32 nor Int64 has a member named 'Seconds'
hint.fb:3:22: error: neither Float32 nor Float64 has a member named 'Minutes'
hint.fb:5:9: error: Int64 has no member named 'Milliseconds'

Suggested fix: In declared_names, also record the Public property_stmt_t members of an Extend on a built-in type, as it does for Methods. Int64's trial would then report the import hint, which doesn't contain "has no member", so object_hint would pick Int64. Add a sema test: "Import std.core\nLocal d = 2.Seconds" should give the std.time hint.

Skeptic: Ran the program (l2.fb) in debug and release. It printed exactly the errors the finding lists: "neither Int32 nor Int64 has a member named 'Seconds'", "neither Float32 nor Float64 has a member named 'Minutes'", and "Int64 has no member named 'Milliseconds'", none of them naming the Import. For comparison, l2b.fb with 5.SaturatingAdd(1) gives "'SaturatingAdd' is in std.math, which this file doesn't import". In compiler/src/driver.cpp, declared_names, for an Extend on a built-in type, collects only function_stmt_t members (around lines 1023-1030), so std.time's unit Properties are never recorded as std names. Step 1's done-when in milestones.md says 'a name from std without its Import is an error that says which Import', and spec 10 says Wait's Duration 'needs Import std.time'.

## S16-lang-3 (medium): Sleep(Duration.MaxValue()) and Sleep(Deadline.Never().Remaining) return at once

Status: open

Where: runtime/src/time.cpp:33

fb_time_sleep computes end = steady_clock::now() + nanoseconds(n). steady_clock counts Int64 nanoseconds from boot, so for n near Int64's maximum the sum overflows, which is undefined behavior and in practice wraps to a negative time. now < end is then false at once, and Sleep returns without sleeping. Sleeping for Deadline.Never().Remaining, which is Duration.MaxValue(), is a natural way to say "forever", and it returns after a few microseconds. Sleep(2000000.Hours) does block, so only Durations near the maximum are affected.

```
Import std.core
Import std.time

Local start = Instant.Now()
Sleep(Deadline.Never().Remaining)
Print("woke after {start.Elapsed}")
```

Expected: Nothing is printed. The thread sleeps for about 292 years. Run under timeout 3, it exits with 124.

Actual: woke after 2.7µs  (Sleep(Duration.MaxValue()) prints "woke after 2.8µs")

Suggested fix: Clamp the end time: if nanoseconds > (steady_clock::time_point::max() - now).count(), set end = steady_clock::time_point::max(), or sleep in bounded chunks. Add a runtime test (tests/runtime/time_tests.cpp) that checks the computed end doesn't wrap for INT64_MAX.

Skeptic: Ran l3.fb (Sleep(Deadline.Never().Remaining)) and l3b.fb (Sleep(Duration.MaxValue())) under timeout 5, in debug and release. All four runs exited with 0 at once and printed 'woke after 4.6µs', '3.2µs', '3µs', and '2.9µs'. In runtime/src/time.cpp, fb_time_sleep computes `end = steady_clock::now() + nanoseconds(nanoseconds)`, which overflows the signed Int64 rep for n near INT64_MAX. That is undefined behavior and in practice wraps negative, so `now < end` is false and Sleep returns. A 'forever' sleep returns immediately.

## S16-pacing-1 (medium): FixedStep.Advance overflows Int when MaxSteps is Int's largest: a debug panic in std, and a negative step count in release

Status: open

Where: std/time/timers.fb:203

The MaxSteps setter accepts any value of 1 or more, including 2147483647, which is a natural way to say "no limit". But Advance computes `((Most + 1) As Int64)`. The `+ 1` happens in Int (Int32) before the widening, so it overflows. In a debug build that is a bare integer-overflow panic at std's line, not the caller's. In a release build it wraps: `most` becomes negative, so `parts` and `Left` go negative, and Advance returns -2147483648 steps. Debug and release give different results.

```
Import std.core
Import std.time

Local step = FixedStep(60)
step.MaxSteps = 2147483647
Print(step.MaxSteps)
Print(step.Advance(1.Seconds))
```

Expected: 2147483647
60

Actual: debug: 2147483647, then "panic: integer overflow in '+' at std/time/timers.fb:203:24"; release (--release): 2147483647, then -2147483648

Suggested fix: Widen before adding: `Local most = ((Most As Int64) + 1).CheckedMultiply(PerStep) ?? 9223372036854775807`. Add this case to time_pacing.fb.

Skeptic: I ran pacing/p1.fb (the finding's program). Debug printed 2147483647, then "panic: integer overflow in '+' at std/time/timers.fb:203:24" (exit 101). Release printed 2147483647, then -2147483648. timers.fb:203 is `((Most + 1) As Int64)`. Most is an Int field, so the +1 happens in Int before the widening. The MaxSteps setter (line 190) only rejects values below 1, so Int's largest is accepted. The panic is in std, and debug and release give different answers.

## S16-pacing-2 (medium): Sleep of a Duration near the longest one returns at once, so Sleep(Deadline.Never().Remaining) doesn't block

Status: open

Where: runtime/src/time.cpp:33

fb_time_sleep computes `steady_clock::now() + nanoseconds(n)`. When n is within the clock's reading (the time since boot) of INT64_MAX, that is signed overflow (undefined behaviour). In practice it wraps to a time in the past, so the loop never runs and Sleep returns at once. A never-passing Deadline's Remaining is Duration.MaxValue() by design, so the natural `Sleep(deadline.Remaining)` returns immediately for Deadline.Never() when it should block forever. Timer.After(Duration.MaxValue()) and Wait(Duration.MaxValue()) both wait correctly; Sleep is the odd one out. Sleep(100000.Days) does block, so only values near the maximum are affected.

```
Import std.core
Import std.time

Local watch = Stopwatch.StartNew()
Sleep(Deadline.Never().Remaining)
Print("returned after", watch.Elapsed < 1.Seconds ? "under a second" : "a second or more")
```

Expected: Blocks indefinitely; nothing is printed (under a 10 s timeout, the process is killed).

Actual: returned after under a second (immediately, in both debug and release)

Suggested fix: Saturate the end time in fb_time_sleep: take `now` once, and if `nanoseconds >= (steady_clock::time_point::max() - now).count()`, use `steady_clock::time_point::max()` as `end`; otherwise use `now + nanoseconds(n)`. Add a runtime test that a near-max end is computed without wrapping, for example by factoring the end computation out.

Skeptic: I ran pacing/p2.fb with `timeout 10`. Debug and release both printed "returned after under a second" and exited 0 at once, though they should have blocked. runtime/src/time.cpp:33 computes `steady_clock::now() + nanoseconds(nanoseconds)`. With Deadline.Never().Remaining == Duration.MaxValue() (timers.fb:115), that is signed int64 overflow (UB). It wraps to a past time, so the sleep loop never runs. This contradicts Sleep's own doc (clock.fb:130): "Blocks the thread for that long".

## S16-pacing-3 (medium): RateLimiter overflows Int64 for a long per or a big burst: a debug panic inside std, a wrong answer in release

Status: open

Where: std/time/timers.fb:256

There are two unchecked Int64 operations. Setup's `Tolerance = Interval * (burst - 1)` overflows when burst/count * per passes about 292 years: RateLimiter(1, 1.Days, 200000) panics in debug with an integer overflow in '*' (no RateLimiter message), and in release wraps to a negative tolerance, so only 1 of a 200000-token burst is let through. TryTake's `Full = ... + Duration.FromNanoseconds(Interval)` overflows for a per near Duration.MaxValue(): RateLimiter(1, Duration.MaxValue()), a reasonable "only once" limiter that the constructor accepts, panics on its first TryTake, at std's own line 256 rather than the caller's. Deadline.After saturates in the same situation ("One too far to count is none"); RateLimiter doesn't.

```
Import std.core
Import std.time

Local now = Instant.FromNanoseconds(1000)
Local burst = RateLimiter(1, 1.Days, 200000)
Local taken = 0
For i = 1 To 5
    If burst.TryTake(now) Then taken += 1
End For

Print(taken)
Local once = RateLimiter(1, Duration.MaxValue())
Print(once.TryTake(now), once.TryTake(now + 1.Seconds))
```

Expected: 5
True False

Actual: debug: "panic: integer overflow in '*' at rate_overflow.fb:5:15". With the burst changed to 1, debug prints 1, then "panic: integer overflow in '+' at std/time/timers.fb:256:16". release: 1, then True False (the burst is wrong).

Suggested fix: Saturate both. Compute Tolerance with `Interval.CheckedMultiply(burst - 1) ?? 9223372036854775807`. In TryTake, use `base.Nanoseconds.CheckedAdd(Interval)` and clamp to Int64's largest Instant, which already reads as "no token for ~292 years". Remaining's `(full - now)` then needs a CheckedSubtract, as Deadline.RemainingAt has. Add both cases to time_pacing.fb.

Skeptic: I ran pacing/p3.fb. Debug gave "panic: integer overflow in '*' at p3.fb:5:15". That is Setup's `Tolerance = Interval * (burst - 1)` (timers.fb:279), shown at the caller's line because of @PanicsAtCaller. Release printed 1, then True False: only 1 of a 200000 burst got through. I also ran pacing/p3b.fb (only RateLimiter(1, Duration.MaxValue())). Debug gave "panic: integer overflow in '+' at std/time/timers.fb:256:16" on the first TryTake, a panic at std's own line. Release printed True False, but only because the wrapped value happens to come out right. The constructor accepts both cases. Neither operation saturates, while Deadline.After does (lines 88-91).

## S16-calendar-1 (low): UnixNanoseconds and UnixMilliseconds panic for values an Int64 holds, in the last second before its minimum

Status: open

Where: std/time/timestamp.fb:101

UnixNanoseconds computes Seconds.CheckedMultiply(1000000000) and then adds Nanos. For a Timestamp before 1970, Seconds is rounded down and Nanos is positive, so the product can go past Int64's minimum even though the sum fits. Timestamp.FromUnixNanoseconds(Int64 min) has Seconds -9223372037 and Nanos 145224192. The product, -9223372037000000000, overflows, and the property panics where it should give the Int64 minimum back. This hits every Timestamp in [-9223372036854775808 ns, -9223372036 s), so FromUnixNanoseconds(x).UnixNanoseconds doesn't round-trip there. UnixMilliseconds at line 92 has the same flaw: Timestamp.FromUnixMilliseconds(-9223372036854775807 - 1).UnixMilliseconds panics with "-292275055-05-16T16:47:04.192Z is past the milliseconds an Int64 holds". Same result in debug and release. time_timestamp.fb and panic_timestamp_nanoseconds.fb only test the upper end.

```
Import std.core
Import std.time

Local low = Timestamp.FromUnixNanoseconds(-9223372036854775807 - 1)
Print(low)
Print(low.UnixNanoseconds)
```

Expected: 1677-09-21T00:12:43.145224192Z
-9223372036854775808

Actual: 1677-09-21T00:12:43.145224192Z
panic: 1677-09-21T00:12:43.145224192Z is past the nanoseconds an Int64 holds at p1.fb:6:7

Suggested fix: When Seconds < 0 and Nanos > 0, move one second into the remainder before multiplying. For nanoseconds: (Seconds + 1).CheckedMultiply(1000000000)?.CheckedAdd(Nanos - 1000000000). For milliseconds: (Seconds + 1).CheckedMultiply(1000)?.CheckedAdd(Nanos / 1000000 - 1000). Add the Int64-minimum round trip for both to time_timestamp.fb.

Skeptic: I ran calendar-refute/p1.fb in debug and release. Both print 1677-09-21T00:12:43.145224192Z and then 'panic: ... is past the nanoseconds an Int64 holds at p1.fb:6:7'. p1b.fb (FromUnixMilliseconds(Int64 min).UnixMilliseconds) panics the same way in both builds with '-292275055-05-16T16:47:04.192Z is past the milliseconds an Int64 holds'. x1.fb shows the problem isn't only at the minimum: FromUnixNanoseconds(-9223372036500000000).UnixNanoseconds also panics. timestamp.fb:92 and :101 compute Seconds.CheckedMultiply(...) before adding Nanos, and for a negative Seconds with a positive Nanos the product overflows even though the sum fits. The doc comment says only values past what an Int64 holds panic, so the code contradicts it. I rate it low because it only hits the last ~0.85 s before 1677-09-21 (and ~0.8 s at year -292 million for milliseconds).

## S16-calendar-2 (low): Timestamp - Timestamp and LocalDateTime - LocalDateTime panic for differences a Duration holds

Status: open

Where: std/time/timestamp.fb:132

Operator -(Timestamp, Timestamp) multiplies the seconds' difference by 1e9 with CheckedMultiply before it adds the nanoseconds' difference. When the seconds' difference is +-9223372037 and the nanoseconds borrow (have the other sign), the product overflows but the true total fits. For example, 9223372036.000000001 s is under Duration.MaxValue (9223372036.854775807 s), yet the operator panics with 'is longer than a Duration holds'. calendar.fb's Between (line 522), behind LocalDateTime - LocalDateTime, has the same flaw. LocalDateTime(Date.FromDays(106752), TimeOfDay.Midnight()) - LocalDateTime(1970, 1, 1, 23, 59, 59) is 106751 days and 1 s (2562024h0m1s), which fits, but it panics with '106752 days is longer than a Duration holds'. ZonedDateTime - ZonedDateTime goes through the Timestamp operator, so it inherits this. Same in debug and release.

```
Import std.core
Import std.time

Local a = Timestamp.FromUnixSeconds(9223372037)
Local b = Timestamp.FromUnixSeconds(0, 999999999)
Print(a - Timestamp.FromUnixSeconds(1), Duration.MaxValue())
Print(a - b)
```

Expected: 2562047h47m16s 2562047h47m16.854775807s
2562047h47m16.000000001s

Actual: 2562047h47m16s 2562047h47m16.854775807s
panic: from 1970-01-01T00:00:00.999999999Z to 2262-04-11T23:47:17Z is longer than a Duration holds at p13.fb:7:7

Suggested fix: When the whole part and the remainder have opposite signs, move one unit across before the checked multiply. In Timestamp's operator, if seconds > 0 and nanos < 0, use (seconds - 1) * 1e9 + (nanos + 1e9), and mirror that for seconds < 0 and nanos > 0. In Between, do the same with days and 86400000000000. Test both cases next to panic_timestamp_between and panic_local_date_time_between.

Skeptic: I ran p2.fb in debug and release. Line 1 is '2562047h47m16s 2562047h47m16.854775807s', then 'panic: from 1970-01-01T00:00:00.999999999Z to 2262-04-11T23:47:17Z is longer than a Duration holds', although 9223372036.000000001 s is under Duration.MaxValue. The mirror, x2.fb's b - a (-9223372036.000000001 s, above MinValue -2562047h47m16.854775808s), panics too. p2b.fb (LocalDateTime(Date.FromDays(106752), midnight) - LocalDateTime(1970,1,1,23,59,59)) panics with '106752 days is longer than a Duration holds' in both builds. timestamp.fb:132 and calendar.fb:522 (Between, called from calendar.fb:475) multiply the whole part before adding the remainder of the opposite sign. The operator's doc says it panics only past the longest Duration. I rate it low because it only hits differences within one second of the ~292-year limit.

## S16-calendar-4 (low): ParseRfc2822 accepts zone offsets of 24 hours or more, making a TimeZone that Fixed and Find refuse

Status: open

Where: std/time/format.fb:335

ReadRfc2822Zone checks only that the minutes are at most 59. Any hours from 00 to 99 pass, so "+2400" through "+9959" are read, and ZonedOf builds TimeZone(0, 359940) for +99:59. That breaks the invariant that TimeZone.Fixed enforces (whole seconds under a day, MAX_OFFSET 86399) and that ReadOffset enforces for ISO and patterns (hours up to 23). The resulting ZonedDateTime's zone name "+99:59" doesn't Find, and its ToString doesn't parse back with ZonedDateTime.Parse. The moment is also shifted by more than four days. Email dates are untrusted input, so this check matters. Same in debug and release.

```
Import std.core
Import std.time

Local parsed = ZonedDateTime.ParseRfc2822("Fri, 23 Oct 2026 14:05:09 +9959")
If parsed == Null
    Print("Null")
Else
    Print(parsed, parsed.Zone, parsed.Offset, parsed.Timestamp)
    Print(TimeZone.Find(parsed.Zone.Name) == Null, ZonedDateTime.Parse("{parsed}") == Null)
End If
```

Expected: Null

Actual: 2026-10-23T14:05:09+99:59 +99:59 99h59m0s 2026-10-19T10:06:09Z
True True

Suggested fix: In ReadRfc2822Zone, return Null when digits.0 / 100 > 23, as ReadOffset does. Add "+2400" and "+9959" to the Null cases in time_formats.fb.

Skeptic: I ran p4.fb in debug and release. Both print '2026-10-23T14:05:09+99:59 +99:59 99h59m0s 2026-10-19T10:06:09Z' and 'True True'. ReadRfc2822Zone (format.fb:335) checks only minutes <= 59, so ZonedOf builds TimeZone(0, 359940). That breaks zone.fb:39-40's documented 'longest offset from UTC a fixed TimeZone has, a second under a day' (MAX_OFFSET 86399), which Fixed enforces and System clamps to. x3.fb shows that ISO's ZonedDateTime.Parse of '+24:00' is Null (ReadOffset caps hours at 23), so the two readers disagree. The bad zone formats as '+99:59' and doesn't Find or parse back. Nothing panicked downstream (Format, + 1.Hours both work), so I rate it low.

## S16-calendar-5 (low): Format's DD writes 3 digits for days of the year past 99, but Parse with DD reads exactly 2

Status: open

Where: std/time/format.fb:730

FormatPiece's 'D' writes Padded(dayOfYear, count), so "DD" gives "296" for Oct 23 (time_formats.out line 13 shows "DD: [296]"). ReadPiece reads a number of two or more letters with exactly count digits, so DD takes "29" and the "6" is left over. Date.Parse(date.Format("yyyy-DD"), "yyyy-DD") is therefore Null for every day from Apr 10 on. Java's DD, which this pattern language follows, parses 2 to 3 digits. The same affects LocalDateTime and ZonedDateTime patterns with DD.

```
Import std.core
Import std.time

Local date = Date(2026, 10, 23)
Local text = date.Format("yyyy-DD")
Print(text)
Local back = Date.Parse(text, "yyyy-DD")
Print(back == Null ? "Null" : "{back}")
```

Expected: 2026-296
2026-10-23

Actual: 2026-296
Null

Suggested fix: For 'D' with count 2 and no adjacent number piece, read 2 to 3 digits (ReadDigits(chars, start, 2, 3)), and say so in the pattern table. Add a DD round trip to time_formats.fb.

Skeptic: I ran p5.fb in debug and release. Both print '2026-296' and then 'Null'. FormatPiece 'D' (format.fb:527-529) writes Padded(296, 2) = '296', and time_formats.out:13 pins 'DD: [296]'. ReadPiece's number branch (format.fb:731) reads exactly count digits for count > 1, so DD takes '29' and the '6' is left over. The pattern table says only z is 'which only Format writes', and the y letter shows the intended 'at least N' parse for multi-letter widths (ReadPiece reads 4 to 10 digits for yyyy). DD's exact-2 read can't read what Format writes for any day from Apr 10 on. D and DDD are fine, so I rate it low.

## S16-calendar-7 (low): Timestamp.Parse rejects valid RFC 3339: a leap second (RFC 3339's own example) and fractions past 9 digits

Status: open

Where: std/time/format.fb:178

Timestamp.Parse is documented as reading what RFC 3339 writes. RFC 3339 allows time-second 60 for a leap second (section 5.8 gives "1990-12-31T23:59:60Z" as an example) and time-secfrac = "." 1*DIGIT. ReadIsoTime returns Null for second > 59, and ReadFraction reads at most 9 digits, leaving the rest unread, so both inputs give Null. This also clashes with ParseRfc2822, which reads "23:59:60" as 23:59:59 ('as most do').

```
Import std.core
Import std.time

For text In ["1990-12-31T23:59:60Z", "1990-12-31T15:59:60-08:00", "2026-10-23T12:30:00.1234567891Z"]
    Local parsed = Timestamp.Parse(text)
    Print("[{text}] -> {parsed == Null ? "Null" : "{parsed}"}")
End For
Local rfc = ZonedDateTime.ParseRfc2822("Mon, 31 Dec 1990 23:59:60 +0000")
Print(rfc == Null ? "Null" : "{rfc}")
```

Expected: [1990-12-31T23:59:60Z] -> 1990-12-31T23:59:59Z
[1990-12-31T15:59:60-08:00] -> 1990-12-31T23:59:59Z
[2026-10-23T12:30:00.1234567891Z] -> 2026-10-23T12:30:00.123456789Z
1990-12-31T23:59:59Z

Actual: [1990-12-31T23:59:60Z] -> Null
[1990-12-31T15:59:60-08:00] -> Null
[2026-10-23T12:30:00.1234567891Z] -> Null
1990-12-31T23:59:59Z

Suggested fix: For the forms with an offset (TIMESTAMP, ZONED), let ReadIsoTime take second 60 and read it as 59, as ParseRfc2822Parts does. In ReadIsoTime, skip any fraction digits past the ninth. Add both cases to time_timestamp.fb.

Skeptic: I ran p7.fb in debug and release. Both print '[1990-12-31T23:59:60Z] -> Null', '[1990-12-31T15:59:60-08:00] -> Null', '[2026-10-23T12:30:00.1234567891Z] -> Null', and then '1990-12-31T23:59:59Z' for the RFC 2822 leap second. Timestamp.Parse's doc (timestamp.fb:64) says it reads 'The Timestamp RFC 3339 writes'. RFC 3339's grammar has time-secfrac = '.' 1*DIGIT and time-second up to 60, and its own section 5.8 examples are these two leap-second strings. ReadIsoTime (format.fb:177) rejects second > 59, and ReadFraction(chars, at + 1, 1, 9) at :169 stops after 9 digits, so a 10th digit fails the offset read. ParseIso's doc states no 9-digit cap. The same module's RFC 2822 reader takes :60 as :59 ('like most'), so the two readers are inconsistent. The 10-digit case alone contradicts the documented contract. I rate it low because both are rare inputs.

## S16-lang-1 (low): A Scheduler's time is a sum of Doubles, so Wait(1.Seconds) isn't done after exactly one second of ticks

Status: open

Where: runtime/src/thread.cpp:665

scheduler_t::tick adds each Tick's seconds to a Double (m_time += seconds). fb_wait_time sets due = now + nanoseconds / 1e9, and a timer fires only when now >= due. Ten Ticks of 0.1 add up to 1.0999999999999999, which is less than 0.1 + 1.0 = 1.1, so a Wait(1.Seconds) that starts during a tick misses its tick. Ticks of 1/30 do the same: 31 of them make 1.0333333333333332, against a due time of 1.0333333333333334. A game ticking a Scheduler at a fixed dt resumes one frame late. The arithmetic is the same as the old fb_wait_seconds, but now that Wait takes a Duration of exact nanoseconds, the miss is easier to see.

```
Import std.core
Import std.time

Global done = False

Async Function Door()
    Await NextFrame()
    Await Wait(1.Seconds)
    done = True
End Function

Local s = Scheduler()
s.Run(Door())
s.Tick(0.1)
For i = 1 To 10
    s.Tick(0.1)
End For
Print("1s after ten more ticks of 0.1: {done}")
s.Tick(0.1)
Print("after one more: {done}")
```

Expected: 1s after ten more ticks of 0.1: True
after one more: True

Actual: 1s after ten more ticks of 0.1: False
after one more: True
(the same in debug and release builds)

Suggested fix: Keep a Scheduler's time and each timer's due time as Int64 nanoseconds. Tick would round its seconds to nanoseconds once per tick (llround(seconds * 1e9)), or take a Duration. fb_wait_time would add the Duration's nanoseconds exactly, and the main scheduler would read steady_clock in nanoseconds. Add a test that ticks 0.1 ten times past a 1-second Wait.

Skeptic: Ran the program (l1.fb) in debug and release. Both print '1s after ten more ticks of 0.1: False' and then 'after one more: True'. In runtime/src/thread.cpp, scheduler_t::tick does `m_time += seconds` on a double (line 665), and fb_wait_time sets due = now + nanoseconds/1e9. So 0.1 + ten Ticks of 0.1 sums to 1.0999999999999999, which is less than the due time of 1.1. Spec 10 says s.Tick(seconds) 'moves its time on by that much', and FixedStep(10).StepSeconds is exactly 0.1, so a Scheduler ticked by a FixedStep resumes a 1-second Wait one tick late. The old fb_wait_seconds already did this. I rate it low: the cost is one tick, and it comes from Double time. The proposed Int64-nanosecond fix is only partial. I simulated it in Python: 0.1 and 1/60 ticks land exactly, but 1/30 and 1/144 ticks still miss by one tick (31 and 145 ticks instead of 30 and 144), because llround(1e9/30) rounds down. A full fix needs a rule for rounding each tick, or Tick taking a Duration.

## S16-lang-4 (low): Scaling or dividing a Duration by an Int8, Int16, UInt8, UInt16, or UInt32 is ambiguous

Status: open

Where: std/time/duration.fb:224

Duration's * and / have overloads for Int32, Int64, and Double. Spec 05 says that when no overload matches exactly and more than one takes the argument by a lossless conversion, the call is ambiguous. Every narrower or unsigned integer converts losslessly to two or three of these overloads, so d * frames with frames: UInt8 is a compile error, as are d / parts with an Int16 and k * d. The comment says a Duration is "Scaled by a whole number, exactly", and these are whole numbers. No test covers anything but Int32 literals and Int64.

```
Import std.core
Import std.time

Local frames: UInt8 = 3
Local parts: Int16 = 4
Local d = 100.Milliseconds
Print(d * frames, d / parts)
```

Expected: 300ms 25ms

Actual: ambig.fb:7:7: error: Operator * is ambiguous for (Duration, UInt8)
ambig.fb:7:19: error: Operator / is ambiguous for (Duration, Int16)
(Int8 and UInt16 give the same error)

Suggested fix: Add exact overloads of * (both sides) and / for Int8, Int16, UInt8, UInt16, and UInt32, each widening to Int64. Or settle in the language that a widening to an integer beats a conversion to a float. Add these types to time_duration.fb.

Skeptic: Ran l4.fb in debug and release. Both give 'Operator * is ambiguous for (Duration, UInt8)' and 'Operator / is ambiguous for (Duration, Int16)'. std/time/duration.fb has * and / overloads for Int32, Int64, and Double. Under spec 05's rule (an exact match beats a lossless conversion; otherwise it's ambiguous), every narrower or unsigned integer converts losslessly to two or three of them. The comment promises 'Scaled by a whole number, exactly'. Other std scalars, such as Vec2i's single Int overload, don't have this problem. I rate it low: it is a compile-time API gap with a clear error and a cast as the workaround, not wrong output.

## S16-lang-5 (low): Scaling by a Double drops nanoseconds past 2^53 ns (about 104 days), and MaxValue() * 1.0 panics

Status: open

Where: std/time/duration.fb:232

Operator *(Duration, Double) and /(Duration, Double) compute Rounded((a.Count As Double) * b). Count As Double already rounds any Count above 2^53 before the multiply, so the result isn't the nearest nanosecond, as the comment promises. (365.Days + 1.Nanoseconds) * 1.0 isn't the Duration itself, and * 2.0 differs from * 2. Duration.MaxValue() * 1.0 rounds Count up to 2^63 and panics with "9223372036854775808 nanoseconds is longer than a Duration holds", even though the exact product fits.

```
Import std.core
Import std.time

Local year = 365.Days + 1.Nanoseconds
Print(year.Nanoseconds, (year * 1.0).Nanoseconds, (year * 2.0).Nanoseconds, (year * 2).Nanoseconds)
Print((year / 1.0).Nanoseconds, (year * 0.5).Nanoseconds)
Print(Duration.MaxValue() * 1.0)
```

Expected: 31536000000000001 31536000000000001 63072000000000002 63072000000000002
31536000000000001 15768000000000000
2562047h47m16.854775807s

Actual: 31536000000000001 31536000000000000 63072000000000000 63072000000000002
31536000000000000 15768000000000000
panic: 9223372036854775808 nanoseconds is longer than a Duration holds at scale.fb:7:7
(the same in debug and release builds)

Suggested fix: Split Count into a Double-exact high part and an Int64 remainder, rest = Count - (high As Int64). Multiply the high part with an exact two-product (p = high * b, e = Fma(high, b, -p)), and add nearbyint(e + rest * b) to p As Int64 using Int64 arithmetic, checked. Alternatively, take an integral b by the Int64 path. Add the 365.Days + 1ns case to time_duration.fb.

Skeptic: Ran l5.fb in debug and release, with identical output: '31536000000000001 31536000000000000 63072000000000000 63072000000000002', then '31536000000000000 15768000000000000', then 'panic: 9223372036854775808 nanoseconds is longer than a Duration holds at l5.fb:7:7'. So (365.Days + 1ns) * 1.0 isn't itself, * 2.0 differs from * 2, and MaxValue() * 1.0 panics although the exact product fits. Operator *(Duration, Double) at duration.fb:232 computes Rounded((a.Count As Double) * b), which rounds Count above 2^53 before the multiply. That contradicts the comment at 221, 'by a Double, rounded to the nearest nanosecond'. I rate it low: it only affects Durations over about 104 days, by up to a few hundred ns, plus the MaxValue edge.

## S16-lang-6 (low): Scaling or dividing by NaN panics with "nan nanoseconds is longer than a Duration holds"

Status: open

Where: std/time/duration.fb:298

Rounded() reports every value that isn't finite or in range as too long, NaN included. So d * NaN and d / NaN panic with "-nan nanoseconds is longer than a Duration holds" in debug builds and "nan nanoseconds ..." in release builds. Fraction(), the units' helper, checks NaN and infinity first and says "a Duration is a number of seconds, not nan". panic_duration_scale_double.fb's comment says it covers a Double "that isn't a number", but it only tests an infinity, so no test covers NaN.

```
Import std.core
Import std.time

Local zero: Double = 0.0
Print(1.Hours * (zero / zero))
```

Expected: A panic that says the scale isn't a number, for example: panic: a Duration can't be scaled by nan at nan.fb:5:7

Actual: debug: panic: -nan nanoseconds is longer than a Duration holds at nan.fb:5:7
release: panic: nan nanoseconds is longer than a Duration holds at nan.fb:5:7

Suggested fix: In Rounded (or in the Double * and / operators), check for NaN first and panic with a message that names it. Add a NaN case to panic_duration_scale_double.fb or a new panic test, or fix that test's comment.

Skeptic: Ran l6.fb (1.Hours * (zero/zero)). Debug gives 'panic: -nan nanoseconds is longer than a Duration holds at l6.fb:5:7', and release gives 'panic: nan nanoseconds is longer than a Duration holds'. Rounded() at duration.fb:296-303 puts NaN into the 'longer than a Duration holds' message, and the message's text differs between builds. Fraction() checks for NaN first and says 'not nan'. tests/programs/panic_duration_scale_double.fb's comment says it covers a Double 'that isn't a number', but it only tests 1.0/zero, an infinity, and its .out shows 'inf nanoseconds...'. No test covers NaN.

## S16-lang-8 (low): decisions.md and the runtime's comments still describe Seconds(n) and RealSeconds(n)

Status: open

Where: docs/decisions.md:612

Wait and RealWait replace the Seconds and RealSeconds built-ins, which no longer exist. The change doesn't touch docs/decisions.md. Its row 612 says "no clash with the built-in `Seconds(n)`", row 332 says "NextFrame, Seconds, and RealSeconds give a Task...", and row 458 says "a program's own Panic or Seconds". No row records the Wait/RealWait decision. runtime/src/thread.cpp:402 still says "the Tasks that NextFrame and Seconds gave", and :595 says "0 for Seconds".

```
Import std.core
Import std.time

Local a = Seconds(2)
Local b = RealSeconds(2)
```

Expected: The docs say these built-ins are gone and were replaced by Wait(2.Seconds) and RealWait(2.Seconds). (The compiler errors below are correct.)

Actual: stale.fb:4:11: error: 'Seconds' isn't declared
stale.fb:5:11: error: 'RealSeconds' isn't declared
In the docs, decisions.md rows 332, 458, and 612 and thread.cpp lines 402 and 595 still name Seconds and RealSeconds.

Suggested fix: Update decisions.md rows 332, 458, and 612 to Wait and RealWait, and add a row for the decision: Wait(time: Duration) on the scheduler's time and RealWait on the real clock replace Seconds(n) and RealSeconds(n). Change thread.cpp's comments at 402 and 595 to say Wait.

Skeptic: Ran l8.fb. 'Seconds' isn't declared and 'RealSeconds' isn't declared, so the built-ins are gone. `git diff --stat b71579a d52d987 -- docs/` shows only spec 01, 02, and 10 and stdlib.md changed. docs/decisions.md row 332 still says 'NextFrame, Seconds, and RealSeconds give a Task...', row 612 still gives 'no clash with the built-in `Seconds(n)`' as its reason, and row 458 says 'a program's own Panic or Seconds'. No row records Wait and RealWait replacing them. runtime/src/thread.cpp:402 still says 'the Tasks that NextFrame and Seconds gave', and :595 says '0 for Seconds'. The history.md and the M4 milestones row are historical and fine.

## S16-pacing-4 (low): FixedStep drops the leftover part-step (Alpha snaps to 0) once a frame passes MaxSteps + 1 steps, though the doc says only whole steps are dropped

Status: open

Where: std/time/timers.fb:206

MaxSteps' comment says that past MaxSteps "the whole steps past them are dropped". Advance instead clamps the accumulated time to (MaxSteps + 1) whole steps, which also throws away the fraction. So a frame of 8.5 steps (MaxSteps 8) leaves Alpha 0.5, but a frame of 9.5 or 100.5 steps leaves Alpha 0. Interpolation after a hitch therefore depends on how long the hitch was. time_pacing.out line 20 encodes this: "... 8 0" after Advance(10.Seconds) with 0.02 steps already pending, where the doc's rule gives 0.02.

```
Import std.core
Import std.time

Local a = FixedStep(10.Milliseconds)
Print(a.Advance(85.Milliseconds), a.Alpha)
Local b = FixedStep(10.Milliseconds)
Print(b.Advance(95.Milliseconds), b.Alpha)
Local c = FixedStep(10.Milliseconds)
Print(c.Advance(1005.Milliseconds), c.Alpha)
```

Expected: 8 0.5
8 0.5
8 0.5

Actual: 8 0.5
8 0
8 0

Suggested fix: Keep the remainder when clamping. Compute the fraction separately, `(Left % PerStep + parts % PerStep) % PerStep` (no overflow), cap the whole steps at MaxSteps, and set Left to that fraction. Alternatively, change the comment to say a long frame also drops the part of a step left over. Update time_pacing.out's line 20 to match.

Skeptic: I ran pacing/p4.fb. Debug and release both printed "8 0.5 / 8 0 / 8 0". Advance (timers.fb:203-208) clamps Left to (Most+1)*PerStep. For 95ms with 10ms steps, Left becomes 9e7: steps = 9, Left % PerStep = 0, return 8. The half step is lost. MaxSteps' doc (line 185) says only "the whole steps past them are dropped". The comment at line 202 says the clamp drops only time that is "dropped anyway". Without the clamp, the fraction would be kept, so the overflow guard changes the result. Alpha jumps from 0.9 at 89ms to 0 at 90ms and above. time_pacing.out line 20 ("... 8 0") encodes the behaviour that contradicts the doc.

## S16-pacing-5 (low): Timer's comment promises it fires at most 2 ms late, and calls its clock the real clock; on Windows it is ~10 ms late, and the clock is monotonic

Status: open

Where: std/time/timers.fb:15

The comment on Timer says "Ticks of the real clock ... which counts whole milliseconds, so a timer waits up to two more than its time, and never less." Two parts of this are wrong.

- **The 2 ms bound:** on Windows, libuv's loop waits through GetQueuedCompletionStatusEx in ms, at the system timer's ~15.6 ms granularity, and the runtime never raises it with timeBeginPeriod. Timer.After(5.Milliseconds) measured about 15.5 ms on this machine (mean 10.6 ms late, worst 11.6 ms over 40 tries). Sleep is just as coarse there. Results depend on the machine, since other processes can raise the timer resolution.
- **"the real clock":** elsewhere in std.time (timestamp.fb:28, clock.fb:2-3) that means the wall clock, which the OS can set back or on. A Timer's ticks run on libuv's monotonic time, and UntilNext uses Instant, so they don't follow wall-clock changes.

```
Import std.core
Import std.time

Local worst = Duration.Zero()
Local total = Duration.Zero()
For i = 1 To 40
    Local begun = Instant.Now()
    Await Timer.After(5.Milliseconds)
    Local late = begun.Elapsed - 5.Milliseconds
    total = total + late
    If late > worst Then worst = late
End For

Print("worst", worst, "mean", Duration.FromNanoseconds(total.Nanoseconds / 40))
```

Expected: worst under 2ms, per the comment (e.g. "worst 1.9ms mean 1.2ms")

Actual: worst 11.5525ms mean 10.578495ms (Windows 11, debug and release alike)

Suggested fix: Either raise the timer resolution on Windows while timers or sleeps are pending (timeBeginPeriod(1) and timeEndPeriod), or reword the comment: the timer never fires early, and fires as soon after its time as the OS's timer allows (about 16 ms on Windows by default). Say "the monotonic clock" instead of "the real clock".

Skeptic: I ran pacing/p5.fb four times on Windows 11. Debug gave worst 11.98ms, mean 10.46ms, then worst 20.71ms, mean 10.83ms. Release gave worst 11.44ms, mean 10.50ms, then worst 17.33ms, mean 10.74ms. That is far over the "waits up to two more than its time" that timers.fb:15-16 promises. io.cpp:400-405 only adds up to 2 ms of rounding, and nothing calls timeBeginPeriod (grep finds none), so the OS's ~15.6 ms tick sets the delay. Only the 2 ms bound is a defect. Spec 10 (lines 182-183 and 197) itself uses "the real clock" to mean real elapsed time as opposed to the scheduler's time. Timer's wording matches that, so that part is not a bug.

## Refuted by the skeptics (no action)

- S16-lang-7: Duration's +, -, *, and / wrap silently in release builds, while its units, Abs, and Timestamp's difference panic in every build. Ran l7.fb. Debug panics with 'integer overflow in '*'', and release wraps (-2124095h34m33.709551616s, MinValue twice, then Abs panics), as reported. That release behavior is the language's rule, not a Duration bug. Spec 02's 'Integer overflow' section says debug builds trap and release builds wrap, and `fb help` says '--release ... let integer overflow wrap instead of panicking'. I checked plain Int64 with l7b.fb: in release, n + 1 at MaxValue, -MinValue, and Abs(MinValue) all wrap to -9223372036854775808, and debug panics. Duration's operator comments (duration.fb:213 and 221-222) say they panic 'as an Int64's overflow does', which means exactly this. The units and Timestamp's difference are range-checked conversions, like Date's construction, not Int64 arithmetic, and Abs's own comment says MinValue panics. So the behavior matches the spec and the comments.
- S16-calendar-6: Pattern offsets drop an offset's seconds, so Format then Parse with the same pattern is Null for local-mean-time zones. I ran p6.fb in debug and release, and the reported output reproduces: Format gives '1850-06-01 12:00:00 -04:56 America/New_York' and Parse gives Null. Both halves are documented behavior, though. format.fb's pattern table (lines 12-14) says X/XX/XXX write '+02', '+0200', '+02:00' (hours and minutes only, as Java's XXX does), and ZonedDateTime.Parse's doc (zone.fb, and ZonedOf at :404) says 'with one [a zone], the offset has to be the zone's then'. The text '-04:56 America/New_York' really isn't New York's offset at that moment (-04:56:02), so refusing it is the documented outcome. Unlike S16-calendar-5, here the formatter writes lossy text by documented design, and the parser correctly refuses inconsistent input. ISO ToString/Parse keep the seconds. Writing seconds in patterns would be a feature addition rather than a fix. It only affects pre-standard-time LMT dates.
