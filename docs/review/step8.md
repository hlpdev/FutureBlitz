# The review of M6 step 8's Markdown half: std.text.markdown over md4c 0.5.2 (MarkdownToHtml, ParseMarkdown's MarkdownNode tree, extensions), passing CommonMark 0.31.2's spec examples (the template half waits for the Value type)

12 confirmed findings: 0 high, 5 medium, 7 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S8-build-2 (medium): PlainText() of a document, list, or table runs its blocks' words together

Status: fixed in 5b815a47, as S8-tree-2

Where: docs/stdlib.md:303

docs/stdlib.md says "PlainText() gives the text in a node without its markup". But MarkdownNode.PlainText() (std/text/markdown.fb:107) joins its children with "", so for any block container (Document, Quote, lists, ListItem, Table, TableRow) the text of separate paragraphs, items, and cells is glued into one word. A program that takes a document's plain text, for a preview or a search index, gets "applespears". The test program only calls PlainText on a heading and an image paragraph.

```
Import std.core
Import std.text.markdown

Print("[" + ParseMarkdown("# Title\n\nFirst paragraph.\n\nSecond one.").PlainText() + "]")
Print("[" + ParseMarkdown("- apples\n- pears").PlainText() + "]")
Print("[" + ParseMarkdown("| a | b |\n|---|---|\n| 1 | 2 |").PlainText() + "]")
```

Expected: Blocks, items, and cells kept apart, e.g. [Title\nFirst paragraph.\nSecond one.], [apples\npears], [a b\n1 2] (or else docs that limit PlainText to spans and headings)

Actual: [TitleFirst paragraph.Second one.]
[applespears]
[ab12]

Suggested fix: Have PlainText put a newline between block children, and a separator between table cells, with a test on a Document. Otherwise narrow the docs and the comment to inline content. That choice is a design change for the user.

Skeptic: I ran rev-scratch/s8/build/plain.fb in debug and release. Both printed `[TitleFirst paragraph.Second one.]`, `[applespears]` and `[ab12]`. std/text/markdown.fb's PlainText() joins every child's PlainText with "", so the words of separate blocks, list items and cells run together. docs/stdlib.md says "`PlainText()` gives the text in a node without its markup", and "applespears" is not the text of "- apples\n- pears". The code contradicts stdlib.md for every block container (Document, Quote, lists, ListItem, Table, TableRow). Only the method comment narrows it to a heading's, link's or image's words. tests/programs/text_markdown.fb:51 calls it only on a heading and an image paragraph. Fixing it means either putting separators between blocks and cells or narrowing the docs, which is the user's call.

## S8-tree-1 (medium): A raw HTML tag that spans lines becomes several Html nodes

Status: fixed in edd97465

Where: runtime/src/markdown.cpp:274

md4c reports raw inline HTML one line at a time. It sends the tag's text up to the line end, then any trailing blanks, then a "\n" of its own, all as MD_TEXT_HTML. text() merges only Text with Text and pushes a new Html event for each piece. A tag CommonMark allows to span lines, like `<span\nclass="x">` or a multi-line `<!-- -->`, therefore comes out as three or more Html nodes, one of them just "\n". cmark and the header's own rule ('text next to text is one') would give one. Anything that walks the tree to find tags, or reads an Html node's Text as one tag, gets pieces. MarkdownToHtml is unaffected because it just concatenates them. Checked in both debug and release.

```
Import std.core
Import std.collections
Import std.text.markdown

// One raw HTML tag that spans two lines, as CommonMark allows.
Local paragraph = ParseMarkdown("a <span\nclass=\"x\">b</span>").Children[0]
For child In paragraph.Children
    Print("{child.Kind} [{child.Text}]")
End For
```

Expected: Text [a ]
Html [<span
class="x">]
Text [b]
Html [</span>]

Actual: Text [a ]
Html [<span]
Html [
]
Html [class="x">]
Text [b]
Html [</span>]

Suggested fix: In text(), add an MD_TEXT_HTML piece to the previous Html event instead of pushing a new one when either the piece is only blanks or newlines, or the previous Html event's text ends in "\n". md4c sends those pieces only inside one multi-line raw HTML span, because a newline between two separate tags is a SOFTBR. Add a tree test with a multi-line tag and comment.

Skeptic: Ran t1.fb in debug and release. ParseMarkdown("a <span\nclass=\"x\">b</span>") gives a paragraph of Text [a ], Html [<span], Html [\n], Html [class="x">], Text [b], Html [</span>]. A multi-line comment does the same: "a <!-- one\ntwo --> b" gives Html [<!-- one], Html [\n], Html [two -->]. Two separate tags on two lines ("<i>\n</i>") correctly get a SoftBreak between them. MarkdownToHtml prints the tag whole. The cause: md4c.c:4465-4471 sends a raw HTML span's line break as its own MD_TEXT_HTML "\n" (after any trailing blanks), and text() at markdown.cpp:274 merges only TEXT with TEXT, so every MD_TEXT_HTML piece becomes a new Html event. One CommonMark raw-HTML construct ends up as several Html nodes, one of them only "\n". Nothing is lost, but anything that reads Html nodes as tags gets fragments, which goes against the lane's own one-node-per-run merging.

