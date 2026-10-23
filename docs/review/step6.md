# The review of M6 step 6, Text I: the built-in Char and 'a' literals, s.Chars, String's Methods by byte position, StringBuilder, Scanner, std.text.unicode over utf8proc, and std.text.encoding. A known gap, the String cap's panic giving no place for + and interpolation, is already open; don't report it

23 confirmed findings: 2 high, 4 medium, 17 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S6-string-1 (high): PadLeft, PadRight, and Center fill with U+0000 by default, not a space: an Extend Method's defaults are never used

Status: open

Where: std/text/string.fb:255

`fill: Char = ' '` is ignored. Each call without a fill pads with NUL bytes. The cause is in the compiler: sema's check_defaults (compiler/src/sema.cpp:12713) goes into a type_stmt_t's members and into function_stmt_t, but never into an extend_stmt_t's members. So an Extend Method's defaults are never checked, and symbol(parameter).value is never set. Every call that leaves one out gets the type's zero: Int 7 becomes 0, "hi" becomes "", True becomes False, 2.5 becomes 0. A mistyped default, `Method Wrong(n: Int = "not an Int")` in an Extend, compiles with no error and gives 0. The bug was already there at f9634d9, but this step is the first code that uses Extend defaults. Find's `from: Int = 0` only works because its default is the zero. The test baked the bug in: in tests/programs/text_strings.out, the line for `"é".PadLeft(3)` holds `[\x00\x00é]`. That's why git shows the file as binary. In a terminal it looks like `[  é]`. Same result in debug and release.

```
Import std.core
Import std.text

// PadLeft, PadRight, and Center fill with a space, U+0020, unless told otherwise.
Print("x".PadLeft(3).Bytes, "x".PadRight(2).Bytes, "x".Center(3).Bytes)

// The cause: an Extend Method's default is never used, so a call without it gets the zero.
Extend String
    Method Seven(n: Int = 7): Int => n
End Extend

Print("x".Seven())
```

Expected: [32, 32, 120] [120, 32] [32, 120, 32]
7

Actual: [0, 0, 120] [120, 0] [0, 120, 0]
0

Suggested fix: In sema's check_defaults, recurse into an extend_stmt_t's members the way it does for a type_stmt_t's, so their defaults are checked and recorded. Add tests: an Extend Method with non-zero Int, String, Bool, Char, and Double defaults called without them, and a sema test that a mistyped default in an Extend is an error. Regenerate text_strings.out so "é".PadLeft(3) is two spaces then é, and add a PadRight and a Center that use the default fill.

Skeptic: Ran strref/s1.fb in debug and --release. Both printed `[0, 0, 120] [120, 0] [0, 120, 0]` then `0`, so the default fill ' ' and `n: Int = 7` are both ignored. s1b.fb, an Extend with `Method Wrong(n: Int = "not an Int")` and `Name(s: String = "hi")`, compiled with no error and printed `0` and `[]`. The code agrees: sema.cpp:12713 check_defaults goes into a type_stmt_t's members and handles function_stmt_t, but never goes into an extend_stmt_t, so symbol(parameter).value is never set for an Extend Method's parameter. od -c of tests/programs/text_strings.out shows `[ \0 \0 303 251 ]` for "é".PadLeft(3), so the test bakes the NUL fill in.

## S6-unicode-1 (high): Encode, Decode, and DecodeAny can't be called off the main thread because Windows1252 is a mutable Global

Status: open

Where: std/text/encoding.fb:26

The Windows-1252 table is `Global Windows1252: Int[]`. Spec 10 makes mutable Globals main-thread-only. Decode reads the table directly (line 115) and Encode reads it through ByteOf (line 511), so the compiler rejects any Spawn, Thread.Start, Parallel For, or Callback that calls Encode, Decode, or DecodeAny. That holds for every encoding, UTF-8 and UTF-16 included, because the check is static. The milestone row says text is what io, data, and net read and write, so decoding in a worker is normal use. Encode fails the same way ("it calls 'Encode', which calls 'ByteOf', which uses the Global 'Windows1252'"). Base64, Hex, Percent, Html, Escape, and std.text.unicode all work in a Spawn. A `Const Table: Int[]` is no fix either: the compiler says it "holds Int32[], which isn't Sendable".

```
Import std.core
Import std.collections
Import std.text
Import std.text.encoding

// Decoding UTF-16 off the main thread.
Local task = Spawn(Function() => Decode([0x68, 0x00, 0x69, 0x00], TextEncoding.UTF16_LE) ?? "-")
Print(task.Wait())
```

Expected: hi

Actual: t11.fb:7:14: error: this work runs on another thread, but it calls 'Decode', which uses the Global 'Windows1252', which only the main thread can, unless it's an Atomic or a Mutex (note at std/text/encoding.fb:115:61). The program doesn't build.

Suggested fix: Replace the Global with a Function that maps 0x80..0x9F through a Select, like `Function Windows1252Code(byte: Int): Int`. Decode calls it; ByteOf searches 0x80..0x9F with it or uses a Select over the 27 code points. Add a test that runs Encode, Decode, and DecodeAny inside a Spawn.

