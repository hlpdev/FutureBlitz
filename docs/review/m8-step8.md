# The review of M8 step 8 (doc comments and fb doc)

26 confirmed findings, each with its Status.

## attach-1 (high): fb test crashes when an example's code has a /// comment: doc_before() reads the example module's lines at the origin's line numbers

compiler/src/parser.cpp:659. doc_before() finds the declaration's line with m_file.location_of(begin, UTF8). For a doc test's module, which has a line_origin_t, location_of() returns the line in the origin file, not in the generated module. That number then goes to m_file.line_text(at.line) and offset_of() on the generated file. It only runs when the module has a doc comment (m_docs isn't empty), so an example whose code holds a `/// ///` line, deep enough in its file, indexes past the module's lines. Debug builds hit an assert; release builds read past the end of m_line_starts. When the line does exist, the check looks at the wrong line.

Shown by: scratchpad/review-m8s8/attach/p7: a library whose a.fb has about 30 lines of Public Functions, then a Function whose doc has `# Examples` with a fence holding `/// /// A shape of one.`, `/// Class Unit Implements Shape`, `///     /// Its area.`, `///     Public Method Area(): Float => 1`, `/// End Class`, `/// Expect(Total([Unit()]) == 1)`. Run: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test . --color never

Expected: The example compiles and its test passes; the doc comments inside the example attach, or are ignored, without touching lines the module doesn't have.

Actual: fb exits with code 3 after `Assertion failed: line < line_count(), file .../compiler/src/source.cpp, line 75`.

Skeptic: I copied p7 to C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/verify-attach/p7. It is about 40 lines of Public Functions, then Double, whose # Examples fence holds `/// /// A shape of one.` and a Class with a `///` Method. `fb.exe test . --color never` printed `Assertion failed: line < line_count(), file .../compiler/src/source.cpp, line 75`, exit 3. The build is Debug (CMakeCache). The code matches the finding: doc_before() (parser.cpp ~659) calls m_file.location_of(begin, UTF8), which maps through m_origin->lines when the file has an origin (source.cpp ~105). It then passes that origin line to m_file.line_text(at.line) and offset_of() on the generated module. In a release build that reads past m_line_starts, and when the line exists it checks the wrong one.

Fix: In doc_before(), find the line in the file itself and not through its origin. For example, add a source_file_t accessor for a raw line (an upper_bound over m_line_starts, as location_of does, without the origin step), or compare offsets with offset_of({ .line }) only. Add a doc test whose example has /// lines, placed far down its file.

Status: fixed: doc_before() reads the line in the file itself, by source_file_t's new line_of(), never through its origin; Parsed's example in the reviewed package has a /// line far below where its module ends

## attach-2 (high): fb doc leaves out every Operator of a Struct, a Class, and an Extend

compiler/src/doc.cpp:1520. Spec 06 says Operators take their type's visibility and no modifier, so modifiers->visibility is PRIVATE on every Operator. shown() keeps a type's member only when it's PUBLIC (or PROTECTED in a Class), and add_extension() keeps only PUBLIC members (line 1622). So no Operator outside an Interface reaches the JSON. tooling.md lists `operator` as a kind and gives `...Vec3.Operator+` as a path. Its doc comment does attach, so there's no warning either. std has 340 `Operator` lines, but std.json has only the 9 in std.core.interfaces; BitSet's `Operator ==`, for one, is missing from its members. The test 'fb doc std has every Public declaration ... with a summary' doesn't catch this because the Operators never make it into the JSON.

Shown by: scratchpad/review-m8s8/attach/p4/a.fb: `/// A struct.` `Public Struct V` / `Public Field X: Int` / `/// Adds two.` / `Operator +(a: V, b: V): V => V(a.X + b.X)` / `End Struct`. Run: fb.exe doc . -o out.json, then list V's members. Also: fb.exe doc std -o std.json, then count kind == "operator".

Expected: V's members are X and an `operator` item `+` with path p4.a.V.Operator+ and doc 'Adds two.'; std.json has std.collections.bit_set.BitSet.Operator== and the other Operators of std's Public types.

Actual: V's members are only X. std.json has 9 operators, all from std.core.interfaces.

Skeptic: `fb.exe doc . -o out.json` on verify-attach/p4: V's members are only ['X']. The documented `Operator +` is missing and there is no warning. `fb.exe doc std -o std.json`: 9 items of kind operator, all from std.core.interfaces (Equatable, Comparable, Numeric). BitSet's members have no Operator==, though std/collections/bit_set.fb:189 has `Operator ==(a: BitSet, b: BitSet)`. In doc.cpp, shown() (~1520) keeps only PUBLIC members (or PROTECTED in a Class) outside Interfaces, and add_extension() (~1622) keeps only PUBLIC ones. Spec 06:480 says Operators 'have their type's visibility, and take no modifier', so their modifiers say PRIVATE. tooling.md:231-235 lists `operator` as a kind, with the path `...Vec3.Operator+`.

Fix: In shown() and add_extension(), treat a function_stmt_t whose kind is OPERATOR as having its type's visibility, and so as shown. Add an Operator to tests/packages/documented and check it in the JSON.

Status: fixed with doc-1: an Operator has its type's visibility, in shown() and add_extension(), and V's + in the reviewed package is in the JSON

## doc-1 (high): Operators of Structs and Classes are left out of fb doc, so std.json is missing about 320 of std's members

compiler/src/doc.cpp:1520. shown() keeps a member only when its modifiers say Public, or Protected on a Class. Under spec 06, an Operator takes no visibility because it has its type's, so its modifiers are the default (Private) and shown() drops it. add_extension (line 1622) drops Operators in Extends the same way. The done-when test ("fb doc std has every Public declaration of std's") doesn't catch this, because its public_paths helper (tests/compiler/doc_tests.cpp:633) uses the same visibility rule.

Shown by: scratchpad/review-m8s8/doc/ovr/o.fb: `Public Struct V` with `/// Adds two.` and `Operator +(a: V, b: V): V => V(a.N + b.N)`. Another module of the package uses `V(1) + V(2)` and compiles. Ran `fb doc ovr -o o.json`. Also ran `fb doc std` and compared std.json against std's sources with scratchpad/review-m8s8/doc/miss.py.

Expected: V's members are [V.N, V.Operator+]. std.json has, for example, std.collections.bit_set.BitSet.Operator==, Counter.Operator[] and Operator[]=, Deque.Operator[], and Map.Operator[].

Actual: V's members are only [ovr.o.V.N]. In std.json the only operators are the 9 declared in Interfaces (Equatable, Comparable, Numeric). About 316 Operators of std's Public Structs and Classes are missing; BitSet's members don't include Operator==.

Skeptic: I copied the reviewer's ovr package to refute/ovr and ran `fb doc ovr -o ovr.json`. Struct V's members are only ovr.o.V.N; `Operator +` is missing, though use.fb's `V(1) + V(2)` compiles. `fb doc std -o std.json` lists only 9 operators, all from Interfaces (Equatable, Comparable, Numeric), so no Struct's or Class's Operator is in std.json. Spec 06:480 says Operators have their type's visibility and take no modifier. shown() (doc.cpp ~1520) only checks the member's own modifiers, which default to PRIVATE, so it contradicts the spec, and M8 row 8's "std's API comes out whole".

Fix: Treat an Operator as having its type's visibility in shown() and add_extension. Have the std test's public_paths do the same, or derive its list without copying fb doc's rule.

Status: fixed with attach-2: an Operator has its type's visibility, and the std test's public_paths takes Operators too, so every one of std's has a summary, its mixed-type overloads' included, and the group comments over some of them are each one's own

## doc-2 (high): An Override with no visibility keyword is left out, though it has the visibility of the member it overrides

compiler/src/doc.cpp:1525. Spec 06 says an Override that doesn't state its visibility "has what it overrides'". shown() reads only the member's own modifiers, which are Private by default, so these members are dropped from the JSON. std writes many Overrides this way.

Shown by: scratchpad/review-m8s8/doc/ovr/o.fb: `Public Class Base` with `Public Method Speak()`, and `Public Class Derived Extends Base` with `Override Method Speak(): String => "b"`. Another module calls `Derived().Speak()`. Ran `fb doc ovr -o o.json`. In std.json, looked at std.term.terminal.FakeTerminal.

Expected: Derived's members are [ovr.o.Derived.Speak], with doc_from ovr.o.Base.Speak. FakeTerminal lists Columns, Rows, Colors, IsInteractive, IsInputInteractive, Write, Flush, ReadInput (Protected), SetInputMode, and ScreenChanged.

Actual: Derived's members are []. FakeTerminal's members are only New, Output, TakeOutput, Flushes, InputMode, Leave, Feed, Resize, and Pause; all 10 of its Overrides are missing.

Skeptic: In ovr.json, Derived has no members; its `Override Method Speak()`, which states no visibility, is dropped. In my std.json, FakeTerminal's members are New, Output, TakeOutput, Flushes, InputMode, Leave, Feed, Resize, Pause, Pause, with none of its Overrides. Spec 06:487-491 says an Override that doesn't say its visibility has what it overrides'. shown() ignores that and reads only the member's own modifiers, so this breaks the spec and the step's done-when.

Fix: When an Override states no visibility, take it from the overridden symbol (analysis' `overrides`), both in shown() and for the item's visibility. Add the case to the std test's public_paths.

Status: fixed: an Override that says no visibility takes the one it overrides, through analysis' overrides, for shown() and the item's visibility; Dog.Speak in the reviewed package, and public_paths takes such Overrides

## attach-3 (medium): An Enum member on a line after a trailing comma loses its /// doc, and fb doc says the doc documents nothing

compiler/src/parser.cpp:952. parse_enum() calls doc_before() once per iteration of its line loop, before the inner loop over comma-separated members. After `Red,` the inner loop calls skip_continuation_newlines() and reads `Green` from the next line in the same iteration, so Green's `///` line is never looked at. Spec 01 says an Enum's members each have a doc 'on the line before the line it starts', and Green starts its line.

Shown by: scratchpad/review-m8s8/attach/p5/a.fb: `/// Colors.` `Public Enum Color` / `/// Red.` `Red,` / `/// Green.` `Green,` / `/// Blue.` `Blue` / `End Enum`. Run: fb.exe doc . -o out.json

Expected: Red, Green, and Blue have the docs 'Red.', 'Green.', and 'Blue.', with no warnings.

Actual: Only Red has a doc. Green and Blue have null, and fb doc warns 'this doc comment documents nothing: a /// comment goes on the lines right before a declaration' at 5:5 and 7:5. `Small = 1,` / `/// Large.` / `Large = 2` loses Large's doc the same way.

Skeptic: `fb.exe doc . -o out.json` on verify-attach/p5 gives Color's members [('Red','Red.'),('Green',None),('Blue',None)] and Size's members [('Small','Small.'),('Large',None)]. It warns 'this doc comment documents nothing' at a.fb:5:5, 7:5, and 15:5. Spec 01:75 says an Enum's members each have a doc 'on the line before the line it starts', and Green, Blue, and Large each start their own line, so their docs should attach.

Fix: After skip_continuation_newlines(), when the next member is first on a new line, call doc_before(peek().span.begin, m_previous_end) again for it. Add the trailing-comma layout to the 'doc comments go on the declarations right after them' test.

Status: fixed: an Enum member first on its line after a comma reads its doc again; the Enum test checks Green and Large, after commas

## attach-4 (medium): Only the first run of //! lines is the module's doc; a later run before the first code is dropped, with a wrong warning

compiler/src/parser.cpp:682. module_doc() returns the first run of consecutive //! lines. Spec 01 says 'The //! lines at the top of a file, before its first line of code, are its module's doc comment'. When //! paragraphs are separated by a blank line (or an ordinary // comment, or an #If), the later ones are dropped. fb doc then warns that the //! comment 'goes at the top of its file, before its first line of code', which is where it already is.

Shown by: scratchpad/review-m8s8/attach/p4/a.fb starts `//! One.` / (blank) / `//! Two.` / `Import std.core`. Run: fb.exe doc . -o out.json

Expected: The module's doc is 'One.\n\nTwo.' (or the spec says that only the first run counts, and the warning says what's actually wrong).

Actual: The module's doc is 'One.', and there's `a.fb:3:1: warning: a //! comment documents its module, so it goes at the top of its file, before its first line of code`.

Skeptic: verify-attach/p4 starts `//! One.` / blank / `//! Two.` / `Import std.core`. fb doc gives the module doc 'One.' and warns at a.fb:3:1 that a //! comment 'goes at the top of its file, before its first line of code', which is where it already is. p1 gives the same warning for its second //! run. module_doc() (parser.cpp ~682) returns only the first run of consecutive //! lines. Spec 01:77 says 'The //! lines at the top of a file, before its first line of code, are its module's doc comment', so both runs count, and the warning's text is wrong for this case.

Fix: Join every //! run before the first token, with a blank line between runs, keeping each line's span. Or, if only one run is meant, say so in spec 01 and give the warning its own message, such as 'a module's //! lines go together, with no blank line between'.

Status: fixed: every //! run before the first code is the module's doc, a run a paragraph (spec 01 says so); the //! test has three runs, an ordinary comment between two

## doc-3 (medium): A link resolves to a member that code where the comment is can't see, such as another package's Private field

compiler/src/doc.cpp:1000. own_member() returns any member with the name, whatever its visibility, and member() searches base classes the same way. The spec says a link names what code where the comment is would find. A link to a Private member of another file's or package's type should name nothing and get a warning, but it resolves silently.

Shown by: base/things.fb: `Public Class Animal` with `Static Field secret: Int` (Private). vis/a.fb imports `base.things As things`; a doc there links `[things.Animal.secret]`, and edge/zoo.fb links `[Dog.secret]` (Dog Extends things.Animal). Code `Return things.Animal.secret` in vis/a.fb gives "error: 'secret' is private to base/things.fb". Ran `fb doc vis -o -` and `fb doc edge -o edge.json`.

Expected: Both links have an empty path, with the warning "'things.Animal.secret' names nothing that code here sees".

Actual: No warning. Both links have path "base.things.Animal.secret", which isn't in any JSON.

Skeptic: I ran `fb doc edge -o edge.json`. Puppy's doc links `[Dog.secret]` and `[things.Animal.secret]` both get path base.things.Animal.secret, with no warning. secret is a Private Static Field of base/things.fb, in another package. own_member() (doc.cpp ~977) returns any member with a matching name whatever its visibility, and member() searches bases the same way. Spec 01 says a link names a declaration "as code where the comment is would", and code in edge can't name secret, so the link should name nothing and be warned about. The path it gets isn't in any JSON.

Fix: In own_member() and the inherited lookup, check each member's visibility against the module the link is in: Private only from the same file, Package only from the same package, Protected only from a Class that extends it, and an Interface's members always.

Status: fixed: a link sees a member only where code there would, by its visibility, a Protected one from the same module or a Class that extends its type, and an Interface's always; [things.Animal.secret] warns

## doc-5 (medium): Links don't find members inherited through a qualified Implements, like `Implements things.Greeter`

compiler/src/doc.cpp:1060. member() looks up a type's Interfaces with in_scope(named->name) and ignores named->qualifier, while the base class and extended() do use the qualifier. When the Interface is named only through an alias or qualifier, the Interface isn't found, and a link to a member the type gets from it, such as a default Method, names nothing.

Shown by: base/things.fb: `Public Interface Greeter` with a default `Method Greet(): String`. qual/q.fb: `Import base.things As things` and `Public Struct Polite Implements things.Greeter`, with doc `[Polite.Greet]`. `p.Greet()` compiles. Ran `fb doc qual -o q.json`.

Expected: [Polite.Greet] resolves to base.things.Greeter.Greet, as [Loud.Greet] does through the unqualified `Implements LoudGreeter`.

Actual: "warning: 'Polite.Greet' names nothing that code here sees", and the path is empty.

Skeptic: I ran `fb doc qual -o qual.json`. It warned "qual/q.fb:5:28: warning: 'Polite.Greet' names nothing that code here sees", and the link's path is empty. `[Loud.Greet]`, through the unqualified `Implements LoudGreeter`, resolves to base.things.Greeter.Greet. In member() (doc.cpp ~1060), the loop over type->interfaces calls in_scope(named->name), ignoring named->qualifier, while the base class above it handles base_qualifier. So `Implements things.Greeter` is never found, and Polite's default Greet, which code calls through `p.Greet()`, can't be linked.

Fix: Resolve the Interface as the base class is resolved: in_scope on the qualifier, then member() for the name.

Status: fixed: a qualified Implements is resolved as a base Class is; Polite's [Polite.Greet] is reviewed.other.Greeter.Greet

## doc-6 (medium): Examples in a module's //! doc are written with "runs": true, but fb test never runs them

compiler/src/doc.cpp:568. documented(), which example_modules() uses, collects only the docs of declarations and their members, not ast.module_doc(). read_doc still finds a //! comment's examples, so the JSON lists them as tests. Spec 09 says each example in a package's doc comments is a test, and spec 01 calls the //! lines a doc comment.

Shown by: md/m.fb starts with `//! The module, with an example.`, a blank `//!` line, `//! # Examples`, and a fence holding `//! Expect(False)`. Ran `fb doc md -o md.json`, then `fb test md`.

Expected: fb test runs a test like `md.m (example)`, which fails; or, if module docs aren't meant to have tests, the JSON doesn't say runs: true (and the docs say so).

Actual: md.json's module doc has examples [{"code": "Expect(False)\n", "line": 5, "runs": true}]. `fb test md` prints "Running 0 tests" and "test result: ok".

Skeptic: md.json's module doc has examples [{code: "Expect(False)\n", line: 5, runs: true}]. `fb test md` printed "Running 0 tests" and "test result: ok", so that example never runs. documented() (doc.cpp ~566) walks only ast.items() and their members and never reads ast.module_doc(). Spec 01 calls the //! lines the module's doc comment and says the code blocks under a doc comment's # Examples are tests that fb test compiles and runs. Spec 09 says each example in a package's doc comments is a test. So the JSON claims the example runs, and nothing runs it.

Fix: Have documented() also return the module doc, named after the module, and add a test. Or document that module docs' examples aren't tests and mark them so in the JSON.

Status: fixed with tests-2: example_modules() reads the module doc, named '<module> (example)'

## doc-7 (medium): An Extend of a specific array type, like `Extend UInt8[]`, is written as an Extend of every `T[]`

compiler/src/doc.cpp:704. extended_name() and extended() return "T[]" for any type that isn't a named type, so `Extend UInt8[] Implements SqlValue` gets name "T[]", path "...Extend.T[]", and extends "T[]", as if it extended every array. Its members would be named "<module>.T[].X". Two Extends of one type in a module also share one path (std.collections.array has two "std.collections.array.Extend.T[]"), though the docs say only overloads share paths.

Shown by: Ran `fb doc std`, then listed the extend items of std.db.database in std.json. Source: std/db/database.fb:147 `Extend UInt8[] Implements SqlValue`.

Expected: name and extends "UInt8[]", and a path that tells it apart from every T[].

Actual: {"name": "T[]", "path": "std.db.database.Extend.T[]", "signature": "Extend UInt8[] Implements SqlValue", "extends": "T[]"}

Skeptic: My std.json has {path: std.db.database.Extend.T[], name: T[], extends: T[], signature: "Extend UInt8[] Implements SqlValue"}. extended_name() and extended() (doc.cpp ~703-718) return "T[]" for any type that isn't a named type. Spec 06:382-383 tells `Extend T[]` (every array) apart from `Extend Int[]` (one array type), and tooling.md's "every `T[]`" means the first, so marking UInt8[]'s Extend as extending T[] is wrong. I don't count the shared paths of two Extends of one type in a module: the documented format `<module>.Extend.<name>`, with the name having no type arguments, makes them share by design (std's Map, Grid, and others do too).

Fix: Take the array's element type into account: "T[]" only when the element is the Extend's own type parameter, otherwise the type as written, like "UInt8[]". Decide, and document, how two Extends of one type in a module get distinct paths.

Status: fixed: an Extend of an array whose element isn't its own type parameter is named as written, like UInt8[], and so is its path (tooling.md says so); the reviewed package's Extend UInt8[]

## doc-8 (medium): A link to an Import alias has the alias as its path, not the module's name

compiler/src/doc.cpp:945. in_scope() returns found_t{ .path = name } for `Import X As C`, so [C] gets path "C". A plain Import's last name gets node.target, the module's real name.

Shown by: vis/a.fb: `Import base.things As things`, `Import std.collections`, and a doc with `[things]` and `[collections]`. Ran `fb doc vis -o -`.

Expected: [things] has path "base.things", as [collections] has "std.collections".

Actual: {"text": "things", "path": "things"}: a path that names nothing in any JSON.

Skeptic: `fb doc vis -o vis.json` gives the link {text: things, path: things} for `Import base.things As things`, while `[collections]` gets std.collections. In in_scope() (doc.cpp ~944), the alias branch returns found_t{ .path = std::string(name) }, and the plain-Import branch uses node.target. tooling.md says a link's path is the path it names; "things" names nothing in any JSON.

Fix: Use node.target (or the imported module's name) as the path for an alias, as the plain-Import branch does.

Status: fixed: an alias's link is the module it names; [things] is reviewed.other

## tests-1 (medium): An example can't call a function that throws: the generated test has no Throws, and the error says to edit a Function the user can't see

compiler/src/doc.cpp:522. example_builder_t::run wraps an example's code in `@Test Function Example()` (and `Function Example()` for fb no_run) with no Throws clause. So `Try` on anything that throws fails to compile, even though spec 09 lets @Test Functions throw and the same code compiles in a tests/ file whose @Test Function has `Throws ArgumentError`. Parsing, file, network, and database APIs all throw, so their examples, the ones most worth writing, can't be written the natural way. The error message also points the user at 'Example', a Function that exists only in the generated module.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/p10 (lib.fb: `Public Function ParseIt(text: String): Int Throws ArgumentError` with the example `Local n = Try ParseIt("12")` / `Expect(n == 12)`; tests/t.fb has the same code in `@Test Function Throws1() Throws ArgumentError`). Command: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test p10 --color never

Expected: The example compiles and passes, as the tests/ version does. A thrown error should fail the test, as a throwing @Test does.

Actual: lib.fb:8:19: error: ArgumentError isn't caught; catch it in a Try block, or add it to the Throws of 'Example'. The build stops, and no tests run.

Skeptic: Copied p10 to verify-tests/p10 and ran `fb test p10 --color never`. It printed `p10/lib.fb:8:19: error: ArgumentError isn't caught; catch it in a Try block, or add it to the Throws of 'Example'` and the build stopped. The same code in a tests/ file with `@Test Function Throws1() Throws ArgumentError` passed (p10b: `test p10b.tests.t.Throws1 ... ok`). doc.cpp:522-528 adds `@Test Function Example()` / `Function Example()` with no Throws, though spec 09 says a test 'can throw' and that an example's 'other code is the test's body'. The message also tells the user to edit a Function that exists only in the generated module. I rated it medium, not high: the example can still call the throwing function inside a Try/Catch block or with Try?. My p10c used `Try ... Catch e As ArgumentError ... End Try` plus `Try? ParseIt("")` and passed.

Fix: Let the generated Example (and the no_run one) throw: give it `Throws Error`, or whatever a throwing @Test is allowed to declare, so Try works in an example and an uncaught error fails the test with its message.

Status: fixed: the generated Functions are 'Throws Error', so an example can Try, and an error it doesn't catch fails it; Parsed's example Trys

## tests-2 (medium): A module's //! doc examples are never run, though fb doc reports them with "runs": true

compiler/src/doc.cpp:568. documented() collects only declarations' and members' doc comments, never ast.module_doc(file), so example_modules() makes no test for an example in a file's //! comment. Spec 01 says the code blocks under # Examples in a doc comment are tests that fb test compiles and runs, and spec 09 says each example in a package's doc comments is a test. fb doc puts the module's example in its JSON with "runs": true, so the reference pages will show an example that nothing ever checks.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/p1/lib.fb starts with `//! # Examples` and a fence holding `Expect(1 == 2)`. Commands: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test p1 --color never, and C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe doc p1 -o -

Expected: A test such as `p1.lib (example)` runs and fails on `Expect(1 == 2)`.

Actual: `Running 5 tests`: none is for the module's example, which never runs. fb doc's module doc has `"code": "Expect(1 == 2)\n", "line": 5, "runs": true`.

Skeptic: `fb test p1` printed `Running 5 tests`: two Add tests, UsesSecret, String.Twice, and Color.Red. None is for the //! module doc's `Expect(1 == 2)` example. `fb doc p1 -o -` lists that module doc and its # Examples. documented() in doc.cpp only collects declarations' and members' docs, never ast.module_doc(). Spec 01 says the code blocks under # Examples in a doc comment are tests that fb test compiles and runs, and calls //! lines the module's doc comment, so this example goes unchecked.

Fix: Add the file's module_doc to what example_modules reads, named after the module (`p1.lib (example)`), and add a test case for it to tests/packages/documented.

Status: fixed with doc-6: a module's //! examples run, as 'reviewed.shapes (example)'

## tests-3 (medium): A Const in an example goes inside the test, so the example's own Functions can't see it

compiler/src/doc.cpp:459. part_of() puts a variable_stmt_t in DECLARATION only when its kind is GLOBAL, so a top-level `Const` (variable_kind_t::CONSTANT) lands in the test Function's body. The spec says an example's declarations go outside its test, and spec 01 counts Const among top-level declarations. A Function or Struct in the same example that uses the Const then fails to compile. The same code compiles with Global, or as a normal file.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/const/lib.fb (example: `Const Size = 3` / `Function Twice(): Int => Size * 2` / `Expect(Twice() == 6)`). Command: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test const --color never

Expected: The example compiles and passes.

Actual: const/lib.fb:9:30: error: 'Size' isn't declared

Skeptic: `fb test const` printed `const/lib.fb:9:30: error: 'Size' isn't declared`. part_of() (doc.cpp:459) returns DECLARATION only for variable_kind_t::GLOBAL, so a CONSTANT goes into the test's body. The same Const/Function code as a normal file (`fb run constb/c.fb`) printed 6. Spec 09 says an example's declarations go outside its test, and spec 01 lists Const among the top-level declarations.

Fix: Treat variable_kind_t::CONSTANT as a DECLARATION in part_of(), as GLOBAL is, and add a test for it in doc_tests.cpp's example-module case.

Status: fixed: a Const is a declaration in part_of(), as a Global is; Parsed's example's Function uses its Const

## attach-5 (low): A /// before a Const or other non-Local declaration in a Function body attaches silently, so fb doc gives no 'documents nothing' warning

compiler/src/parser.cpp:717. parse_statement() skips only variable_kind_t::LOCAL. Any other declaration in a block, such as a Const in a Function body, gets the doc in the AST. Nothing documents it, but check_comments() counts its lines as attached. Spec 01 lists what has a doc (top-level declarations and members) and says fb doc warns about a doc comment that documents nothing. A Local in the same body does get the warning, so the two behave differently.

Shown by: scratchpad/review-m8s8/attach/p4/a.fb: `Public Function F(): Int` / `/// A local const, documenting nothing.` / `Const K = 2` / `/// A local, documenting nothing.` / `Local x = K` / ... Run: fb.exe doc . -o out.json

Expected: Both comments in the body warn 'this doc comment documents nothing'.

Actual: Only the Local's comment (10:5) warns. The Const's comment gets no warning and appears nowhere.

Skeptic: In the same p4 run, Function F's body has `/// A local const...` before `Const K = 2` (line 8) and `/// A local...` before `Local x` (line 10). fb doc warns only at a.fb:10:5. The Const's comment gets no warning and appears nowhere in the JSON. parse_statement() (parser.cpp ~717) skips only variable_kind_t::LOCAL, so the doc attaches to the body's Const in the AST and check_comments counts it as attached. Spec 01:71-76 gives docs only to top-level declarations and members, and says fb doc warns about a doc comment that documents nothing.

Fix: Attach docs only at the top level and in type, Interface, Extend, and Extern bodies (for example with a flag set by the parse_block() callers), not in Function, Method, or other code blocks.

Status: fixed: docs attach only at the top level and in a Struct, Class, Interface, Extend, or Extern block (spec 01 says so); the Const in Body's code warns, at 54, and the Enum test's K has none

## attach-6 (low): fb doc warns once per line of a multi-line doc comment that documents nothing

compiler/src/doc.cpp:1259. check_comments() goes through the lexer's doc spans, one per line, and warns for each unattached line. tooling.md says fb doc warns about 'each doc comment on nothing', so a 10-line orphan comment gives 10 identical warnings instead of one.

Shown by: scratchpad/review-m8s8/attach/p2/f.fb: `/// x` / `/// y` / (blank) / `/// z` / `Public Function M()`. Run: fb.exe doc . -o out.json

Expected: One warning for the comment at 9:1.

Actual: Two warnings, at f.fb:9:1 and f.fb:10:1.

Skeptic: verify-attach/p2/f.fb has `/// x` / `/// y` / blank / `/// z` / a declaration. fb doc printed two identical warnings, f.fb:9:1 and f.fb:10:1, for one two-line comment. tooling.md:197 says fb doc warns once for 'each doc comment on nothing', and the two lines are one comment.

Fix: Group consecutive unattached /// lines (and //! lines) into one comment, and warn once at its first line.

Status: fixed: check_comments groups a run of unattached lines and warns once, at its first; the two lines at 59 warn once

## doc-10 (low): Links in headings aren't found, resolved, or checked

compiler/src/doc.cpp:300. read_line() handles a heading with show() and a new section, but never calls find_links on it. A [Name] in a heading isn't in the doc's links, so it has no path for M9 and gets no warning when it names nothing.

Shown by: md/m.fb: `/// # See [A1]`, a blank `///` line, then `/// Text.`, before `Public Function A4()`. Ran `fb doc md -o md.json`.

Expected: links has [A1] with path md.m.A1.

Actual: links is []; the section heading is "See [A1]".

Skeptic: In md.json, A4 (`/// # See [A1]`, blank, `/// Text.`) has links [] and a section heading "See [A1]". read_line's heading branch (doc.cpp ~300) calls end_paragraph, show, and a new section, but never find_links. tooling.md says links lists each `[Name]` in the doc, and fb doc should warn about one that names nothing, so a heading's link gets neither a path nor a check.

Fix: Call find_links on a heading's text, and test it.

Status: fixed: a heading's text has its links found; the heading test

## doc-11 (low): A backtick with no closing one hides the rest of the paragraph's links and the summary's end

compiler/src/doc.cpp:384. find_links and first_sentence open a code span at any backtick run and keep it open to the end of the paragraph. In CommonMark, a backtick run with no matching closer is a literal character, so the text after it is still prose.

Shown by: md/m.fb: `/// Use ` and see [A1] here. Next.` before `Public Function A3()`. Ran `fb doc md -o md.json`.

Expected: summary "Use ` and see [A1] here.", and a link to A1.

Actual: summary "Use ` and see [A1] here. Next." and links [].

Skeptic: In md.json, A3 (`/// Use ` and see [A1] here. Next.`) has summary "Use ` and see [A1] here. Next." and links []. Spec 01 says the summary ends at the first `.` outside code and that the text is Markdown written Rust's way. In CommonMark, a backtick run with no closer of the same length is a literal backtick, not a code span, so `.` after it is outside code and [A1] is a link.

Fix: Treat a backtick run as opening a span only when a run of the same length closes it later in the paragraph.

Status: fixed: a run of backticks opens code only when a run as long closes it in the paragraph, as CommonMark has it; the backtick test, with a span across lines too

## doc-12 (low): A link to a type parameter, like [T], warns that it names nothing code there sees

compiler/src/doc.cpp:1302. in_scope() doesn't know about the enclosing declaration's type parameters, so [T] in a generic Interface's or Class's doc gets the warning "'T' names nothing that code here sees". Code there does see T, so the message is false.

Shown by: inh/i.fb: `Public Interface Holder<T>` with `/// Holds [T].` on `Method Hold(x: T)`. Ran `fb doc inh -o i.json`.

Expected: No warning; or a link marked as a type parameter; or a message that says type parameters can't be linked.

Actual: "inh/i.fb:13:15: warning: 'T' names nothing that code here sees".

Skeptic: `fb doc inh -o inh.json` printed "inh/i.fb:13:15: warning: 'T' names nothing that code here sees" for `/// Holds [T].` in `Interface Holder<T>`. Spec 01's lookup list (type members, module, imports, built-ins) has no type parameters, so the link naming nothing is consistent with the spec. But the warning's text is false: code there does see T. Only the message is wrong, so it's low.

Fix: Resolve the type parameters of the declaration and its type (for example, mark them builtin-like with no path), or word the warning for that case.

Status: fixed: a link to one of its declaration's or its type's type parameters isn't a link, so it doesn't warn and has no path; Box's [T]

## doc-13 (low): A /// comment on an Extend gets a warning that says to put it right before a declaration, where it already is

compiler/src/doc.cpp:1288. Spec 01 gives no doc to an Extend itself, so the comment documents nothing. But the warning says "a /// comment goes on the lines right before a declaration", and an Extend is a declaration the comment is right before. Every extend item in the JSON has "doc": null.

Shown by: edge/zoo.fb: `/// Extends String.` right before `Extend String`. Ran `fb doc edge -o edge.json`.

Expected: A message saying an Extend has no doc of its own (only its members do), or the Extend takes the doc.

Actual: "edge/zoo.fb:40:1: warning: this doc comment documents nothing: a /// comment goes on the lines right before a declaration".

Skeptic: `fb doc edge` printed "edge/zoo.fb:40:1: warning: this doc comment documents nothing: a /// comment goes on the lines right before a declaration" for `/// Extends String.` right before `Extend String`. Spec 01 doesn't list an Extend among what has a doc comment, so warning is correct. But the advice tells the writer to do what they already did, and the comment is right before the Extend. Only the wording is wrong, so it's low.

Fix: Give an Extend (and an Extern block) its own wording in check_comments, or let it carry the doc in the JSON.

Status: fixed: a comment on an Extend or an Extern block says it has no doc of its own, but its members do; the warning at 46

## doc-14 (low): The docs say a link's text is as written without backticks or a (), but type arguments are dropped too

docs/tooling.md:253. link_target() also removes type arguments, so [Map<K, V>.Get] and [Box<T>.Get] have text "Map.Get" and "Box.Get". tooling.md's JSON format and doc.hpp's doc_link_t::target (compiler/include/fb/doc.hpp:51) describe the text as written, without backticks or a "()", and don't mention type arguments.

Shown by: edge/zoo.fb: `/// Generic box. Holds a [Box.Value], [Box<T>.Get].` Ran `fb doc edge -o edge.json`.

Expected: Either the text is as written, "Box<T>.Get", or the docs say type arguments are left out.

Actual: {"text": "Box.Get", "path": "edge.zoo.Box.Get"}

Skeptic: edge.json gives `[Box<T>.Get]` the link {text: "Box.Get", path: edge.zoo.Box.Get}. tooling.md's JSON format says a link's text is "as written without backticks or a `()`" and says nothing about dropping type arguments, so the docs (and doc_link_t::target's comment) don't match what fb doc does.

Fix: Add "and its type arguments" to tooling.md's links description and to doc_link_t::target's comment.

Status: fixed: tooling.md and doc_link_t::target's comment say a link's text loses its type arguments too (docs only)

## doc-9 (low): fb doc's message for a path that doesn't exist says the path isn't in a package

compiler/src/driver.cpp:3181. compile_docs() reports a missing file or folder as not being in a package. fb build and fb test say "cannot read '<path>': No such file or directory". Separately, with no argument outside a package, input_of (cli/src/commands.cpp:539) tells fb doc users to "give a package's folder, or a .fb file", but fb doc rejects a .fb file outside a package, and the message doesn't mention std.

Shown by: In scratchpad/review-m8s8/doc: `fb doc nope`, `fb doc nope.fb`, `fb build nope.fb`, and `fb doc` from an empty folder.

Expected: `fb doc nope.fb` gives "cannot read 'nope.fb': No such file or directory", as fb build does. The no-argument message fits fb doc, naming a package's folder, one of its .fb files, or std.

Actual: "error: 'nope.fb' isn't in a package, which a project.toml makes; fb doc documents a package, or std" (the same for `nope`). From an empty folder: "there's no project.toml in this folder or above it; give a package's folder, or a .fb file".

Skeptic: From refute/: `fb doc nope` and `fb doc nope.fb` both print "error: 'nope.fb' isn't in a package, which a project.toml makes; fb doc documents a package, or std" (the same for 'nope'), while `fb build nope.fb` prints "error: cannot read 'nope.fb': No such file or directory". `fb doc` in an empty folder prints "there's no project.toml in this folder or above it; give a package's folder, or a .fb file", but `fb doc loose.fb` then rejects a .fb file outside a package, and the message never mentions std. Both are misleading diagnostics, not wrong output.

Fix: In compile_docs, check that the path exists first and report it as compile_file does. Give fb doc its own no-package message in input_of. Add both to the CLI test.

Status: fixed: compile_docs reports a path that isn't there as fb build does, and fb doc with none outside a package names a package's folder, one of its .fb files, or std; both in the fb doc CLI test

## tests-4 (low): Overloads' examples get the same test name, so failures are ambiguous and --filter can't pick one

compiler/src/doc.cpp:631. The test name is `<module>.<path> (example N)`, and N counts only within one declaration's doc. Overloads share their path, so each overload's first example is `X (example)`. fb test then lists two tests with the same name, one ok and one FAILED, the failures section doesn't say which overload failed, and no --filter selects just one. Overloads are common: std has many, such as Database.Execute's eight.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/p1/lib.fb (`Public Function Add(a: Int, b: Int)` and `Public Function Add(a: Float, b: Float)`, each with one example). Commands: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test p1 --color never, and the same with --filter "Add (example)"

Expected: Each example has its own name, for example the second overload's numbered on from the first (`p1.lib.Add (example 2)`), so the failure and the filter are unambiguous.

Actual: `test p1.lib.Add (example) ... ok` then `test p1.lib.Add (example) ... FAILED`, and `---- p1.lib.Add (example) ----` in the failures. --filter "Add (example)" runs both.

Skeptic: `fb test p1` printed `test p1.lib.Add (example) ... ok` and then `test p1.lib.Add (example) ... FAILED`. `--filter "Add (example)"` ran both (`Running 2 tests`), and no filter picks one overload. example_modules() numbers examples within each doc comment only, and overloads share a path. Rated low: the failure message still gives the line (p1/lib.fb:31:5), so the failing overload can be found.

Fix: Number examples per path across overloads, keeping a count of examples by path in example_modules(), or add something that tells the overloads apart. Add a test with two overloads that both have examples.

Status: fixed: examples are numbered by path across overloads; Twice's two are '(example)' and '(example 2)'

## tests-5 (low): Errors in the lines fb adds to an example point past the end of the fence's line

compiler/src/doc.cpp:547. The Imports, `@Test Function Example()`, and `End Function` lines that fb adds all have the fence's location as their origin. location_of() then adds the column inside the generated line, so a message about one of them gets a column past the end of the 7-character "/// ```" line, and the caret sits at the line's end. It happens when an example declares its own `Function Example()` (or `Compiles()` in a no_run one), when a Struct is left open, or when the file's module can't be imported (a library file named my-lib.fb, which fb check accepts). Each gives a message about code the user never wrote, at a column that isn't on the line.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/collide/lib.fb (example: `Function Example(): Int => 1`). Also .../tests/p8/lib.fb (an unclosed `Struct S`) and .../tests/p7/sub/my-lib.fb. Commands: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe test collide (and p8, and p7) --color never

Expected: A column on the fence's line, and a message that makes sense for what the user wrote. For example, the generated names could be ones a user can't collide with, and the message could say the example's Function clashes with the test fb makes.

Actual: collide/lib.fb:7:20: error: 'Example' is already declared with the same parameter types, on the line "/// ```". Also p8: lib.fb:7:11: error: a Struct can only hold Fields, ..., and p7: sub/my-lib.fb:7:12 and 7:21, "there's no module p7.sub.my".

Skeptic: `fb test collide` printed `collide/lib.fb:7:20: error: 'Example' is already declared with the same parameter types` on the 7-character line "/// ```", with the caret at the end of the line. p8 gave `p8/lib.fb:7:11` and p7 gave `sub/my-lib.fb:7:12` and `7:21` on that same fence line, about code fb generated. run() gives every added line (Imports, Example, End Function) the fence's location, and the column inside the generated text is added to it, so the column falls past the end of the line.

Fix: Map every column of a generated line to the fence's first column (keep the line origin but clamp, or don't add the column for added lines), and give the generated Functions names an example can't declare.

Status: fixed: a line fb adds maps every column to the fence's first, and the generated Functions are DocTest__Example and DocTest__Compiles, which an example could declare only on purpose, since every name is one it can; the module example declares Example

## tests-6 (low): Comments written for a group of declarations became only the first one's doc, so it says the wrong thing

std/collections/deque.fb:21. In some places the conversion turned a comment that covered several declarations into the doc of the first one, and gave the others new docs of their own. The first one's summary now describes its siblings too: Deque.AddFirst says "Adds the `value` first, or last" (AddLast has its own doc, which loses the panic note). Value.Array says "An empty Array or Object, to add to." (std/core/value.fb:127). LocalDateTime.AddDays says "The same time that many days, months, or years later" (std/time/calendar.fb:469), and so does ZonedDateTime.AddDays (std/time/zone.fb:332). These are the summaries M9's reference pages will show.

Shown by: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe doc std -o C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/std.json; the summaries of std.collections.deque.Deque.AddFirst, std.core.value.Value.Array, std.time.calendar.LocalDateTime.AddDays, and std.time.zone.ZonedDateTime.AddDays

Expected: Each summary is about its own declaration: "Puts `value` at the front", "A new, empty Array", "The same time `count` days later, or earlier". The panic note goes on AddLast too.

Actual: Each summary describes the whole group, which is wrong for the one declaration it's on.

Skeptic: I regenerated std.json with `fb doc std`. Summaries: Deque.AddFirst 'Adds the `value` first, or last.'; Value.Array 'An empty Array or Object, to add to.'; LocalDateTime.AddDays and ZonedDateTime.AddDays both 'The same time ... that many days, months, or years later...'. Each siblings' doc follows right after (AddLast 'Puts `value` at the back.', Object 'A new, empty Object.', AddMonths and AddYears have their own). So these four group comments landed on the first declaration only, and AddLast lost the panic note (std/collections/deque.fb:20-35).

Fix: Reword those four docs, and look for other comments that covered several declarations before the conversion.

Status: fixed: Deque.AddFirst, Value.Array, and LocalDateTime's and ZonedDateTime's AddDays are each their own, AddLast has the panic note, and so are the other group comments found: the Operators' (BigInt's, Duration's, the times' + and -), Value's [] and []=, Vec2's Abs, and Duration.TotalMicroseconds

## tests-7 (low): std's 348 undocumented Enum members pass the summary test because it skips Enum members

tests/compiler/doc_tests.cpp:646. tooling.md says every Public declaration of std's, and every member its users see, has a summary, and that a test checks each in fb doc std's JSON. The test's summaries() leaves out `enum member` items, as well as extends, which can't take a doc. In fb doc std's JSON, 348 of the 359 Enum members have no doc, such as std.core.error.IOErrorKind.NOT_FOUND and ArgumentErrorKind.MISSING. So the design's claim isn't true, and the test doesn't check it.

Shown by: C:/Users/Hunter/Projects/build-m8-doc/cli/fb.exe doc std -o C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s8/tests/std.json, then a count of the items by kind and by whether they have a summary: ('enum member', no doc) 348, ('enum member', with one) 11

Expected: Either every Enum member has a summary and the test checks them, or tooling.md says Enum members are exempt.

Actual: 348 Enum members have no doc, and the test passes because it skips them.

Skeptic: Counting fb doc std's JSON: 348 Enum members have no summary and 11 have one (for example std.compress.archive.ArchiveEntryKind.FILE has none). tests/compiler/doc_tests.cpp's summaries() skips `enum member` (and extend), but tooling.md says 'every member its users see, has a summary' and that a test checks each. Enum members are members in format 1 and in spec 01, so the doc and the test disagree. Leaving out extend is fine, since spec 01 gives an Extend no doc comment.

Fix: Document std's Enum members and drop the exemption, or change tooling.md's wording to match the test.

Status: fixed: std's Enum members have docs, each on its own line, and the std test takes them