## S8-tree-2 (medium): PlainText() runs the text of separate blocks together

Status: fixed in 5b815a47: each block is on lines of its own, and a row's cells are apart by a tab

Where: std/text/markdown.fb:107

PlainText() joins its children's text with "", whatever they are. Called on a Document, list, table, or quote, it glues the words of neighboring blocks together: 'Hello\n\nWorld' becomes 'HelloWorld', a tight list's 'one' and 'two' become 'onetwo', and a table's cells become 'abcd'. This contradicts its doc ('The text in it, without the markup') and stdlib.md ('gives the text in a node without its markup'). Getting a document's plain text, for an excerpt or a search index, is a normal use. It is also inconsistent: a CodeBlock's Text ends in "\n", so a code block does get separated from the next block while paragraphs don't. Same in debug and release.

```
Import std.core
Import std.collections
Import std.text.markdown

Print("[" + ParseMarkdown("Hello\n\nWorld").PlainText() + "]")
Print("[" + ParseMarkdown("- one\n- two").PlainText() + "]")
Print("[" + ParseMarkdown("# Title\nBody text").PlainText() + "]")
Print("[" + ParseMarkdown("| a | b |\n|---|---|\n| c | d |").PlainText() + "]")
```

Expected: Each block's words kept apart, e.g. [Hello
World], [one
two], [Title
Body text], and [a b
c d] (or whichever separator the docs settle on)

Actual: [HelloWorld]
[onetwo]
[TitleBody text]
[abcd]

Suggested fix: Separate block children (kinds below Emphasis) with a newline: between paragraphs, headings, list items, quotes, and table rows. Separate a row's cells with a space or tab. Drop or normalize a CodeBlock's trailing newline so the separators stay uniform. Say in the doc comment and stdlib.md what separates blocks, and test PlainText() on a Document with several blocks, a list, and a table.

Skeptic: Ran t2.fb and t2b.fb in debug and release. Output: [HelloWorld], [onetwo], [TitleBody text], [abcd]. Within one paragraph, "Hello\nWorld" gives [Hello\nWorld] (the soft break becomes "\n"), but two paragraphs "Hello\n\nWorld" give [HelloWorld]. A bigger break separates less than a line break does, and words from different blocks merge into one. "    code\n\nafter" gives [code\nafter] only because the CodeBlock's Text keeps its trailing newline. markdown.fb:114-118 joins children with "" whatever their kind. PlainText() is public on every node, Document and Table included, and its doc ('The text in it, without the markup ... with a newline for each break') and stdlib.md ('gives the text in a node without its markup') don't fit fused words like 'HelloWorld' or 'abcd'.

## S8-tree-3 (medium): PlainText() overflows the stack on deeply nested Markdown

Status: fixed in 5b815a47: PlainText walks the tree with a stack of its own

Where: std/text/markdown.fb:115

PlainText() recurses once per tree level. ParseMarkdown builds the tree with an explicit stack and the runtime releases it iteratively (decision 678), so both handle 100,000 nested block quotes, and so does MarkdownToHtml (checked). PlainText() on the same tree crashes with a stack overflow (0xC00000FD): from about 7,000 levels in a debug build and about 20,000 in a release build (5,000 works in both; 7,000 crashes debug only; 20,000 crashes both). That is 7 to 20 KB of '>' characters, so untrusted Markdown, like a user comment, can crash the program through std's own method. The debug and release limits also differ.

```
Import std.core
Import std.collections
Import std.text.markdown

// 20,000 nested block quotes: 20 KB of Markdown.
Local parts: String[] = []
For i = 1 To 20000
    parts.Add(">")
End For
Local document = ParseMarkdown(parts.Join("") + " a")
Print("parsed")
Print(document.PlainText())
```

Expected: parsed
a

Actual: No output: the process exits with 0xC00000FD (stack overflow) in both debug and release (fb reports 'exited with code 0xC00000FD'). 'parsed' is lost with the unflushed stdout.

Suggested fix: Walk the tree iteratively in PlainText(), with an explicit stack of (node, next child index) or a pre-order list, appending to one parts array. Add a test that takes PlainText() of a document nested about 100,000 deep.

Skeptic: Ran t3.fb (20,000 nested '>'): debug and release both print 'error: t3.exe exited with code 0xC00000FD' and nothing else. t3b.fb (7,000): debug crashes with 0xC00000FD, release prints 'parsed' and 'a'. As a control, t3c.fb parses 100,000 nested quotes, walks down to depth 100002 iteratively, and gets MarkdownToHtml's result, all fine ('parsed 100002 [a]', 'True'). So only PlainText() fails, and it fails because it recurses once per level (markdown.fb:115-116). Markdown of about 7 KB can crash a debug program, and 20 KB a release one, through std's own method, even though ParseMarkdown and the runtime's release (decision 678) were built to handle this depth.

