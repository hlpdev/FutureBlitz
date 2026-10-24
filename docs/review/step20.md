# The review of M6 step 20's formats (std.data II): TOML 1.0, CSV, INI, XML 1.0 (a streaming reader, a tree, a writer, XmlPath, and Value conversion), and YAML 1.2's core schema, each over std.core's Value (Protocol Buffers is another lane's)

39 confirmed findings: 2 high, 14 medium, 23 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## xml-2 (high): Xml.Write's default indent adds whitespace text to mixed content written before its first text

Status: fixed in f625602: Xml.Write tells the writer an element holds text before writing its children, so nothing in it is indented, before its first text either; data_xml.fb writes four mixed elements indented and reads each back as the same <p>, which gained "\n  " before

Where: std/data/xml.fb:528

Xml.Write's comment and the stdlib.md bullet say that an element that holds text isn't indented and keeps its own spaces. XmlWriter only marks an element as mixed when its first Text or CData call arrives, and Xml.Write writes the tree through it in order. Any element, comment, or PI that comes before the element's first text is still put on a new indented line, which adds "\n  " text inside the mixed element. With the default indent (2), Xml.Write(Xml.Read(doc)) changes the text of ordinary mixed content such as <p><b>bold</b> text</p>. Xml.Write has the whole tree and can tell up front which elements hold text.

```
Import std.core
Import std.data.xml

Local root = Try Xml.Read("<p><b>bold</b> text</p>")
Print("[" + root.Text + "]")
Local text = Xml.Write(root, 2, False)
Print(text)
Local back = Try Xml.Read(text)
Print("[" + back.Text + "]")
Print(back.Text == root.Text)
Local c = Try Xml.Read("<p><!--note-->text</p>")
Print(Xml.Write(c, 2, False))
Print("[" + (Try Xml.Read(Xml.Write(c, 2, False))).Text + "]")
```

Expected: [bold text]
<p><b>bold</b> text</p>
[bold text]
True
<p><!--note-->text</p>
[text]

Actual: [bold text]
<p>
  <b>bold</b> text</p>
[
  bold text]
False
<p>
  <!--note-->text</p>
[
  text]

Suggested fix: In Xml.WriteNode, check whether an element has any non-empty TEXT or CDATA child before writing its children, and tell the writer the element is mixed from its start (for example through a Package method or a flag on StartElement). Say in XmlWriter's comment that, when streaming, only content after the first text goes unindented. Add a test that reads back a written mixed element.

Skeptic: Ran x2.fb (debug). Xml.Write(Xml.Read("<p><b>bold</b> text</p>"), 2, False) gave "<p>\n  <b>bold</b> text</p>", which reads back as "[\n  bold text]" (False). The comment case gave "<p>\n  <!--note-->text</p>". stdlib.md says Xml.Write is "indented but in an element that holds text", and Write's own comment says the same. XmlWriter.Content (xml.fb:528-532) checks Mixed, but Mixed only becomes True at the first Text/CData call, so anything written before the first text gets a NewLine. This silently changes the text of ordinary mixed content under the default indent.

## xml-4 (high): No limit on entity expansion: a 617-byte billion-laughs document expands to 30 MB, and the 9-level one to 3 GB

Status: fixed in f625602: XmlReader counts the bytes of entities' text it reads, in content, attribute values, and the internal subset's parameter entities, and throws past ten times the document's size and a MiB; data_xml_limits.fb gives the 617-byte billion laughs in text and in an attribute, and a parameter entity read 20000 times, each a ParseError at once, which ran for 24 s and 30 MB before, and still reads an entity a thousand times

Where: std/data/xml_reader.fb:335

Enter only limits nesting depth (64) and rejects self-reference. Nothing caps the total replacement text an entity expands to, or the number of expansions. Attribute values (AttributeChar, line 452) and parameter entities read as declarations (SubsetReference, line 606) have no cap either; a 634-byte PE version with 10^6 expansions also runs. Ten references at each of 7 levels turn 617 bytes into 30,000,000 bytes of text, about 24 s in a release build. Each further level multiplies time and memory by 10, so the classic 9-level document exhausts memory. expat, libxml2, .NET, and Java all stop this with an amplification or total-size limit. The review scope names this limit, but std.data has none and no test.

```
Import std.core
Import std.collections
Import std.data.xml

// A billion laughs cut to 10^7: 617 bytes that name "lol" ten million times.
Local parts: String[] = ["<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol0 \"lol\">"]
For i = 1 To 7
    Local refs = ""
    For j = 1 To 10
        refs = refs + "&lol{i - 1};"
    End For
    parts.Add("<!ENTITY lol{i} \"{refs}\">")
End For
parts.Add("]><lolz>&lol7;</lolz>")
Local text = parts.Join("")
Try
    Local root = Try Xml.Read(text)
    Print(text.Bytes.Count, "bytes read into", root.Text.Bytes.Count, "bytes of text")
Catch e As ParseError
    Print(e.Message)
End Try
```

Expected: A ParseError from an expansion limit, for example "entities expand to more than ... at line 1, column ...", reached quickly

Actual: 617 bytes read into 30000000 bytes of text   (about 24 s in release; one more level is 10x that, and 9 levels need about 3 GB)

Suggested fix: Count the bytes and characters that entity expansion produces (content, attribute values, and parameter-entity text) and throw a ParseError past a limit. Use both an absolute cap and a ratio to the document's size, as libxml2 does (for example 10x the input plus a few MB). Add tests for the text, attribute, and parameter-entity forms.

Skeptic: Ran x4.fb with --release. It printed `617 bytes read into 30000000 bytes of text` after real 0m23.58s. xml_reader.fb's Enter (335-341) only checks self-reference and Sources.Count > 64, and AttributeChar and SubsetReference have no cap either. Nothing bounds total expansion, so each extra level costs 10x time and memory: a classic billion-laughs DoS on any XML text a program reads.

## csv-ini-1 (medium): Ini.Write writes keys with tabs at their ends and section names with spaces or tabs at their ends, which Ini.Read trims

Status: fixed in 722e11f: Ini.Write panics at a key that starts or ends with a tab, as with a space, and at a section's name that starts or ends with either, and its comment lists them; panic_data_ini_key_tab.fb and panic_data_ini_section_space.fb

Where: std/data/ini.fb:138

Ini.Read trims spaces and tabs around keys and section names (SkipSpace and TrimEnd treat 32 and 9 alike). Ini.Write's key check (AddEntry) only panics at a space at either end, not a tab. Its section-name check (Write, lines 114-118) only rejects ']' and line breaks, not spaces or tabs at the ends. Write therefore produces text that silently reads back as different data. A key "\tindented" reads back as "indented". The sections " s " and "s" both read back as "s", so the second one's keys overwrite the first's. A key of just "\t" gives text that Read rejects. Ini.Write's doc comment also leaves out the panic it already has for a key that starts or ends with a space.

```
Import std.core
Import std.data.ini

Local document = Value.Object()
document["\tindented"] = Value("1")
Local spaced = Value.Object()
spaced["x"] = Value("1")
document[" s "] = spaced
Local plain = Value.Object()
plain["x"] = Value("2")
document["s"] = plain
Local written = Ini.Write(document)
Local again = Try Ini.Read(written)
Print(again == document)
Print(again)

Local tab = Value.Object()
tab["\t"] = Value("v")
Try
    Ini.Read(Ini.Write(tab))
    Print("reads")
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try
```

Expected: Ini.Write panics at the key "\tindented", as it does for " a" (and at the section " s "), or the text reads back as the same document: True

Actual: False
{"indented": "1", "s": {"x": "2"}}
SYNTAX a key is missing before its '=', at line 1
(the same in --release)

Suggested fix: In AddEntry, panic for a key whose first or last byte is a space or a tab (9), not only a space. In Write's section loop, panic for a section name whose first or last byte is a space or a tab. Update Ini.Write's doc comment to list these rules (and the space rule it already has). Add panic tests for a tab-ended key and a space-ended section name.

Skeptic: I ran the finding's program as csvini-refute/f1.fb, in debug and in --release. Write produced "\tindented = 1\n\n[ s ]\nx = 1\n\n[s]\nx = 2\n". The output then printed False, {"indented": "1", "s": {"x": "2"}} and "SYNTAX a key is missing before its '=', at line 1", the same in both builds. In ini.fb, AddEntry (lines 138-139) rejects a space at a key's ends but not a tab. Yet Read's SkipSpace and TrimEnd (lines 184-199) trim spaces and tabs alike. AddEntry's own value quoting (lines 171-175) already treats 9 like 32. Write's section check (lines 114-118) rejects only ']', CR and LF, while Read trims the name inside the brackets (lines 47-48). So " s " and "s" collide, and the first section's keys are silently lost. stdlib.md says Ini.Write "panics at what INI can't hold", and the data_ini test says Write's output reads back as the same, so this breaks the documented contract. The doc comment at lines 96-101 also leaves out the space rule that AddEntry already enforces. Medium, because Write loses data without any error.

## toml-1 (medium): Dates and times keep the trailing 0s of their fraction, which docs/stdlib.md and the lane's design say are dropped

Status: fixed in 44ab0b8: Time() leaves out a fraction's 0s at its end, and its '.' when they're all it has, as stdlib.md says; data_toml.out's times read "1979-05-27T07:32:00.5Z", "...00.12-07:00", and "07:32:00", which kept their 0s before

Where: std/data/toml.fb:917

docs/stdlib.md's TOML bullet says a date or time reads as its RFC 3339 text "with 'T' and 'Z', and its fraction of a second without the 0s at its end". The lane's own call says the same, and so does make_data.py's moment(), which removes the '.' too when only 0s are left. But Time() copies the fraction exactly as it's written (out.AddAll(Bytes[start..At])). toml.fb's header comment ("as it's written otherwise") agrees with the code, not with the docs. tests/programs/data_toml.out locks in the untrimmed text ("1979-05-27T07:32:00.500Z", "07:32:00.000"). toml-test still passes only because none of its inputs has a trailing 0: milliseconds.toml has .6 while its JSON has .600, which make_data.py trims to match.

```
Import std.core
Import std.data.toml

// docs/stdlib.md: a date or time is a String of its RFC 3339 text, "with 'T' and 'Z', and its
// fraction of a second without the 0s at its end".
Local read = Try Toml.Read("a = 07:32:00.000\nb = 1979-05-27T07:32:00.500Z\nc = 1979-05-27 07:32:00.120-07:00\nd = 1987-07-05T17:45:56.600+08:00")
For key In read.Keys()
    Print(key, read[key] ?? Value())
End For
```

Expected: a "07:32:00"
b "1979-05-27T07:32:00.5Z"
c "1979-05-27T07:32:00.12-07:00"
d "1987-07-05T17:45:56.6+08:00"

Actual: a "07:32:00.000"
b "1979-05-27T07:32:00.500Z"
c "1979-05-27T07:32:00.120-07:00"
d "1987-07-05T17:45:56.600+08:00"

Suggested fix: In Time(), after reading the fraction's digits, drop the trailing '0's, and the '.' when none are left, as make_data.py's moment() does. Then update toml.fb's header comment and data_toml.out, and add a case with a trailing 0 to data_toml.fb.