Skeptic: I ran t1.fb, which calls Decode(..., UTF16_LE) in a Spawn. It doesn't build: "t1.fb:7:14: error: this work runs on another thread, but it calls 'Decode', which uses the Global 'Windows1252' ... note: std/text/encoding.fb:115:61". Encode("hi", TextEncoding.UTF8) in a Spawn is refused the same way, through ByteOf at encoding.fb:511:21. Base64.Encode plus Html.Escape in a Spawn builds and prints aGk=&lt;. The check is static, so every encoding, UTF-8 included, is main-thread-only. std/text/encoding.fb:26 is the only Global in all of std. The table never changes, and decisions.md:80 says a Global that never changes is a Const. Anything later in io, data, or net that decodes through this would also be main-thread-only.

## S6-char-1 (medium): A Char works as a fixed array's size, as if it were a number

Status: open

Where: compiler/src/sema.cpp:12086

The fixed-array size check never looks at the size expression's type. It reads the folded value with integer_value(), which takes any std::uint64_t data, and a Char's constant is stored that way. So `Int['a']` is an array of 97 Ints and `Int[NEWLINE]` with `Const NEWLINE = '\n'` has 10, in debug and release. That breaks this step's own rule that a character literal is a Char and never a number, and the error text ('must be an integer'). Indexes, slice bounds, shift amounts, Steps, and Const type arguments all reject a Char ('an index must be an integer, not Char', 'N takes Int32, not Char'), so only sizes miss it. An Enum value gets through the same hole: `Local a: Int[E.B]` with B = 5 builds an array of 5.

```
Import std.core

// A Char isn't a number, so it can't be a fixed array's size.
Const NEWLINE = '\n'
Local lines: Int[NEWLINE]
Local grid: Byte['a', '\u{2}']
Print(lines.Count, grid.Count)
```

Expected: Compile errors at NEWLINE, 'a', and '\u{2}': a fixed array's size must be an integer known at compile time, like 64 or 8 * 8 (or the same with 'not Char').

Actual: It builds and prints "10 194" (debug and --release).

Suggested fix: In the size loop (sema.cpp around 12077-12092), after the INVALID check, reject a type that isn't an integer: `if (!is_integer(type)) { error(span_of(expression), "a fixed array's size must be an integer known at compile time, like 64 or 8 * 8, not {}", shown_name(type)); return type_id_t::INVALID; }`. Or make integer_value() give nullopt unless is_integer(value->type). Add a sema test for a Char literal, a Char Const, and an Enum value as a size.

Skeptic: I ran the finding's program (final/char-refute/c1.fb) and it printed "10 194" in both debug and --release. `Local b: Int[True]` is rejected with "a fixed array's size must be an integer known at compile time", and `xs['a']` is rejected with "an index must be an integer, not Char". So only the size check lets a Char through. The cause: sema.cpp 12063-12092 calls check(expression, INT64) and keeps the type, but it only tests for INVALID. It then calls integer_value() (12123), which accepts any const_value_t whose data is int64_t or uint64_t, and a folded Char has that data. That breaks spec 02 (a Char is not Numeric, and a character literal is never a number) and contradicts the error's own wording. The Enum part also held: c1b.fb with `Int[E.B]`, B = 5, built and printed 5 in debug and release. That hole predates this step, but the same fix closes it.

## S6-unicode-2 (medium): Html.Unescape panics on integer overflow for an 8-digit hex entity in debug, but not in release

Status: open

Where: std/text/encoding.fb:479

EntityValue accepts up to 8 digits (line 474) and adds them up in a 32-bit Int: `code = code * (hex ? 16 : 10) + digit`. Any hex reference from &#x80000000; to &#xFFFFFFFF; overflows on the last digit. A debug build panics, so a page with such an entity crashes the program. A release build wraps to a negative number, which Char.From rejects, and prints U+FFFD. The doc says a number that isn't a Unicode scalar value stands for U+FFFD, so the input is expected, and debug and release give different results. The same 8-digit cap also leaves &#x0000000041; as it is (10 digits) instead of "A".

```
Import std.core
Import std.text
Import std.text.encoding

Print(Html.Unescape("&#x80000000;"))
```

Expected: � (U+FFFD) in both debug and release

Actual: debug: panic: integer overflow in '*' at std/text/encoding.fb:479:16 (exit 101); release: �

Suggested fix: Stop adding up once code passes 0x10FFFF, for example `If code > 0x10FFFF Then Return "\u{FFFD}"` inside the loop, or keep a capped flag. Skip leading zeros instead of capping the digit count. Add &#x80000000; and &#xFFFFFFFF; to text_encoding.fb.

Skeptic: I ran Html.Unescape("&#x80000000;"). Debug panics with "panic: integer overflow in '*' at std/text/encoding.fb:479:16" and exits 101. Release prints U+FFFD for &#x80000000; and &#xFFFFFFFF;. Line 474 allows 8 digits and line 479 adds them up in a 32-bit Int, so any 8-digit hex past 0x7FFFFFFF overflows. A decimal entity can't overflow: 8 digits is at most 99999999. The digit cap also leaves &#x0000000041; and &#99999999999; as they are. The doc at 453-454 says a number that isn't a scalar value stands for U+FFFD. Panicking on input data breaks stdlib.md's convention that only a bug panics.

## S6-unicode-4 (medium): DisplayWidth gives 1 for emoji presentation sequences (a character plus U+FE0F), which terminals draw 2 wide

Status: open

Where: runtime/src/unicode.cpp:275