## S8-tree-4 (medium): A code span in a table cell keeps its \| instead of giving |

Status: fixed in ee7ea462, in the tree and, through patch_md4c.cmake, in md4c's HTML

Where: runtime/src/markdown.cpp:254

GitHub's tables spec (example 200) makes `\|` in a cell a literal pipe, including inside code spans: `| b `\|` az |` gives <code>|</code>. md4c 0.5.2 handles this for ordinary text and for **strong** text, but passes a code span's text through raw. The Code node's Text is therefore `\|` and MarkdownToHtml gives <code>\|</code>. The module and stdlib.md say they provide 'GitHub's tables', and code containing a pipe in a table cell is common in docs (shell pipes, union types like `string \| number`). The CommonMark spec tests don't cover it because tables are an extension.

```
Import std.core
Import std.collections
Import std.text.markdown

// GitHub's tables spec, example 200: a \| in a cell is a |, in a code span too.
Local document = ParseMarkdown("| f\\|oo  |\n| ------ |\n| b `\\|` az |\n| b **\\|** im |")
Local body = document.Children[0].Children[1]
Print(document.Children[0].Children[0].Children[0].Children[0].PlainText())
Print(body.Children[0].Children[0].Children[1].Text)
Print(body.Children[1].Children[0].Children[1].PlainText())
Print(MarkdownToHtml("| f\\|oo  |\n| ------ |\n| b `\\|` az |"))
```

Expected: f|oo
|
|
... <td>b <code>|</code> az</td> ...

Actual: f|oo
\|
|
... <td>b <code>\|</code> az</td> ...

Suggested fix: In the tree builder, when a Code span is inside a TH or TD, replace each \| in its text with | (the parse_t open stack already knows the enclosing cell). For HTML, do the same in a text hook around md4c-html's code-span text, or render through the same event path. If that isn't wanted, document it as md4c's difference from GFM. Either way, test GFM example 200.

Skeptic: Ran t4.fb (fixed so the \\| escapes reach the Markdown; a bash heredoc had removed them) in debug and release. Output: header 'f|oo' (correct), the code span's Text '\|' (wrong, GFM gives '|'), the strong text '|' (correct). MarkdownToHtml, with the defaults and with GitHub, gives <td>b <code>\|</code> az</td>, but GFM spec example 200 gives <code>|</code>. text() at markdown.cpp:254-260 appends a code span's text as md4c sends it, and md4c leaves the \| inside code spans. markdown.fb's header and the MarkdownExtensions doc promise 'GitHub's tables', so this is visible wrong output for a common pattern (pipes or union types in code inside a table).

## S8-build-1 (low): A leading UTF-8 BOM turns the first block into a paragraph, in HTML and in the tree

Status: fixed in c9bcc97c

Where: runtime/src/markdown.cpp:327

fb_markdown_html calls md_html with renderer flags 0, so MD_HTML_FLAG_SKIP_UTF8_BOM is off, and fb_markdown_parse passes the BOM straight to md_parse. When text starts with U+FEFF, which Windows editors often save, the first line's '#', '>', '-' or fence no longer starts a block. md4c's own md2html sets MD_HTML_FLAG_SKIP_UTF8_BOM (md2html.c:39), its pathological-tests.py expects "﻿foo" to give "<p>foo</p>", and cmark drops a leading BOM too. A release build gives the same output.

```
Import std.core
Import std.text.markdown

Print(MarkdownToHtml("\u{FEFF}# Title\n\ntext"))
Local doc = ParseMarkdown("\u{FEFF}# Title\n\ntext")
Print(doc.Children[0].Kind, doc.Children[0].PlainText().Bytes.Count)
```

Expected: <h1>Title</h1>
<p>text</p>

Heading 5

Actual: <p>﻿# Title</p>
<p>text</p>

Paragraph 10   (the U+FFEF bytes EF BB BF stay at the start of the paragraph)

Suggested fix: In markdown_of(), drop a leading EF BB BF before md4c reads the text, so the HTML and the tree both skip it, and add a test for both.

Skeptic: I ran rev-scratch/s8/build/bom.fb in debug and release. Both printed `<p>` EF BB BF `# Title</p>\n<p>text</p>` then `Paragraph 10` (checked with od -c), so the BOM stays and the heading is lost, in HTML and in the tree. In runtime/src/markdown.cpp, markdown_of() copies the bytes and only replaces NULs, and fb_markdown_html passes renderer flags 0 to md_html. md4c-html.c:556-561 skips a BOM only under MD_HTML_FLAG_SKIP_UTF8_BOM. md4c's own md2html.c:39 sets that flag, its pathological-tests.py:23-24 expects "﻿foo" to give <p>foo</p>, cmark (CommonMark's reference) skips a leading BOM, and the project's own spec (01-lexical.md, decisions.md:29) ignores one. Severity is low, not medium: a BOM rarely reaches a String once std.text.encoding's planned BOM detection decodes files. The fix is to strip a leading EF BB BF in markdown_of(), with a test for both paths.