Skeptic: Ran rv-step20/toml/t1.fb (the finding's program) in debug and release. Both printed: a "07:32:00.000", b "1979-05-27T07:32:00.500Z", c "1979-05-27T07:32:00.120-07:00", d "1987-07-05T17:45:56.600+08:00". docs/stdlib.md's Toml.Read bullet, added in this change, says a date or time reads as its RFC 3339 text "with 'T' and 'Z', and its fraction of a second without the 0s at its end", and the lane's call says the same. toml.fb Time() (lines 917-925) copies the fraction's digits exactly as written with out.AddAll(Bytes[start..At]) and trims nothing. tests/programs/data_toml_suite/make_data.py moment() does trim the trailing 0s, and drops the '.' when no digits are left, so the suite's expected data agrees with the docs while the reader doesn't. tests/programs/data_toml.out line 10 locks in the untrimmed "...07:32:00.500Z" and "07:32:00.000".

## toml-3 (medium): Toml.Read and Toml.Write recurse per nesting level and crash with a stack overflow on a few KB of nested arrays

Status: fixed in 44ab0b8: arrays and inline tables nest 256 deep at most, a ParseError past that when read and a panic when written, and Write writes its tables with a stack of its own, however deep; data_toml.fb reads and writes back 256 deep, throws at 257, 300 inline tables, and 5000 arrays deep, and writes back tables 1000 deep, and panic_data_toml_deep.fb panics at 257, where 2000 and 3000 deep crashed before

Where: std/data/toml.fb:457

ReadValue, ArrayValue, and InlineTable recurse once per level of nesting, and so does WriteInline (line 67). Neither has a depth limit. A 4 KB document of arrays nested 2000 deep kills a debug program with 0xC00000FD (stack overflow): no ParseError, no panic, and output already printed is lost. Release crashes at 5000 deep. Toml.Write crashes on a Value nested 3000 deep in debug and 20000 deep in release. std.core's Value goes the other way on purpose: its docs say ToString, ==, Hash, and Clone "go through a Value however deep without recursing". So a Value that prints fine crashes Toml.Write, and untrusted TOML text can crash the reader.

```
Import std.core
Import std.collections
Import std.data.toml

// A 4 KB document of arrays nested 2000 deep, and a Value nested 20000 deep, which ToString,
// ==, Hash, and Clone go through without recursing.
Local parts: String[] = ["a = "]
For i = 1 To 2000
    parts.Add("[")
End For
For i = 1 To 2000
    parts.Add("]")
End For
Try
    Local read = Toml.Read(parts.Join(""))
    Print("read", read.Count)
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try

Local root = Value.Object()
Local at = Value.Array()
root["a"] = at
For i = 1 To 20000
    Local inner = Value.Array()
    at.Add(inner)
    at = inner
End For
Print(root.ToString().Bytes.Count)
Print(Toml.Write(root).Bytes.Count)
```

Expected: read 1
40009
40007
(or a ParseError and a panic that say the nesting is too deep, but no crash)

Actual: Debug: "error: f3.exe exited with code 0xC00000FD" (stack overflow in Toml.Read), and nothing is printed. Release: the same exit code, from Toml.Write at 20000 deep, and nothing is printed.

Suggested fix: Read nested arrays and inline tables with an explicit stack of open containers, or set a nesting limit that throws a ParseError. Write with an explicit stack, as Value.ToString does. Add a deep case to the tests.

Skeptic: Ran each depth as its own program, because output buffered before the crash is lost. Reading 'a = ' followed by N '[' and N ']': debug reads 1000 deep but crashes at 2000 with "error: r2000.exe exited with code 0xC00000FD". Release reads 2000 deep and crashes at 5000. Toml.Write on an Array nested N deep: debug writes 1000 deep (2007 bytes) and crashes at 3000. Release writes 3000 deep and crashes at 10000 and 20000. All crashes are 0xC00000FD, with no ParseError and no panic. Value.ToString and Clone/== on the same 20000-deep Value work fine (t3s.fb printed 40009 and True). That matches value.fb's header, which says ToString, ==, Hash, and Clone go through a Value however deep without recursing. ReadValue, ArrayValue, InlineTable, and WriteInline recurse once per level and have no depth limit. A 4 KB untrusted document crashing the process is wrong behavior, and the project has fixed crashes like this elsewhere (decisions.md, step 4: release chains that ran out of stack).

## toml-4 (medium): Reading an array of tables is quadratic in its length: NestOf copies the whole array for every key

Status: fixed in 44ab0b8: NestOf goes into an array of tables' last table through Value's index, without copying the array; program_tests.cpp's "TOML's array of tables reads in time in proportion to its length" reads 20000 tables of four keys in a debug build in under 10 seconds, which took about 6 in release before

Where: std/data/toml.fb:420

NestOf goes into an array of tables' last table through ElementsOf(next), which is value.AsArray(). That gives a new copy of every element (AsArray is documented as "a new array of the Value's own elements"). NestOf runs for every key/value line and every [table] header under an [[array]], so each one copies all the entries so far. Measured in release: 40000 four-key [[package]] entries (3.4 MB) take 26.7 s, against 0.6 s for the same content as separate [pN] tables. Each doubling of the entries takes about 4.4 times as long.

```
Import std.core
Import std.collections
Import std.time
Import std.data.toml

// The same entries, four keys each, as an array of tables and as tables of their own: reading
// the array of tables doubles takes four times as long as its entries double.
Function Document(count: Int, array: Bool): String
    Local parts: String[] = []
    For i = 1 To count
        parts.Add(array ? "[[package]]\n" : "[p{i}]\n")
        parts.Add("name = \"p{i}\"\nversion = \"1.0.{i}\"\nsource = \"registry\"\nchecksum = \"abc\"\n")
    End For
    Return parts.Join("")
End Function

For count In [10000, 20000, 40000]
    Local tables = Document(count, False)
    Local array = Document(count, True)
    Local watch = Stopwatch.StartNew()
    Local a = Try Toml.Read(tables)
    Local plain = watch.Elapsed.TotalSeconds
    watch.Restart()
    Local b = Try Toml.Read(array)
    Local listed = watch.Elapsed.TotalSeconds
    Print(count, "tables:", plain, "array of tables:", listed)
End For
```

Expected: The array-of-tables times grow linearly and stay close to the plain-table times (well under 2 s at 40000 entries).

Actual: (--release)
10000 tables: 0.1952685 array of tables: 1.3956917
20000 tables: 0.2996675 array of tables: 5.9495084
40000 tables: 0.6210006 array of tables: 26.6721548

Suggested fix: In NestOf, take the last element without copying: `If next.Count == 0 Then Return Null` and then `next = next[next.Count - 1] ?? Value()` (Value's index operator), instead of ElementsOf(next).

Skeptic: Ran rv-step20/toml/t4.fb with --release, the finding's program at 5000/10000/20000 entries with a check that every entry was read. Results: 5000 entries, tables 0.073 s vs array of tables 0.296 s; 10000, 0.148 s vs 1.154 s; 20000, 0.290 s vs 6.883 s. Plain tables grow linearly, while the array of tables grows about 4-6x per doubling. In the code, NestOf (toml.fb lines 419-422) calls ElementsOf(next), which text.fb line 17 defines as value.AsArray(). value.fb line 210 shows AsArray returning Elements?.Clone(), so every key/value line and header under an [[array]] copies the whole array so far.

## xml-1 (medium): XmlWriter.CData splits "]]>" with one ']' too many, so the text reads back changed

Status: fixed in f625602: CData writes "]]]]><![CDATA[>" for each "]]>" and goes past all three bytes; data_xml.fb writes five texts holding "]]>" and "]]" and reads each back as it was

Where: std/data/xml.fb:450

When the text holds "]]>", CData writes "]]]]><![CDATA[" in place of the first ']' and then writes the second ']' and the '>' as usual. That gives "]]]]><![CDATA[]>", which reads back as "]]" + "]>", i.e. "]]]>". Xml.Write uses the same path, so any CDATA node holding "]]>" comes back with an extra ']'. data_xml.fb doesn't catch it: its round-trip check compares Write(Read(Write(t))) with Write(t), and both sides carry the same extra ']'. Nothing checks the text that reads back.

```
Import std.core
Import std.data.xml

Local root = XmlNode("a")
root.Add(XmlNode.NewCData("x ]]> y"))
Local text = Xml.Write(root, 0, False)
Print(text)
Local back = Try Xml.Read(text)
Print("[" + back.Text + "]")
Print(back.Text == "x ]]> y")
```

Expected: <a><![CDATA[x ]]]]><![CDATA[> y]]></a>
[x ]]> y]
True

Actual: <a><![CDATA[x ]]]]><![CDATA[]> y]]></a>
[x ]]]> y]
False

Suggested fix: When a "]]>" starts at i, write "]]]]><![CDATA[>" and step past all three bytes, or write "]]]><![CDATA[" in place of the first ']' only. Add a test that reads the written CDATA back and compares its Text with the original string.

Skeptic: Ran x1.fb (debug). It printed `<a><![CDATA[x ]]]]><![CDATA[]> y]]></a>`, then `[x ]]]> y]`, then False. At xml.fb:449-451, when bytes[i..i+2] is "]]>", CData writes "]]]]><![CDATA[" and then `Continue`s past only that one byte. The second ']' and the '>' are still copied, so the text reads back with an extra ']'.

## xml-10 (medium): XmlPath gives '|' the lowest precedence; XPath gives it the highest, so i[a | b = '2'] means something else

Status: fixed in f625602: '|' joins a comparison's operands, Primary's, so it binds tighter than the comparisons, and the top rule and predicates are Or; data_xml.fb's i[a | b = '2'] gives the second i alone, and panic_data_xml_path_union.fb panics at '|' joining what isn't a path

Where: std/data/xml_path.fb:481

In XPath 1.0's grammar, UnionExpr sits below UnaryExpr, so '|' binds tighter than =, <, and, and or. PathParser.Union is the top rule and calls Or for each operand. i[a | b = '2'] therefore parses as a | (b = '2'), and the predicate is true for any i that has an a. XPath reads it as (a | b) = '2'. The same goes for a | b and c, and so on.

```
Import std.core
Import std.collections
Import std.data.xml

Local root = Try Xml.Read("<r><i><a>1</a></i><i><b>2</b></i><i><a>3</a><b>4</b></i></r>")
Function Show(root: XmlNode, path: String)
    Local found: String[] = []
    For node In root.Query(path)
        found.Add(node.Text)
    End For
    Print(path, "=>", found.Join(" "))
End Function

// XPath binds '|' tighter than '=', so this is (a | b) = '2': the second i, and no other.
Show(root, "i[a | b = '2']")
Show(root, "i[(a | b) = '2']")
Show(root, "i[a | b][1]")
```

Expected: i[a | b = '2'] => 2
i[(a | b) = '2'] => 2
i[a | b][1] => 1

Actual: i[a | b = '2'] => 1 34
i[(a | b) = '2'] => 2
i[a | b][1] => 1

Suggested fix: Parse unions where comparisons take their operands: Comparison calls a UnionOperand that reads Primary and then, while '|' follows, more Primaries. The top-level rule becomes Or. Keep the IsPath check for the whole path.

Skeptic: Ran x10.fb (debug). `i[a | b = '2']` gave `1 34`, while `i[(a | b) = '2']` gave `2`. PathParser.Union (xml_path.fb:481-493) is the top rule and parses each operand with Or(), and predicates call Union() (line 740), so '|' has the lowest precedence. In XPath 1.0, UnionExpr sits under UnaryExpr, below the comparisons, so it binds tighter than = and and/or. A valid XPath expression silently gets a different meaning, where the subset should either match XPath or reject the expression.

## xml-3 (medium): Printing an XmlNode that isn't an element panics, and the panic points inside std

Status: fixed in f625602: XmlNode's ToString writes each kind as XML: text escaped, a CDATA section, a comment, a processing instruction, or an attribute as name="value", and it's @PanicsAtCaller; data_xml.fb prints each kind a query gives, and panic_data_xml_node_string.fb panics at the caller's line

