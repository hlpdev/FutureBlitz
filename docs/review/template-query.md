# The review of M6 step 32 audit gaps: templates over Save (Struct, Class, Map data) and std.db's Query<T> (steps 8 and 23)

4 confirmed findings: 0 high, 0 medium, 4 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## query-1 (low): db_query.fb never checks that a missing column takes a Field's non-zero default

Status: fixed in 1bfee465: db_query reads a Settings Class with defaults of 7 and True from a Database's Query and a Statement's, where no column has them, and a Statement's Query<Settings, Int>(5) throws its ParseError

Where: tests/programs/db_query.fb:30

The lane's call and docs/stdlib.md say a Field no column has takes its default, or else its zero. db_query.fb only checks the zero half. Player's Fields have no defaults. Summary's defaults (Players = 0, Top = 0.0) equal their zeros, and both get columns anyway. So if Query<T> filled a missing column with the zero and ignored the default, every test would still pass. The code is right (shown below), but this part of the done-when isn't tested. A ParseError thrown from a Statement's Query<T>(values...) isn't tested either.

```
Import std.core
Import std.db

Class Settings
    Field Name: String
    Field Volume = 7
    Field Muted = True
End Class

Try
    Local db = Database.Open(":memory:")
    Local statement = db.Prepare("SELECT ? AS Name")
    Try statement.Bind(1, "main")
    For s In db.Query<Settings>("SELECT 'main' AS Name")
        Print(s.Name, s.Volume, s.Muted)
    End For
    For s In statement.Query<Settings>()
        Print(s.Name, s.Volume, s.Muted)
    End For
    Print(statement.Query<Settings, Int>(5)[0].Name)
Catch e As Error
    Print(e.Message)
End Try
```

Expected: main 7 True
main 7 True
[0].Name: expected String, found 5 (and db_query.fb checks this behavior)

Actual: main 7 True
main 7 True
[0].Name: expected String, found 5 (debug and release). The behavior is right, but no test in tests/programs/db_query.fb checks a non-zero default for a missing column, or a ParseError from a Statement's Query.

Suggested fix: In db_query.fb, add a T with non-zero defaults (for example Volume = 7, Muted = True) that's queried without those columns, plus a Statement.Query<T, A>(value) whose row fails to load, and add the expected lines to db_query.out.

Skeptic: I ran q1.fb under scratchpad/rv-query/cov, in debug and release. It printed 'main 7 True', 'main 7 True' and '[0].Name: expected String, found 5'. The behavior is right: Database.Query and Statement.Query<T>() fill a missing column with the Field's non-zero default, and Statement.Query<T, Int> throws the ParseError. The coverage gap is real, though. In tests/programs/db_query.fb, Player's Fields have no defaults, so its 'default, or else its zero' block only exercises zeros. Summary's defaults (0, 0.0) equal its zeros, and both of its Fields get columns. Both ParseError cases go through db.Query<T>(sql); none goes through a Statement's Query. No other test program calls Query<. stdlib.md (around line 1320) promises that 'a Field no column has takes its default, or else its zero', and nothing tests the default half.

## query-2 (low): No panic test for Query<T, A...> given the wrong number of values

Status: fixed in d7977d12: panic_db_query_values and panic_db_statement_query_values give Database's Query and a Statement's another number of values than the SQL takes, each panicking at the program's own line

Where: tests/programs/panic_db_query_parameters.fb:14

The only panic test is Database.Query<T>(sql) on SQL that takes a parameter. The new overloads that bind values (Database.Query<T, A..H> and Statement.Query<T, A..H>) are documented to panic at the caller when given another number of values than the statement takes, as Rows does, and to panic on a closed Database. No test checks either. Both work today: the panic is reported at the user's line, as shown below.

```
Import std.core
Import std.db

Struct Named
    Field Name: String?
End Struct

Try
    Local db = Database.Open(":memory:")
    Print(db.Query<Named, Int>("SELECT ? AS Name, ? AS X", 1).Count)
Catch e As Error
    Print(e.Message)
End Try
```