## S8-build-6 (low): The C++ port of normalize.py has no attribution; NOTICE.md covers only spec.json

Status: fixed in 4b20fb30

Where: tests/runtime/data/commonmark/NOTICE.md:1

markdown_tests.cpp:194-201 says html_normalizer_t is "HTML as CommonMark's normalize.py has it". It follows normalize.py's MyHTMLParser handler for handler and copies its 50 block tags verbatim, in the same order. md4c's test/LICENSE.md, which ships with that normalize.py, says the normalization code came from Karl Dubost's markdowntest under the MIT license (Copyright (c) 2013 Karl Dubost), and that CommonMark's test software is John MacFarlane's under a BSD license. Both require their notice in copies or substantial portions. NOTICE.md has only the spec's CC-BY-SA notice, and markdown_tests.cpp has no copyright or license text. I found this by reading the files; no program can show it.

```
// Not a runtime defect: shown by reading tests/runtime/markdown_tests.cpp:194-215 and
// md4c 0.5.2's test/LICENSE.md and test/normalize.py.
Import std.core

Print("see description")
```

Expected: NOTICE.md, or a comment above html_normalizer_t, gives normalize.py's MIT notice (Karl Dubost, 2013) and CommonMark's test-software BSD notice (John MacFarlane, 2014).

Actual: grep finds no Dubost, MIT, BSD, copyright, or license text in markdown_tests.cpp, and NOTICE.md covers only spec.json.

Suggested fix: Add both notices to tests/runtime/data/commonmark/NOTICE.md, saying they cover markdown_tests.cpp's html_normalizer_t, and point to it from the class's comment.

Skeptic: I compared tests/runtime/markdown_tests.cpp:194-420 with md4c 0.5.2's test/normalize.py (in build-rev4/_deps/md4c-src/test). html_normalizer_t is a handler-by-handler translation of MyHTMLParser: data, endtag and starttag with the br and block-tag stripping, startendtag as a start tag, comments, decls, PIs, refs and output_char's four escapes, plus the CDATA pass-through. Its 50-entry block-tag array is copied in the same order. The lane itself calls it "a C++ port of normalize.py", and the class comment says "HTML as CommonMark's normalize.py has it". md4c's test/LICENSE.md puts that test software under John MacFarlane's BSD license ("Redistributions of source code must retain the above copyright notice") and normalize.py's code under Karl Dubost's 2013 MIT license. grep finds no copyright or license text in markdown_tests.cpp, and tests/runtime/data/commonmark/NOTICE.md covers only spec.json's CC-BY-SA. The project attributes the test data it took, but not this port. The fix is cheap: add both notices to NOTICE.md and point to it from the class comment.

## S8-html-1 (low): A numeric reference to a surrogate becomes three U+FFFD in MarkdownToHtml's text and titles, and %ED%A0%80 in a URL, where CommonMark and ParseMarkdown have one U+FFFD

Status: fixed in ee7ea462, through patch_md4c.cmake

Where: runtime/src/markdown.cpp:327

md4c-html's render_utf8_codepoint (md4c-src/src/md4c-html.c:197) only replaces code point 0 and code points past U+10FFFF. It UTF-8-encodes a surrogate from &#xD800;/&#55296; as the bytes ED A0 80. fb_markdown_html passes those bytes to fb_string_from_bytes, which turns each bad byte into its own U+FFFD, so one reference comes out as three. In an href, md4c percent-encodes the raw surrogate bytes, giving /%ED%A0%80, which isn't valid UTF-8. CommonMark says an invalid code point becomes U+FFFD, and cmark and commonmark.js give one U+FFFD (%EF%BF%BD in an href). The runtime's own append_code_point already does this for the tree, so ParseMarkdown and MarkdownToHtml disagree on the same input. Nothing tests the HTML side. The spec has no surrogate example, and markdown_tests.cpp checks surrogates only in the tree's events.

```
Import std.core
Import std.text.markdown

// CommonMark has a reference to an invalid code point as one U+FFFD, as cmark and commonmark.js
// give it, and as ParseMarkdown's tree does.
Local html = MarkdownToHtml("&#xD800;")
Print(html.Chars.Count, html.Bytes.Count)
Print(ParseMarkdown("&#xD800;").Children[0].Children[0].Text.Chars.Count)
Print(MarkdownToHtml("[a](/&#55296; \"&#xDFFF;\")"))
```

Expected: 9 11
1
<p><a href="/%EF%BF%BD" title="�">a</a></p>



Actual: 11 17
1
<p><a href="/%ED%A0%80" title="���">a</a></p>

(same in debug and release)