Where: std/data/xml.fb:130

XmlNode implements Printable through ToString, which is Xml.Write(Self, 0, False), and Xml.Write panics when root isn't an ELEMENT. Printing a TEXT, CDATA, COMMENT, INSTRUCTION, or ATTRIBUTE node therefore panics. Those are exactly what queries such as "//@id" or "b/text()" return. ToString isn't @PanicsAtCaller either, so the panic names std/data/xml.fb:130 and not the program's line.

```
Import std.core
Import std.data.xml

// XmlNode is Printable, but its ToString is Xml.Write's, which panics at a node that isn't an
// element: printing what a query of text() or @id gives panics, inside std.
Local root = Try Xml.Read("<a id='1'><b>hi</b></a>")
For node In root.Query("@id | b/text()")
    Print(node.Kind)
    Print(node)
End For
```

Expected: ATTRIBUTE
id="1" (or the attribute's value)
TEXT
hi

Actual: ATTRIBUTE
panic: an XML document's root is an element, not ATTRIBUTE at std/data/xml.fb:130:41

Suggested fix: Have ToString write any kind of node as XML: escaped text for TEXT, <![CDATA[...]]>, <!--...-->, <?t ...?>, and name="value" for an ATTRIBUTE. A Package writer path that allows a non-element at the top would do it. Add a test that prints each kind.

Skeptic: Ran x3.fb (debug). It printed ATTRIBUTE, then `panic: an XML document's root is an element, not ATTRIBUTE at std/data/xml.fb:130:41` (exit 101). XmlNode Implements Printable, and its ToString (xml.fb:130) is Xml.Write(Self, 0, False), which panics for any kind that isn't ELEMENT (line 202). ToString has no @PanicsAtCaller, so the panic names std's line. Print or interpolation of any TEXT, CDATA, COMMENT, INSTRUCTION, or ATTRIBUTE node (what text() and @x queries give) crashes. I rate it medium, not high, because .Content and .Text give the value.

## xml-5 (medium): Entities in an attribute value nest without a limit and crash the process on a stack overflow

Status: fixed in f625602: an attribute value's entities are read through the reader's sources, as content's are, through Enter, whose 64-deep limit and expansion count they get, without recursing; data_xml_limits.fb gives a 6000-deep chain in an attribute the 64-deep ParseError, which crashed before, and reads a 60-deep one

Where: std/data/xml_reader.fb:452

In content, Enter throws a ParseError past 64 nested entities. In an attribute value, AttributeChar recurses once for each nested entity and clones the list of open entities at every level, with no depth limit. A chain of 6000 distinct entities (about 150 KB of text) used in an attribute ends the process with a stack overflow (0xC00000FD) in debug and with "panic: out of memory" in release. The same chain used in content gets the ParseError. In debug, 3000 levels is already enough to crash.

```
Import std.core
Import std.collections
Import std.data.xml

// Entities nested 6000 deep: in content, Enter throws past 64; in an attribute's value,
// AttributeChar recurses once for each, with no limit.
Function Chain(depth: Int, attribute: Bool): String
    Local parts: String[] = ["<!DOCTYPE a [<!ENTITY e0 'x'>"]
    For i = 1 To depth
        parts.Add("<!ENTITY e{i} '&e{i - 1};'>")
    End For
    parts.Add(attribute ? "]><a b='&e{depth};'/>" : "]><a>&e{depth};</a>")
    Return parts.Join("")
End Function

For attribute In [False, True]
    Try
        Local root = Try Xml.Read(Chain(6000, attribute))
        Print(attribute, "reads:", root.Attribute("b") ?? root.Text)
    Catch e As ParseError
        Print(attribute, e.Message)
    End Try
End For
```

Expected: False entities are nested more than 64 deep at line 1, column 147825
True entities are nested more than 64 deep at line 1, column ...

Actual: release: False entities are nested more than 64 deep at line 1, column 147825
panic: out of memory
(debug: the process exits with 0xC00000FD, a stack overflow)

Suggested fix: In AttributeChar, apply Enter's limit: throw the same ParseError once entities.Count passes 64, or drive the attribute's entities through the Sources stack iteratively the way content does. That also removes the per-level Clone.

Skeptic: Ran x5.fb (6000-deep chain). Debug: the process exited with 0xC00000FD. Release: `False entities are nested more than 64 deep at line 1, column 147825`, then `panic: out of memory` (exit 101). A 100-deep chain (x5b.fb) gives the 64-deep ParseError in content, but in an attribute it reads 'x' with no error. AttributeChar (xml_reader.fb:452-459) recurses once per entity and clones `entities` each level, with no depth limit.

## xml-6 (medium): Text, Xml.Write, and Xml.ToValue recurse once per level and overflow the stack on documents a few thousand elements deep; Query's keys grow quadratically

Status: fixed in f625602: Text, Xml.Write, ToValue, FromValue, and XmlPath's descendants go through stacks of their own, and a query's places compare by numbers Query gives the tree's nodes in its order, not cloned keys; ToValue puts each element's Value in its parent's before filling it, as Value looks through what's put in it; data_xml_limits.fb reads, writes, queries, and converts a tree 10,000 deep, which crashed before

Where: std/data/xml.fb:132

Xml.Read builds the tree without recursing and reads 100,000 levels fine. After that, XmlNode.Text (AddValue), Xml.Write (WriteNode), and Xml.ToValue each recurse once per level. A 70 KB document nested 10,000 deep crashes each of them with 0xC00000FD in both debug and release. ToValue already crashes at 2,000 levels in debug and Write at 5,000. Query("//a") on the same tree panics with out of memory, because XmlPlace.Child clones the whole key path for every place (xml_path.fb:73), which is O(depth^2). std otherwise avoids this: Value's ToString, ==, and Clone go however deep without recursing, and the runtime made release non-recursive because a few thousand linked objects ran out of stack.

```
Import std.core
Import std.collections
Import std.data.xml

// Elements nested 10,000 deep, 70 KB of text: Xml.Read builds the tree without recursing, but
// Text, Write, and ToValue recurse once a level, and Query's places copy their keys.
Local parts: String[] = []
For i = 1 To 10000
    parts.Add("<a>")
End For
parts.Add("x")
For i = 1 To 10000
    parts.Add("</a>")
End For
Local root = Try Xml.Read(parts.Join(""))
Print(root.Text)
Print(Xml.Write(root, 0, False).Bytes.Count)
Print(Xml.ToValue(root).Kind)
Print(root.Query("//a").Count)
```

Expected: x
70001
OBJECT
10000

Actual: the process exits with 0xC00000FD (stack overflow) in debug and release. Run alone, the Write and ToValue lines each do the same, and the Query line gives "panic: out of memory".

Suggested fix: Walk the tree with an explicit stack in AddValue, WriteNode, ToValue, FromValue, and PathEvaluator.Descendants. Give XmlPlace a parent link plus a child index (comparing by walking up), or a document-order number, in place of a cloned key array. Otherwise set a documented nesting limit in XmlReader that throws a ParseError.

Skeptic: Ran each operation alone on a tree 10,000 elements deep (x6_*.fb). root.Text, Xml.Write, and Xml.ToValue each exit with 0xC00000FD in both debug and release. Query("//a") gives `panic: out of memory` in debug (release gives 10000). Xml.Read itself succeeds: the query program printed "read". AddValue (xml.fb:132), WriteNode (210), ToValue (238), and Descendants (xml_path.fb:354) recurse once per level, and XmlPlace.Child clones the whole key (xml_path.fb:74). decisions.md (step 4 row) treats running out of stack on a few thousand linked objects as a bug, and Value walks without recursing.

## xml-7 (medium): With standalone='yes', declarations after an unread parameter entity are dropped, so well-formed documents are rejected

Status: fixed in f625602: SubsetReference stops taking declarations past an external parameter entity only in a document that isn't standalone; data_xml_rules.fb reads both of the finding's standalone documents

Where: std/data/xml_reader.fb:600

XML 1.0 section 5.1: "Except when standalone=\"yes\", they MUST NOT process entity declarations or attribute-list declarations encountered after a reference to a parameter entity that is not read ...; when standalone=\"yes\", processors MUST process these declarations." SubsetReference sets Stopped for an external parameter entity whatever Standalone is. In a standalone document, an entity declared after %e; is then ignored, and a reference to it is the 'isn't declared' error, because the document is standalone. Attribute defaults declared there are lost the same way.

```
Import std.core
Import std.data.xml

// XML 1.0's 5.1: past a parameter entity it doesn't read, a processor doesn't take declarations,
// "except when standalone='yes'".
Function Show(text: String)
    Try
        Local root = Try Xml.Read(text)
        Print("reads:", root.Text, root.Attribute("b") ?? "-")
    Catch e As ParseError
        Print(e.Message)
    End Try
End Function

Show("<?xml version='1.0' standalone='yes'?><!DOCTYPE a [<!ENTITY % e SYSTEM 'e.dtd'>%e;<!ENTITY x 'y'>]><a>&x;</a>")
Show("<?xml version='1.0' standalone='yes'?><!DOCTYPE a [<!ENTITY % e SYSTEM 'e.dtd'>%e;<!ATTLIST a b CDATA 'v'>]><a/>")
```

Expected: reads: y -
reads:  v

Actual: the entity &x; isn't declared at line 1, column 106
reads:  -

Suggested fix: In SubsetReference, set Stopped only when !Standalone. Add both cases to data_xml_rules.fb.

Skeptic: Ran x7.fb (debug). With standalone='yes', `<!DOCTYPE a [<!ENTITY % e SYSTEM 'e.dtd'>%e;<!ENTITY x 'y'>]><a>&x;</a>` gives `the entity &x; isn't declared at line 1, column 106`, and the ATTLIST case gives `reads:  -` (no default v). SubsetReference (xml_reader.fb:600-603) sets Stopped for an external PE whatever Standalone is. XML 1.0 section 5.1 says processors MUST process those declarations when standalone="yes". The document is well-formed (x is declared in the internal subset outside any PE), yet it is rejected.

## xml-9 (medium): XmlWriter.Attribute writes the same attribute twice, which isn't well-formed

Status: fixed in f625602: XmlWriter keeps the open start tag's attributes' names and panics at one given twice; panic_data_xml_writer_twice.fb

Where: std/data/xml.fb:388

XmlWriter is documented to panic at what XML can't hold, but Attribute doesn't track the names already written on the open start tag. Calling Attribute("x", ...) twice writes <a x="1" x="2"/>. XmlReader rejects that document, and so would any other XML parser.

```
Import std.core
Import std.data.xml

Local writer = XmlWriter()
writer.StartElement("a")
writer.Attribute("x", "1")
writer.Attribute("x", "2")
writer.EndElement()
Local text = writer.Finish()
Print(text)
Try
    Xml.Read(text)
    Print("reads")
Catch e As ParseError
    Print(e.Message)
End Try
```

Expected: panic: the attribute x is written twice at <program>:7:1

Actual: <a x="1" x="2"/>
the attribute x is written twice at line 1, column 15

Suggested fix: Keep a Set of the open start tag's attribute names, clear it in StartElement, and panic in Attribute when a name repeats. Add a panic_data_xml_writer_duplicate test.

Skeptic: Ran x9.fb (debug). XmlWriter wrote `<a x="1" x="2"/>` with no panic, and Xml.Read then threw `the attribute x is written twice at line 1, column 15`. Attribute (xml.fb:388-412) keeps no record of names already written on the open tag. stdlib.md says the writer panics at what XML can't hold, so it silently produces a document that isn't well-formed.

## yaml-1 (medium): A BOM followed by a comment line is rejected

Status: fixed in a52a99e: a byte order mark skipped at a line's start counts as the start of a line for a comment's '#'; data_yaml.fb reads a mark and a comment line before a mapping

Where: std/data/yaml_scanner.fb:201

SkipToToken skips a leading UTF-8 BOM by moving At to 3, then checks that the byte before a '#' is a space or a line break. When the '#' comes right after the BOM, that byte is the BOM's last one (0xBF), so the check throws. YAML 1.2's l-document-prefix is exactly 'c-byte-order-mark? l-comment*', and a file saved with a BOM (common from Windows tools) that starts with a comment line won't read. A BOM before '---', '%', or a key reads fine. The error's column also counts the invisible BOM.

```
// A UTF-8 BOM, then a comment line: YAML's l-document-prefix, a BOM and comments.
Import std.core
Import std.data.yaml

Try
    Local doc = Try Yaml.Read("\u{FEFF}# settings\nname: app\n")
    Print(doc)
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try
```

Expected: {"name": "app"}

Actual: SYNTAX a comment has a space before its '#' at line 1, column 2

Suggested fix: Treat the end of the skipped BOM as a line start. For example, remember where the BOM ended and skip the space-before-'#' check when At is that place, or test `At > 0 && At != bomEnd && ...`. Add the case to data_yaml.fb.

Skeptic: I ran y1.fb in debug and release. Yaml.Read("\u{FEFF}# settings\nname: app\n") printed 'SYNTAX a comment has a space before its '#' at line 1, column 2'. A BOM followed by 'name: app' read fine, and so did a BOM, a blank line, then a comment. The cause is yaml_scanner.fb:188, which skips the BOM by setting At to 3. Line 201 then checks Bytes[At - 1], the BOM's last byte 0xBF, which is neither white space nor a line break, so it throws. YAML 1.2's l-document-prefix is c-byte-order-mark? l-comment*, and the spec's own Example 5.1 is exactly a BOM followed by '# Comment only.'

## yaml-4 (medium): Quoted scalars reject non-C0 characters YAML 1.2 must allow, so some JSON doesn't read

Status: fixed in a52a99e: a quoted scalar's characters are checked as JSON's strings', any but a C0 control other than a tab; data_yaml.fb reads DEL, U+0080, U+FEFF, and U+FFFE in quotes, and still rejects U+0001 in quotes and U+0080 in a plain scalar

Where: std/data/yaml_scanner.fb:603

FlowScalar checks each character of a quoted scalar with CheckPrintable, the c-printable set, which rejects DEL, C1 controls (U+0080 to U+009F but U+0085), U+FEFF, U+FFFE, and U+FFFF. YAML 1.2, section 5.1, says: 'To ensure JSON compatibility, YAML processors must allow all non-C0 characters inside quoted scalars' (nb-json is x09 | [x20-x10FFFF]). JSON text holding such a raw character in a string is valid JSON, and so valid YAML 1.2, but Yaml.Read throws.

```
// JSON text, which YAML 1.2 reads: its processors must allow every non-C0 character in quotes.
Import std.core
Import std.data.yaml

For text In ["{{\"a\": \"x\u{7F}y\"}}", "{{\"a\": \"x\u{80}y\"}}", "{{\"a\": \"x\u{FEFF}y\"}}", "{{\"a\": 'x\u{FFFE}y'}}"]
    Try
        Local doc = Try Yaml.Read(text)
        Local s = (doc["a"] ?? Value()).AsString() ?? ""
        Print(s.Bytes.Count)
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: 3
4
5
5

Actual: SYNTAX YAML can't hold the control character U+007f at line 1, column 9
SYNTAX YAML can't hold the character U+0080 at line 1, column 9
SYNTAX a byte order mark can only start a document at line 1, column 9
SYNTAX YAML can't hold the character U+fffe at line 1, column 9

Suggested fix: Inside '' and "" scalars, check with nb-json instead: reject only C0 controls other than tab (and the line breaks, which FlowSpaces handles), and accept DEL, C1, U+FEFF, U+FFFE, and U+FFFF. Keep c-printable for plain scalars, block scalars, and comments. The writer already escapes these, which the spec recommends.

Skeptic: I ran y4.fb in debug and release, and the output matched the reported 'actual' exactly. U+007F, U+0080 and U+FFFE inside quotes gave 'YAML can't hold ...', and U+FEFF gave 'a byte order mark can only start a document', each a SYNTAX error at column 9. FlowScalar (yaml_scanner.fb:603) checks quoted content with CheckPrintable, which is the c-printable set. YAML 1.2 §5.1 says processors 'must allow all non-C0 characters inside quoted scalars' (nb-double-char is built from nb-json, x09 | x20-x10FFFF), and 1.2.2 §5.2 allows BOMs inside quoted scalars for JSON compatibility. So valid JSON with raw DEL or C1 characters (C1 is common in mojibake) won't read.

## yaml-5 (medium): A tag that isn't the core schema's on a collection is SYNTAX, not the documented OTHER

Status: fixed in a52a99e: Finish throws OTHER for a collection's tag that isn't one of the core schema's seven, and SYNTAX only for a core tag on the wrong kind; data_yaml.fb gives !foo, !!set, and !!omap OTHER and !!str and !!seq on the wrong kind SYNTAX

Where: std/data/yaml.fb:523

yaml.fb's Read doc and docs/stdlib.md both say a tag that isn't the core schema's is a ParseError of OTHER. Resolve keeps that promise for scalars. Finish, though, checks a collection's tag only against !!seq or !!map and throws SYNTAX, 'a sequence's tag is !!seq, not ...', for any other tag, core or not. So `!foo [x]`, `!!set {x, y}`, and `!!omap` (all common: CloudFormation's `!GetAtt [a, b]`, YAML 1.1's set and omap) give SYNTAX, and a program that catches OTHER to report an unsupported tag misses them.

```
// A tag that isn't the core schema's is a ParseError of OTHER, on a scalar and on a collection.
Import std.core
Import std.data.yaml

For text In ["a: !foo x", "a: !foo [x]", "a: !!set {{x, y}}", "a: !!omap\n- x: 1"]
    Try
        Yaml.Read(text)
        Print("reads")
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: OTHER for all four, e.g. 'OTHER the tag !foo isn't the core schema's ...'

Actual: OTHER the tag !foo isn't the core schema's at line 1, column 9
SYNTAX a sequence's tag is !!seq, not !foo at line 1, column 9
SYNTAX a mapping's tag is !!map, not tag:yaml.org,2002:set at line 1, column 10
SYNTAX a sequence's tag is !!seq, not tag:yaml.org,2002:omap at line 2, column 1

Suggested fix: In Finish, first throw ErrorAt(..., OTHER, "the tag {tag} isn't the core schema's") when the tag is none of the core schema's seven (and isn't '!'). Keep SYNTAX only for a core tag on the wrong kind, like `!!str [x]`. Add a collection case to data_yaml.fb's failures.