Expected: panic: a value is given for each of the statement's parameters, 2, not 1 at <file>.fb:9:11 (and a panic_ test checks this)

Actual: panic: a value is given for each of the statement's parameters, 2, not 1 at p1.fb:10:11 (right, with the program's own line; the same holds for Statement.Query<T, Int, Int> on SQL with one parameter, and for Query on a closed Database), but no tests/programs/panic_*.fb checks this

Suggested fix: Add panic_db_query_values.fb (or extend panic_db_query_parameters.fb's coverage with a second program) that calls db.Query<Named, Int>(sql, 1) on SQL with two parameters, so the panic's place at the caller is checked through the Database and Statement overloads.

Skeptic: In debug and release, p1.fb (db.Query<Named, Int> on SQL that takes 2 parameters) panics with 'a value is given for each of the statement's parameters, 2, not 1 at p1.fb:10:11'. p2.fb (st.Query<Named, Int, Int> on SQL that takes 1) panics with '... 1, not 2 at p2.fb:11:11'. Both report the caller's line, so the behavior is right. The tests don't cover it: panic_db_query_parameters.fb only checks Database.Query<T>(sql) with zero values. The 16 new @PanicsAtCaller overloads, Database.Query<T, A..H> and Statement.Query<T, A..H>, each depend on their own attribute to put the panic at the caller, and no panic_*.fb test calls any of them. stdlib.md says a wrong number of values panics at the caller.

## template-1 (low): Rendering a Struct or Class nested past 256 levels panics, though the docs say a template's data can nest to any depth

Status: fixed in 504c3bb0: stdlib.md says a template and a Value nest however deep, but data Save makes a Value of only 256 levels, past which Render panics at its caller; panic_template_save_depth renders a list of 200 Nodes, then panics at its line for 300

Where: std/text/template.fb:37

Render<T> (and Templates.Render<T>, line 77) first turns the data into a Value with Save. Save stops at MostDepth = 256 levels of Fields and elements and panics past that. docs/stdlib.md's template bullet still says "A template and its data nest however deep, since neither is read or rendered with calls." That was true when only a Value was rendered. With Struct and Class data, though, a plain linked list or tree more than 256 levels deep panics, even with no cycle in it. The panic message also says the Class "holds itself", which is wrong here. A Value of the same shape renders fine, so the same data works or fails depending only on how it is passed. Debug and release behave the same.

```
Import std.core
Import std.text.template

Class Node
    Field Label = ""
    Field Next: Node? = Null
End Class

// A list 300 Nodes long, which a Value of the same shape renders.
Local head = Node()
head.Label = "0"
Local tail = head
For i = 1 Until 300
    Local made = Node()
    made.Label = "{i}"
    tail.Next = made
    tail = made
End For

Local shaped = Value.Object()
Local at = shaped
For i = 0 Until 300
    at["Label"] = Value("{i}")
    If i < 299
        Local inner = Value.Object()
        at["Next"] = inner
        at = inner
    End If
End For

Local template = Try Template(r"{{Label}}")
Print(Try template.Render(shaped))
Print(Try template.Render(head))
```

Expected: 0
0

Actual: 0
panic: Save goes 256 levels deep into Fields and elements, and no further; a Class that holds itself, inside it, can't be saved at std/text/template.fb:37:79
(exit code 101, in both debug and release)

Suggested fix: Either make Save, or the conversion used for templates, work without recursion so data of any depth converts (only a real cycle should panic), or change docs/stdlib.md's template bullet (and Save's docs) to state the 256-level limit for Struct and Class data and say that Render panics past it. Add a test for whichever is chosen.

