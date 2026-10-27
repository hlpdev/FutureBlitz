# The review of M6 step 8's template half: std.text.template, Mustache semantics in Handlebars' spelling over Values and Maps

6 confirmed findings: 0 high, 4 medium, 2 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## parse-1 (medium): A block's tag with a blank after '#', or a line break in it, is BAD_TEMPLATE, though {{/ if }} and {{ ! }} are read

Status: fixed in 75e45023: a block's tag skips blanks, line breaks among them, after its '#', and its word ends at the next blank, line breaks too; text_template renders a padded {{# if title }}, an {{#each}} with a line feed before its name, and an {{#unless}} with a tab and line feed after the '#' and a CRLF before its name

Where: std/text/template.fb:278

Tag's '#' branch takes the block's word as content[1..space], where space is the first Space or Tab after index 1. A blank straight after '#' gives an empty word, and a line feed between the word and the name is part of the word, so both throw "isn't a block". Mustache's spec allows padding here (its Sections 'Padding' test is "|{{# boolean }}={{/ boolean }}|" giving "|=|"), Handlebars' lexer skips whitespace, newlines included, between a tag's tokens, and this reader already accepts the same padding in {{/ if }}, {{> name}}, {{& name}}, and {{ ! }}, because those go through Trimmed. The tests have no padded block tag.

```
Import std.core
Import std.collections
Import std.text.template

Function Show(label: String, text: String, data: Value)
    Try
        Print(label + ": [" + Try Template(text).Render(data) + "]")
    Catch e As FormatError
        Print(label + ": {e.Kind}: {e.Message}")
    End Try
End Function

Local d = Value.Object()
d["name"] = Value("Ann")
Local people = Value.Array()
people.Add(d)
d["people"] = Value([Value("x")])
Show("padded", r"|{{# if name }}={{/ if }}|", d)
Show("over lines", "{{{{#each\npeople}}}}{{{{this}}}}{{{{/each}}}}", d)
```

Expected: padded: [|=|]
over lines: [x]