Skeptic: I ran y5.fb. 'a: !foo x' gave OTHER, but 'a: !foo [x]' gave "SYNTAX a sequence's tag is !!seq, not !foo". '!!set {x, y}' gave SYNTAX ('a mapping's tag is !!map, not tag:yaml.org,2002:set'), and '!!omap' with a sequence gave SYNTAX. Finish (yaml.fb:523-529) throws Fail, which is SYNTAX, for any tag on a collection that isn't !!seq or !!map. That plainly contradicts Read's doc, stdlib.md, and the lane's own decision that a tag outside the core schema is a ParseError of OTHER.

## csv-ini-2 (low): CSV's ParseError Column counts a UTF-8 BOM as a character on line 1

Status: fixed in 722e11f: PlaceOf, which every format's errors use, doesn't count a byte order mark at the start as a column; data_csv.fb's error after a mark is at column 2

Where: std/data/csv.fb:173

CsvReader skips a leading BOM by starting At at 3 but keeps the BOM's bytes in Bytes. ErrorAt/PlaceOf (std/data/text.fb:110) count from byte 0, so the BOM counts as one column. Every error on the first line of a BOM'd file, which is what Excel's "CSV UTF-8" writes, reports a column one too far right. TOML gets this right because its reader drops the BOM from its bytes, so the formats disagree.

```
Import std.core
Import std.data.csv
Import std.data.toml

Try
    Csv.Read("\u{FEFF}a\"b")
Catch e As ParseError
    Print(e.Line, e.Column)
End Try
Try
    Csv.Read("a\"b")
Catch e As ParseError
    Print(e.Line, e.Column)
End Try
Try
    Toml.Read("\u{FEFF}a = ?")
Catch e As ParseError
    Print(e.Line, e.Column)
End Try
```

Expected: 1 2
1 2
1 5

Actual: 1 3
1 2
1 5
(the same in --release)

Suggested fix: Do what TOML does and leave the BOM out of CsvReader's Bytes, or pass PlaceOf the offset to start counting from. Add a test of an error's column after a BOM.

Skeptic: I ran f2.fb in debug and in --release. It printed "1 3", "1 2" and "1 5" in both builds, so with a BOM the CSV error lands one column further right than the same text without one. CsvReader's constructor (csv.fb:171-173) keeps the BOM in Bytes and only starts At at 3. ErrorAt/PlaceOf (text.fb:110-123) count code points from byte 0, so U+FEFF counts as a column. TOML's reader (toml.fb:262) and XML's (xml_reader.fb:64) leave the BOM out of the bytes they keep, and TOML reports column 5 correctly, so the formats disagree. stdlib.md says each reader's ParseError has the Column it's at, so a column off by one on every first-line error after a BOM is a real (minor) bug.

## csv-ini-3 (low): A lone CR ends a CSV row, but ParseError's Line and Column don't count it as a line, and the docs say lines end with LF or CRLF

Status: fixed in 722e11f: a CR alone ends a line in CSV and INI, and PlaceOf counts it so, as csv.fb's and ini.fb's comments and stdlib.md now say; data_csv.fb reads "a\rb\r\nc\nd" as four rows and its error after a CR at line 2, column 2, and data_ini.fb reads a CR's lines

Where: std/data/csv.fb:203

Row() ends a row at any byte 13, so Csv.Read("a\rb") gives two rows. CsvReader's Lines, which ReadRecords' row-count error uses, counts that CR as a new line too. ErrorAt/PlaceOf (text.fb:115) only counts LF, though, so an error after a lone CR lands on the previous line at a far column. The same text's lines are counted two ways. csv.fb's header comment and docs/stdlib.md say lines end with LF or CRLF and don't mention a lone CR.

```
Import std.core
Import std.data.csv

Print((Try Csv.Read("a\rb")).Count)
Try
    Csv.Read("x\ry\"z")
Catch e As ParseError
    Print(e.Line, e.Column)
End Try
Try
    Csv.ReadRecords("a,b\r1\n")
Catch e As ParseError
    Print(e.Line, e.Column)
End Try
```

Expected: The reader's two counts agree. If a lone CR ends a row, as Row() has it: 2 / 2 2 / 2 1. If only LF and CRLF end a row, as the docs say, the CR doesn't split "a\rb".

Actual: 2
1 4
2 1
(the same in --release)

Suggested fix: Pick one rule. Either count a lone CR as a line end in the place CSV's errors use and say so in csv.fb's comment and stdlib.md, or end rows only at LF and CRLF as documented. Add a test of a lone CR.

Skeptic: I ran f3.fb in debug and in --release. It printed 2, "1 4" and "2 1" in both builds. Row() (csv.fb:203) ends a row at a lone CR and increments Line, so Lines and the row-count error count the CR as a new line. PlaceOf (text.fb:115) counts only byte 10, so an error after the lone CR in "x\ry\"z" reports line 1, column 4, while the reader itself treats 'y' as starting line 2. The same reader therefore counts lines two ways. The csv.fb header comment and stdlib.md say lines end with LF or CRLF and never mention a lone CR ending a row.

## csv-ini-4 (low): ReadRecords' and Ini.Read's ParseErrors don't say their column, and their Column is always 1

Status: fixed in 722e11f: ReadRecords' errors are at the header's name given again and at the row's start, and Ini.Read's at the line's first byte, its '=', or its header's end, each through ErrorAt, which says where; data_csv.fb's and data_ini.fb's errors give their columns, the second "b" at column 7 and INI's '=' at column 5

Where: std/data/csv.fb:37

The step says every reader's ParseError has Line and Column and says them, and stdlib.md says each reader throws one with the Line and Column it's at. ErrorAt's messages end with "at line L, column C". ReadRecords' header-twice error says neither line nor column and gives Column 1, not the duplicate's column. Its row-count error says only the line (csv.fb:45). Every Ini.Read error (ini.fb:43-80) says only "at line N" and gives Column 1, even when the fault is mid-line, like a missing key before an indented '='.

```
Import std.core
Import std.data.csv
Import std.data.ini

Try
    Csv.ReadRecords("a,b,c,b\n1,2,3,4\n")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
Try
    Csv.ReadRecords("a,b\n1,2,3\n")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
Try
    Ini.Read("[s]\n    = 1\n")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
Try
    Csv.Read("a,b\"c")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
```

Expected: The duplicate header reported at line 1, column 7 (the second "b"), with the message saying where. The row and INI errors' messages say their column too (INI's '=' at line 2, column 5), like the last line's "at line 1, column 4".