fb_unicode_display_width gives each cluster its widest character's utf8proc_charwidth. Many common emoji are text-default characters followed by VARIATION SELECTOR-16: ❤️, ⚠️, ✔️, keycaps like 1️⃣, and ZWJ sequences made only of such characters like 👁️‍🗨️. utf8proc gives each part width 1 and FE0F width 0, so the whole cluster comes out 1. UTS #51 gives these sequences emoji presentation, and terminals draw them 2 columns wide, as Windows Terminal, kitty, and wcwidth's wcswidth do. The doc comment says "an emoji made of several is 2", and this breaks it. Column alignment is wrong for text with these common emoji.

```
Import std.core
Import std.text
Import std.text.unicode

// Emoji presentation sequences: a character and VARIATION SELECTOR-16, which asks for its emoji form.
For text In ["\u{2764}\u{FE0F}", "\u{26A0}\u{FE0F}", "1\u{FE0F}\u{20E3}", "\u{1F441}\u{FE0F}\u{200D}\u{1F5E8}\u{FE0F}"]
    Print(DisplayWidth(text), text.GraphemeCount)
End For
```

Expected: 2 1
2 1
2 1
2 1

Actual: 1 1
1 1
1 1
1 1

Suggested fix: In fb_unicode_display_width, count a cluster that contains U+FE0F as 2 columns. Ideally limit this to clusters whose base is Emoji or Extended_Pictographic, or a keycap base. Add these cases to text_unicode.fb.

Skeptic: Debug and release both print "1 1" four times, for the heart, the warning sign, the keycap, and the eye-in-speech-bubble ZWJ sequence. fb_unicode_display_width (runtime/src/unicode.cpp:258-279) takes the cluster's largest utf8proc_charwidth, and that is 1 for each base and 0 for FE0F. The DisplayWidth(String) doc (std/text/unicode.fb:59-60) says "an emoji made of several is 2", and a ZWJ emoji coming out 1 contradicts that. UAX #11 recommends treating emoji presentation sequences as Wide, and Windows Terminal draws them 2 columns wide.

## S6-unicode-5 (medium): Html.Unescape takes quadratic time when many '&' come before a ';'

Status: open

Where: std/text/encoding.fb:341

For every '&', Unescape calls `text.Find(";", found)`, which scans to the next ';' however far away, then slices everything in between and passes it to EntityValue. Text with many bare '&' (query strings, prose) and a ';' near the end copies O(n) bytes per '&'. With no ';' at all, each '&' still scans the rest of the text. A 1.6 MB input takes about 32 s in a release build, which is a denial-of-service risk for untrusted HTML. Measured in release: 50000 repetitions take 0.3 s, 100000 take 0.7 s, 200000 take 2.4 s, and 400000 take about 32 s.

```
Import std.core
Import std.text
Import std.text.encoding

// 1.6 MB: 400000 bare "&" and one entity at the end.
Local query = "&b=2".Repeated(400000) + "&amp;"
Print(Html.Unescape(query).Length)
```

Expected: 1600001, in well under a second with fb run --release (the work should be linear)

Actual: 1600001 after about 32 seconds (release)

Suggested fix: Only look a bounded distance past the '&' for the ';'. The longest entity Unescape knows is "#x" plus 8 digits, so stop at the first character that can't be in an entity name, or after about 10 bytes. Never slice the rest of the text.

Skeptic: I timed the release exes of the program at three sizes. 100000 repetitions took 1.66 s, 200000 took 5.0 s, and 400000 (1.6 MB) took 45.7 s, printing 1600001. The work is clearly quadratic. Html.Unescape (encoding.fb:341-343) calls text.Find(";", found) for every '&' and then slices from found+1 up to that ';', however far, so each bare '&' copies the rest of the text. On untrusted HTML that is a denial-of-service risk.

## S6-char-2 (low): The types and MIR dumps print a folded Char bare, and a control character raw

Status: open

Where: compiler/src/mir.cpp:114

mir.cpp's constant() promises that 'Constants print as they would in source code', and the types dump (dump.cpp around 1230) prints folded values. Both send a Char constant through to_text(), which gives the character itself. So `65 As Char` shows as `A`, which reads like a variable. `'"'` shows as a lone `"`, and `9 As Char` or `'\n'` puts a raw tab or line break into the dump. Meanwhile the same dump prints a Char literal as `'"'` or `U+000A` (dump.cpp's build(character_expr_t)), and it quotes Strings.

```
Import std.core

Const TAB = 9 As Char
Local quote = '"'
Local letter = 65 As Char
Print(TAB, quote, letter)
```

Expected: `fb build --show types,mir --color never` shows each Char constant the way the literal is shown, e.g. `(const TAB = (as 9: Int32 Char): Char = U+0009)`, `(local letter = ...: Char = 'A')`, and in MIR `_0 = '"'`, `_1 = 'A'`, `_2 = to_text(U+0009)`.

Actual: types: `(const TAB = (as 9: Int32 Char): Char = <raw TAB byte>)` and `(local letter = (as 65: Int32 Char): Char = A)`; mir: `_0 = "`, `_1 = A`, `_2 = to_text(<raw TAB byte>)`.

Suggested fix: Give a Char constant its own spelling in mir.cpp's constant() and in dump.cpp's value printing, sharing the rule build(character_expr_t) already uses ('x' for visible ASCII, U+XXXX otherwise). Add a mir_tests and a dump_tests case with a folded Char such as `10 As Char`.