Suggested fix: Make md4c-html treat D800-DFFF the way it treats 0 and anything past 10FFFF. One option is a FetchContent PATCH_COMMAND on md4c-html.c:197 so the condition reads `0 < codepoint && codepoint <= 0x10ffff && (codepoint < 0xd800 || codepoint > 0xdfff)`. Post-processing the output can't fix the href, because a literal %ED%A0%80 in the input must pass through unchanged. Add a markdown_tests.cpp case: html_of("&#xD800; [a](/&#55296; \"&#xDFFF;\")") == "<p>\xEF\xBF\xBD <a href=\"/%EF%BF%BD\" title=\"\xEF\xBF\xBD\">a</a></p>\n".

Skeptic: I wrote h1.fb under C:/Users/Hunter/Projects/FutureBlitz/.claude/worktrees/rev-scratch/s8/html-refute/ exactly as the finding has it. Debug and release both print `11 17`, then `1`, then `<p><a href="/%ED%A0%80" title="���">a</a></p>`. So MarkdownToHtml("&#xD800;") gives three U+FFFD, while ParseMarkdown's text for the same input is one. The code agrees. In md4c-html.c, render_utf8_codepoint only swaps in U+FFFD under `if(0 < codepoint && codepoint <= 0x10ffff)`, so a surrogate goes out as the raw bytes ED A0 80. fb_markdown_html (markdown.cpp) hands md4c's bytes straight to fb_string_from_bytes, and that repairs each bad byte into its own U+FFFD. The href is percent-encoded from those raw bytes. The runtime's own append_code_point says CommonMark makes a surrogate U+FFFD, and that's what the tree does. CommonMark says invalid code points become U+FFFD. So the HTML path breaks the rule the runtime states, and the two APIs disagree on the same input. No spec example has a surrogate, so the 652 still pass and the milestones row holds. The input is pathological and the damage is only replacement characters, so I rate it low, not medium.

## S8-html-2 (low): Autolinks/GitHub leave common bare URLs unlinked or cut short (ports, %-escapes, ~, parentheses in a path, commas), unlike GitHub's Markdown that the docs promise

Status: fixed in c4b34984: the docs give md4c's rules, and text_markdown pins them

Where: std/text/markdown.fb:29

markdown.fb says MarkdownExtensions are "GitHub's ... links from bare URLs", and on GitHub: "All four, as GitHub's Markdown has them". stdlib.md:293 says `Autolinks` gives "bare URLs, www. addresses, and e-mail addresses". md4c 0.5.2's permissive URL autolinks are much narrower. By design (md4c's own test/spec-permissive-autolinks.txt), a path may hold only letters, digits, '/', '.', '-' and '_', and the host is followed by nothing but a path, query, or fragment. Any other character, such as ':' for a port, '%', '~', '+', '@', or '(' in a path, causes the right-boundary check to fail. Then nothing is linked at all, or the link is cut short (a comma mid-path). GFM's extended autolinks take any non-space, non-'<' characters after a valid domain and trim only trailing punctuation, so GitHub links each of these URLs whole. URLs with ports, %20, or Wikipedia-style parentheses are everyday input. No test pins the actual rules.

```
Import std.core
Import std.text.markdown

// Each is a bare URL GitHub's Markdown links whole.
Print(MarkdownToHtml("http://example.com:8080/a\n\n" +
                     "https://example.com/a%20b\n\n" +
                     "https://example.com/~me\n\n" +
                     "https://en.wikipedia.org/wiki/C_(programming_language)\n\n" +
                     "https://example.com/a,b", MarkdownExtensions.GitHub))
```

Expected: <p><a href="http://example.com:8080/a">http://example.com:8080/a</a></p>
<p><a href="https://example.com/a%20b">https://example.com/a%20b</a></p>
<p><a href="https://example.com/~me">https://example.com/~me</a></p>
<p><a href="https://en.wikipedia.org/wiki/C_(programming_language)">https://en.wikipedia.org/wiki/C_(programming_language)</a></p>
<p><a href="https://example.com/a,b">https://example.com/a,b</a></p>



Actual: <p>http://example.com:8080/a</p>
<p>https://example.com/a%20b</p>
<p>https://example.com/~me</p>
<p>https://en.wikipedia.org/wiki/C_(programming_language)</p>
<p><a href="https://example.com/a">https://example.com/a</a>,b</p>

(same in debug and release; three more GFM spec autolink examples differ too: www.google.com/search?q=(business))+ok, ...commonmark&hl;, www.commonmark.org/he<lp)

Suggested fix: One option is to say in markdown.fb (lines 19-20 and 29) and stdlib.md:293 which bare URLs md4c links: http/https/ftp, a host of letters, digits, '.', '-' and '_', and a path of those plus '/', then a query and a fragment. Anything else, ports and %-escapes included, needs <...>. Drop "as GitHub's Markdown has them", and add a text_markdown case that pins these rules. The other option is to patch md4c to follow GFM's extended-autolink rules.