Actual: 1 1 the header names "b" twice
2 1 the row at line 2 has as many fields as the header, 2, not 3
2 1 a key is missing before its '=', at line 2
1 4 a field with a '"' in it is in '"'s, with its '"'s doubled at line 1, column 4
(the same in --release)

Suggested fix: Build these errors with ErrorAt at the right byte. For the header, keep each header field's start offset (or its row start plus the field's column). For INI, use the byte where the fault is: the line's first byte, the '=', or the header's end. Then their Line, Column, and message match the other readers'. Update the tests' expected output.

Skeptic: I ran f4.fb in debug and in --release, with the same output in both. The duplicate header gave "1 1 the header names \"b\" twice", although the second b is at column 7. The row-count error gave "2 1 ... not 3", with no column in its message. The INI error gave "2 1 a key is missing before its '=', at line 2", with no column, although the '=' is at column 5. Csv.Read's own error, for comparison, gave "... at line 1, column 4". In the code, csv.fb:37 and :45-46 and ini.fb:43-80 build ParseError directly with column 1 instead of using ErrorAt. TOML, XML and YAML all go through ErrorAt; the only other direct construction is yaml.fb:28. stdlib.md says each reader throws a ParseError "with the Line and Column it's at", and the stated rule for this step is that every reader's ParseError has Line and Column and says them. These errors break both.

## csv-ini-5 (low): Csv.Write writes a row of no fields as an empty line, which reads back as a row of one empty field

Status: fixed in 722e11f and 794d2a8: Csv.Write panics at a row of no fields, which CSV can't hold, so WriteRecords does at records with no keys, and no records are no text; panic_data_csv_empty_row.fb, panic_data_csv_no_keys.fb, and panic_data_csv_records.fb

Where: std/data/csv.fb:62

Write takes any String[][], and a row with Count 0 writes just "\n". Read gives an empty line back as a row of one empty field, so the rows don't round-trip. WriteRecords hits the same case: [{}] writes a header of no names and a row of no fields ("\n\n"), which ReadRecords gives back as [{"": ""}]. The docs say what Write writes reads back as the same rows. The one degenerate shape CSV can't represent is neither rejected nor documented.

```
Import std.core
Import std.collections
Import std.data.csv

Local rows: String[][] = [[], ["a"]]
Local back = Try Csv.Read(Csv.Write(rows))
Print(back == rows, back[0].Count)
Local records = Value.Array()
records.Add(Value.Object())
Print(Try Csv.ReadRecords(Csv.WriteRecords(records)))
```

Expected: True 0 and [{}], or Write and WriteRecords panic at a row of no fields since CSV can't hold one

Actual: False 1
[{"": ""}]
(the same in --release)

Suggested fix: Panic in Write at a row with no fields (WriteRecords then panics for records with no keys at all). Or document that a row of no fields reads back as one empty field. Add a test either way. Note that panic_data_csv_record.fb currently expects WriteRecords([{}]) to give "\n\n".

Skeptic: I ran f5.fb in debug and in --release. It printed "False 1" and [{"": ""}] in both builds. Write (csv.fb:65-71) writes a zero-field row as just LF. Read documents an empty line as a row of one empty field, so [] comes back as [""]. WriteRecords([{}]) writes "\n\n", and ReadRecords turns that into a record with a key "" that never existed. The lane's own data_csv.fb says what Write and WriteRecords write "reads back as the same". Write doesn't panic at this shape and its docs don't mention it. panic_data_csv_record.fb checks only that the output is "\n\n", not that it reads back. This is a degenerate shape, but the round-trip loss and the invented key are real.

## csv-ini-6 (low): A first CSV field or first INI key that starts with U+FEFF is lost on a round trip

Status: fixed in 722e11f: Csv.Write quotes a field that starts with a byte order mark, and Ini.Write panics at a key that starts with one; data_csv.fb reads back a first field of "\u{FEFF}id", and panic_data_ini_key_mark.fb

Where: std/data/csv.fb:129

Csv.Read and Ini.Read skip a leading EF BB BF as a byte order mark. Csv.Write's AddField and Ini.Write's AddEntry write a first field or key that starts with U+FEFF unchanged, so the text starts with those bytes and Read strips them. A header like "\u{FEFF}id", which a program can get from a BOM'd file read some other way, comes back as "id". Write doesn't guard against it.

```
Import std.core
Import std.collections
Import std.data.csv
Import std.data.ini

Local rows: String[][] = [["\u{FEFF}id", "name"]]
Print((Try Csv.Read(Csv.Write(rows))) == rows)
Local document = Value.Object()
document["\u{FEFF}key"] = Value("v")
Print((Try Ini.Read(Ini.Write(document))) == document)
```