Skeptic: I ran `fb build --show types,mir --color never` on the finding's program and piped it through cat -A. The types dump printed `(const TAB = (as 9: Int32 Char): Char = ^I)` with a raw tab byte and `(local letter = (as 65: Int32 Char): Char = A)` with a bare A. The MIR printed `_0 = "` (a lone quote), `_1 = A`, and `_2 = to_text(^I)`. In mir.cpp, constant() is documented as "Constants print as they would in source code", yet it sends every non-String value to to_text(), which gives a Char's character itself. dump.cpp's type annotation (around 1230) does the same, while build(character_expr_t) at dump.cpp:372 already spells a Char as 'x' or U+XXXX. The fault is cosmetic and limited to debug output, but the code contradicts its own stated contract and puts raw control bytes into the dumps.

## S6-char-3 (low): text_chars.fb labels decimal codes with the hex U+ prefix

Status: open

Where: tests/programs/text_chars.fb:9

The test prints "U+{c.Code}", but Code is a decimal Int. The expected output reads "U+97" for 'a' (U+0061), "U+128512" for the emoji (U+1F600), and "U+769" for the combining acute (U+0301). In U+ notation those name other characters: U+0097 is a C1 control. So the test's .out file says something wrong about what it checks.

```
Import std.core
Import std.text

Print("U+{'a'.Code} U+{'\u{1F600}'.Code}")
```

Expected: A label that matches its number: "U+0061 U+1F600", or decimal without the prefix, like "code=97 code=128512".

Actual: U+97 U+128512