Skeptic: Ran h2.fb in debug and release. Both print `<p>http://example.com:8080/a</p>`, `<p>https://example.com/a%20b</p>`, `<p>https://example.com/~me</p>` and `<p>https://en.wikipedia.org/wiki/C_(programming_language)</p>`, none of them linked, then `<p><a href="https://example.com/a">https://example.com/a</a>,b</p>`. This is md4c's documented permissive-autolink rule (test/spec-permissive-autolinks.txt: scheme, host, then a path of a limited set of characters, and the autolink must be followed by whitespace or a few punctuation marks). GFM's extended URL autolinks link all five whole. stdlib.md and the milestones row don't promise GFM parity. md4c's own MD_DIALECT_GITHUB is these same four flags, so calling the combination `GitHub` isn't the bug. The bug is markdown.fb's comments, which say more than md4c does. Line 29 says "All four, as GitHub's Markdown has them" and line 1 says "GitHub's ... autolinks", but common URLs (a port, %20, ~, Wikipedia-style parentheses) aren't linked, and one with a comma is cut short. This is a documentation fix: state md4c's rules and pin them in a test. md4c doesn't need patching, so low.

## S8-html-3 (low): GitHub tables aren't GitHub's: a header with more cells than the delimiter row becomes a table that silently drops cells, and \| in a cell's code span keeps its backslash

Status: fixed in ee7ea462 (\| in a cell's code span) and c4b34984 (the docs give md4c's table rules, and text_markdown pins them)

Where: std/text/markdown.fb:19

markdown.fb calls the tables extension "GitHub's tables", and says GitHub is "as GitHub's Markdown has them". GFM's spec says the header row must have the same number of cells as the delimiter row, or the lines aren't a table. md4c builds the table anyway, with the delimiter row's column count, and drops the extra header and body cells ("b" and "2" below vanish from the output). GFM's spec (the f\|oo / b `\|` az example) makes \| a literal '|' in a cell even inside a code span. md4c's documented rule (its test/spec-tables.txt) is that pipes inside code spans don't split cells, and it leaves the backslash in, so a README table written for GitHub shows `x \| y`. md4c also suppresses tables over 128 columns. The docs mention none of this, and no test pins it.

```
Import std.core
Import std.text.markdown

// GitHub's tables: a header row with more cells than the delimiter row isn't a table, and "\\|"
// is a '|' in a cell, inside a code span too.
Print(MarkdownToHtml("| a | b |\n|---|\n| 1 | 2 |", MarkdownExtensions.GitHub))
Print(MarkdownToHtml("| a |\n|---|\n| `x \\| y` |", MarkdownExtensions.GitHub))
```

Expected: <p>| a | b |
|---|
| 1 | 2 |</p>

<table>
<thead>
<tr>
<th>a</th>
</tr>
</thead>
<tbody>
<tr>
<td><code>x | y</code></td>
</tr>
</tbody>
</table>



Actual: <table>
<thead>
<tr>
<th>a</th>
</tr>
</thead>
<tbody>
<tr>
<td>1</td>
</tr>
</tbody>
</table>

<table>
<thead>
<tr>
<th>a</th>
</tr>
</thead>
<tbody>
<tr>
<td><code>x \| y</code></td>
</tr>
</tbody>
</table>

(same in debug and release)

Suggested fix: Either say in markdown.fb and stdlib.md that the tables are md4c's: the delimiter row sets the column count and extra cells are dropped, pipes in code spans don't split cells and \| keeps its backslash there, and the limit is 128 columns. Drop "GitHub's" and "as GitHub's Markdown has them" and pin the behavior in a test. Or patch md4c to follow GFM: reject a table whose header and delimiter cell counts differ, and unescape \| in cells.

Skeptic: The heredoc mangled the finding's program (`\\|` became `\|`, an fb compile error), so I rewrote h3.fb with the Write tool. Debug and release both print a one-column table, `<th>a</th>` and `<td>1</td>`, for `| a | b |\n|---|\n| 1 | 2 |`. The header text `b` and the body cell `2` are dropped without a word. The second input prints `<td><code>x \| y</code></td>`. GFM's spec says a header row whose cell count differs from the delimiter row's isn't a table, and its f\|oo example makes `\|` a '|' inside a code span. md4c does neither. Its spec-tables.txt only says pipes in code spans don't split cells, it never documents the dropped header cells, and TABLE_MAXCOLCOUNT is 128 in md4c.c. These are md4c's semantics, which the design chose, and stdlib.md only says "tables". But markdown.fb calls them "GitHub's tables" and says GitHub is "as GitHub's Markdown has them", and the program shows otherwise, including text the user wrote vanishing. This is a doc fix plus a test that pins the behavior, so low.

## S8-html-5 (low): The test's normalize.py port claims it's "never laxer" than Python's, but it equates HTML that normalize.py tells apart

Status: fixed in 4ac2fd91 and 511b24cb: the port keeps what normalize.py keeps

