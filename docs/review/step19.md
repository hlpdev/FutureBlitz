# The review of M6 step 19 (std.data's JSON and MessagePack, and overloads that differ by their count of type parameters)

10 confirmed findings: 1 high, 5 medium, 4 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## overloads-1 (high): Extend T[] Methods: written type arguments are counted against the array's T too, so the wrong overload runs

Status: fixed in c3825a86 (m6-next's partial inference, which this rebased onto, already counts an
Extend of T[]'s own type parameters in rank() and in the count error; overload_type_count and a sema
test pin Pick() and Pick<U>() on an array)

Where: compiler/src/sema.cpp:23238

rank() and choose_overload()'s new 'counted' check compare the written type-argument count with symbol(...).type_parameters.size(). For a Method that an Extend of T[] adds, type_parameters starts with the array's T, which add_member() correctly strips with own_type_parameters(). So add_member accepts Pick() and Pick<U>() as two overloads, but at a call xs.Pick<String>() the plain Pick() (type_parameters [T], size 1) matches the one written argument and Pick<U> (size 2) is dropped: the non-generic overload silently runs. With Pick<U>() and Pick<U, V>(), xs.Pick<String>() is refused with 'no overload of 'Pick' takes 1 type argument'. Extends of Int, of a generic Struct, and a Struct's own Methods are fine; only array Extends are hit.

```
Import std.core
Import std.collections

Extend T[]
    Method Pick(): String => "plain"
    Method Pick<U>(): String => "generic"
End Extend

Local xs = [1, 2, 3]
Print(xs.Pick())
Print(xs.Pick<String>())
```

Expected: plain
generic

Actual: plain
plain   (debug and --release alike; and with Pick<U>()/Pick<U, V>() instead, xs.Pick<String>() fails to compile: "no overload of 'Pick' takes 1 type argument")

Suggested fix: Use own_type_parameters(declared).size() in rank() and own_type_parameters(symbol(candidate)).size() in choose_overload()'s 'counted' check, and add a sema test with an Extend T[]'s overloads by count.

Skeptic: I ran the finding's program (o1.fb). Debug and --release both printed 'plain\nplain', so xs.Pick<String>() silently runs the non-generic overload. With Pick<U>/Pick<U, V> (o1b.fb), xs.Pick<String>() fails with "no overload of 'Pick' takes 1 type argument". In the code, rank() (sema.cpp ~23238) compares the written count with declared.type_parameters.size(), and choose_overload's 'counted' check uses symbol(candidate).type_parameters.size(). add_member (~4237) uses own_type_parameters(), and that function (5570) strips the array's T from Extend T[] Methods. Spec 07 says written type arguments pick the overload with that many of its own type parameters.

## json-1 (medium): JsonReader.Line and Column count the whole text before the token on every call, so using them per token is quadratic

Status: fixed in 9213bebd (the reader counts its line and column on from the last place asked for,
so asking at every token costs no more than reading; data_json_stream reads the places of a
50000-line document at every token)

Where: runtime/src/json.cpp:565

fb_json_line and fb_json_column count newlines (and code points) in text[0, start) from the start of the document on every call. JsonReader.Line and Column are public properties meant to say where each token is, so a streaming reader that records each token's place (for a user's own errors or a schema check) does O(n) work per token. On a 40000-element array (about 230 KB), Next alone takes 7 ms, but Next plus Line on each token takes 21.5 s in a release build. The test data in data_json_stream is too small to show this. The ParseError path calls each once, so it is fine there.

```
// JsonReader.Line on each token: each call counts the text from its start.
Import std.core
Import std.collections
Import std.data
Import std.time

Local parts: String[] = []
For i = 1 To 40000
    parts.Add("{i}")
End For
Local text = "[" + parts.Join(",\n") + "]"

Local watch = Stopwatch.StartNew()
Local plain = JsonReader(text)
Try
    While plain.Next() != JsonToken.END
    End While
Catch e As ParseError
    Print("ParseError {e.Message}")
End Try
Print("Next alone: {watch.Elapsed}")

watch = Stopwatch.StartNew()
Local placed = JsonReader(text)
Local line = 0
Try
    While placed.Next() != JsonToken.END
        line = placed.Line
    End While
Catch e As ParseError
    Print("ParseError {e.Message}")
End Try
Print("Next and Line: {watch.Elapsed}, last line {line}")
```

Expected: Next and Line takes about as long as Next alone, a few milliseconds (fb run --release).