Expected: True
True (or a panic from Write at text it can't write back)

Actual: False
False
(the same in --release)

Suggested fix: In Csv.Write, quote a field that starts with U+FEFF, so the text starts with '"' rather than a BOM. In Ini.Write, panic at a key that starts with U+FEFF, or write a BOM first whenever the output would start with one. Add a round-trip test.

Skeptic: I ran f6.fb in debug and in --release. It printed False and False in both builds. Csv.Write's AddField (csv.fb:129-147) quotes only for a space at the ends, the separator, '"', CR or LF. A first field starting with U+FEFF is therefore written unquoted, the text starts with EF BB BF, and CsvReader (csv.fb:173) skips it as a BOM, so "\u{FEFF}id" comes back as "id". Ini.Write's AddEntry likewise writes the first key unchanged, and Ini.Read (ini.fb:24) strips the BOM. This contradicts the lane's tests' claim that Write's output reads back as the same, and stdlib.md's statement that Ini.Write panics at what INI can't hold. It's low because it happens only when that field or key is the first thing written.

## toml-2 (low): Text that ends inside an inline table, an array, or a string can throw SYNTAX instead of the documented UNEXPECTED_END

Status: fixed in 44ab0b8: inside a string, an array, or an inline table, an error where the text has ended is an UNEXPECTED_END; data_toml.fb gives the finding's five texts and two more UNEXPECTED_END, and "a = " still SYNTAX

Where: std/data/toml.fb:458

Toml.Read's doc comment and docs/stdlib.md promise UNEXPECTED_END "for text that ends inside a string, an array, or an inline table". Several ways of ending there give SYNTAX instead: ending right after a key's '=' inside an inline table (ReadValue: "a key's value is missing"), after a '.' in an inline table's key (SimpleKey), inside a \u or \U escape of an unclosed string (CodePoint: "the escape has 4 hex digits"), and after a sign inside an array (Scalar). A caller that checks the Kind to decide whether more text is coming gets these wrong. data_toml.fb only tests endings that land on the checks that already use Ended().

```
Import std.core
Import std.data.toml

// Toml.Read's doc: UNEXPECTED_END "for text that ends inside a string, an array, or an inline
// table".
Function Kind(text: String)
    Try
        Toml.Read(text)
        Print("reads")
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End Function

Kind("a = {{ b = ")
Kind("a = {{ b.")
Kind("a = \"\\u12")
Kind("a = \"\"\"x\\U0001")
Kind("a = [1, -")
```

Expected: Five lines starting with UNEXPECTED_END, one for each text, each of which ends inside an inline table, a string, or an array.

Actual: SYNTAX a key's value is missing at line 1, column 11
SYNTAX a key is letters, digits, '_', and '-', or a quoted string at line 1, column 9
SYNTAX the escape has 4 hex digits at line 1, column 6
SYNTAX the escape has 8 hex digits at line 1, column 9
SYNTAX the value isn't one TOML has at line 1, column 9

Suggested fix: Throw Ended() when At >= Bytes.Count is the reason a check fails while inside a string, an array, or an inline table: in CodePoint, and in ReadValue/SimpleKey/Scalar when the reader is inside a container (pass a flag down, or check for the end before failing). Add these cases to data_toml.fb.

Skeptic: Ran rv-step20/toml/t2.fb in debug and release (written with the Write tool, because a bash heredoc collapsed the \\ escapes). Every one of the finding's five texts gives SYNTAX: "a key's value is missing" (col 11), "a key is letters, digits..." (col 9), "the escape has 4 hex digits" (col 6), "the escape has 8 hex digits" (col 9), and "the value isn't one TOML has" (col 9). Other endings give SYNTAX too: "a = [1, +", "a = [1, 0x" ("the integer has digits after its base"), and "a = [1, 1979-" ("the date or time has 2 digits here"). Endings the code already checks with Ended() do give UNEXPECTED_END: "a = [1, ", "a = { b ", "a = \"abc", and "a = \"\\". Toml.Read's doc comment (toml.fb lines 16-19) and stdlib.md both promise UNEXPECTED_END for text that ends inside a string, an array, or an inline table. ReadValue (line 458), SimpleKey (line 451), CodePoint (line 712), Scalar (line 755), Based, and Number all use Fail at the end of the text. I rated it low: an error is still thrown at a correct position, and only its Kind is wrong.

## toml-5 (low): An inline table's frozen-key error names the key's parent, or no key at all

Status: fixed in 44ab0b8: InlineTable checks its key's parent for an inline table or an array, as KeyValue does, so a key given again is "is defined already" by its own name; data_toml.fb gives the finding's four

Where: std/data/toml.fb:531

InlineTable checks flags.IsFrozen(key), which includes the key's last part, but its message names Joined(key[0..key.Count - 1]), the key's parent. So when a key that held an array or inline table is given again, the message starts with an empty name (" is an inline table..."). For a dotted key, it names the parent table ("a") rather than the array at "a.b". The top-level check in KeyValue tests only the parent, so its messages are right.

```
Import std.core
Import std.data.toml

// A key given twice in an inline table, the first time an array or an inline table.
Function Message(text: String)
    Try
        Toml.Read(text)
        Print("reads")
    Catch e As ParseError
        Print(e.Message)
    End Try
End Function

Message("x = {{ a = [1], a = 2 }}")
Message("x = {{ a = {{ b = 1 }}, a = 2 }}")
Message("x = {{ a = 1, a = 2 }}")
Message("x = {{ a.b = [1], a.b = 2 }}")
```

Expected: a is defined already at line 1, column 16
a is defined already at line 1, column 22
a is defined already at line 1, column 14
a.b is defined already at line 1, column 18
(or messages that name a and a.b as the inline table or array)

Actual:  is an inline table or an array, which can't be added to at line 1, column 16
 is an inline table or an array, which can't be added to at line 1, column 22
a is defined already at line 1, column 14
a is an inline table or an array, which can't be added to at line 1, column 18

Suggested fix: Check flags.IsFrozen(key[0..key.Count - 1]) to match KeyValue, so a repeated stem falls through to "{Joined(key)} is defined already". Or name the frozen prefix itself in the message. Add a case to data_toml.fb.

Skeptic: Ran rv-step20/toml/t5.fb in debug and release. "x = { a = [1], a = 2 }" gives " is an inline table or an array, which can't be added to at line 1, column 16", starting with an empty name. "x = { a = { b = 1 }, a = 2 }" gives the same empty name at column 22. "x = { a.b = [1], a.b = 2 }" gives "a is an inline table or an array...", but a is a table and the array is a.b. "x = { a = 1, a = 2 }" correctly gives "a is defined already". In InlineTable (lines 530-532), the check is flags.IsFrozen(key), which includes the key's own last part, but the message names Joined(key[0..key.Count - 1]), the key's parent. The text is still rejected, so only the message is wrong.

## xml-11 (low): Markup cut off at an entity's end is reported as UNEXPECTED_END though the document goes on

Status: fixed in f625602: Ended gives SYNTAX, saying the markup doesn't end before its entity's text does, when the reader is in an entity; data_xml.fb gives the finding's four

Where: std/data/xml_reader.fb:230

Ended() is used wherever AtEnd() is true: in StartTag (230), Name (907), Comment (476), CData, Instruction, AttributeValue, and Literal. AtEnd tests the current source, which can be an entity's text. A start tag, comment, or reference that an entity's text leaves unfinished is therefore a ParseError of UNEXPECTED_END, although the document doesn't end there. docs/stdlib.md says UNEXPECTED_END is for text that ends inside something, and SYNTAX covers the rest.

```
Import std.core
Import std.data.xml

// Markup split across an entity's end: the document goes on, so it's a SYNTAX error, not an
// UNEXPECTED_END.
Function Show(text: String)
    Try
        Xml.Read(text)
        Print("reads")
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End Function

Show("<!DOCTYPE a [<!ENTITY e '<b'>]><a>&e;/></a>")
Show("<!DOCTYPE a [<!ENTITY e '<!-- c'>]><a>&e; --></a>")
Show("<!DOCTYPE a [<!ENTITY e 'x&#38;'>]><a>&e;amp;</a>")
Show("<!DOCTYPE a [<!ENTITY e '&#38;'>]><a b='&e;amp;'/>")
```

Expected: SYNTAX ... (four times)

Actual: UNEXPECTED_END the start tag <b> doesn't end at line 1, column 38
UNEXPECTED_END the comment has no '-->' at line 1, column 42
UNEXPECTED_END a name is missing at line 1, column 42
UNEXPECTED_END a name is missing at line 1, column 44

Suggested fix: Have Ended() return a SYNTAX error (for example "... ends inside the entity's text") when Sources.Count > 1, or check Sources.Count at each AtEnd. Add a case to data_xml.fb's Failure list.

Skeptic: Ran x11.fb (debug). All four cases give UNEXPECTED_END (`the start tag <b> doesn't end at line 1, column 38`, `the comment has no '-->'...`, `a name is missing...` twice), although the document continues past each one. XmlReader.Next's comment (xml_reader.fb:82-84) reserves UNEXPECTED_END for "a document that ends inside an element or markup", but Ended() is used wherever AtEnd() tests the current source, which can be an entity's text. Only the error kind is wrong, so I rate it low.

## xml-12 (low): text() treats text and the CDATA next to it as separate nodes, unlike XPath's single text node

Status: fixed in f625602: XmlPath's places take a run of text and CDATA children as one text node, a TEXT of their text when there are more than one, on every axis; data_xml.fb counts a/text() as XPath does and matches 'xyz'

Where: std/data/xml_path.fb:373

XPath 1.0's data model has no CDATA nodes: a run of character data, CDATA sections included, is one text node. XmlPath matches each TEXT and CDATA child as its own text() node. So count(text()), text()[1], and text() = '...' all differ from XPath whenever CDATA sits next to text, and neither the code's comments nor stdlib.md mention it.

```
Import std.core
Import std.collections
Import std.data.xml

// XPath's data model has no CDATA nodes: text and CDATA next to each other are one text node.
Local root = Try Xml.Read("<r><a>x<![CDATA[y]]>z</a></r>")
Print(root.Query("a/text()").Count)
Print(root.Query("a[text() = 'xyz']").Count)
Print(root.QueryFirst("a/text()")?.Content ?? "-")
```

Expected: 1
1
xyz

Actual: 3
0
x

Suggested fix: On the child axis, merge each run of adjacent TEXT and CDATA children into one text place for text() and node() (and string comparisons). Otherwise document the difference in xml_path.fb's header and in stdlib.md.

Skeptic: Ran x12.fb (debug). For <a>x<![CDATA[y]]>z</a>, Query("a/text()").Count is 3, a[text() = 'xyz'] matches 0, and the first text() is "x". Matches (xml_path.fb:373) treats each TEXT and each CDATA child as its own text() node. XPath 1.0's data model makes adjacent character data and CDATA one text node, so the counts and comparisons differ, and neither xml_path.fb's header nor stdlib.md says so. It is rare in practice, and documenting the difference would also resolve it.

## xml-13 (low): string() of a number writes an exponent, which XPath's string() never does

Status: fixed in f625602: NumberText writes the shortest digits without an exponent, and Infinity and -Infinity; data_xml.fb matches string() of 1e21, 1e-7, 1.5, .25, 123456789012, and 1.2345e-7

Where: std/data/xml_path.fb:845

NumberText is "{number}", which writes 1e21 as "1e+21" and 1e-7 as "1e-07". XPath 1.0 section 4.2 writes a number in decimal form with no exponent: "1000000000000000000000" and "0.0000001" (and Infinity as "Infinity"). The function's comment says it writes numbers as XPath's string() does, so comparisons against string(n) or contains(., n) go wrong for large or small n.

```
Import std.core
Import std.collections
Import std.data.xml

Local root = Try Xml.Read("<r><v>1000000000000000000000</v><v>1e+21</v><v>0.0000001</v><v>1e-07</v></r>")
For path In ["v[. = string(1000000000000000000000)]", "v[. = string(0.0000001)]"]
    Local found: String[] = []
    For node In root.Query(path)
        found.Add("[" + node.Text + "]")
    End For
    Print(path, "=>", found.Join(" "))
End For
```

Expected: v[. = string(1000000000000000000000)] => [1000000000000000000000]
v[. = string(0.0000001)] => [0.0000001]

Actual: v[. = string(1000000000000000000000)] => [1e+21]
v[. = string(0.0000001)] => [1e-07]

Suggested fix: Write the shortest round-trip digits without an exponent (expanding with zeros) and "Infinity"/"-Infinity" for infinities. Otherwise change the comment to say the exponent form is kept.

Skeptic: Ran x13.fb (debug). `v[. = string(1000000000000000000000)]` matched `[1e+21]`, and `v[. = string(0.0000001)]` matched `[1e-07]`. NumberText (xml_path.fb:845) is "{number}", which writes an exponent. XPath 1.0 section 4.2 string() writes decimal form with no exponent, and the function's comment says it writes numbers as XPath's string() does.

## xml-14 (low): Some paths that aren't XPath are accepted silently and don't panic

Status: fixed in f625602: a number with two points or that FloatOf can't read, and count() or name() of what isn't a path, fail the path; panic_data_xml_path_number.fb, panic_data_xml_path_count.fb, and panic_data_xml_path_name.fb

Where: std/data/xml_path.fb:562

stdlib.md says "A path that isn't one panics at Query's caller." Three cases get through. The number scanner takes any run of digits and '.' and turns text FloatOf can't read into 0, so v[1.2.3] quietly selects nothing. count() and name() take any argument and read its Places, so count('x') is 0 and name(1) is "", where XPath calls both errors because they need a node-set.

```
Import std.core
Import std.data.xml

// Paths that aren't XPath: each should panic, as "A path that isn't one panics at Query's caller."
Local root = Try Xml.Read("<r><v>0</v></r>")
Print(root.Query("v[1.2.3]").Count)
Print(root.Query("v[count('x') = 0]").Count)
Print(root.Query("v[name(1) = '']").Count)
```

Expected: panic: the XPath "v[1.2.3]" isn't one XmlPath reads: ... (and likewise for the other two)

Actual: 0
1
1

Suggested fix: In Primary, call Fail when FloatOf returns Null or a second '.' appears. In the call check, Fail when count's argument, or name's when it has one, isn't a path (IsPath()). Add panic tests.

Skeptic: Ran x14.fb (debug). It printed 0, 1, 1, with no panic. Primary's number scanner (xml_path.fb:560-567) takes any run of digits and '.' and uses `FloatOf(...) ?? 0.0`, so 1.2.3 silently becomes 0. The call check (598-615) only counts arguments, so count('x') and name(1) read the empty Places of a non-node-set argument. stdlib.md says "A path that isn't one panics at Query's caller", and XPath 1.0 rejects all three.

## xml-15 (low): Two comments say the code panics where it doesn't

Status: fixed in f625602: Enter's comment says it throws a ParseError past 64 entities deep and past the expansion cap, and FromValue's says an Array's item that's an Array, or an attribute or "#text" that's an Array or an Object, panics

Where: std/data/xml_reader.fb:334

Enter's comment says it "panics past this much nesting", but it throws a ParseError and takes no limit parameter for "this much" to refer to. Xml.FromValue's comment (std/data/xml.fb:272) says it panics when "an Array's item is an Array or an Object". An Object item is in fact the normal case, xmltodict's list of elements, and works; only an Array item panics. A non-scalar "#text" also panics, and the comment doesn't say so.

```
Import std.core
Import std.collections
Import std.data.xml

// Enter's comment says it panics past 64 entities deep, but it throws a ParseError; FromValue's
// says an Array's item that's an Object panics, but it's an element, as xmltodict has it.
Local parts: String[] = ["<!DOCTYPE a [<!ENTITY e0 'x'>"]
For i = 1 To 70
    parts.Add("<!ENTITY e{i} '&e{i - 1};'>")
End For
parts.Add("]><a>&e70;</a>")
Try
    Xml.Read(parts.Join(""))
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try

Local value = Value.Object()
Local items = Value.Array()
Local item = Value.Object()
item["@n"] = Value(1 As Int64)
items.Add(item)
value["item"] = items
Print(Xml.FromValue("list", value))
```

Expected: The comments describe this behavior: a ParseError past 64 entities deep, and an Object item becoming an element

Actual: SYNTAX entities are nested more than 64 deep at line 1, column 1491
<list><item n="1"/></list>

Suggested fix: Enter: "Starts reading entity's text, and throws a ParseError past 64 entities deep." FromValue: "... an attribute's value or \"#text\" is an Array or an Object, an Array's item is an Array, or value is an Array."

Skeptic: Ran x15.fb (debug). It printed `SYNTAX entities are nested more than 64 deep at line 1, column 1491` (a thrown ParseError, not a panic) and `<list><item n="1"/></list>` (an Object item works). Enter's comment (xml_reader.fb:334) says it "panics past this much nesting", while the code throws and no limit is named. FromValue's comment (xml.fb:272-273) reads as if an Array's Object item panics, but only an Array item does (line 290), and it doesn't mention that a non-scalar "#text" panics through ScalarText.

## xml-8 (low): After an unread parameter entity, an ATTLIST still adds attributes to an element that already has one

Status: fixed in f625602: past an unread parameter entity, an ATTLIST is read into a map of its own and left out, whether the element has one already or not; data_xml_rules.fb reads the finding's documents with only the first ATTLIST's attributes, and checks one past it

Where: std/data/xml_reader.fb:747

ListDeclaration skips registering a new element's list once Stopped is set (`If !Stopped Then Lists[element] = declared`). When the element already has a list from an earlier ATTLIST, though, `declared[name] = attribute` adds the new attribute to that live map anyway. Section 5.1 says such declarations must not be processed in a document that isn't standalone. The result depends on whether an earlier ATTLIST exists: the later attribute's default is given and its type's space collapsing applies, but with no earlier ATTLIST neither happens.

```
Import std.core
Import std.collections
Import std.data.xml

// XML 1.0's 5.1: past a reference to a parameter entity it doesn't read, a non-validating
// processor doesn't take attribute-list declarations.
Function Show(text: String) Throws ParseError
    Local root = Try Xml.Read(text)
    Local parts: String[] = []
    For name, value In root.Attributes
        parts.Add("{name}=[{value}]")
    End For
    Print(parts.Join(" "))
End Function

Try Show("<!DOCTYPE a [<!ATTLIST a x CDATA 'one'><!ENTITY % e SYSTEM 'e.dtd'>%e;<!ATTLIST a y CDATA 'two'>]><a/>")
Try Show("<!DOCTYPE a [<!ENTITY % e SYSTEM 'e.dtd'>%e;<!ATTLIST a y CDATA 'two'>]><a/>")
Try Show("<!DOCTYPE a [<!ATTLIST a x CDATA #IMPLIED><!ENTITY % e SYSTEM 'e.dtd'>%e;<!ATTLIST a y NMTOKENS #IMPLIED>]><a y='  p   q '/>")
```

Expected: x=[one]

y=[  p   q ]

Actual: x=[one] y=[two]

y=[p q]

Suggested fix: In ListDeclaration, read and check the declaration but leave Lists alone when Stopped (and not standalone, per xml-7): parse into a throwaway map. Add a test.

Skeptic: Ran x8.fb (debug). It printed `x=[one] y=[two]`, then an empty line, then `y=[p q]`. ListDeclaration (xml_reader.fb:714-718, 747) skips registering a new map when Stopped but writes into the existing live map via `declared[name] = attribute`. A y declared after the unread %e; therefore gets its default and NMTOKENS collapsing only when an earlier ATTLIST for the element exists, which contradicts section 5.1 and is inconsistent. It is real but obscure (external PE plus repeated ATTLIST), so I rate it low.

## yaml-10 (low): Read's ParseError for more than one document doesn't say its line and column

Status: fixed in a52a99e: the parser keeps where each document starts, and Read's error is at the second's, through ErrorAt; data_yaml.fb's error is at line 3, column 1, where "--- c" starts

Where: std/data/yaml.fb:28

Every other YAML ParseError comes from ErrorAt, whose message ends 'at line L, column C'. The lane's design says every reader's ParseError says its line and column. Read's error for a text of several documents is built directly, with Line 1 and Column 1, and its message doesn't say them. Line 1, column 1 is also not where the second document starts.

```
// Each of Yaml.Read's ParseErrors says its line and column, as the others do.
Import std.core
Import std.data.yaml

For text In ["a: 1\na: 2", "a\n---\nb"]
    Try
        Yaml.Read(text)
    Catch e As ParseError
        Print(e.Line, e.Column, e.Message)
    End Try
End For
```

Expected: 2 1 the mapping has the key "a" twice at line 2, column 1
2 1 the text holds 2 documents, which ReadAll reads at line 2, column 1

Actual: 2 1 the mapping has the key "a" twice at line 2, column 1
1 1 the text holds 2 documents, which ReadAll reads

Suggested fix: Throw ErrorAt at the second document's start (have the parser record each document's first byte), or at least append the ' at line 1, column 1' that ErrorAt adds. Update data_yaml.out.