Suggested fix: In text_chars.fb, change the label to "code={c.Code}" (formatting isn't in yet), and regenerate text_chars.out.

Skeptic: I ran `Print("U+{'a'.Code} U+{'\u{1F600}'.Code}")` and it printed `U+97 U+128512`. tests/programs/text_chars.fb:9 prints "U+{c.Code}" with a decimal Code, and text_chars.out records U+97, U+90, U+233, U+128512, U+769 and so on. Spec 02 uses U+ for hex code points ("U+0000 to U+10FFFF"), so these labels name different characters: U+97 would be a C1 control, not 'a'. The same mistake is in tests/programs/text_strings.fb:113 (Escaped() builds "U+{c As Int}"), and its .out lines 38-42 read `U+101 U+769`, `U+128512`, `U+128104 U+8205 ...`. Both test outputs state something false about what they check, so both should be fixed (a decimal label such as code=97, or hex once formatting lands) and their .out files regenerated.

## S6-string-10 (low): The done-when's 'each Method's tests include text beyond ASCII, combining marks, and emoji' isn't fully checked

Status: open

Where: tests/programs/text_builder.fb:19

text_builder.fb's `.Replace("😀", "👨‍👩‍👧")` replaces nothing: the line before, `text[7] = '?'`, already replaced the 😀. So StringBuilder.Replace never gets an emoji or a combining mark. Scanner's TakeLine, TakeWhile, TakeUntil, and TakeRest only ever return ASCII in the test: "", "lair", " +", and digits. In text_strings.fb, TrimStart and TrimEnd only see "\u{A0}é " and " é\u{A0}", with no combining mark or emoji, and PadRight has no combining mark.

```
Import std.core
Import std.text

// text_builder.fb's steps up to its Replace("😀", "👨‍👩‍👧"): by then the 😀 is gone.
Local text = StringBuilder("héllo 😀 wörld")
text.Insert(0, "¡").Insert(text.Length, "!")
text.Remove(3, 5)
text[0] = '¿'
text[7] = '?'
Print(text.ToString().Contains("😀"))
text.Replace("ö", "oe").Replace("😀", "👨‍👩‍👧")
Print(text.ToString().Contains("👨‍👩‍👧"))
```

Expected: True
True (the test's emoji Replace actually replaces something)

Actual: False
False

Suggested fix: Do the emoji Replace while a 😀 is still there, and add a combining-mark case. Give Scanner's TakeLine, TakeWhile, TakeUntil, and TakeRest text with é, a combining mark, and an emoji to return. Add combining-mark and emoji cases to TrimStart, TrimEnd, and PadRight.

Skeptic: Ran strref/s10.fb, which printed False then False: by the time text_builder.fb:19 runs Replace("😀", ...), text[7] = '?' has already replaced the 😀. text_builder.out lines 4-5 show `¿hllo ? wörld!` then `¿hllo ? woerld!`. In text_builder.out, Scanner's TakeLine gives "" and "", TakeWhile "lair", TakeUntil " +", and TakeRest digits, all ASCII. text_strings.fb:47 gives TrimStart and TrimEnd only "\u{A0}é " and " é\u{A0}", with no combining mark or emoji, and line 59's PadRight has only an emoji. The milestones row's done-when needs text beyond ASCII, combining marks, and emoji in each Method's tests.

## S6-string-2 (low): PadLeft, PadRight, and Center overflow on a width near Int's least, and debug and release differ

Status: open

Where: std/text/string.fb:256

`Local missing = width - CharCount` overflows when width < -2147483648 + CharCount. A debug build panics inside std with an integer overflow. A release build wraps to a huge positive count, tries to pad about 2^31 characters, and panics at the String cap. The doc says a width the String already reaches gives the String itself. Lines 264 and 273 have the same expression.

```
Import std.core
Import std.text

// A width no more than its CharCount gives the String itself.
Local least = -2147483647 - 1
Print("[{"ab".PadLeft(least, '-')}] [{"ab".PadRight(least, '-')}] [{"ab".Center(least, '-')}]")
```

Expected: [ab] [ab] [ab]

Actual: debug: panic: integer overflow in '-' at std/text/string.fb:256:25
release: panic: a String holds at most 2147483647 bytes

Suggested fix: Compare before subtracting: `If width <= CharCount Then Return Self`, then `Local missing = width - CharCount`. Do the same in PadRight and Center, and add a test with Int's least width.

Skeptic: Ran strref/s2.fb. Debug: `panic: integer overflow in '-' at std/text/string.fb:256:25`, exit 101. Release: `panic: a String holds at most 2147483647 bytes`, exit 101. s2c.fb shows least+2 gives [ab], while least+1 already panics in debug. string.fb:256, 264, and 273 compute `width - CharCount` before comparing, and the doc says a width the String already reaches gives the String itself. I rated it low because only a width near Int's least triggers it.

## S6-string-3 (low): Split (and Replace) on a String of the most bytes overflows Length + 1 in debug

Status: open

Where: std/text/string.fb:155

Split calls `Parts(separator, Length + 1)`. For a String of 2147483647 bytes, the most one holds, Length + 1 overflows Int. Debug panics at the caller, release works. Replace calls Split, so it fails the same way.

```
Import std.core
Import std.text

Local big = "x".Repeated(2147483647)
Print(big.Length)
Print(big.Split(",").Count)
```

Expected: 2147483647
1

Actual: debug: 2147483647, then panic: integer overflow in '+' at t5.fb:6:7
release: 2147483647
1

Suggested fix: Give Parts a way to mean no limit, such as a count of 0 that skips the `parts.Count < count - 1` test, so Split passes no Length + 1.

Skeptic: Ran strref/s3.fb. Debug printed 2147483647, then `panic: integer overflow in '+' at s3.fb:6:7`, exit 101. Release printed 2147483647 and 1. string.fb:155 calls `Parts(separator, Length + 1)`, which overflows at the String cap. Replace goes through Split (line 201), so it fails the same way.

## S6-string-4 (low): Repeated on an empty String loops count times: "".Repeated(2147483647) takes about a minute

Status: open

Where: runtime/src/text.cpp:135

fb_text_repeat checks the cap only when `once` isn't empty, then runs `for (i < count) repeated += once` anyway. An empty String with a large count spins through billions of empty appends to make "". It took 45 s in a debug build and 60 s with --release on the review build, whose runtime is built Debug. It's O(count) however the runtime is built.

```
Import std.core
Import std.text

Local empty = ""
Print("[{empty.Repeated(2147483647)}]")
```

Expected: [] at once

Actual: [] after about 45 to 60 seconds

Suggested fix: In fb_text_repeat, return new_string("") right away when once is empty or count is 0, or do the same in std's Repeated before the call.

Skeptic: Ran strref/s4.fb, `"".Repeated(2147483647)` in debug. It printed `[]` after 46.7 s of real time, while the same program with count 1 took 0.7 s. runtime/src/text.cpp fb_text_repeat checks the cap only when `once` isn't empty, then still runs `for (i < count) repeated += once`, which is O(count) for an empty String.

## S6-string-5 (low): ToLower's final sigma doesn't follow Unicode's Final_Sigma: Cased and Case_Ignorable are incomplete

Status: open

Where: runtime/src/unicode.cpp:71

is_cased counts only Lu, Ll, and Lt. Unicode's Cased also includes Other_Uppercase and Other_Lowercase: Roman numerals like U+2160, circled letters, U+00AA and U+00BA, modifier letters, and others. is_case_ignorable lacks the Word_Break MidLetter and MidNumLet code points U+0387 (Greek ano teleia), U+055F, U+05F4, U+FE13, U+FE52, U+FE55, U+FF07, U+FF0E, and U+FF1A. So all five cases below come out the other way from Unicode 15's rules. Python 3.12's str.lower() on the same text gives True True True False False.

```
Import std.core
Import std.text

// Whether each capital sigma lowered to the final sigma, U+03C2, as Unicode's Final_Sigma says.
For text In ["ⅠΣ", "ªΣ", "Α\u{387}Σ", "ΑΣ\u{387}Β", "ΑΣ\u{FF0E}Β"]
    Print(text.ToLower().Contains('ς'))
End For
```

Expected: True
True
True
False
False

Actual: False
False
False
True
True

Suggested fix: Make is_cased cover Other_Uppercase and Other_Lowercase (a small range table, since utf8proc doesn't expose them), add the missing MidLetter and MidNumLet code points to is_case_ignorable, and test these cases.

Skeptic: Ran strref/s5.fb. FB printed False False False True True. Python 3.12.10 (Unicode 15.0) on the same five strings printed True True True False False. In runtime/src/unicode.cpp, is_cased counts only Lu, Ll, and Lt, missing Other_Uppercase and Other_Lowercase such as U+2160 and U+00AA. is_case_ignorable's extra list lacks U+0387 and U+FF0E. stdlib.md says ToLower is full Unicode, and Final_Sigma is part of that.

## S6-string-6 (low): Scanner.TakeLine drops a "\r" at the end of Text that no "\n" follows

Status: open

Where: std/text/scanner.fb:103

The Scanner's doc says a line ends at "\n". TakeLine says it gives the line without its line break, "\n" or "\r\n". But it always runs `line.TrimSuffix("\r")`, even when no "\n" was taken. So the last line "key\r", which has no line break, loses its "\r".

```
Import std.core
Import std.text

// The text has no line break, so its one line is all of it, "\r" too: 4 bytes.
Local scanner = Scanner("key\r")
Print((scanner.TakeLine() ?? "-").Length)
```

Expected: 4

Actual: 3

Suggested fix: Trim the "\r" only when the "\n" was taken: `If Take("\n") Then Return line.TrimSuffix("\r")`, then `Return line`. Or document that a "\r" at the end is dropped too. Add the case to text_builder.fb.

Skeptic: Ran strref/s6.fb. `Scanner("key\r").TakeLine()` gave Length 3, not 4. `Scanner("a\rb\r")` gave 3 ("a\rb") and AtEnd True. scanner.fb:101-103 always runs `line.TrimSuffix("\r")`, even when Take("\n") took nothing. Yet the class doc says a line ends at "\n", and TakeLine's doc says the line break it drops is "\n" or "\r\n".

## S6-string-7 (low): Count("") panics with FindAll's name

Status: open

Where: std/text/string.fb:117

Count leaves the check to FindAll, so the user who called Count reads a message about FindAll. The place is right.

```
Import std.core
Import std.text

Local word = "é😀"
Print(word.Count(""))
```

Expected: panic: Count takes a String to look for that isn't empty at s7.fb:5:7

Actual: panic: FindAll takes a String to look for that isn't empty at s7.fb:5:7

Suggested fix: Have Count call CheckPart(part, "Count") before FindAll, and add a panic program for it.

Skeptic: Ran strref/s7.fb and got `panic: FindAll takes a String to look for that isn't empty at s7.fb:5:7`. Count (string.fb:117) leaves the check to FindAll, while Replace, for comparison, calls CheckPart(old, "Replace") before it calls Split. So the user who called Count reads FindAll's name.

## S6-string-8 (low): StringBuilder's [] past its Length says the range is 0 to the Length, which [] itself rejects

Status: open

Where: std/text/builder.fb:138

For at > Length, CheckCharacter falls through to CheckPosition, whose message says "from 0 to the StringBuilder's Length, 6". But [] panics on at == Length, with a different message: "before the StringBuilder's Length". So the message for 7 names 6 as allowed when it isn't.

```
Import std.core
Import std.text

Local text = StringBuilder("é😀")
Print(text[7])
```

Expected: panic: [] takes a character's position, before the StringBuilder's Length, 6, not 7 at s8.fb:5:11

Actual: panic: [] takes a position from 0 to the StringBuilder's Length, 6, not 7 at s8.fb:5:11

Suggested fix: In CheckCharacter, use the "before the StringBuilder's Length" message for any at >= Bytes.Count (and for at < 0), then call CheckPosition for the mid-character check.

Skeptic: Ran strref/s8.fb, StringBuilder("é😀")[7], and got `panic: [] takes a position from 0 to the StringBuilder's Length, 6, not 7`. s8b.fb, [6], gives `[] takes a character's position, before the StringBuilder's Length, 6, not 6`. So the message for 7 names 6 as allowed when [] rejects it. builder.fb:132-139 CheckCharacter special-cases only at == Bytes.Count, then falls through to CheckPosition's 0-to-Length message.

## S6-string-9 (low): Repeated's String-cap panic has no place, though Repeated is @PanicsAtCaller, and a test expects that

Status: open

Where: runtime/src/text.cpp:130

This is separate from the open gap for + and interpolation. Repeated is @PanicsAtCaller, but its cap check is in fb_text_repeat, which panics with a null location. panic_string_too_long.out expects that placeless message, so the test locks in the missing place. If the open gap covers every cap panic, fold this into it.

```
Import std.core
Import std.text

Local word = "é😀"
Print(word.Repeated(400_000_000).Length)
```

Expected: panic: a String holds at most 2147483647 bytes at s9.fb:5:12

Actual: panic: a String holds at most 2147483647 bytes

Suggested fix: Check the cap in std's Repeated before the call (`If count > 0 && Length > 2147483647 / count Then Panic("a String holds at most 2147483647 bytes")`) so @PanicsAtCaller gives the caller's place, and update panic_string_too_long.out.

Skeptic: Ran strref/s9.fb and got `panic: a String holds at most 2147483647 bytes` with no place, exit 101. Repeated is @PanicsAtCaller (string.fb:247), but the cap panic comes from fb_text_repeat's `fb_panic(..., nullptr)` (text.cpp:130). tests/programs/panic_string_too_long.out pins the placeless line. Spec 09 says @PanicsAtCaller makes every panic in its code report the caller's place, and stdlib.md's conventions say the same. Phase 1 treated placeless runtime panics as findings (A10-6). The open gap as given covers + and interpolation only, so this is separate, though it may fold into that gap.

## S6-unicode-10 (low): stdlib.md lists utf8proc's license as MIT only, while the build and the driver also name the Unicode license

Status: open

Where: docs/stdlib.md:128

third_party/CMakeLists.txt sets FB_UTF8PROC_LICENSES to "the MIT license and the Unicode license", and the notice file holds both. Building a program that uses utf8proc prints "Bundled utf8proc 2.12.0, under the MIT license and the Unicode license". The Bundled C libraries table in stdlib.md says just "MIT". This step updated that row's Backs column but not its License column. Other rows, like libuv's, list every license that applies.

```
Import std.core
Import std.text

Print("straße".ToUpper())
```

Expected: fb build's notice and docs/stdlib.md's table name the same licenses for utf8proc

Actual: fb build prints "Bundled utf8proc 2.12.0, under the MIT license and the Unicode license", while stdlib.md's table says "MIT"

Suggested fix: Change the row's License column to "MIT, with Unicode's data under the Unicode license".

Skeptic: Building any program that uses utf8proc prints "Bundled utf8proc 2.12.0, under the MIT license and the Unicode license". That comes from FB_UTF8PROC_LICENSES at third_party/CMakeLists.txt:272. The utf8proc row of the Bundled C libraries table (docs/stdlib.md:128) says only "MIT". This step's diff changed that row's Backs column but left the License column. The libuv row, by contrast, lists every license the build names.

## S6-unicode-3 (low): Base64.Decode and Base32.Decode accept '=' padding beyond what fills the last group

Status: open

Where: std/text/encoding.fb:582

SymbolValues strips every trailing '='. It then checks only that the padded length is a whole number of groups and that the unpadded remainder is allowed. It never checks that the number of '=' is what fills the last partial group. So "aGk=====" (5 '='), "aGkh====" (a whole group of '=' after a complete group), and "====" all decode, the last one to empty bytes. Base32 behaves the same. The doc says Decode gives Null when the text isn't Base64, and RFC 4648 only allows padding that completes the final quantum.

```
Import std.core
Import std.collections
Import std.text
Import std.text.encoding

// "=" past what fills out the last group, or a whole group of them, isn't Base64 or Base32.
Print(Base64.Decode("aGk=====") ?? [0], Base64.Decode("aGkh====") ?? [0], Base64.Decode("====") ?? [0])
Print(Base32.Decode("NBUQ============") ?? [0], Base32.Decode("========") ?? [0])
```

Expected: [0] [0] [0]
[0] [0]

Actual: [104, 105] [104, 105, 33] []
[104, 105] []

Suggested fix: In SymbolValues, when any '=' was stripped, require `source.Count - count == (group - count % group) % group` and `count % group != 0`. That means the padding exactly fills out a partial last group, which also rejects "====". Add these cases to text_encoding.fb.

Skeptic: Debug and release both print "[104, 105] [104, 105, 33] []" and "[104, 105] []". SymbolValues (encoding.fb:578-583) strips every trailing '='. It then checks only that the padded length is whole groups and that count % group is in ends, never how many '=' there were. Python's strict decoders reject all five inputs ("Excess data after padding", "Excess padding not allowed", "Leading padding not allowed", "Incorrect padding"). The Decode docs say Null when the text isn't Base64 or Base32. Being lenient about extra padding does no real harm, so I rated it low.

## S6-unicode-6 (low): A lone regional indicator is 1 column as a Char but 2 as a String

Status: open

Where: runtime/src/unicode.cpp:274

fb_unicode_display_width forces width 2 for every regional indicator so that a flag (two indicators) comes out 2. A lone indicator is also one cluster, so DisplayWidth("🇫") gives 2. DisplayWidth('🇫') uses utf8proc_charwidth and gives 1. The String overload's doc and the runtime header both say a cluster takes "as many as its widest character", which DisplayWidth(Char) says is 1. The forced 2 was meant only for flags.

```
Import std.core
Import std.text
Import std.text.unicode

// A lone regional indicator, as a Char and as a String of just it.
Print(DisplayWidth('\u{1F1EB}'), DisplayWidth("\u{1F1EB}"))
```

Expected: 1 1 (the same width both ways, as the docs say)

Actual: 1 2

Suggested fix: Give a cluster width 2 only when it holds two regional indicators, a flag, and otherwise use its widest character. Or make the Char overload agree and update both docs. Add a lone indicator to text_unicode.fb.

Skeptic: Debug and release both print "1 2". The String overload forces width 2 for any regional indicator (unicode.cpp:274-275), but the Char overload gives utf8proc_charwidth, which is 1. The String doc (unicode.fb:59) says a cluster takes "as many as its widest character", which for a lone indicator is 1. The two overloads disagree, and the String one contradicts its doc. The forced 2 was only needed for flags.

## S6-unicode-7 (low): ToLower's final-sigma rule misses some Case_Ignorable and Cased characters

Status: open

Where: runtime/src/unicode.cpp:80

is_case_ignorable lists Mn, Me, Cf, Lm, and Sk, plus eight punctuation code points. Unicode's Case_Ignorable also includes every Word_Break MidLetter and MidNumLet character, and these are missing: U+0387 GREEK ANO TELEIA, U+055F, U+05F4, U+FE13, U+FE52, U+FE55, U+FF07, U+FF0E, and U+FF1A. is_cased only counts Lu, Ll, and Lt, while Unicode's Cased also includes Other_Uppercase and Other_Lowercase, like U+2160 Ⅰ and U+24B6 Ⓐ. So capital sigma lowers wrongly next to them. Python's str.lower gives the expected results. I checked every assigned code point's ToUpper, ToLower, and CaseFold, and 1500 random strings, against Python. Apart from Unicode 16 additions, these are the only differences.

```
Import std.core
Import std.text

// U+0387 GREEK ANO TELEIA is Case_Ignorable, and U+2160 ROMAN NUMERAL ONE is Cased.
Print("\u{391}\u{387}\u{3A3}".ToLower(), "\u{391}\u{3A3}\u{387}\u{392}".ToLower(), "\u{2160}\u{3A3}".ToLower())
```

Expected: α·ς ασ·β ⅰς

Actual: α·σ ας·β ⅰσ

Suggested fix: Add the missing MidLetter and MidNumLet code points to is_case_ignorable. In is_cased, also count a character that has a case mapping (utf8proc_tolower(c) != c or utf8proc_toupper(c) != c), which covers Ⅰ and Ⓐ. Add the three cases to text_strings.fb.

Skeptic: The program prints "α·σ ας·β ⅰσ". Python (Unicode 15) gives U+03B1 U+0387 U+03C2, U+03B1 U+03C3 U+0387 U+03B2, and U+2170 U+03C2. U+0387 is Po, which is_case_ignorable's category list misses, and it isn't among the eight extra code points. U+2160 is Nl, which is_cased (Lu, Ll, Lt only) misses. The code's comments say "as Unicode's Case_Ignorable has them", and string.fb:242 promises Unicode's full case mapping with final sigma. Unicode's Case_Ignorable includes MidLetter characters like U+0387, and Cased includes Other_Uppercase characters like U+2160.

## S6-unicode-8 (low): Escape's doc says it writes a C string literal's contents, but C reads its output differently

Status: open

Where: std/text/encoding.fb:359

Escape and Unescape use FutureBlitz's own escapes from 01-lexical: \xHH takes exactly two digits, \0, and \u{...}. The doc comment says Escape writes text "as a C string literal's contents write it", and Unescape's doc says the same. C reads things differently. In C, \x takes every hex digit after it, so Escape("\u{7}A") = \x07A is the single character 'z' (gcc confirms 1 char, 122). \0 followed by a digit is an octal escape, so Escape("\u{0}1") = \01 is U+0001. \u{85} isn't C syntax at all.

```
Import std.core
Import std.text
Import std.text.encoding

Print(Escape("\u{7}A"), Escape("\u{0}1"), Escape("\u{85}"))
```

Expected: Either output a C compiler reads back as the same text, or docs that don't promise C

Actual: \x07A \01 \u{85}. C reads the first as one character, 'z', and the second as U+0001, and rejects the third.

Suggested fix: Reword the Escape and Unescape comments, and stdlib.md's "C-style", to say they write and read FutureBlitz's string escapes (01-lexical: \xHH is exactly two digits, \u{...}).

Skeptic: The program prints "\x07A \01 \u{85}". Escape's doc (encoding.fb:359) and the file's header comment (line 4) say C string literals. In C, \x takes every hex digit after it, so \x07A is one character, 0x7A. \01 is the octal escape U+0001, and \u{85} isn't C syntax at all. The output is FutureBlitz's own escapes from spec 01, so the docs are wrong, not the code.

## S6-unicode-9 (low): Html.Escape, Html.Unescape, and Escape have no test with a combining mark

Status: open

Where: tests/programs/text_encoding.fb:89

The milestone's done-when says each Method's tests include text beyond ASCII, combining marks, and emoji. Html's test text (line 89, "…Tom & Jerry's é😀…"), Html.Unescape's entity string, and Escape's raw text (line 94) have no combining mark. Only Unescape's \u{301} case has one. IsNormalized's test in text_unicode.fb has no emoji. The functions do handle these cases; the program below shows it. Only the tests are missing.

```
Import std.core
Import std.text
Import std.text.encoding

Local text = "<b>e\u{301}</b> 😀"
Print(Html.Escape(text), Html.Unescape(Html.Escape(text)) == text, Escape("e\u{301}\t😀"), Unescape(Escape("e\u{301}\t😀")) == "e\u{301}\t😀")
```

Expected: text_encoding.fb covers Html.Escape, Html.Unescape, and Escape on text with a combining mark (this program prints: &lt;b&gt;é&lt;/b&gt; 😀 True é\t😀 True)

Actual: The test programs don't cover these cases

Suggested fix: Put an "e\u{301}" in text_encoding.fb's page and raw strings and in the Html.Unescape line, and an emoji in text_unicode.fb's IsNormalized line, then regenerate the .out files.

Skeptic: In tests/programs/text_encoding.fb, Html.Escape's page on line 89 has é and the 😀 emoji but no combining mark. Html.Unescape's line 91 has no literal combining mark, and its only emoji comes from an entity. Escape's raw string on line 94 has é and 😀 but no combining mark. In text_unicode.fb, the IsNormalized line (25-26) has combining marks but no emoji. The milestone's done-when needs non-ASCII text, combining marks, and emoji in each Method's tests. The functions themselves work: the program prints "&lt;b&gt;é&lt;/b&gt; 😀 True é\t😀 True". Only the tests are missing.

## Refuted by the skeptics (no action)

- S6-char-4: Only 0xD800 tests the run-time As Char check. This is a coverage gap; the behavior is correct. I ran four programs in debug and --release, and each printed 65 and then panicked with "the value isn't a Unicode scalar value, which a Char holds" at the As: an Int8[] of [0x41, -1] (c4.fb), a UInt32 0x110000 (c4b.fb), a UInt16 0xDFFF (c4c.fb), and a UInt64 0x1_0000_0041 (c4d.fb). `Local x = 0xDFFF As Char` (c4e.fb) is a compile error: "57343 isn't a Unicode scalar value". codegen.cpp:2950 to_char() sign-extends signed sources (LLVMBuildIntCast2 with is_signed(from)), compares the result unsigned against 0x10FFFF and the surrogate range, and only then truncates. That matches spec 02. Nothing I saw is wrong, and the spec and the milestones row (whose done-when is about text beyond ASCII, STRASSE, and encoding round-trips) don't require these particular tests, so this isn't a bug under the review criteria. Adding the tests would still be worthwhile hardening.