Actual: Next alone: 7.3115ms
Next and Line: 21.5250804s, last line 40000

Suggested fix: Have the reader track the line and the start of the current line (in bytes, plus a code-point column count) as it advances. skip_space and read_string already see every newline, and a newline outside strings can only appear in whitespace. Then record line and column at reader.start when a token or an error starts. Otherwise, cache the last (offset, line, line_start) and count only forward from it, since start only grows.

Skeptic: Ran the finding's program, unchanged, as j1.fb in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-step19/json-reader/ with fb run --release. Output: 'Next alone: 6.3006ms' and 'Next and Line: 20.9110478s, last line 40000'. Reading Line on every token is about 3000 times slower than Next alone. The code matches: in runtime/src/json.cpp lines 565-580, fb_json_line takes text.substr(0, start) and runs ranges::count for '\n' on every call, and fb_json_column does an rfind plus a count_if over the same prefix. So each call is O(start), and calling it per token is quadratic. JsonReader.Line and Column (std/data/json.fb:98-105) are public properties documented as where the token starts, so calling them per token is ordinary use.

## overloads-2 (medium): A Class's inherited Method and a generic one of the same parameters in a derived Class are taken as an override, not two overloads (and Override crashes the compiler)

Status: fixed in c3825a86 (same_parameters() compares the counts of own type parameters too, so a
derived Class's Get<T>() is an overload beside an inherited Get(), and Override on one of another
count says the base has no Method like it, where it crashed; a sema test and overload_type_count,
both ways round)

Where: compiler/src/sema.cpp:6117

add_member() now lets one type declare Get() and Get<T>() (that works), but the inheritance check's 'alike' list still uses same_parameters() alone, ignoring the own type-parameter count. A derived Class that adds Get<T>() beside Base's Get() is told "'Get' overrides Base's; write 'Override Method Get'", and writing Override is then refused because 'a generic Method can't be Abstract or Override'; the call d.Get() also no longer sees Base's Get(). Reversed (Base Get<T>(), Derived Get()) it's again "overrides Base's", and adding Override as told makes fb fail with 'fb failed unexpectedly: unordered_map::at' (that crash also happens on an older build, build-rev, so it predates this change, but it is reached through the same missing count check).

```
Import std.core

Class Base
    Method Get(): String => "base plain"
End Class

Class Derived Extends Base
    Method Get<T>(): String => "derived generic"
End Class

Local d = Derived()
Print(d.Get())
Print(d.Get<Int>())
```

Expected: base plain
derived generic

Actual: inh.fb:8:12: error: 'Get' overrides Base's; write 'Override Method Get'
inh.fb:12:7: error: can't tell what T is in this call; write the type arguments, like d.Get<Int>(...)
(With Base having Method Get<T>() and Derived having Override Method Get(), compiling prints: error: fb failed unexpectedly: unordered_map::at)