Actual: padded: BAD_TEMPLATE: line 1, column 2: {{#}} isn't a block; they're {{#each}}, {{#if}}, and {{#unless}}
over lines: BAD_TEMPLATE: line 1, column 1: {{#each
people}} isn't a block; they're {{#each}}, {{#if}}, and {{#unless}}

Suggested fix: In the '#' branch, skip blanks after the '#' (IsBlank, which includes line breaks), then end the word at the next IsBlank byte rather than only at Space or Tab, and take the path from there with Trimmed as now. Add padded and multi-line block tags to tests/programs/text_template.fb.

Skeptic: Ran the program at C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/rv-template/rd/p1.fb and p5.fb in debug and release. `|{{# if name }}={{/ if }}|` gives `BAD_TEMPLATE: line 1, column 2: {{#}} isn't a block`. So does Mustache's own Sections 'Padding' case, `|{{# name }}={{/ name }}|`. `{{#each\npeople}}` gives `{{#each\npeople}} isn't a block`. For comparison, `{{#if  name }}` (two spaces) renders `|=|` and `{{#each\tpeople}}` renders `[x]`. Cause: Tag's '#' branch (std/text/template.fb around line 278) starts the word at index 1 and ends it at the first Space or Tab only. A blank right after '#' gives an empty word, and a line feed becomes part of the word. Close tags, partials, `{{& }}`, and comments go through Trimmed and accept the same padding. docs/stdlib.md calls the templates Mustache-style, and the spec's padding test fails.

## parse-4 (medium): {{this.name}} silently renders nothing instead of the scope's name (or a BAD_TEMPLATE)

Status: fixed in 900cdd35: a path whose first step is "this" is the innermost scope's own name, read from that scope alone, not looked up outward, as Handlebars has it; stdlib.md says so, and text_template renders {{#each this.items}} with {{this.name}}, an outer {{this.title}} inside it that gives nothing, and {{this.title}} at the top

Where: std/text/template.fb:315

PathOf treats only the whole name "this" or "." as the scope itself. "this.name" becomes the steps ["this", "name"], and Lookup searches the scopes for a key named "this", finds none, and renders empty with no error. In Handlebars' spelling, which this template follows, {{this.name}} is the usual way to write a field of an {{#each}}'s item. Here it reads without complaint and gives nothing. The reader already rejects "./name" and "../name" as BAD_TEMPLATE, so this.name is the one form that is accepted but wrong.

```
Import std.core
Import std.collections
Import std.text.template

Local d = Value.Object()
Local people = Value.Array()
For who In ["Ann", "Bo"]
    Local person = Value.Object()
    person["name"] = Value(who)
    people.Add(person)
End For
d["people"] = people
Print("[" + Try Template(r"{{#each people}}{{this.name}} {{/each}}").Render(d) + "]")
```

Expected: [Ann Bo ]   (or a BAD_TEMPLATE saying this.name isn't a name)

Actual: [  ]

Suggested fix: In PathOf, when the first step is "this" and more steps follow, drop it and mark the path as relative to the innermost scope only (Handlebars' meaning), so Lookup starts from that scope and doesn't walk outward. Otherwise, throw BAD_TEMPLATE for a "this." path. Document whichever is chosen and test it.

Skeptic: Ran p4.fb in debug and release. `{{#each people}}{{this.name}} {{/each}}` renders `[  ]`, while `{{name}}` in the same block renders `[Ann Bo ]`. At top level, `{{this.name}}` renders `[]` even though the data has name=Ann. PathOf (std/text/template.fb around line 315) treats `this` as the scope only when it is the whole name, so `this.name` becomes the steps ["this", "name"]. Lookup then searches for a key called `this`, finds none, and prints nothing with no error. docs/stdlib.md defines `{{this}}` as the scope itself and says each later step reads from what the step before gave. With that, `this.name` should be the scope's name, and it is the usual Handlebars way to write an each item's field. The reader already rejects `./name` and `../name` (both give BAD_TEMPLATE `has a step with no name`), so this is the one Handlebars scope form it accepts and then gets wrong without a word.

## render-1 (medium): {{this.name}}, Handlebars' spelling of a name in the current scope, always renders nothing

Status: fixed in 900cdd35, with parse-4, the same bug

Where: std/text/template.fb:315

PathOf only treats `this` as the scope itself when it's the whole name. In `this.name` it becomes a step called "this", and Lookup looks for a key named "this" in every scope, finds none, and gives Null, so the tag renders as nothing and no error is raised. The step's design is Mustache semantics in Handlebars' spelling, and `{{#each items}}{{this.name}}{{/each}}` is one of the most common Handlebars idioms. docs/stdlib.md says `{{this}}` is the scope itself, so `{{this.name}}` should be that scope's name.

```
Import std.core
Import std.text.template

Local item = Value.Object()
item["name"] = Value("apple")
Local data = Value.Object()
data["items"] = Value([item])
data["user"] = item
Print(Try Template(r"{{#each items}}[{{this.name}}]{{/each}} [{{this.user.name}}]").Render(data))
```

Expected: [apple] [apple]

Actual: [] []  (the same in debug and release)

Suggested fix: In PathOf, when the first step is "this" and more steps follow, drop it and mark the path as current-scope-only, the way Handlebars scopes `this.x` to the current context. In Lookup, start a path marked that way at the innermost scope and don't walk outward. Or, more simply, drop the leading "this" and keep the usual lookup. Alternatively, reject `this.` paths with BAD_TEMPLATE. Add `{{this.name}}` to text_template.fb.

Skeptic: Ran the finding's program (rv-template/rendering/r1.fb) in debug and release: both print "[] []" where "[apple] [apple]" is meant. In std/text/template.fb:315 PathOf only maps `this` to the scope when it's the whole name (`If whole == "this" || whole == "." Then Return []`). In `this.name`, "this" becomes an ordinary first step, and Lookup searches the scopes for a key named "this", so the tag renders as nothing and nothing is thrown. docs/stdlib.md says `{{this}}` is the scope itself, and the step's design is Handlebars' spelling, where `{{this.name}}` is a common idiom. Silently printing nothing is wrong either way: the fix is to drop or scope a leading `this` step, or to reject it with BAD_TEMPLATE. Add a test.

## render-2 (medium): A .html template rendered as a partial of a non-HTML template doesn't escape

Status: fixed in 8c87e5c4: each frame being rendered says whether it escapes for HTML; blocks take what they're in, and a partial escapes when what renders it does or its own name ends with ".html" or ".htm"; stdlib.md and the Templates comment say so, and text_template renders row.html inside mail.txt, escaped there and the text around it not

Where: std/text/template.fb:74

Templates.Render decides escaping once, from the root template's name with IsHtml(name), and every partial inherits that choice. docs/stdlib.md and the class comment both say that a template of Templates whose name ends with ".html" or ".htm" escapes for HTML, and the lane's stated rule is the same. Yet `row.html` stops escaping when it's included with `{{> row.html}}` from `mail.txt`, so data that is escaped when row.html is rendered directly reaches the output raw. This is an escaping hole for HTML fragments shared between text and HTML pages.

```
Import std.core
Import std.text.template

Local data = Value.Object()
data["x"] = Value("<b>")
Local set = Templates()
Try set.Add("row.html", r"<td>{{x}}</td>")
Try set.Add("mail.txt", r"{{> row.html}}")
Print(Try set.Render("row.html", data))
Print(Try set.Render("mail.txt", data))
```

Expected: <td>&lt;b&gt;</td>
<td>&lt;b&gt;</td>

Actual: <td>&lt;b&gt;</td>
<td><b></td>

Suggested fix: Make a frame's html the root's html OR'd with IsHtml(partial name): store an Html flag on Frame, push the PARTIAL frame with `frame.Html || IsHtml(part.Text)`, have child frames inherit it, and escape VALUEs by the current frame's flag. Or, if only the root is meant to decide, reword docs/stdlib.md and the Templates comment to say so. Add the case to the test.

Skeptic: Ran rv-template/rendering/r2.fb in debug and release. The output is "<td>&lt;b&gt;</td>" and then "<td><b></td>": row.html escapes when it's rendered directly but not when mail.txt includes it with {{> row.html}}. Templates.Render (template.fb:74) passes IsHtml(name) once, for the root, and partials never check their own names. docs/stdlib.md says "A template of `Templates` whose name ends with \".html\" or \".htm\", in any case, escapes for HTML, and so do the partials it renders". The class comment at template.fb:43 says the same. row.html is such a template, yet it doesn't escape when it's a partial. This contradicts the doc's first clause and leaves an escaping hole for HTML fragments that text templates share. The test (text_template.fb:108) covers only a non-HTML partial in a .txt root.

## render-3 (low): The indented-partial cache key collides with a template whose name contains a newline

Status: fixed in bccea600: Templates keeps the templates it has read by name, then by indentation, so a name with a line break in it can't be an indented partial's key, and Add replaces just that name's; text_template renders "a" indented as a partial and then the template named "a\n  "

Where: std/text/template.fb:94

PartsOf caches an indented partial in the same `Read` map under the key "{name}\n{indent}", and caches unindented templates under their bare name. A template whose name is itself "a\n  " therefore shares a key with partial "a" at indentation "  ". Once `{{> a}}` has been rendered standalone with two spaces, Render("a\n  ") returns the cached, indented parts of "a" in place of its own text (the reverse order gives the partial the wrong template too).

```
Import std.core
Import std.text.template

Local data = Value.Object()
Local set = Templates()
Try set.Add("a", "one\n")
Try set.Add("a\n  ", "other\n")
Try set.Add("p", r"  {{> a}}" + "\n")
Print(Try set.Render("p", data))
Print(Try set.Render("a\n  ", data))
```

Expected:   one

other


Actual:   one

  one


Suggested fix: Keep indented partials in a separate map, for example `Indented: Map<String, Part[]>` keyed by name and indent, or key it by a (name, indent) tuple, so it can't collide with a template's own name. Clear it in Add as well.

Skeptic: Ran rv-template/rendering/r3.fb in debug and release. The output is "  one\n\n  one\n", not "  one\n\nother\n": Render("a\n  ") returns partial a's cached indented parts in place of its own text. PartsOf (template.fb:94) keys indented partials as "{name}\n{indent}" in the same Read map that holds templates under their bare names, and Add accepts any name, newlines included, so the keys collide. It's contrived, since a template name rarely has a newline, but the output is wrong. Fix it with a separate map, or a key that can't collide.

## render-4 (low): Several Render paths the docs promise have no test

Status: fixed in 965402c0: text_template renders Templates from a Map<String, String> and a Map<String, Value>, an {{#unless}} with an {{else}}, @key over an Array, and nested {{#each this}}, and throws MISSING_VALUE for a partial a Templates doesn't have

Where: tests/programs/text_template.fb:99

The following work, as the program here shows, but text_template.fb never exercises them: Templates.Render(name, Map<String, Value>) and Render(name, Map<String, String>); MISSING_VALUE for a missing partial inside Templates (the test only covers a missing root, "gone", and a partial in a lone Template); {{#unless}} with {{else}}; @key in an each over an Array; and nested {{#each this}}. The project's rule is that every promised path has a test.

```
Import std.core
Import std.collections
Import std.text.template

Local set = Templates()
Try set.Add("page.html", r"{{who}} {{> card}}")
Try set.Add("card", r"[{{who}}]{{#unless who}}none{{else}}some{{/unless}}")
Local texts = Map<String, String>()
texts["who"] = "<me>"
Print(Try set.Render("page.html", texts))
Local values = Map<String, Value>()
values["who"] = Value("<you>")
Print(Try set.Render("page.html", values))
Try set.Add("missing", r"a {{> nowhere}} b")
Try
    Print(Try set.Render("missing", values))
Catch e As FormatError
    Print("{e.Kind}: {e.Message}")
End Try

Local list = Value.Object()
list["xs"] = Value([Value("a"), Value("b")])
Print(Try Template(r"{{#each xs}}{{@key}}{{@index}}{{/each}}|{{#each this}}{{#each this}}{{.}}{{/each}}{{/each}}").Render(list))
```

Expected: These lines are in tests/programs/text_template.fb and .out:
&lt;me&gt; [&lt;me&gt;]some
&lt;you&gt; [&lt;you&gt;]some
MISSING_VALUE: there's no template named "nowhere"
01|ab

Actual: The program prints those lines, but text_template.fb doesn't cover these cases.

Suggested fix: Add these cases to tests/programs/text_template.fb and their output to text_template.out.

Skeptic: Ran rv-template/rendering/r4.fb in debug and release, and it prints the expected four lines. I read tests/programs/text_template.fb in full, and it never exercises: Templates.Render(name, Map<String, Value>) or Render(name, Map<String, String>), since Maps go only through a lone Template at lines 84-89; MISSING_VALUE for a missing partial inside Templates, since line 151 is a missing root and line 140 a partial in a lone Template; {{#unless}} with {{else}}, since line 67 and line 79 have no else; @key in an each over an Array; and nested {{#each this}}. These are documented paths with no test. The behavior is correct today, so this is low, but under the project's strictness rule it should be added to the test and its .out.

## Refuted by the skeptics (no action)

- parse-2: Handlebars' {{!-- --}} comment ends at its first }}, so commenting out a tag prints the rest. Ran p2.fb. `a{{!-- {{name}} --}}b` gives `[a --}}b]`, `a{{!-- has }} inside --}}b` gives `[a inside --}}b]`, and `a{{!-- plain --}}b` gives `[ab]`, so the behavior is as reported. But docs/stdlib.md lists the tags in full and only has `{{! }}` as a comment. The lane's list does the same. Mustache's comment also ends at the first `}}`. `{{!-- --}}` is never promised, so this is a feature that isn't there, not a break of the spec, stdlib.md, or the milestones row.
- parse-3: A backslash can't come before a value: C:\{{dir}} renders C:{{dir}}, and \\{{dir}} doesn't escape the backslash. Ran p5.fb, written with the Write tool because bash heredocs collapse backslashes. `C:\{{dir}}` gives `[C:{{dir}}]` and `C:\\{{dir}}` gives `[C:\{{dir}}]`, as reported. This is exactly what docs/stdlib.md and the lane's decision describe: a backslash right before `{{` makes it a literal `{{`. Neither promises Handlebars' `\\{{` escape. Not being able to put a backslash right before a value in template text is a design gap the docs don't cover, not a contradiction of them. The data can carry the backslash instead.