Where: tests/runtime/markdown_tests.cpp:198

html_normalizer_t's comment says it's "stricter than Python's HTMLParser in places, never laxer". referenced() decodes an empty numeric reference, "&#;" or "&#x;", as code point 0. normalize.py's HTMLParser keeps it verbatim, because its charref pattern needs a digit. start_tag() treats a '/' at the end of an unquoted attribute value as self-closing, so <a href=x/> and <a href=x /> both come out as <a href="x">. normalize.py keeps href="x/" for the first. The spec result itself holds: running md4c's output and the expected HTML through md4c's own test/normalize.py also gives 652 of 652, the 111 non-exact ones differ only in whitespace and <br /> against <br>, and the "1280 of the 1304" count is right. Only the "never laxer" claim is wrong. (Shown with the class copied into a driver. This is C++ test code, so the program below is a Catch2 case to put in markdown_tests.cpp, not a .fb program.)

```
// C++ (append to tests/runtime/markdown_tests.cpp, which has normalized() in scope):
TEST_CASE("the normalizer tells apart what normalize.py does", "[runtime]") {
    using namespace std::string_view_literals;
    // normalize.py: "<p>&#;</p>" against "<p>\0</p>", and <a href="x/"> against <a href="x">.
    CHECK(normalized("<p>&#;</p>") != normalized("<p>&#0;</p>"));
    CHECK(normalized("<a href=x/>") != normalized("<a href=x />"));
}
```

Expected: Both CHECKs pass, as with Python's normalize.py: '<p>&#;</p>' vs '<p>\x00</p>', and '<a href="x/">' vs '<a href="x">'.

Actual: Both fail: the port gives '<p>\x00</p>' for both of the first pair and '<a href="x">' for both of the second.

Suggested fix: Reword the comment: it's stricter in places, and laxer only for HTML that neither md4c nor the spec writes (an empty numeric reference, or an unquoted value ending in '/'). Or keep a digitless &#; / &#x; verbatim in referenced(), and treat the '/' as self-closing only after whitespace or a quoted value, with this test.

Skeptic: I copied md4c's test/normalize.py (read in full, benign) to my scratch folder and ran it with `python -I` on Python 3.12. It gives '<p>&#;</p>' -> '<p>&#;</p>', '<p>&#0;</p>' -> '<p>\x00</p>', '<p>&#x;</p>' -> '<p>&#x;</p>', '<a href=x/>' -> '<a href="x/">', and '<a href=x />' -> '<a href="x">'. Then I built a driver (norm_driver.cpp) from markdown_tests.cpp lines 23-49 and 201-575 (append_utf8, digit_value, html_normalizer_t, normalized) plus md4c's entity.c. It prints `<p>\x00</p>` for all of &#;, &#0; and &#x;, `<a href="x">` for both `<a href=x/>` and `<a href=x />`, and `pair1 differ: 0` / `pair2 differ: 0`. Two places cause this. referenced() runs the digit loop over an empty name, giving code point 0. start_tag() treats any trailing '/' as self-closing, even inside an unquoted value. So the comment's "never laxer" (and "tells apart all that one does") is false: the port treats as equal HTML that normalize.py tells apart. Neither md4c nor the spec examples produce these forms, so the spec result stands. The fix is to the comment or the port, plus a test, so low.

## S8-tree-5 (low): Tight's docs suggest only HTML drops a tight list's paragraphs, but the tree drops them too

Status: fixed in c4b34984

Where: std/text/markdown.fb:84

The field comment says a tight list is one 'which HTML shows without paragraphs', and stdlib.md lists only 'a list's Start and Tight'. Both suggest that, as in cmark's and other CommonMark trees, the tree still has a Paragraph in each item and only the HTML leaves it out. In fact md4c sends no paragraph block for a tight list, so a tight item's Text sits directly under its ListItem, while the same item in a loose list sits under a Paragraph. Code that walks the tree looking for Paragraphs misses every tight list item's text. The code may be what the lane intended, but the docs don't say so.

```
Import std.core
Import std.collections
Import std.text.markdown

// The same item, in a tight list and in a loose one.
Local tight = ParseMarkdown("- one\n- two").Children[0]
Local loose = ParseMarkdown("- one\n\n- two").Children[0]
Print(tight.Tight, tight.Children[0].Children[0].Kind)
Print(loose.Tight, loose.Children[0].Children[0].Kind)
```

Expected: Per the docs' wording: True Paragraph / False Paragraph (or docs that say a tight item holds its text directly)

Actual: True Text
False Paragraph

Suggested fix: Say in the Tight comment and in stdlib.md's Markdown bullet that a tight list's items hold their spans and text directly, with no Paragraph. Alternatively, wrap a tight item's leading inlines in a Paragraph node in ParseMarkdown so the tree matches other CommonMark trees.