Suggested fix: In the inherited-Method check, count a Method as 'alike' only when own_type_parameters() sizes are equal too (generic Methods aren't in the table, so a generic one never overrides or is overridden), and add tests for both directions plus the Override case.

Skeptic: The finding's program (o2.fb) gives "'Get' overrides Base's; write 'Override Method Get'" and then "can't tell what T is" at d.Get(). The reverse case without Override (o2c.fb) also says it overrides. With Override (o2b.fb) the compiler fails with 'fb failed unexpectedly: unordered_map::at', which is a compiler crash. In sema.cpp ~6114, the 'alike' filter uses same_parameters() only and doesn't compare own type-parameter counts. Spec 05/07 now say Methods differing by type-parameter count are overloads, and decisions.md says generic Methods aren't in the table, so they can't override.

## overloads-3 (medium): A Class's Method is refused as 'taking the place of' an Interface's generic default that differs only by its type-parameter count

Status: fixed in c3825a86 (a Class's Method takes no generic default's place when a call's arguments
can't tell one of the default's type parameters, which only a call that writes it picks; a default a
call can tell, like Put<T>(x: T), still is, in a sema test; overload_type_count calls both)

Where: compiler/src/sema.cpp:6145

The 'covered' check (report_outside_override) treats a Class's Read() as an instance of an Interface's generic default Read<T>(), because Read<T>'s T doesn't appear in its parameters and so any Method of those parameters matches. Under the new rule a call without type arguments can never pick Read<T>() (T can't be inferred) and a call with one never picks Read(), so the two are distinct overloads, but the Class can't declare it.

```
Import std.core

Interface Reader
    Method Read<T>(): String => "default generic"
End Interface

Class Plain Implements Reader
    Method Read(): String => "plain"
End Class

Local p = Plain()
Print(p.Read())
Print(p.Read<Int>())
```

Expected: plain
default generic

Actual: iface2.fb:7:24: error: Plain's 'Read' would take the place of Reader's generic default, which is what a call through Reader runs; give it another name

Suggested fix: In the covered check, skip a default whose own type-parameter count differs from the Method's when one of the default's own type parameters can't be inferred from its parameters (or simply when the counts differ, matching add_member), and add a test.

Skeptic: The finding's program (o3.fb) is refused: "Plain's 'Read' would take the place of Reader's generic default ... give it another name". The 'covered' check (sema.cpp ~6143) matches with takes_instance_of() and ignores the count rule. Here T can't be inferred from Read<T>()'s parameters, so under spec 07's new paragraph p.Read() can never pick the default and p.Read<Int>() can never pick Plain's Read(). The two are distinct overloads, which add_member accepts on a single type, but this path rejects them.

## overloads-4 (medium): Function values ignore written type arguments and the count rule: Name<Int> with a Func type is 'ambiguous'

Status: fixed in c3825a86 (a function value's written type arguments are an overload's first ones,
and without any, a generic one the Func expected can't tell doesn't fit; a sema test and
overload_type_count)

Where: compiler/src/sema.cpp:18093

check_function_value() picks among overloads only by func_of(overload) == wanted and never looks at type_arguments. With Name(text: String) and Name<T>(text: String), both have Func(String): String, so `Local h: Func(String): String = Name<Int>` is reported ambiguous although the written <Int> can only mean the generic one (spec 07: a Function used as a value takes its type arguments 'from what's written'), and `= Name` is ambiguous although T can't be inferred from the Func, which by spec 07's new paragraph means only the plain one fits. A lone generic Name<T> used as `Name<Int>` works, so it's the overload path.

```
Import std.core

Function Name(text: String): String => "plain " + text
Function Name<T>(text: String): String => "generic " + text

Local f: Func(String): String = Name
Print(f("a"))
Local h: Func(String): String = Name<Int>
Print(h("c"))
```

Expected: plain a
generic c

Actual: fv.fb:6:33: error: the function value 'Name' is ambiguous (could be either)
fv.fb:10:33: error: the function value 'Name' is ambiguous (could be either)

Suggested fix: In check_function_value(), keep only overloads whose own type-parameter count equals the written count when type arguments are written, and when none are written drop generic overloads whose type parameters the expected Func can't give; add sema tests.

Skeptic: The finding's program (o4.fb) reports "the function value 'Name' is ambiguous" for both '= Name' and '= Name<Int>'. A lone generic Name<T> used as Name<Int> (o4b.fb) works and prints 'generic c'. check_function_value (sema.cpp 18071) picks among overloads only by func_of(overload) == wanted. It never uses the type_arguments parameter for overload selection, so it contradicts spec 07 (a function value takes its type arguments 'from what's written') and the new count rule.

## overloads-5 (medium): Spec 13's base64 reading of a UInt8[] overrides 'one Value alone' for 4-character numbers, so "0012" no longer loads as [12]

Status: fixed in a5fd012e (base64 comes first for a UInt8[], so any bytes Json.Write writes read
back, and spec 13 says so, with "0012" as the String that's both; decisions.md's row says what goes
on as before, the line is wrapped, and core_load_errors loads "0012" as a UInt8[] and as a UInt8.
Both rules can't hold for a String like "0012"; reading a number first would break JSON's round trip
of bytes like [211, 77, 118])

Where: docs/spec/13-compile-time.md:243

Spec 13 now says a UInt8[] is taken as a String of base64 or as an Array of integers (and a T[] as one Value alone), but not which wins when a String is both. LoadBytes tries base64 first, and every 4-character string of digits (or '+'), such as "0012", "0255", "+012", is valid padded base64, so it gives three bytes instead of the one number it loads as alone. The lane's notes argue 'no number from 0 to 255 is padded base64', which is false with leading zeros; decisions.md's row also says a String that isn't base64 'is still WRONG_TYPE', but "12" loads as [12]. The new spec sentence is also one 135-character line, where the file wraps at 100.

```
Import std.core

Print(Try Load<UInt8>(Value("0012")))
Print(Try Load<UInt8[]>(Value("12")))
Print(Try Load<UInt8[]>(Value("0012")))
```

Expected: 12
[12]
[12]   (or, if base64 is meant to win, spec 13 saying so)

Actual: 12
[12]
[211, 77, 118]

Suggested fix: Either read a String that is a valid integer as one Value alone before trying base64, or state the precedence in spec 13 and correct decisions.md's 'still WRONG_TYPE' wording and the notes' claim; rewrap the spec 13 line at 100 characters; add the "0012" case to core_load_errors.

Skeptic: The finding's program (o5.fb) printed '12', '[12]', '[211, 77, 118]' in both debug and release. "0012" therefore loads as one integer for UInt8 but as three base64 bytes for UInt8[], and spec 13 doesn't say which reading wins. decisions.md's new row says a String that isn't padded base64 'is still WRONG_TYPE', yet "12" loads as [12], so the row contradicts the behavior and spec 13's 'one Value alone'. The lane notes' claim that 'no number from 0 to 255 is padded base64' is false for zero-padded numbers. docs/spec/13-compile-time.md line 244 is 135 characters long, while the lines around it are wrapped under 100.

## json-2 (low): A leading byte order mark counts as a column, so every place on line 1 is off by one

Status: fixed in 9213bebd (a byte order mark isn't counted: places start after it; data_json_stream
reads places after one, in bytes and in a String)

Where: runtime/src/json.cpp:571

make_reader skips a UTF-8 BOM by setting at = 3, but it keeps the BOM in text. fb_json_column counts the code points in text[0, start), so the BOM (U+FEFF, one code point) adds 1 to the column of every token and every ParseError on the first line. Documents saved by Windows tools often start with a BOM, and for those the ParseError's column is wrong (1:3 for an error at 1:2). JsonReader.Column has the same problem (BEGIN_ARRAY at 1:2 instead of 1:1). The bytes test in data_json_stream reads a document with a BOM but never prints a place.

```
// A byte order mark, which the reader leaves out, still counts as a column on line 1.
Import std.core
Import std.data

Function Place(reader: JsonReader): String
    Try
        reader.Next()
        Local first = "{reader.Token} at {reader.Line}:{reader.Column}"
        reader.Next()
        Return first
    Catch e As ParseError
        Return "ParseError at {e.Line}:{e.Column}"
    End Try
End Function

Print(Place(JsonReader([91, 120, 93] As UInt8[])))
Print(Place(JsonReader([239, 187, 191, 91, 120, 93] As UInt8[])))
Print(Place(JsonReader("\u{FEFF}[x]")))
```

Expected: ParseError at 1:2
ParseError at 1:2
ParseError at 1:2

Actual: ParseError at 1:2
ParseError at 1:3
ParseError at 1:3

Suggested fix: Drop the BOM from reader.text in make_reader (text.erase(0, 3), or construct from text.substr(3)) instead of starting at 3. Otherwise, start the line or column count after the BOM. Add a place check for the BOM case to data_json_stream.

Skeptic: Ran j2.fb, the finding's program plus a valid '[1]' case, in debug and release. Both builds printed: 'ParseError at 1:2' (no BOM), 'ParseError at 1:3' (BOM bytes), 'ParseError at 1:3' (String starting with U+FEFF), and 'BEGIN_ARRAY at 1:2' for a BOM followed by [1]. That should be 1:1. Cause: make_reader (runtime/src/json.cpp:509-517) skips the BOM by setting at = 3 but keeps it in text. fb_json_column then counts the code points in text[0, start), so the BOM adds one column to every place on line 1. That contradicts json.fb's comment that the BOM 'is left out' and the documented column 'from 1'. I rated it low because only line-1 columns are wrong, and only for documents that start with a BOM.

## json-3 (low): After a Next that throws partway through a string, GetString gives the refused string's partial text for the earlier token

Status: fixed in 9213bebd (read_string decodes into a local string and gives it to the reader only
once the string ends; data_json_stream)

Where: std/data/json.fb:109

read_string clears reader.string and appends as it goes. When it fails (a bad escape, bytes that aren't UTF-8, a control character), Next throws, but JsonReader.Current still holds the previous token. If that token was a KEY or a STRING, Token still reports it, yet GetString returns the failed string's partial text instead of that token's text (or a panic). For example, after KEY("name"), Next throws on "bc\q", and then Token is KEY but GetString() is "bc". The token and its text disagree, and a caller that reports the last good key in its own error message shows the wrong one.

```
// After a Next that throws partway through a string, Token is still the KEY before it, but
// GetString gives the refused string's text so far.
Import std.core
Import std.data

Local reader = JsonReader(r"""
    {"name": "bc\q"}
    """)
Try? reader.Next()
Try? reader.Next()
Print("{reader.Token} {reader.GetString()}")
Local threw = False
Try
    reader.Next()
Catch e As ParseError
    threw = True
End Try
Print("{threw} {reader.Token} {reader.GetString()}")
```

Expected: KEY name
True KEY name

Actual: KEY name
True KEY bc

Suggested fix: In read_string, decode into a local std::string and move it into reader.string only on success, so a failure leaves the last token's text in place. Otherwise, have JsonReader.Next record that it failed and make the Get methods panic, for example 'needs a token, and Next threw'. Add the case to a test.

Skeptic: Ran j3.fb, the finding's program, in debug and release. Both printed 'KEY name' and then 'True KEY bc'. After Next throws, Token is still KEY, but GetString returns the rejected string's partial text 'bc' instead of 'name'. Cause: read_string (runtime/src/json.cpp:217-306) calls reader.string.clear() and then appends as it decodes, so a failure partway through (here the bad escape \q at line 268) leaves partial text in reader.string. JsonReader.Next (std/data/json.fb:66-80) throws before it updates Current, and GetString (json.fb:109-113) only checks Current and then returns fb_json_text(State). So the token and its text disagree.

## msgpack-1 (low): MessagePack.Read's depth leaves out an empty array or map, so it allows one level more than JSON does

Status: fixed in 322145e2 (an empty array or map counts toward the depth; data_msgpack reads [[[]]],
[[]], and [{}] at the depths JSON refuses them at)

Where: std/data/msgpack.fb:293

Unpacking.Opened only pushes (and depth-checks) an array or map that has values (`Pending == 0 Then Return`), so an empty innermost container is never counted. With depth 2, [[[]]] is accepted, and with depth 1, [[]] and [{}] are accepted. The Read doc comment says OUT_OF_RANGE is "for arrays and maps nested past depth", and Json.Read refuses the same documents. data_msgpack only tests depth with a non-empty innermost array ([145,145,145,192]), so this case is never checked. Debug and release both do this.

```
Import std.core
Import std.data

// [[[]]] is three arrays deep; at a depth of 2 JSON refuses it, and MessagePack should too.
Function Packed(bytes: UInt8[], depth: Int): String
    Try
        Return "{MessagePack.Read(bytes, depth)}"
    Catch e As ParseError
        Return "{e.Kind}"
    End Try
End Function

Function Text(text: String, depth: Int): String
    Try
        Return "{Json.Read(text, depth)}"
    Catch e As ParseError
        Return "{e.Kind}"
    End Try
End Function

Print(Text("[[[]]]", 2), Packed([145, 145, 144] As UInt8[], 2))
Print(Text("[[]]", 1), Packed([145, 144] As UInt8[], 1))
Print(Text("[{{}}]", 1), Packed([145, 128] As UInt8[], 1))
```

Expected: OUT_OF_RANGE OUT_OF_RANGE
OUT_OF_RANGE OUT_OF_RANGE
OUT_OF_RANGE OUT_OF_RANGE

Actual: OUT_OF_RANGE [[[]]]
OUT_OF_RANGE [[]]
OUT_OF_RANGE [{}]

Suggested fix: In Opened, check depth for every array or map read (if read is an ARRAY or OBJECT and Open.Count >= Most, throw), and return without pushing only after that check when Pending is 0. Add [145,145,144] at depth 2 and [145,144] at depth 1 to data_msgpack.

Skeptic: Ran the program (plus [[1]] at depth 1) in debug and release: 'OUT_OF_RANGE [[[]]]', 'OUT_OF_RANGE [[]]', 'OUT_OF_RANGE [{}]', while [145,145,1] at depth 1 gives OUT_OF_RANGE. So a non-empty innermost container counts toward depth and an empty one doesn't. The cause is std/data/msgpack.fb's Opened (about line 293): `If (!read.IsArray && !read.IsObject) || Pending == 0 Then Return` runs before the `Open.Count >= Most` check. Read's doc says OUT_OF_RANGE is 'for arrays and maps nested past depth', and Json.Read refuses the same documents. It's low because an empty container can't nest further, so the stack bound still holds. Only the limit is off by one, and it disagrees with JSON.

## msgpack-2 (low): Step 19's milestones row and a decisions.md row still promise @Since and @Default, which don't exist

Status: fixed in b6c21913 (step 19's milestones row says what was built: Load and Save with @Rename,
@Skip, @Flatten, Loadable and Saveable, and versioned saves through @Renamed, a Field's default, and
OnLoaded; decisions.md's MessagePack row and Value row no longer name @Since or @Default)

Where: docs/milestones.md:188

Step 19's scope in docs/milestones.md says "`@Rename`, `@Skip`, `@Default`, `@Flatten`, and custom Encode/Decode; MessagePack, with `@Since` and `@Renamed` for versioned saves". docs/decisions.md line 620 also says "MessagePack is std's binary format, with `@Since` and `@Renamed` keeping old saves loading", and line 772 says Load/Save honor "@Default". std/core/load.fb declares only Rename, Skip, Flatten, and Renamed. stdlib.md's design says a Field's own default and OnLoaded do the work instead. This step closes, so its row and these decisions say something the code doesn't do.

```
Import std.core
Import std.data

Struct Profile
    Field Name: String
    @Since(2) @Default(1) Field Level: Int
End Struct

Local p = Try MessagePack.Read<Profile>(MessagePack.Write(Profile("Ada", 3)))
Print(p.Name, p.Level)
```

Expected: Either it builds and prints "Ada 3", as the milestones row and decisions.md describe, or the docs no longer name @Since and @Default.

Actual: t5.fb:6:6: error: '@Since' isn't an attribute; an Attribute declaration makes one
t5.fb:6:16: error: expected identifier, found 'Default'

Suggested fix: Change step 19's milestones row to what was built (@Rename, @Skip, @Flatten, @Renamed, a Field's default, OnLoaded, Loadable/Saveable). Supersede decisions.md line 620's @Since and line 772's @Default with a note pointing at the step 17/19 rows, or list @Since in the Deferred work if it's still wanted.

Skeptic: The program fails to build: "'@Since' isn't an attribute" and "expected identifier, found 'Default'". docs/milestones.md:188 (step 19's row, which this change leaves alone) still lists `@Default` and "MessagePack, with `@Since` and `@Renamed` for versioned saves". docs/decisions.md:620 still says MessagePack has "`@Since` and `@Renamed` keeping old saves loading", and :772 still says Load/Save honor @Default. stdlib.md:55 lists only Rename, Skip, Flatten, and Renamed in std/core/load.fb, and stdlib.md:871 and spec 13:259 use a Field's default instead. The step is closing, so its row and these decisions describe attributes that don't exist.

## Refuted by the skeptics (no action)

- msgpack-3: A 4-character numeric String loads into a UInt8[] as base64, while UInt8 and Int[] read it as the number. I reproduced the output: "0012", "0255", and "+127" load into a UInt8[] as base64 ([211, 77, 118] and so on), and "7" loads as [7]. But spec 13 (docs/spec/13-compile-time.md, about lines 241-244) says plainly that a UInt8[] is taken from 'a String of their standard base64, with its = padding', and "0012" is valid padded base64, so Load does what the spec says. A String that is both a number and base64 is ambiguous by nature, and the spec picks base64 for UInt8[]. The lane's claim 'no number from 0 to 255 is padded base64' holds for numbers written plainly; "0012" has leading zeros. The decisions.md:774 row's 'is still WRONG_TYPE' reads as 'goes on as before', where "ab" is WRONG_TYPE and "7" is [7]; the lane notes say this too. That wording is ambiguous, not a contradiction, and the behavior matches the spec.
- msgpack-4: Save writes a @Flags value with no flags as the STRING "0", and data_serialize's expected output locks it in. I reproduced 'STRING "0" INT 66' and '[161, 48] [66]', and Load<Status>(Save(none)) gives back 0. Spec 13 says an Enum saves as 'its member's name, as its text has it', with an integer only 'for a @Flags value with bits no member has'. A value with no flags has no such bits, so the integer rule doesn't cover it. Its text (spec 06:441-442) is "0", and compiler/src/lower.cpp save_enum writes the Enum's text when no unnamed bits are set, which matches 'as its text has it'. No spec or stdlib.md text names INT 0 or "" for the no-flags case, and the value round-trips. The code is lane 4's Save, from the base commit, not this step's. It looks odd but plainly contradicts nothing, so I refute it. It could go to the user as a design question.