Skeptic: Ran the finding's program (rv-query/tmpl/t1.fb) in debug and release: prints "0" for the 300-deep Value, then "panic: Save goes 256 levels deep into Fields and elements, and no further; a Class that holds itself, inside it, can't be saved at std/text/template.fb:37:79", exit 101, in both. The panic is Save's specified behavior (spec 13 lines 281-283: Save goes 256 levels deep into Fields and elements and panics past that), so Save isn't wrong, and its message is a general statement, not a claim that this list holds itself. But the change widened docs/stdlib.md's template bullet to data given through Save, and the same bullet still says "A template and its data nest however deep, since neither is read or rendered with calls". That is now false for Struct or Class data, which panics past 256 levels, so stdlib.md contradicts what Render<T> does. The fix is in the docs: say that Struct, Class and Map data nest only as deep as Save goes (256), and that Render panics past that, plus a test. Low severity, because Save's limit is specified.

## template-2 (low): A Save panic inside Render points at std's template.fb, not at the caller's line

Status: fixed in e4901e46: both Render<T> are @PanicsAtCaller, so Save's panic names the caller's line; panic_templates_save_cycle renders a Node that holds itself through Templates, and panic_template_save_depth goes through a Template

Where: std/text/template.fb:37

std's convention (docs/stdlib.md, "Missing, failed, or a bug") is that a bug panics at the caller's line, using @PanicsAtCaller. Template.Render<T> and Templates.Render<T> (line 77) call Save. Save panics for a Class that holds itself, or for data nested past 256 levels, and reports the place where Save was called. Neither Render<T> has @PanicsAtCaller, so the panic names std/text/template.fb:37:79 instead of the user's Render call.

```
Import std.core
Import std.text.template

Class Node
    Field Label = ""
    Field Next: Node? = Null
End Class

Local n = Node()
n.Next = n
Print(Try Template(r"{{Label}}").Render(n))
```

Expected: panic: Save goes 256 levels deep into Fields and elements, and no further; a Class that holds itself, inside it, can't be saved at e.fb:11:...

Actual: panic: Save goes 256 levels deep into Fields and elements, and no further; a Class that holds itself, inside it, can't be saved at std/text/template.fb:37:79

Suggested fix: Mark both generic Render<T> overloads @PanicsAtCaller, so Save's panic passes the caller's place on. Add a panic_ test whose .out names the program's own line.

Skeptic: Ran t2.fb (a Node whose Next is itself): "panic: ... can't be saved at std/text/template.fb:37:79", exit 101, so the panic names std's line, not the caller's line 11. stdlib.md's Conventions (line 280) say a bug panics at the caller's line (@PanicsAtCaller), and spec 13 says Save panics "where it's called". Neither generic Render<T> (template.fb:37, :77-78) is marked @PanicsAtCaller. I checked that the fix works: p1.fb, a user generic `@PanicsAtCaller Function Saved<T>(data: T): Value => Save(data)` called on the same Node, panics "at p1.fb:13:7", the caller's line. So marking both Render<T> overloads would pass the caller's place on.

## Refuted by the skeptics (no action)

- template-3: Rendering a type Save can't write gives a compile error inside std's template.fb, with nothing pointing at the caller, and the docs say any data works. Reproduced it. t3.fb gives "std/text/template.fb:37:79: error: Save can't write Map<Int32, String> here; a Map's keys are an Object's, which are Strings", with no note at the user's line. This is the compiler's general behavior for Load/Save errors in generic instances, not something this change got wrong. lower.cpp:7287-7318 (cannot_convert) reports these errors only when lowering an instance, at the Save inside the generic body. A user generic shows the same: p2.fb, `Function Saved<T>(data: T): Value / Return Save(data)` called with a Map<Int, String>, reports only p2.fb:5:12, inside the generic, with no instantiation note. Nothing in the spec or decisions requires an "instantiated from" note, so this is a compiler-wide diagnostics improvement, not a defect of this step. On the docs: stdlib.md says Render fills from "the Value Save makes of any other data", which ties it to Save's domain, and Save's domain is listed in std.data's bullet and spec 13. A type Save can't write is a compile error, as it is for any direct Save call. So the code doesn't plainly contradict the spec, stdlib.md, or the milestones row.
