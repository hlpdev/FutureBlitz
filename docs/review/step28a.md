# The review of M6 step 28's first half: IpAddress, SocketAddress, Url and QueryParams per the WHATWG URL Standard, and Dns over libuv (the sockets, IPC, and LAN discovery come later; urltestdata.json's case 732, which needs NFKC, is a known gap until std.text.unicode lands)

9 confirmed findings: 1 high, 2 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S28-dns-1 (high): ResolveAsync's lookup writes into an output array its coroutine may already have freed

Status: fixed in 6f628e6 (fb_io_resolve_async keeps only UInt8[]'s Class, not the coroutine's array, and the I/O thread finishes the Task<UInt8[]> with a new array of the code and the records, so nothing of a coroutine that's gone is touched; a blocking lookup still fills its caller's array, under the lock, while the caller waits; net_dns_dropped, the finding's program, prints 0 bytes, 34 before)

Where: runtime/src/dns.cpp:147

fb_io_resolve_async keeps a raw pointer to `output`, the UInt8[] that ResolveAsync's coroutine frame owns, and the I/O thread later writes the records into it (put_addresses: count, fb_array_reserve, memcpy). The lookup retains only the Task, not the array. The comment says "The caller holds `output` until then", but spec 10 says a coroutine still waiting on a Scheduler is destroyed when that Scheduler goes, and lets go of what it holds. So when a Scheduler is dropped while one of its coroutines awaits Dns.ResolveAsync, the array is freed, and the I/O thread then writes into freed memory. The same happens at program end to a coroutine on a Scheduler that nothing ticks, if its lookup hasn't finished yet. In the program below, the freed array's memory goes to a new empty array, which then holds the 34 bytes of localhost's two records. In release, some runs crash with 0xC0000005 or 0xC0000374 (heap corruption). fb_io_input_async avoids this because it puts its result in the Task, which the request retains.

```
Import std.core
Import std.net

Async Function Look(): Int Throws NetError
    Local found = Try Await Dns.ResolveAsync("localhost")
    Return found.Count
End Function

// The Scheduler goes with the lookup under way, and its coroutines with it, which let go of
// what they hold: ResolveAsync's output array among it.
Function Drop()
    Local paused = Scheduler()
    paused.Run(Look())
End Function

Drop()
Local others: UInt8[][] = []
For i = 1 To 1000
    others.Add([])
End For

// Waits for the lookup to finish.
For i = 1 To 5
    Try? Dns.Resolve("localhost")
End For

Local total = 0
For o In others
    total += o.Count
End For
Print("bytes in 1000 new empty arrays: {total}")
```

Expected: bytes in 1000 new empty arrays: 0

Actual: Debug, every run: "bytes in 1000 new empty arrays: 34" (a new empty array got localhost's two 17-byte records). Release: usually the same line, but some runs crash with "error: t7.exe exited with code 0xC0000005" or "0xC0000374".

Suggested fix: Don't let the I/O thread write into an object the coroutine owns. Keep the records in lookup_t, then hand them over through the Task's result, as fb_io_input_async hands over its String (for example, a Task whose result is a UInt8[] the runtime makes, plus the code). Retaining `output` from the main thread doesn't work on its own: a UInt8[] isn't Shared, so a later release on the I/O thread would race the main thread's non-atomic refcount. Add this program as a test.

Skeptic: I ran the finding's program (C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s28a/dnsv/t1.fb) 3 times in debug and 6 times with --release. Every run printed "bytes in 1000 new empty arrays: 34", so localhost's two 17-byte records landed in an array that had just been created empty. None of my release runs crashed, but the wrong count alone shows the I/O thread writing into freed memory. As a control, t1c.fb is the same program with the Scheduler kept as a Global, so it is never dropped. It prints 0 in both debug and release, which shows the cause is the Scheduler going away. The code agrees: in runtime/src/dns.cpp, lookup_t keeps `fb_array_t* output` as a raw pointer, and fb_io_resolve_async retains only the Task (fb_object_retain(&task->header)), never `output`. on_resolved then calls put_addresses(found, lookup->output), which sets count, calls fb_array_reserve and does a memcpy on the I/O thread. In std/net/dns.fb, `output` is a Local of ResolveAsync's coroutine. Spec 10 says a coroutine still waiting when its Scheduler goes "lets go of what it holds", and s.Run moves the coroutines it waits for along with it, so dropping the Scheduler frees `output` while the lookup is still running. The runtime.hpp comment "The caller holds `output` until then" is false in this case. fb_io_input_async avoids the problem because its result goes in the retained Task.

## S28-query-ip-2 (medium): QueryParams.Sort is a quadratic insertion sort that rebuilds both names' UTF-16 arrays on every comparison

Status: fixed in fea6f6d (Sort merge-sorts (order, pair) entries with the array's Sort(before), each name's Utf16Order worked out once: its code points, with U+E000 to U+FFFF moved above U+10FFFF, which orders them as their UTF-16 units do, with no allocation in a comparison; the program test "QueryParams' Sort is a merge sort, which a query of many pairs doesn't slow", 20000 pairs in reverse within 10 seconds, 1.2 there, past 30 before)

Where: std/net/url.fb:255

Sort moves pairs one place at a time in an insertion sort. Each comparison calls Utf16Before, which builds two new Int[] of UTF-16 units. For n pairs in reverse order that is n^2/2 comparisons, each with two allocations. A query of 4000 pairs is about 40 KB, so a single request URL can hold it, and once the HttpServer exists it is a cheap way to tie up a worker. The rest of std sorts with T[].Sort(before), a stable merge sort (spec 14), which gives the same stable order.

```
Import std.core
Import std.net

Local params = QueryParams()
For i = 0 Until 4000
    params.Add("k{9999 - i}", "v")
End For
params.Sort()
Local first = ""
For pair In params
    first = pair.0
    Break
End For
Print(params.Count, first)
```

Expected: 4000 k6000, within a fraction of a second. The same 4000 pairs and the same Utf16Before comparison through (String, String)[].Sort(before) finish in 0.24 s, release build, run alone.

Actual: 4000 k6000, after 12.6 s (release build, the built exe timed alone)

Suggested fix: Sort with the array's stable merge sort, e.g. Pairs.Sort(Function(a: (String, String), b: (String, String)) => Utf16Before(a.0, b.0)). Better still, work out each name's UTF-16 units once (sort (units, pair) entries, or compare code points directly, ordering astral ones above U+FFFF-range BMP units the way UTF-16 does) so a comparison doesn't allocate. Add a test with many pairs.

Skeptic: I ran the finding's program unchanged (f2.fb in the same folder), built with --release and timed with `time ./f2.exe`. It printed "4000 k6000" after 12.93 s. For comparison, f2b.fb puts the same 4000 reversed pairs in a (String, String)[] and sorts them with pairs.Sort(Function(a, b) => Utf16Before(a.0, b.0)), using a copy of url.fb's Utf16Before and Utf16. Release printed "4000 k6000" in 0.43 s. The code at std/net/url.fb:253-265 is an insertion sort that moves pairs one place at a time. Each comparison calls Utf16Before (url.fb:450), and Utf16 builds two new Int[] every call, so input in reverse order costs about n^2/2 allocating comparisons. The order it gives is correct and stable. The flaw is the quadratic cost on input an attacker controls (a query of about 40 KB takes about 13 s), when std already has a stable merge sort that gives the same order about 30x faster.

## S28-url-2 (medium): An xn-- label skips the checks its Unicode form gets: U+FFFD, noncharacters, surrogates, spaces, and ignored code points all pass

Status: fixed in 2000536 (DomainToAscii's mapping is Mapped, which an xn-- label's decoded code points have to come through unchanged, so U+FFFD, noncharacters, C1 controls, spaces, ignored code points, and what maps to something else fail it; PunycodeDecode gives Null for a surrogate; net_url's first block of xn-- hosts)

Where: std/net/url.fb:1044

DomainToAscii rejects a host typed with U+FFFD, a noncharacter, or a C1 control. It maps a Unicode space to ' ', which then fails, and it drops ignored code points such as U+00AD. The same host written as the xn-- label of its Punycode gets only two checks after decoding: no uppercase ASCII and no C1 controls. So http://\u{FFFD}.com/ fails but http://xn--zn7c.com/, which is U+FFFD, parses. The same holds for U+FDD0 (xn--x-v49h), U+3000 (xn--x-43t), and a soft hyphen (xn--ab-5da, which should be ab.com's label or fail, not stay as it is). PunycodeDecode also lets a surrogate through (xn--x-qc4g decodes to U+D800), because it only checks n against 0x80 and 0x10FFFF. UTS #46 checks a decoded label against the same validity criteria as any other label, so each of these hosts fails in Node's URL, which is what the WHATWG standard says. Checked in debug and release.

```
Import std.core
Import std.net

// Each pair: a host, then the xn-- label its Punycode is. Both should fail.
For text In ["http://\u{FFFD}.com/", "http://xn--zn7c.com/", "http://\u{FDD0}x.com/",
             "http://xn--x-v49h.com/", "http://\u{3000}x.com/", "http://xn--x-43t.com/",
             "http://xn--ab-5da.com/", "http://xn--x-qc4g.com/"]
    Print("{text} -> {Url.Parse(text)?.ToString() ?? "failure"}")
End For
```

Expected: http://\u{FFFD}.com/ -> failure
http://xn--zn7c.com/ -> failure
http://\u{FDD0}x.com/ -> failure
http://xn--x-v49h.com/ -> failure
http://\u{3000}x.com/ -> failure
http://xn--x-43t.com/ -> failure
http://xn--ab-5da.com/ -> failure
http://xn--x-qc4g.com/ -> failure
(each \u{...} printed as its character)

Actual: http://\u{FFFD}.com/ -> failure
http://xn--zn7c.com/ -> http://xn--zn7c.com/
http://\u{FDD0}x.com/ -> failure
http://xn--x-v49h.com/ -> http://xn--x-v49h.com/
http://\u{3000}x.com/ -> failure
http://xn--x-43t.com/ -> http://xn--x-43t.com/
http://xn--ab-5da.com/ -> http://xn--ab-5da.com/
http://xn--x-qc4g.com/ -> http://xn--x-qc4g.com/

Suggested fix: Make PunycodeDecode return Null for a code point from 0xD800 to 0xDFFF. Then run a decoded label through the same mapping and rejection DomainToAscii applies to a label that isn't ASCII, and fail the label when that rejects it or changes it in any way (uppercase, fullwidth, the full stops, spaces, ignored code points, U+FFFD, C1 controls, noncharacters). One helper can do both jobs. Add the xn-- forms to net_url.fb as failures.

Skeptic: I ran f2.fb in debug and release. The Unicode forms fail (`http://\u{FFFD}.com/`, `\u{FDD0}x`, `\u{3000}x`), but `http://xn--zn7c.com/`, `xn--x-v49h`, `xn--x-43t`, `xn--ab-5da`, and `xn--x-qc4g` all parse. f2b.fb shows the same for the other orderings (`xn--x-w49h`, `xn--x-53t`, `xn--x-rc4g`). Python's codec confirms what these decode to: zn7c is U+FFFD, x-v49h is U+FDD0 then x, x-43t is U+3000 then x, ab-5da is a, U+00AD, b, and x-qc4g is U+D800 then x. Node 24 fails every one. In the code, a decoded label is only checked for uppercase ASCII and C1 controls (lines 1044-1047), and PunycodeDecode only checks n against 0x80 and 0x10FFFF (line 1173), so a surrogate gets through. UTS #46 applies its validity criteria to a decoded xn-- label too: each code point must be valid or a deviation. So the U+FFFD, noncharacter, space, and ignored code point rejections that DomainToAscii already applies at lines 1017-1027 can be bypassed by writing the xn-- form. The lane's decision limits which code points are rejected; it doesn't cover skipping those rejections for a decoded label.

## S28-dns-2 (low): A name over 255 bytes throws OTHER ("invalid argument"), not HOST_NOT_FOUND, and Check's failure path is untested

Status: fixed in 441eca1 (the runtime turns libuv's UV_EINVAL for a name it won't look up into UV_EAI_NONAME, so Check throws HOST_NOT_FOUND on any OS, and a long Unicode name that IDNA makes too long too; net_dns's 309-byte name through Resolve and ResolveAsync, which checks a code coming back through the Task as well)

Where: std/net/dns.fb:46

Named() makes HOST_NOT_FOUND the error for names that "getaddrinfo would read another way, or not at all": the empty name, which libuv rejects with UV_EINVAL, and a name with a NUL. A name longer than 255 bytes takes the same path in libuv. uv__idna_toascii can't fit it in its 256-byte hostname_ascii and returns UV_EINVAL, on Windows and Unix alike, before any query. Check() then throws OTHER "looking up ... failed: invalid argument". The docs and Resolve's comment give HOST_NOT_FOUND for a name with no address, and an over-long name has none, just as "" has none. Also, no test in net_dns.fb reaches Check with a non-zero code from the runtime, blocking or Async: both failing cases there are caught by Named before any lookup. A failure code coming back through the Task<Int32>'s result is never checked. An over-long name is a failing lookup that needs no network on any OS.

```
Import std.core
Import std.net

// A name longer than DNS's 255 bytes has no address, as an empty one hasn't, and nothing asks
// the network for it: libuv turns both down before any lookup.
Local long = ""
For i = 1 To 150
    long = long + "a."
End For
long = long + "localhost"
For host In ["", long]
    Try
        Print(Dns.Resolve(host))
    Catch e As NetError
        Print(host.Bytes.Count, e.Kind)
    End Try
End For
```

Expected: 0 HOST_NOT_FOUND
309 HOST_NOT_FOUND

Actual: 0 HOST_NOT_FOUND
309 OTHER

Suggested fix: In Named, throw HOST_NOT_FOUND for a name whose bytes are over 255 (DNS's limit, and libuv's buffer). Or map UV_EINVAL from the lookup to HOST_NOT_FOUND in Check. Then add the over-long name to net_dns.fb, through both Resolve and ResolveAsync, so a code returned by the runtime is checked on both paths.

Skeptic: I ran C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s28a/dnsv/t2.fb (the finding's program, plus an Async case and a 247-byte control). It printed "0 HOST_NOT_FOUND", "309 OTHER" and "async OTHER". The 247-byte control printed "247 HOST_NOT_FOUND". So a name with no address throws HOST_NOT_FOUND at 247 bytes but OTHER at 309 bytes, through both Resolve and ResolveAsync. The cause is in libuv's src/win/getaddrinfo.c (lines 247-276): it converts the name into a `char hostname_ascii[256]` buffer with uv__idna_toascii and returns rc (UV_EINVAL) when the name doesn't fit, before any query. Check() in std/net/dns.fb maps only NO_DATA and NO_NAME to HOST_NOT_FOUND, so this case becomes OTHER "looking up ... failed". That goes against docs/stdlib.md ("a NetError of HOST_NOT_FOUND for a name with none") and Resolve's own comment, and against Named's stated intent of treating names getaddrinfo won't read the same way as the empty name, which also fails with EINVAL in libuv. The test gap is real too: in tests/programs/net_dns.fb, every failing case ("", "a\0b", and ResolveAsync("")) is caught by Named before any lookup, so no test checks a non-zero code coming back from fb_io_resolve or from the Task<Int32>. Low severity, because it only affects which error Kind is thrown in an edge case.

## S28-query-ip-1 (low): The form parser gives one U+FFFD per byte of a truncated UTF-8 sequence, where WHATWG's UTF-8 decode gives one per maximal subpart

Status: fixed in ff5fdf8 (FormDecoded and Url.Decode read their bytes with url.fb's Utf8Decoded, the Encoding Standard's decoder, one U+FFFD for a maximal subpart; fb_string_from_bytes keeps its settled rule; net_query's truncated sequences)

Where: std/net/url.fb:428

FormDecoded, which QueryParams(query) and Url.Params use, turns the percent-decoded bytes into a String with fb_string_from_array. That goes through fb_string_from_bytes, whose rule is one U+FFFD for each byte that doesn't start a well-formed code point. The urlencoded parser in the URL Standard says to 'UTF-8 decode without BOM', which is the Encoding Standard's decoder. That decoder gives a single U+FFFD for a valid lead byte followed by valid continuations that is cut short, either at the end or by a byte that isn't a continuation. Browsers' URLSearchParams, Python's decode(errors='replace') and TextDecoder all do this. So a truncated 3-byte or 4-byte sequence gives 2 or 3 replacement characters where the standard gives 1. Stray bytes, overlong forms, surrogates and %C2 still match, which is why WPT's urlencoded-parser cases pass. The same thing happens in Url.Decode, whose doc says 'read as UTF-8, with U+FFFD for what isn't'. The per-byte rule is settled (decisions.md) for text that comes from C, but std.net claims to follow WHATWG here.

```
Import std.core
Import std.net

// WHATWG's form parser decodes with the Encoding Standard's UTF-8 decoder, which gives one
// U+FFFD for a truncated sequence (a maximal subpart), as browsers' URLSearchParams do.
Local params = QueryParams("a=%F0%9F%98&b=%E2%82x&c=%E2%82%AC")
For pair In params
    Print(pair.0, pair.1.Chars.Count, Url.Encode(pair.1))
End For
Print(Url.Decode("%F0%9F%98").Chars.Count)
```

Expected: a 1 %EF%BF%BD
b 2 %EF%BF%BDx
c 1 %E2%82%AC
1

Actual: a 3 %EF%BF%BD%EF%BF%BD%EF%BF%BD
b 3 %EF%BF%BD%EF%BF%BDx
c 1 %E2%82%AC
3
(the same in debug and release)

Suggested fix: Turn FormDecoded's output, and Url.Decode's, into text with a WHATWG UTF-8 decode: one U+FFFD for a lead byte plus the continuation bytes it validly takes before the sequence breaks, then go on at the byte that broke it (and keep a BOM). Use a small std.net helper or a separate runtime entry point, and leave fb_string_from_bytes's settled per-byte rule alone. Add %F0%9F%98, %E2%82x, and %F0%9F%98=%F0%9F to net_query.fb.

Skeptic: I ran the finding's program, unchanged, at C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s28a/qip/f1.fb. Debug and release both printed "a 3 %EF%BF%BD%EF%BF%BD%EF%BF%BD / b 3 %EF%BF%BD%EF%BF%BDx / c 1 %E2%82%AC / 3". The code matches: FormDecoded (std/net/url.fb:428-433) and Url.Decode (url.fb:168-170) pass the percent-decoded bytes to fb_string_from_array. In runtime/src/string.cpp:202-220 that becomes fb_string_from_bytes, which writes one U+FFFD for each byte where decode() returns a 1-byte replacement, so a truncated F0 9F 98 gives three. The URL Standard's urlencoded parser runs 'UTF-8 decode without BOM' on the names and values. That is the Encoding Standard's decoder, which gives one U+FFFD per maximal subpart, so a should have 1 char and b should have 2 (U+FFFD and x). url.fb's header and stdlib.md's std.net section both say Url and QueryParams follow the WHATWG URL Standard, so the code contradicts the spec it claims. I rated it low, not medium: it only shows up on malformed percent-encoding, and the output still has replacement characters, only too many of them. Hosts aren't affected, because U+FFFD fails a host either way. WPT's urlencoded cases still pass.

## S28-url-1 (low): The Punycode decoder reads {, |, }, ~, and DEL as digits, so it accepts invalid xn-- hosts

Status: fixed in 58a5f7e (a Punycode digit is an ASCII letter or digit, or the label fails; net_url's second block)

Where: std/net/url.fb:1159

PunycodeDecode works out a digit as `c >= 48 && c <= 57 ? c - 22 : Lower(c) - 97` and only checks that the result is 0 to 35. Code points 0x7B to 0x7F ({ | } ~ DEL) come out as 26 to 30, the values of 0 to 4, so a label written with them decodes as if those digits were there. The decoder should reject them. '|' and DEL are caught later by the forbidden-code-point check, but '{', '}', and '~' are not, so the URL parses and keeps the invalid label as its host. RFC 3492 allows only a-z, A-Z, and 0-9 as digits. ICU and Node's URL both fail these hosts, and the WHATWG parser should too. Checked in debug and release.

```
Import std.core
Import std.net

// Punycode's digits are a-z and 0-9; ~ here reads as 3, so the label decodes to münchen.
For text In ["http://xn--mnchen-3ya.example/", "http://xn--mnchen-~ya.example/",
             "http://xn--mnchen-}}ya.example/", "http://xn--mnchen-{{ya.example/"]
    Print("{text} -> {Url.Parse(text)?.ToString() ?? "failure"}")
End For
```

Expected: http://xn--mnchen-3ya.example/ -> http://xn--mnchen-3ya.example/
http://xn--mnchen-~ya.example/ -> failure
http://xn--mnchen-}ya.example/ -> failure
http://xn--mnchen-{ya.example/ -> failure

Actual: http://xn--mnchen-3ya.example/ -> http://xn--mnchen-3ya.example/
http://xn--mnchen-~ya.example/ -> http://xn--mnchen-~ya.example/
http://xn--mnchen-}ya.example/ -> http://xn--mnchen-}ya.example/
http://xn--mnchen-{ya.example/ -> http://xn--mnchen-{ya.example/

Suggested fix: Accept a digit only for an ASCII letter or digit, e.g. `Local digit = IsDigit(c) ? c - 22 : (IsAlpha(c) ? Lower(c) - 97 : -1)`. Add these hosts as failures to net_url.fb.

Skeptic: I ran f1.fb (the finding's program) in debug and release. Both builds printed `http://xn--mnchen-~ya.example/ -> http://xn--mnchen-~ya.example/`, and the same for `}` and `{`, so all three parse. In url.fb line 1159, a digit is `c >= 48 && c <= 57 ? c - 22 : Lower(c) - 97` and line 1160 only checks 0 <= digit < 36, so code points 0x7B to 0x7E read as 26 to 29. RFC 3492's digits are only a-z, A-Z, and 0-9. Python's punycode codec fails `b'mnchen-~ya'` with "Invalid extended code point '~'", and Node 24's `new URL` fails all three hosts. A failed decode is an error under UTS #46, so the WHATWG parser must fail these. The fix works. I rated it low, not medium: the parser accepts a host it should reject but keeps the text exactly as typed, so no host is ever turned into a different one.

## S28-url-3 (low): A label that starts with xn-- but isn't all ASCII is Punycode-encoded again instead of failing

Status: fixed in bdc824c (a label that isn't all ASCII fails when it starts with xn--, once mapped, so XN-- does too; net_url's third block)

Where: std/net/url.fb:1053

In DomainToAscii, only a label that is all ASCII is checked for the xn-- prefix. A label like "xn--ü" goes down the other branch and becomes "xn--" + Punycode("xn--ü"), so http://xn--ü.com/ parses with host xn--xn---3ra.com. UTS #46's processing step 4.1 says a label that starts with "xn--" and has any code point past U+007F is an error. Before that step existed, decoding "ü" as Punycode failed anyway. Node's URL fails it, and the WHATWG parser should too. An uppercase XN-- prefix maps to xn-- and gives the same wrong result. Checked in debug and release.

```
Import std.core
Import std.net

Print(Url.Parse("http://xn--\u{FC}.com/")?.ToString() ?? "failure")
Print(Url.Parse("http://XN--\u{FC}.com/")?.ToString() ?? "failure")
```

Expected: failure
failure

Actual: http://xn--xn---3ra.com/
http://xn--xn---3ra.com/

Suggested fix: In the branch for a label that isn't all ASCII, return Null when the mapped label starts with 120, 110, 45, 45 ("xn--") before encoding it. Add the case to net_url.fb.

Skeptic: I ran f3.fb in debug and release. Both `http://xn--\u{FC}.com/` and `http://XN--\u{FC}.com/` printed `http://xn--xn---3ra.com/`. In DomainToAscii, the xn-- check (lines 1039-1040) runs only inside the all-ASCII branch. A label that isn't all ASCII goes to the else branch (1053-1061) and is Punycode-encoded with its xn-- prefix included; Python agrees that 'xn--ü' encodes to `xn---3ra`. UTS #46 15.1 step 4.1 says a label that starts with xn-- and has a code point past U+007F is an error, and before 15.1 decoding it as Punycode failed anyway. Node 24 fails both URLs. I rated it low: this is a rare constructed input, and the result is a parse that should have been a failure.

## S28-url-4 (low): QueryParams and Url.Params give one U+FFFD per bad byte, not one per maximal subpart as URLSearchParams does

Status: fixed in ff5fdf8 (the same fix as S28-query-ip-1; Url.Decode's comment says how many U+FFFDs it gives)

Where: std/net/url.fb:433

FormDecoded, which QueryParams(query) and Url.Params use, turns percent-decoded bytes into text with fb_string_from_array. That runtime function replaces each byte that doesn't start a well-formed code point with its own U+FFFD. The form-urlencoded parser instead uses the Encoding Standard's "UTF-8 decode without BOM", which replaces each maximal subpart of a bad sequence with a single U+FFFD. So a truncated sequence like %F0%9F%98 should become one U+FFFD and %E2%82x should become U+FFFD then x. Here they become three U+FFFDs and two U+FFFDs then x, and ToString writes the extra %EF%BF%BD back out. Node's URLSearchParams prints 1, 2, 1 code points and a=%EF%BF%BD&b=%EF%BF%BDx&c=%EF%BF%BD. Url.Decode behaves the same way, though its doc only says "with U+FFFD for what isn't". urltestdata.json has no searchParams case with a truncated sequence, so its test run doesn't catch this. Checked in debug and release.

```
Import std.core
Import std.net

Local params = QueryParams("a=%F0%9F%98&b=%E2%82x&c=%C3")
For pair In params
    Print("{pair.0}: {pair.1.Chars.Count} code points")
End For
Print(params)
Print(Url.Parse("http://h/?a=%F0%9F%98")?.Params?.ToString() ?? "failure")
Print(Url.Decode("%F0%9F%98").Chars.Count)
```

Expected: a: 1 code points
b: 2 code points
c: 1 code points
a=%EF%BF%BD&b=%EF%BF%BDx&c=%EF%BF%BD
a=%EF%BF%BD
1

Actual: a: 3 code points
b: 3 code points
c: 1 code points
a=%EF%BF%BD%EF%BF%BD%EF%BF%BD&b=%EF%BF%BD%EF%BF%BDx&c=%EF%BF%BD
a=%EF%BF%BD%EF%BF%BD%EF%BF%BD
3

Suggested fix: Decode percent-decoded bytes with the Encoding Standard's UTF-8 decoder, one U+FFFD per maximal subpart. This can be a small helper in url.fb that FormDecoded and Decode use, or a runtime variant of fb_string_from_bytes (Unicode's own recommended practice is the same). Add a truncated-sequence case to net_query.fb and say how many U+FFFDs in Url.Decode's comment.

Skeptic: I ran f4.fb in debug and release. QueryParams gives 3, 3, and 1 code points and serializes `a=%EF%BF%BD%EF%BF%BD%EF%BF%BD&b=%EF%BF%BD%EF%BF%BDx&c=%EF%BF%BD`, and Url.Params does the same. Node 24's URLSearchParams on the same input gives 1, 2, 1 and `a=%EF%BF%BD&b=%EF%BF%BDx&c=%EF%BF%BD`. FormDecoded (url.fb line 433) uses fb_string_from_array, which runtime/src/string.cpp writes one U+FFFD for each byte that doesn't start a well-formed code point (`decoded.length == 1`). The URL Standard's form-urlencoded parser requires the Encoding Standard's UTF-8 decode without BOM, which writes one U+FFFD per maximal subpart. QueryParams is meant to follow the WHATWG standard as URLSearchParams does, so this is a real difference. For Url.Decode, which isn't a WHATWG API, one U+FFFD per byte matches the language's own convention (decisions.md line 548, spec/12-ffi.md), so I'd count only its doc comment's vagueness as an issue there. Hosts aren't affected, since any U+FFFD fails a host. I rated it low because only malformed input is affected, and only in how many U+FFFDs it gets.

## S28-url-5 (low): The docs say only ASCII letters, fullwidth ASCII, and the ideographic full stops map in a host; the code also drops ignored code points and maps spaces

Status: fixed in 17fb9fe (url.fb's header and stdlib.md's Url entry list the mapping as the code has it, and stdlib.md the other fixes; net_url's soft hyphen and em space show it)

Where: std/net/url.fb:4

url.fb's header says that until std.text.unicode lands "only ASCII's case maps, and fullwidth ASCII and the ideographic full stops". DomainToAscii's comment points back to that, and docs/stdlib.md line 642 says the same. The code also drops U+00AD, U+200B, U+2060, and U+FEFF as ignored code points. It maps the Unicode spaces (U+00A0, U+1680, U+2000 to U+200A, U+202F, U+205F, U+3000) to a space, so those hosts fail. And it rejects U+FFFD, C1 controls, and noncharacters. So the docs describe less than the code does, and "only" is wrong.

```
Import std.core
Import std.net

// A soft hyphen is dropped and an em space maps to a space, which the docs don't say happens.
Print(Url.Parse("http://a\u{AD}b.com/")?.ToString() ?? "failure")
Print(Url.Parse("http://a\u{2003}b.com/")?.ToString() ?? "failure")
```

Expected: Per the docs, which map neither, both hosts become Punycode: http://xn--ab-5da.com/ and a Punycode host for the em space.

Actual: http://ab.com/
failure

Suggested fix: Make the url.fb header, DomainToAscii's comment, and stdlib.md's Url entry say what the code does: ASCII letters, fullwidth ASCII, and the ideographic full stops map; the spaces map to a space, which no host has; soft hyphens and zero widths are left out; U+FFFD, C1 controls, and noncharacters are rejected; everything else becomes Punycode.

Skeptic: I ran f5.fb in debug and release. `http://a\u{AD}b.com/` gives `http://ab.com/` (the soft hyphen is dropped), and `http://a\u{2003}b.com/` fails (the em space maps to a space). url.fb lines 4-6 say "only ASCII's case maps, and fullwidth ASCII and the ideographic full stops", and DomainToAscii's comment (1006-1009) points back to that. stdlib.md lines 641-643 say the same. The code (1017-1027) also maps the Unicode spaces to a space, drops U+00AD, U+200B, U+2060, and U+FEFF, and rejects U+FFFD, C1 controls, and noncharacters, which the lane's own decision lists. The docs say less than the code does, and "only" is wrong. This is a documentation-only fix.