Skeptic: Ran t5.fb in debug and release: 'True Text' and 'False Paragraph'. A tight list's item holds its Text directly, while the same item in a loose list holds a Paragraph. text_markdown.out shows this too ('ListItem / Text "three"'), so the lane meant it. But the Tight comment at markdown.fb:84-86 ('which HTML shows without paragraphs') reads as if only the HTML drops the paragraph, as in CommonMark's model where tight items still contain paragraphs and only rendering differs. Neither that comment nor stdlib.md tells a tree-walker that tight items have no Paragraph. This is a doc fix only.

## Refuted by the skeptics (no action)

- S8-html-4: A leading UTF-8 BOM isn't skipped, so the document's first block is wrong (a heading becomes a paragraph starting with U+FEFF). Ran h4.fb in debug and release. Both print `<p>﻿# Title</p>` and a Chars.Count of 16, so the BOM isn't skipped. That isn't wrong by CommonMark. The spec never mentions a BOM: md4c's test/spec.txt has no "byte order" or "BOM". U+FEFF is an ordinary character, and a '#' after it isn't at a line's start within 0 to 3 spaces of indentation, so a paragraph is the spec's answer. Skipping it is a convention. md4c's md2html CLI turns on MD_HTML_FLAG_SKIP_UTF8_BOM by default, which is why pathological-tests.py expects <p>foo</p>, and cmark strips it on input. The md4c library leaves it off. stdlib.md and the milestones row say nothing about a BOM in Markdown text. The project's BOM rules belong elsewhere: spec 01-lexical and decisions.md:29 cover .fb source, and decisions.md:599 puts BOM detection in std's text readers. By the time text is a String, a leading U+FEFF is already part of it. Not a bug against the spec or the docs.
- S8-tree-6: The tree test never checks Strong, Code, SoftBreak, Html, a None alignment, or a loose OrderedList. Ran t6.fb in debug: it prints exactly the expected lines (Strong [], Text [ ], Code [b], SoftBreak [], Html [<i>], Text [c], Html [</i>], 'TableHeaderCell None', 'False'). So every kind and value the finding names works. The finding is only that tests/programs/text_markdown.fb never puts Strong, Code, SoftBreak, inline Html, a None alignment, or a loose OrderedList in a tree. markdown_tests.cpp does check code_kind 20 against the runtime. No behavior is wrong and nothing contradicts the spec, stdlib.md, or the milestones row, so this is a coverage suggestion, not a defect. The tests that S8-tree-1 to 5 need would cover most of these kinds anyway.
- S8-build-3: No driver or CLI test checks that Markdown links md4c, only when used, with md4c's notice. The behavior is right, as the finding admits. html.fb printed `Bundled md4c 0.5.2, under the MIT license; ship its notices ...: .../third_party/notices/md4c.txt` and `<h1>Hi <em>there</em></h1>`. nm found 3 md_parse/md_html/entity_lookup symbols in html.exe and 0 in a MarkdownNode-only program, which printed no Bundled line. objdump shows only KERNEL32 and api-ms-win-crt DLLs. The notice file is md4c's LICENSE.md, with "Copyright © 2016-2024 Martin Mitáš". The missing test follows existing practice: driver_tests.cpp:628 and cli_tests.cpp:390 cover only libuv, SQLite and mbedTLS, and five other bundled libraries (xxHash, BLAKE3, Argon2, crypt_blowfish, Monocypher) have no such cases either. Linking is driven by the bundled_libraries table, and tests/programs/text_markdown.fb would fail to link if the fb_markdown_ prefix or the archive were wrong. No wrong behavior, and nothing contradicts the spec, stdlib.md or the milestones row.
- S8-build-4: The spec test runs only with no extensions, not with MarkdownToHtml's default. I ran ext.fb. The default and None both gave `<p>a | b</p>\n<ul>\n<li>c | d</li>\n</ul>`, and the finding's own harness reports 652/652 with extensions 3. So nothing is wrong. markdown_tests.cpp:664 runs the spec loop with html_of(markdown), extensions 0. That is what stdlib.md claims: "CommonMark, which it passes all 652 of 0.31.2's spec examples of". The pass claim is about CommonMark alone (None); tables and task lists are extensions, not CommonMark. The milestones row ("Markdown passes CommonMark's spec tests") is met by the None run. Also running the default would be a coverage nicety, not a bug fix.
- S8-build-5: docs/stdlib.md doesn't say MarkdownToHtml passes raw HTML and javascript: links through. I ran xss.fb and got `<script>alert(1)</script>\n<p><a href="javascript:alert(2)">click</a> <img src=x onerror=alert(3)></p>`. That is what CommonMark requires: raw HTML blocks and inline HTML pass through, and link destinations aren't filtered. The spec examples the step must pass depend on it, and spec_tests runs cmark in unsafe mode. stdlib.md says the module is CommonMark and passes the spec, so the behavior matches the docs. A safety sentence or a NoHtml option (MD_FLAG_NOHTML) is a design suggestion for the user, not a defect against the spec, stdlib.md or the milestones row.