Skeptic: I ran y10.fb. The duplicate-key error printed '2 1 ... at line 2, column 1', but the error for several documents printed '1 1 the text holds 2 documents, which ReadAll reads', with no 'at line, column' text and a place that isn't the second document's (line 2). yaml.fb:28 builds this ParseError directly instead of through ErrorAt. That contradicts the lane's 'every reader's ParseError has Line and Column and says them' and stdlib.md's 'the Line and Column it's at'.

## yaml-11 (low): With CR-only line breaks, every YAML error says line 1

Status: fixed in 722e11f: PlaceOf counts a CR that no LF follows as a line break, as YAML's scanner does; data_yaml.fb (a52a99e) gives a CR text's duplicate key at line 3, as its LF and CRLF ones

Where: std/data/text.fb:115

The YAML scanner treats a lone CR as a line break (LineBreak, IsBreakByte), as YAML 1.2's b-break does. But ErrorAt's PlaceOf counts only LFs, so in a text with old-Mac CR line breaks every ParseError says line 1, with a column counted across the whole text. The same error over LF breaks says line 3.

```
// A CR alone is one of YAML's line breaks, which the scanner reads as one, so the duplicate key
// is on line 3.
Import std.core
Import std.data.yaml

Try
    Yaml.Read("a: 1\rb: 2\rb: 3\r")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
Try
    Yaml.Read("a: 1\nb: 2\nb: 3\n")
Catch e As ParseError
    Print(e.Line, e.Column, e.Message)
End Try
```

Expected: 3 1 the mapping has the key "b" twice at line 3, column 1
3 1 the mapping has the key "b" twice at line 3, column 1

Actual: 1 11 the mapping has the key "b" twice at line 1, column 11
3 1 the mapping has the key "b" twice at line 3, column 1

Suggested fix: Have PlaceOf count a CR that no LF follows as a line break, or give YAML's errors the scanner's own Line and Column. Add a CR case to data_yaml.fb's failures.

Skeptic: I ran y11.fb. With CR-only breaks, 'a: 1\rb: 2\rb: 3\r' gave '1 11 the mapping has the key "b" twice at line 1, column 11', while the same text with LF breaks gave line 3, column 1. The YAML scanner treats a lone CR as a line break (LineBreak, IsBreakByte, as YAML's b-break does). But PlaceOf (text.fb:115), which ErrorAt uses, counts only LF, so the Line and Column reported aren't where the error is.

## yaml-12 (low): A BOM at the start of a later document is rejected

Status: fixed in a52a99e: a byte order mark can start a line where a document can start, at the stream's start or after a "...", comment lines too; data_yaml.fb reads documents after "..." that start with one, and still rejects one inside a document

Where: std/data/yaml_scanner.fb:188

YAML 1.2's stream grammar lets each document's prefix start with a byte order mark (l-document-prefix ::= c-byte-order-mark? l-comment*, which can follow a '...'). The scanner skips a BOM only at byte 0. After that, CheckPrintable throws 'a byte order mark can only start a document', even where one does start a document.

```
// A BOM can start any document of a stream (YAML 1.2's l-document-prefix), not only the first.
Import std.core
Import std.collections
Import std.data.yaml

Try
    Local documents = Try Yaml.ReadAll("a: 1\n...\n\u{FEFF}--- b\n")
    Print(documents.Count, documents[1])
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try
```

Expected: 2 "b"

Actual: SYNTAX a byte order mark can only start a document at line 3, column 1

Suggested fix: In SkipToToken, also skip a BOM at column 0 when a document can start there (at the stream's start, or after a DOCUMENT_END), not only when At == 0.

Skeptic: I ran y12.fb: ReadAll("a: 1\n...\n\u{FEFF}--- b\n") gave 'SYNTAX a byte order mark can only start a document at line 3, column 1', even though the BOM there does start a document. SkipToToken (yaml_scanner.fb:188) skips a BOM only when At == 0. YAML 1.2's stream grammar allows l-document-prefix (c-byte-order-mark? l-comment*) after a l-document-suffix, and §5.2 says BOMs may appear at the start of any document.

## yaml-13 (low): A second %TAG for '!' or '!!' in one document isn't an error

Status: fixed in a52a99e: Documents keeps the handles a document's %TAG directives give, and throws at one given twice, '!' and '!!' too; data_yaml.fb gives all three

Where: std/data/yaml.fb:381

YAML 1.2, 6.8.2, says it's an error to give more than one %TAG directive for the same handle in a document, even with the same prefix. Documents checks repeats only for named handles, because '!' and '!!' are put in Handles before the directives run. So two '%TAG !' directives are read, and the last one wins.

```
// A handle's %TAG comes once in a document (YAML 1.2, 6.8.2), "!" and "!!" too.
Import std.core
Import std.data.yaml

For text In ["%TAG !e! tag:a,2000:\n%TAG !e! tag:b,2000:\n--- x", "%TAG ! tag:a,2000:\n%TAG ! tag:yaml.org,2002:\n--- !str x"]
    Try
        Print(Try Yaml.Read(text))
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: SYNTAX %TAG gives the handle !e! twice at line 2, column 1
SYNTAX %TAG gives the handle ! twice at line 2, column 1

Actual: SYNTAX %TAG gives the handle !e! twice at line 2, column 1
"x"

Suggested fix: Keep a set of the handles this document's %TAG directives gave, and throw for any handle given twice, '!' and '!!' included, apart from the defaults put in Handles.

Skeptic: I ran y13.fb. Two '%TAG !e!' directives gave 'SYNTAX %TAG gives the handle !e! twice', but '%TAG ! tag:a,2000:' followed by '%TAG ! tag:yaml.org,2002:' then '--- !str x' read without error and printed "x", so the last one wins. yaml.fb:381 exempts '!' and '!!' from the repeat check because Handles holds them as defaults. YAML 1.2 §6.8.2 makes more than one %TAG for the same handle in a document an error, even with the same prefix.

## yaml-2 (low): Write writes a key over 1024 characters as an implicit key, which doesn't read back

Status: fixed in a52a99e: WriteNode writes a key of more than 1024 characters as written after '?', on its line, its ':' on the next; data_yaml.fb writes back 1100- and 2201-character keys, with scalar and block values, in a sequence

Where: std/data/yaml.fb:76

WriteNode always writes an Object's key as `key: value`, an implicit (simple) key. YAML limits an implicit key to 1024 characters, and the scanner enforces that limit (StaleKeys). So Write's output for an Object with a long key, about 1100 characters here, is text that Yaml.Read (and any conforming reader) rejects. Write's documented promise, the same Value read back, fails. A key that escapes to more than 1024 characters (many \n or \u escapes) fails the same way.

```
// Write's output for a key of more than 1024 characters doesn't read back.
Import std.core
Import std.data.yaml

Local key = ""
For i = 1 To 1100
    key += "k"
End For
Local value = Value.Object()
value[key] = Value("v")
Local written = Yaml.Write(value)
Try
    Print((Try Yaml.Read(written)) == value)
Catch e As ParseError
    Print(e.Kind, e.Line, e.Column)
End Try
```

Expected: True

Actual: SYNTAX 1 1101

Suggested fix: When the key as written (plain or quoted) is longer than the limit, or a safer bound like libyaml's 128, write it as an explicit key: `? key` on its line, then `: value` (a block value goes on the lines after) at the same indent. Add a long-key round trip to data_yaml.fb.

Skeptic: I ran y2.fb: Yaml.Read(Yaml.Write(o)) for an Object with one 1100-character key printed 'SYNTAX 1 1101'. WriteNode (yaml.fb:76) always writes a key as an implicit 'key: value' and never as '? key'. YAML restricts an implicit key to 1024 characters, and the scanner enforces that (StaleKeys), so Write produces text that no conforming reader accepts. That breaks Write's doc ('plain where the core schema reads it back') and the milestones row's round trip. Severity is low because keys this long are rare.

## yaml-3 (low): The 1024 limit on a simple key counts bytes, not characters

Status: fixed in a52a99e: StaleKeys counts a simple key's length in columns, which are characters; data_yaml.fb reads a 600-character key of 2-byte characters and writes it back

Where: std/data/yaml_scanner.fb:876

StaleKeys drops a possible simple key once `At - key.At > 1024`, and At counts bytes. YAML 1.2 (and libyaml) allows the ':' up to 1024 Unicode characters after the key's start. So a valid key of 600 two-byte characters (1200 bytes) gives a ParseError with a misleading message, 'a mapping's value can't start here'. For 3-byte characters (CJK), any key over 341 characters fails. Write then produces text Read rejects even for keys well under the limit: the second half of the program writes a 600-character key and can't read it back.

```
// A key of 600 characters, each 2 bytes: within YAML's 1024-character limit for a simple key.
Import std.core
Import std.data.yaml

Local key = ""
For i = 1 To 600
    key += "é"
End For
Try
    Local doc = Try Yaml.Read(key + ": v")
    Print(doc.Count)
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try
Local value = Value.Object()
value[key] = Value("v")
Try
    Print((Try Yaml.Read(Yaml.Write(value))) == value)
Catch e As ParseError
    Print(e.Kind, e.Line, e.Column)
End Try
```

Expected: 1
True

Actual: SYNTAX a mapping's value can't start here at line 1, column 601
SYNTAX 1 601

Suggested fix: Count characters. The key's line is already required to be the current one, and CheckPrintable advances Column by one per character, so `Column - key.Column > 1024` works. Add a multi-byte key case.

Skeptic: I ran y3.fb in debug and release. A 600-character key of 'é' (1200 bytes) printed 'SYNTAX a mapping's value can't start here at line 1, column 601', and the round trip printed 'SYNTAX 1 601'. yaml_scanner.fb:876 tests `At - key.At > 1024`, and At counts bytes. That contradicts its own comment ('up to 1024 characters long') and YAML 1.2's limit of 1024 Unicode characters, so valid YAML is rejected. Severity is low because it only shows with long multi-byte keys (over 341 CJK characters).

## yaml-6 (low): Text that ends inside a flow collection is SYNTAX, not UNEXPECTED_END

Status: fixed in a52a99e: a flow sequence or mapping that the text ends inside is an UNEXPECTED_END at its start, as Read's comment now says; data_yaml.fb gives five

Where: std/data/yaml.fb:661

docs/stdlib.md says each reader's ParseError is 'UNEXPECTED_END for text that ends inside something', and TOML gives it for an unclosed array or inline table. YAML gives UNEXPECTED_END only for an unclosed quoted scalar. `a: [1, 2` and `a: {b: 1` give SYNTAX, with a message about ',' when the real problem is the missing ']' or '}'. (yaml.fb's own doc names only quoted scalars, so the two docs disagree.)

```
// Text that ends inside a flow collection, as it does inside a quoted scalar.
Import std.core
Import std.data.yaml

For text In ["a: \"x", "a: [1, 2", "a: {{b: 1", "[1, 2"]
    Try
        Yaml.Read(text)
        Print("reads")
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: UNEXPECTED_END for all four (e.g. 'the flow sequence has no closing ']'')

Actual: UNEXPECTED_END the quoted scalar has no closing quote at line 1, column 4
SYNTAX a flow sequence's entries have ',' between them at line 1, column 9
SYNTAX a flow mapping's entries have ',' between them at line 1, column 9
SYNTAX a flow sequence's entries have ',' between them at line 1, column 6

Suggested fix: In FlowSequence and FlowMapping (and FlowCheckedNode), when the token where an entry, ',' or the end should be is STREAM_END, throw ErrorAt(..., UNEXPECTED_END, "the flow sequence has no closing ']'") at the collection's start. Update yaml.fb's Read doc to match, and add the case to data_yaml.fb.

Skeptic: I ran y6.fb. An unclosed quoted scalar gave UNEXPECTED_END. 'a: [1, 2', 'a: {b: 1' and '[1, 2' each gave SYNTAX "... entries have ',' between them" (yaml.fb:661/709, which call Fail on the STREAM_END token). For comparison, TOML's 'a = [1, 2' and 'a = {b = 1' give 'UNEXPECTED_END the array has no ']'' and '... no '}''. stdlib.md says each reader throws UNEXPECTED_END for text that ends inside something, and these texts end inside a flow collection.

## yaml-7 (low): Hex and octal integers past a UInt64 round to the wrong Double

Status: fixed in a52a99e: a hex or octal integer past a UInt64 rounds once, from its leading 64 bits with a sticky bit, scaled by the bits after them; data_yaml.fb gives the finding's two their nearest Doubles

Where: std/data/yaml.fb:302

IntegerOf builds a Double for a 0x or 0o integer past a UInt64 by rounding after every digit, `real = real * base + digit`. That double rounding can miss the nearest Double, which docs/stdlib.md implies ('an integer past a UInt64' is a DOUBLE) and which the decimal path gets right through FloatOf. 0x20000000000001800 is 2^65 + 6144, whose nearest Double is 2^65 + 8192, but it reads as 2^65. 0o40000000000000000140000 is 2^68 + 49152, nearest 2^68 + 65536, but it reads as 2^68. (Checked with Python's float().)

```
// Integers past a UInt64 are Doubles, the nearest: 0x20000000000001800 is 2^65 + 6144, whose
// nearest Double is 2^65 + 8192, 36893488147419111424, and 0o40000000000000000140000 is
// 2^68 + 49152, whose nearest is 2^68 + 65536, 295147905179352891392.
Import std.core
Import std.data.yaml

Local doc = Try Yaml.Read("hex: 0x20000000000001800\noctal: 0o40000000000000000140000")
Print(doc)
```

Expected: {"hex": 36893488147419111424.0, "octal": 295147905179352891392.0}

Actual: {"hex": 36893488147419103232.0, "octal": 295147905179352825856.0}

Suggested fix: For base 8 and 16, round once. Keep the leading 64 bits in the UInt64, set its lowest bit as a sticky bit when any dropped digit is nonzero, convert that to a Double (one rounding), then multiply by 2^(bits dropped), which is exact. Or turn the digits into decimal text for FloatOf. Add the two cases to data_yaml.fb's plains.

Skeptic: I ran y7.fb in debug and release: it printed {"hex": 36893488147419103232.0, "octal": 295147905179352825856.0}, which are exactly 2^65 and 2^68. 0x20000000000001800 is 2^65+6144, and the Double spacing there is 8192, so the nearest Double is 2^65+8192 = 36893488147419111424. 0o40000000000000000140000 is 2^68+49152, the spacing is 65536, and the nearest is 2^68+65536. IntegerOf (yaml.fb:302) rounds after every digit, and its comment admits only the decimal path rounds once. The decimal path does give the nearest Double, so the two are inconsistent, and the hex and octal results aren't the integer's value as a Double.

## yaml-8 (low): A reserved directive gives no token, so a '%' line inside or after a document is skipped

Status: fixed in a52a99e: a reserved directive gives a RESERVED_DIRECTIVE token, which Documents takes where directives go and leaves out; data_yaml.fb gives the finding's three SYNTAX, reads one before a "---", and reads the skeptic's valid fourth

Where: std/data/yaml_scanner.fb:276

FetchDirective returns without a token for any directive that isn't %YAML or %TAG. The parser then can't check where the directive sits, which it does check for %YAML and %TAG. A line like '%include other.yaml' after a document's content is silently dropped, though YAML 1.2 only allows a directive before '---' and after a '...' (PyYAML and libyaml throw). '%FOO bar' with no '---' after it is also read, and so is a reserved directive after a document that no '...' ended.

```
// A '%' line is a directive only before a document's "---", after a "..." ends the one before.
Import std.core
Import std.data.yaml

For text In ["a: 1\nb: 2\n%include other.yaml\n", "a: |\n  b\n%c\n", "%FOO bar\nx\n", "--- a\n%FOO bar\n--- b\n"]
    Try
        Local documents = Try Yaml.ReadAll(text)
        Print("reads", documents.Count, documents[documents.Count - 1])
    Catch e As ParseError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: a SYNTAX ParseError for each of the four

Actual: reads 1 {"a": 1, "b": 2}
reads 1 {"a": "b\n"}
reads 1 "x"
reads 2 "b"

Suggested fix: Emit a token for a reserved directive too (a RESERVED_DIRECTIVE kind, or VERSION/TAG_DIRECTIVE's loop accepting a third kind) that Documents takes with the others, so the 'comes after ...' and 'comes before its ---' checks apply. Add the cases to data_yaml.fb's failures.

Skeptic: I ran y8.fb. 'a: 1\nb: 2\n%include other.yaml' read as {a:1,b:2}, 'a: |\n  b\n%c' read as {a:'b\n'}, and '%FOO bar\nx' read as 'x'. A further case, y8c.fb's '%FOO bar\n...', read 0 documents, while '%YAML 1.2\n...' and '%YAML 1.2\nx' throw "directives come before its '---'". FetchDirective (yaml_scanner.fb:274-279) returns no token for a reserved directive, so Documents' placement checks never see it, and invalid YAML is accepted silently. One correction: the finding's fourth case, '--- a\n%FOO bar\n--- b', is valid YAML. The column-0 line continues the top-level plain scalar, as in yaml-test-suite's XLQ9, and y8b.fb shows it reads correctly as 'a %FOO bar'. So only the other cases are bugs.

## yaml-9 (low): An !!int past a UInt64 says it isn't an integer, not that it's out of range

Status: fixed in a52a99e: Resolve throws OUT_OF_RANGE for an !!int whose text is a core-schema integer that resolved to a Double; data_yaml.fb gives two, decimal and hex, and "1.5" still SYNTAX

Where: std/data/yaml.fb:553

docs/stdlib.md says each reader throws 'OUT_OF_RANGE for a number its type doesn't hold', and TOML does so for an integer past an Int64. With an explicit !!int, an integer past what a UInt64 holds resolves to a DOUBLE, and Resolve throws SYNTAX with the message that the text isn't an integer, which it is.

```
// An !!int past what a UInt64 holds is an integer out of range, not text that isn't one.
Import std.core
Import std.data.yaml

Try
    Yaml.Read("a: !!int 99999999999999999999")
Catch e As ParseError
    Print(e.Kind, e.Message)
End Try
```

Expected: OUT_OF_RANGE the integer is past what a UInt64 holds at line 1, column 10 (or similar)

Actual: SYNTAX !!int tags an integer, and "99999999999999999999" isn't one at line 1, column 10

Suggested fix: In Resolve's !!int branch, when the text matches the core schema's integer forms but resolved to a DOUBLE (IntegerOf's !whole path), throw ErrorAt(..., OUT_OF_RANGE, ...) instead. Add the case to data_yaml.fb.

Skeptic: I ran y9.fb: 'a: !!int 99999999999999999999' gave 'SYNTAX !!int tags an integer, and "99999999999999999999" isn't one at line 1, column 10'. The text is a core-schema integer, but CoreValue resolved it to a DOUBLE, so Resolve's check at yaml.fb:553 fails it as not an integer. stdlib.md says readers throw OUT_OF_RANGE for a number its type doesn't hold, and the message is also factually wrong.
