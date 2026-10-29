# The review of M8 step 4 (the LSP)

35 confirmed findings, each with its Status.

## features-1 (high): A plain assignment's target isn't a use: references miss writes, and rename breaks the code

compiler/src/sema.cpp:11764. analysis_t::uses is filled only by note_use(), and the target of a plain `x = value` assignment (a Local, a Global, or a field used by name in a Method or a Property's Set) never reaches it. Compound assignments (`count += 1`) and `Self.count = 2` are recorded. So hover, definition, references, and rename all miss every plain write.

Shown by: Scratchpad S = C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/features. S/t2/b.fb has a Class Counter with `Field count`, `count = 5` in a Method, `count = v` in a Property's Set, and top-level `Local m = 1` / `m = 2`. `PYTHONUTF8=1 python S/t3.py S/t2/b.fb "count = 5@0" "m = 2@0"` drives fb-lsp's hover, definition, and references. `python S/ren.py S/t2/b.fb "Local m@6@total"` and `"Field count@6@n"` apply the rename edits, and `fb.exe run S/t2/b.fb.renamed.fb` builds the result.

Expected: Hover on `count` / `m` in `count = 5` / `m = 2` shows the Field or Local. References include those lines, and a rename changes them, so the renamed program still builds.

Actual: Hover there gives only the expression type `Int32`, definition gives null, and references give []. The rename of m makes 2 edits and leaves `m = 2`, so the build fails with "'m' isn't declared". The rename of count makes 4 edits and leaves `count = 5` and `count = v`, failing with 2 errors "'count' isn't declared".

Skeptic: Ran t3.py on t2/b.fb, which builds and prints '3 2'. Hover at `count = 5` and `m = 2` gives only ```fb Int32```, definition is None and references are []. Hover at `count += 1` gives the Field, with refs [(2,10),(5,8),(6,13),(10,19)], so lines 4 and 13 are missing. Renaming `Local m` to total makes 2 edits, and the build of the result fails at `m = 2`. Renaming `Field count` to n makes 4 edits, and the build fails with 2 errors at `count = 5` and `count = v`.

Fix: In sema, record a use (note_use or a direct uses push) for the name an assignment's target resolves to, for Locals, Globals, and fields or Static Fields named alone. Add tests that rename a variable that is assigned.

Status: fixed: sema records an assignment's target, a Local, a Global, or a field by name, as a use (note_written in check_target_place); names_tests' "references and renames reach a variable's and a field's assignments" renames both, and the result checks

## features-2 (high): A Method's declared name resolves to its Self parameter: wrong hover, references, rename, and semantic token

lsp/src/query.cpp:135. Sema gives each non-static Method's Self PARAMETER symbol the Method's name span (as in required_accessor, sema.cpp:4484). query_t::names() adds every symbol with a span in the file as a declaration. When two declarations share a span it keeps the lower symbol id, which is Self, so the Method's own name is taken to name Self. Static Methods are unaffected.

Shown by: S/t2/m.fb: Struct P and Class C, each with `Method A(): Int` and `Method Bee(): Int => 2`, called as p.A() and c.A(). `PYTHONUTF8=1 python S/t3.py S/t2/m.fb "A(): Int@0" "A(): Int@0#1"`, `python S/sem.py S/t2/m.fb`, and `python S/ren.py S/t2/m.fb "A(): Int@0@Alpha"`, then `fb.exe run S/t2/m.fb.renamed.fb`.

Expected: Hover on `A` in `Method A()` shows "Method P.A(): Int32". References give the declaration and the use on line 16, rename changes both, and the semantic token is method + declaration.

Actual: Hover shows "Self: P" ("Self: C" for the Class), and references give only [(3, 11)]. The semantic token type is `parameter`. Rename makes 1 edit and leaves `p.A()`, so the build fails with "P has no member named 'A'". The feature test hovers only a use of Squared, never its declaration.

Skeptic: In t2/m.fb, which builds and prints '1 1 2 2', hover on the declaration `A` in Struct P shows 'Self: P', and in Class C it shows 'Self: C'. References from there give only [(3,11)], while hover at the use p.A gives 'Method P.A(): Int32' with refs [(3,11),(16,10)]. sem.py tags line 3 col 11 'A' and 'Bee' as `parameter` + declaration. Renaming from the declaration to Alpha makes 1 edit, and the build fails at line 17 because p.A() is unchanged.

Fix: Leave Self (and the synthetic `value` of a Set) out of names(), for example by skipping PARAMETER symbols named Self or value whose span is a function's name, or by giving them an empty span. Add tests that hover, find references, and rename from a Method's declaration.

Status: fixed, with front-1: names() takes a declaration only where its name is written, so a Method's Self and a Set's value, whose spans are the Method's name, aren't; names_tests' "a Method's own name is the Method's, not its Self's"

## features-3 (high): Uses of Enum values aren't recorded: references find only the declaration, and rename breaks every use

compiler/src/sema.cpp:11764. `Color.Red` (in an expression or a Case) records no use of the value's symbol. Hover on `Red` there shows the type of `Color`, definition is null, references from the declaration give only the declaration, and rename changes only the declaration.

Shown by: S/t2/e.fb: `Enum Color` with Red and Green, `Local c = Color.Red`, and `Select c` with `Case Color.Red` / `Case Color.Green`. `PYTHONUTF8=1 python S/t3.py S/t2/e.fb "Color.Red@7" "Green\n@0"`, and `python S/ren.py S/t2/e.fb "Red\n@0@Crimson"`, then `fb.exe run S/t2/e.fb.renamed.fb`.

Expected: References of Red give its declaration and both `Color.Red` uses, and rename changes all three.

Actual: Hover on `Red` in `Color.Red` gives "Color" with no definition. References from the declaration give [(2, 4)] only. Rename makes 1 edit, and the build fails with "Color has no member named 'Red'" twice.

Skeptic: In t2/e.fb, which builds and prints 'red', hover on Red in `Color.Red` gives ```fb Color``` with definition None. References from the declaration of Red give [(2,4)] only. Renaming Red to Crimson makes 1 edit, and the build fails with "Color has no member named 'Red'" at 6:17 and 8:12.

Fix: Record a use for an Enum value named after its type (and in Case values) in sema's qualified-name or member path. Add a rename test for an Enum value.

Status: fixed, with front-3: check_enum_value records the value's Const at its name (sema keeps each Enum's values' Consts), in code and in Cases; names_tests' "an Enum's values are named where code uses them"

## features-4 (high): A type's name before a Static member, and in Extends, isn't a use: renaming a Class breaks it

compiler/src/sema.cpp:11764. `Shape.Make()`, `Shape.made += 1`, and `Class Square Extends Shape` record no use of the type Shape. Its references miss them, and a rename of the Class leaves them unchanged. The Static members themselves are found.

Shown by: S/t2/s.fb (it runs and prints "0 1 6"): Class Shape with a Static Field made and a Static Method Make, and Class Square Extends Shape. `PYTHONUTF8=1 python S/t3.py S/t2/s.fb "Class Shape@6" "Shape.made +=@0"`, and `python S/ren.py S/t2/s.fb "Class Shape@6@Figure"`, then `fb.exe run S/t2/s.fb.renamed.fb`.

Expected: References of Shape include lines 5, 13 (Extends), 18, and 25, and the renamed program builds.

Actual: References give [(2,6),(4,26),(6,15)]. Hover on `Shape` in `Shape.made` is null. Rename makes 3 edits, and the build fails with 6 errors: "'Shape' isn't declared" (3 times), "'Shape' isn't a Class", "'Area' has Override, but Square extends no Class", and "'Super' needs a Class that extends another...".

Skeptic: In t2/s.fb, which prints '0 1 6', references from `Class Shape` give [(2,6),(4,26),(6,15)]. Hover on Shape in `Shape.made +=` and in `Extends Shape` both give None. Renaming Shape to Figure makes 3 edits, and the build fails with 6 errors: 'Shape' isn't declared (6:9, 19:11, 26:20), 'Shape' isn't a Class, 'Area' has Override but Square extends no Class, and the Super error.

Fix: Record uses for a type name that qualifies a Static member or Enum value, and for the types in Extends and Implements clauses. Add tests.

Status: fixed, with front-2 and front-3: uses for the Class after Extends (declare_base), the type after Extend (extension_target), and a type before its Static member, in code and in a Static Method's call; names_tests' "a Class is named after Extends, Extend, and before its Statics" renames one, and the result checks

## front-1 (high): References, rename, hover, and definition from a Method's or New's own name pick its Self parameter

lsp/src/query.cpp:135. Sema gives each Method's and New's implicit Self parameter the same span as the Method's name, and gives Self the lower symbol id. query_t::names() keeps one name per span, choosing the declaration with the lowest id, so it keeps Self. Every request made at a Method's, an Interface Method's, or a New's declaration then works on Self. In front/u/main.fb, Base's `Method Grow` is symbol id1131 and Self is id1130, both with span begin 333. It's the same for Area (1115 and 1116) and New (1122 and 1123).

Shown by: S=C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/front; the test file is $S/u/main.fb (line 21 is `Method Grow(n: Int)`, line 53 is `b.Grow(1)`). Run `python $S/ren.py u/main.fb 21:Grow Expand` and `python $S/refs.py u/main.fb 21:Grow 53:Grow` from $S. `$S/harness.exe uses u/main.fb` lists the symbols: harness.cpp links build-m8-lsp/compiler/libfb_compiler.a and dumps analysis.uses and the symbols.

Expected: Hover at the declaration shows the Method. References list 21:12 and 53:3, and rename changes both.

Actual: Hover shows "Self: Base". References from 21:Grow give only ['21:12-16'], while from 53:Grow they give ['21:12-16', '53:3-7']. Rename from the declaration changes only line 21 and leaves `b.Grow(1)` broken.

Skeptic: Ran ren.py u/main.fb 21:Grow Expand from a copy at review-m8s4/vfront. Hover at the declaration gave "Self: Base", and rename made one edit, at line 21 only, which leaves b.Grow(1) broken. refs.py gave 21:Grow -> ['21:12-16'] and 53:Grow -> ['21:12-16','53:3-7']. The harness symbol dump shows Self k5 id1130 and Grow k9 id1131, both at offset 333, and Area/Self (1115/1116) and New/Self (1122/1123) the same way. query_t::names() in lsp/src/query.cpp sorts tied spans by the lower symbol id and then runs unique() on span, so the declaration's Self wins.

Fix: In names(), when spans tie, prefer the symbol whose kind isn't a PARAMETER named Self, or leave implicit Self parameters out. Better still, give Self no span, or the Method's whole span, in sema. Add an LSP test that renames from a Method's declaration.

Status: fixed, with features-2

## front-2 (high): No uses for a Class named after Extends, or a type named after Extend

compiler/src/sema.cpp:11764. analysis_t::uses has no entry for the base Class in `Class Child Extends Base`, nor for the type in `Extend Base`. Implements and `Interface X Extends Y` do get uses. As a result, references to a Class miss its subclasses' Extends lines, hover and definition on `Extends Base` give null, and renaming the Class breaks every subclass. tooling.md says sema records each use of a symbol.

Shown by: In $S/u/main.fb, `$S/harness.exe uses u/main.fb` has no use at 26:20 (`Class Child Extends Base`) or 43:8 (`Extend Base`). The same goes for $S/w/main.fb's 7:20 `Extends Error` and 18:19 `Extends Animal`, and $S/x/main.fb's 12:8 `Extend Gem`. `python $S/apply.py u/main.fb 12:Base Root r3/main.fb`, then `fb check r3/main.fb`. Also `python $S/ren.py u/main.fb 26:Base Root`.

Expected: Renaming Base edits lines 12, 26, and 43, and the result checks clean. Hover and definition on `Extends Base` reach Base.

Actual: The rename makes 1 edit, at (12, 7). fb check then gives: 'Base' isn't a Class; 'Super' needs a Class that extends another; 'Area' has Override, but Child extends no Class; 'size' isn't declared. Hover and definition at 26:Base are null.

Skeptic: harness uses u/main.fb has no use at 26:20 (Extends Base) or 43:8 (Extend Base). apply.py u/main.fb 12:Base Root made 1 edit at (12,7). fb check r3/main.fb then gave 8 errors: 'Base' isn't a Class at 26:21, 'Base' isn't a type at 43:8, the Super, Override, and size errors, and more. ren.py at 26:Base gave null hover and null definition, and rename was refused. tooling.md:121 says sema records each use of a symbol.

Fix: Call note_use, or push a use, where a Class's Extends type and an Extend's type are resolved. Extend the sema test "analysis keeps where each symbol is used" to cover both.

Status: fixed, with features-4; uses_tests' "analysis records every kind of use the language has" has both

## front-3 (high): No uses for Enum members, or for a type before a Static member

compiler/src/sema.cpp:11764. `Mood.Happy` records a use of Mood but none of Happy. `Animal.count` and `L.Counter.Zero()` record the Static Field or Method but not the type before it. Go to definition, references, and rename on an Enum member at its uses therefore find nothing; hover on `Mood.Happy` shows the Enum type for the whole `Mood.Happy` range; and renaming a Class used as a Static qualifier breaks the program.

Shown by: $S/w/main.fb: `python $S/ren.py w/main.fb 36:Happy Glad` and `python $S/refs.py w/main.fb 23:Happy 36:Happy`. Then `python $S/apply.py w/main.fb 44:Animal Beast r6/main.fb` and `fb check r6/main.fb`. Also $S/v/main.fb, line 9 `L.Counter.Zero()`: `python $S/apply.py v/main.fb 9:Counter Tally r7/main.fb`. `$S/harness.exe uses w/main.fb` shows uses only at 36:14 'Mood' and 43:8 'count', not at 36:19 'Happy' or 43:1 'Animal'.

Expected: References to Happy include 36:19; definition from 36:19 goes to 23:5; rename works on it. Renaming Animal edits 43:1 and 53:7 too.

Actual: Definition at 36:Happy is null. Hover shows "Mood" for range 13-23. Rename says "only a name the workspace declares can be renamed". References from 23:Happy give only the declaration. Renaming Animal makes 3 edits, and fb check then says 'Animal' isn't declared at 43:1 and 53:7, plus the Extends errors. Rename at `L.Counter` is refused.

Skeptic: ren.py w/main.fb 36:Happy Glad: hover shows "Mood" over chars 13-23, definition is null, and rename is refused ('only a name the workspace declares can be renamed'). refs.py: 23:Happy -> only the declaration, 36:Happy -> []. Uses for w have 36:14 'Mood' and 43:8/53:14 'count', but nothing at 36:19 Happy or at 43:1/53:7 Animal. apply.py w/main.fb 44:Animal Beast made 3 edits. fb check r6 then gave 8 errors, including 'Animal' isn't declared at 43:1 and 53:7. In v/main.fb, line 9 has only a use of 'Zero', and rename at Counter is refused.

Fix: Record a use for the member symbol when a qualified name resolves to an Enum member, and for the type symbol when a type name qualifies a Static member. Add these cases to the uses test.

Status: fixed, with features-3 and features-4, a qualified type before a Static member (L.Counter.Zero()) too

## features-10 (medium): Completion after "Super." gives nothing

lsp/src/completion.cpp:172. members() takes the object's type from sema. For `Super` in an Override Method, while `Super.` is being typed, that type isn't set, so add_instance_members returns nothing.

Shown by: S/t2/c.fb: Class Dog Extends Animal with an Override Method. `PYTHONUTF8=1 python S/comp.py S/t2/c.fb "Local q = 1=>Local q = Super.|"`, compared with `"Local q = 1=>Local q = Self.|"`.

Expected: Super. lists Animal's members (Speak, name, Hash...).

Actual: Super. gives 0 items, while Self. gives ['Hash', 'Speak', 'age', 'name'].

Skeptic: In t2/c.fb's Override Method Speak, completion at `Local q = Super.|` gives 0 items. At `Local q = Self.|` it gives ['Hash','Speak','age','name'].

Fix: In check_unnamed_member (sema), set the type of a Super object to the base Class, or in members() map a Super name to the owner's base type. Add a test.

Status: fixed: check_unnamed_member gives "Super." the base Class's type, without the error a Super not before a call gets; editing_tests' completion test

## features-11 (medium): Signature help gives nothing for an overloaded function while its call is typed, and never lists overloads

lsp/src/features.cpp:518. signature_help uses only the symbol sema resolved for the call. While arguments are missing, an overloaded name doesn't resolve, so the result is null, which is when signature help matters. Even when the call resolves, only one signature is sent.

Shown by: S/t2/sig2.fb has `Function Add(a: Int, b: Int)` and `Function Add(a: Float, b: Float, c: Float)`, then `Local x = Add(`, `Local y = Add(1.0, `, and `Local w = Twice(Add(1, `. `PYTHONUTF8=1 python S/sig.py S/t2/sig2.fb "Add(\n@4" "Add(1.0, @9" "Add(1, @7"`.

Expected: Both Add signatures, with activeSignature set to the one that fits the arguments so far.

Actual: null for each of the three positions. A non-overloaded `Twice(` at the same point works.

Skeptic: sig.py on t2/sig2.fb gives None at `Add(`, at `Add(1.0, `, and at `Twice(Add(1, ` inside Add. `Twice(` alone gives ('Function Twice(n: Int32): Int32', 0, 1). In t2/sig.fb, a resolved `Add(1.0, 2.0, 3.0)` sends only 1 signature. So an overloaded call gets nothing while it is being typed.

Fix: When the callee names an overload set, or doesn't resolve, collect every FUNCTION/METHOD/CONSTRUCTOR symbol with that name visible there. Send them all, and pick activeSignature by arity. Add a test with overloads.

Status: fixed: signature help sends every Function, Method, or New the callee's name has there (overloads(), in features.cpp), resolved or not, with activeSignature the one it resolved to, or else the first that takes that many arguments; editing_tests' "signature help lists a call's overloads..."

## features-13 (medium): Hover on a lambda parameter whose type is inferred shows "?"

lsp/src/query.cpp:299. In a valid program, an inferred lambda parameter's symbol type stays INVALID, and signature() prints it as '?'. It's the same in std/text/fuzzy.fb's `scored.Sort(Function(a, b) => ...)`.

Shown by: S/t2/l.fb (it builds and runs) has `nums.Sort(Function(a, b) => a < b)`. `PYTHONUTF8=1 python S/t3.py S/t2/l.fb "a < b@0" "(a, b)@1"`. Also `python S/big.py text/fuzzy.fb "a.1.Length"`.

Expected: ```fb a: Int32``` (or the tuple type in fuzzy.fb).

Actual: ```fb a: ?``` at both the declaration and the use.

Skeptic: In t2/l.fb, which builds and prints '3 6', hover on `a` in `a < b` and on `a` in `Function(a, b)` both show ```fb a: ?```. Definition and references themselves are correct, at [(3,19),(3,28)]. Hover 'with types' is the design, and inferred lambda parameters are common in Sort and similar calls.

Fix: Have sema write the inferred type back to the lambda parameter's symbol, or have hover use the type of the name expression at the use. Add a test.

Status: fixed: names() prefers, among a lambda's parameters' copies that the checks while a call's types are inferred make, the one whose type is known; names_tests' hover test

## features-5 (medium): Rename of a Method that's overridden or implements an Interface's doesn't rename the others

lsp/src/features.cpp:398. references() matches only the exact symbol key. Renaming a base Method, or an Interface's Method, leaves the Overrides and implementations, and the reverse is also true. So a rename turns valid code into errors.

Shown by: `PYTHONUTF8=1 python S/ren.py S/t2/s.fb "s.Area@2@Size"` then `fb.exe run S/t2/s.fb.renamed.fb`. And S/t2/i.fb (Interface Shown with Method Show, implemented by Class V): `python S/ren.py S/t2/i.fb "s.Show@2@Display"` then `fb.exe run S/t2/i.fb.renamed.fb`.

Expected: Renaming Shape.Area also renames Square's Override Area, and renaming Shown.Show renames V.Show, or the rename is refused.

Actual: The first build fails with "'Area' has Override, but Shape has no Method like it". The second fails with "V implements Shown, so it needs a Method 'Display'".

Skeptic: Renaming s.Area to Size in t2/s.fb makes 3 edits, and the build fails with "'Area' has Override, but Shape has no Method like it". In t2/i.fb, I fixed the reviewer's program mistake: V(1) gave 'too many arguments to V', so it is now V(), which builds and prints 'v'. Renaming s.Show to Display makes 2 edits, and the build fails with "V implements Shown, so it needs a Method 'Display'".

Fix: For a Method, Property, or Operator, also collect the keys of the members it overrides or implements and of those that override or implement it, and rename them together. Add a test.

Status: fixed, with front-5: a rename takes rename_set(): what overrides the Method or Property or what it overrides, and the Interface members it implements and their other implementations (symbol_t::overrides, default_method, and analysis_t::conformances), from the checks of each of their files and what imports them, and refuses when one is outside the workspace; references stay the one symbol's. names_tests' Method and Interface rename tests

## features-6 (medium): Rename checks no conflicts: a new name that clashes or shadows gives broken code

lsp/src/server.cpp:449. server_t::rename checks only is_identifier() and prepareRename. It never checks whether the new name is already declared in the scope, or would shadow or be shadowed at a use. The task asks specifically about rename conflicts.

Shown by: S/t2/a.fb has `Local a = 1` and `Local b = 2`: `PYTHONUTF8=1 python S/t2.py` renames a to b and gets edits [(19,6,7),(25,8,9)]. S/t2/r.fb (`Local count = 1` / `Print(count + 1)`): `python S/ren.py S/t2/r.fb "Local count@6@Print"` then `fb.exe run S/t2/r.fb.renamed.fb`.

Expected: An error such as RequestFailed saying 'b' is already declared here, or that the new name would hide Print. No edit that changes the program's meaning or breaks it.

Actual: Both renames are applied. The first gives two `Local b` declarations. The second fails to build with "'Print' isn't a function".

Skeptic: t2.py has a syntax error (an unterminated triple-quoted string), so I used ren.py instead. In t2/a.fb, which prints '3 5 3', renaming `Local a` to b is accepted with 2 edits, and the build fails with "'b' is already declared in this scope". In t2/r.fb, renaming count to Print is accepted with 2 edits, and the build fails with "'Print' isn't a function". No conflict check is done.

Fix: Before building the edit, look up the new name at the declaration's scope and at each use's scope in each compilation, and refuse with REQUEST_FAILED when it resolves to another symbol. Add tests.

Status: fixed: rename_conflict() refuses a name another variable of the function (or of the file's top-level code) has, before or after, another member of the type or the Classes it extends has, or that code where the name is declared or used sees already, builtins and built-in types among them; names_tests' "a rename to a name code already sees there is refused"

## features-7 (medium): Rename accepts names the language rejects (non-ASCII), and renames New

lsp/src/features.cpp:459. is_identifier() accepts any byte >= 0x80, but docs/spec/01-lexical.md says identifiers are "ASCII letters, digits, and `_`". prepareRename also allows a constructor's `New`, whose name can't change.

Shown by: `PYTHONUTF8=1 python S/ren.py S/t2/r.fb "Local count@6@zähler"` then `fb.exe run S/t2/r.fb.renamed.fb`. And S/t2/sig4.fb (Struct Point with `New(x: Int, y: Int)`): `python S/ren.py S/t2/sig4.fb "New(x@0@Make"` then `fb.exe run S/t2/sig4.fb.renamed.fb`.

Expected: InvalidParams for 'zähler', with the existing message about names. prepareRename is null on `New`, and the rename is refused.

Actual: zähler gets 2 edits, and the build fails with "unexpected character U+00E4". New gets 1 edit, and the build fails with "a Struct can only hold Fields, Methods, Properties, and News".

Skeptic: lsp/src/features.cpp:464-466 is_identifier accepts any byte >= 0x80, but docs/spec/01-lexical.md:70 says identifiers are ASCII letters, digits, and _. Renaming count to zähler makes 2 edits, and the build fails with 'unexpected character U+00E4'. prepareRename on `New(` in t2/sig4.fb returns {placeholder:'New', range 4:4-4:7}. Renaming it to Make makes 1 edit, and the build fails with 9 errors, starting with 'a Struct can only hold Fields, Methods, Properties, and News'.

Fix: Make is_identifier ASCII-only, as the lexer is. In prepare_rename, return null for CONSTRUCTOR symbols, or for a span whose text is "New". Add tests.

Status: fixed: is_identifier() takes ASCII only, as spec 01 says, and a New, by its kind or its text, can't be renamed; names_tests' "a rename takes only ASCII names, and never a New's"

## features-9 (medium): Completion in a type never offers the builtin types or type parameters, and offers values and keywords

lsp/src/completion.cpp:118. names() adds locals, module names, builtin functions, and keywords, but no builtin type (Int, Int32, String, Bool, Float, Char...) and no type parameter of the enclosing generic. Inside a type, after ':' or in `<...>`, it gives the same list as anywhere else.

Shown by: S/t1/a.fb's last line is `Local n: Poi`: `PYTHONUTF8=1 python S/t1.py` prints the labels starting with Int/Str/Bool/Float/UInt/Char/Doub, which are only ['Interface', 'Struct']. S/t2/tp.fb has `Function F<TItem>` and `Struct Box<TVal>`: `FULL=1 python S/comp.py S/t2/tp.fb "Local x = a=>Local x: TI|" "Local y = V=>Local y: TV|"` lists neither TItem nor TVal.

Expected: `Local n: |` offers Int, String, Float, Bool, and the other builtin types, the types in scope, and the type parameters in scope, without Locals, Functions, and statement keywords.

Actual: No builtin type appears, and no type parameter. The list is the full 144 to 147 names and keywords, including locals like `p` and `s`, and Print.

Skeptic: In t1/a.fb, completion at `Local n: In|` lists only 'Interface' among the Int* labels, with no Int or Int32. In t2/tp.fb, `Local x: TI|` inside F<TItem> has neither TItem nor TVal but does include a, x, and Print. `Local y: TV|` inside Box<TVal> lacks TVal. A type parameter is a name in scope, which the design's completion bullet covers. Filtering by type context would be an enhancement, not the bug.

Fix: Add the builtin types (types.hpp's names) and the enclosing declarations' type parameters to the names. When the text before the word ends in ':' / 'As' / '<' in a declaration, offer only types and modules. Add a test.

Status: fixed: completion has the built-in types, by each of their names, and the type parameters of the enclosing type and function, and after a declaration's ':' or after As, Extends, or Implements, only what names a type; editing_tests' "completion where a type goes..."

## front-4 (medium): An Alias has no uses where it's written as a type or called as a constructor

compiler/src/sema.cpp:11764. With `Alias Coins = Money`, `Local m: Coins` records no use at all. `Coins(2)` records a use of Money's New, which query_t::name_of() maps to the type Money and then drops, because the text 'Coins' doesn't end in 'Money'. References to an Alias find only its declaration; hover and definition on `Coins` give null; renaming it breaks the code. Module aliases (`Import "./lib" As L`) also have no uses: hover and definition on `L.` give null, and rename is refused.

Shown by: In $S/u/main.fb (line 47 `Alias Coins = Money`, line 56 `Local m: Coins = Money(1) + Coins(2)`), run `python $S/apply.py u/main.fb 47:Coins Cash r4/main.fb`, then `fb check r4/main.fb`. Also `python $S/ren.py u/main.fb 56:Coins Cash`, and `python $S/ren.py v/main.fb 8:L Lib`.

Expected: Renaming Coins edits 47:7, 56:10, and 56:29, and hover on `Coins` shows the Alias.

Actual: The rename makes 1 edit. fb check then says "56:10: 'Coins' isn't a type" and "56:29: 'Coins' isn't declared". Hover and definition at 56:Coins are null.

Skeptic: apply.py u/main.fb 47:Coins Cash made 1 edit at (47,7). fb check r4 then said "56:10: 'Coins' isn't a type" and "56:29: 'Coins' isn't declared". The uses dump has nothing at 56:10, and 56:29 'Coins' maps to Money's New (id1141). ren.py at 56:Coins gave null hover and null definition, and rename was refused. ren.py v/main.fb 8:L Lib gave null definition, and rename was refused.

Fix: When a written type or a callee resolves through an Alias, record a use of the Alias symbol at its name. Teach name_of() that a constructor reached through an Alias names the Alias.

Status: fixed: named_type() records an Alias's use where it's written as a type, and a call of an Alias's constructors records the Alias at its callee; names_tests' "an Alias is named as a type and as a constructor"

## front-5 (medium): Renaming an overridden Method or an Interface Method doesn't rename the others

lsp/src/features.cpp:431. Uses link each call only to the symbol it resolved to, and references() matches symbols by declaration key. So a Method, its Overrides, and the Interface Method it implements are separate names. Renaming any one of them leaves the program broken. The task lists Overrides and Interface Methods among the cases to cover.

Shown by: $S/u/main.fb (Shape.Area at line 8, Base.Area at 17, Child's Override Area at 30, uses at 44 and 49). Run `python $S/apply.py u/main.fb 49:Area Measure r1/main.fb`, then `fb check r1/main.fb`. Run `python $S/apply.py u/main.fb 44:Area Measure r2/main.fb`, then `fb check r2/main.fb`.

Expected: A rename keeps the program checking: it renames the whole override and implementation family, or it refuses.

Actual: r1 makes 2 edits, at (8,12) and (49,36), and fb check says 'Base implements Shape, so it needs a Method 'Measure''. r2 makes 2 edits, at (17,12) and (44,28), and fb check says 'Base implements Shape, so it needs a Method 'Area'' and ''Area' has Override, but Base has no Method like it'.

Skeptic: apply.py u/main.fb 49:Area Measure made 2 edits, at (8,12) and (49,36), and fb check said 'Base implements Shape, so it needs a Method 'Measure''. Renaming at 44:Area made edits at (17,12) and (44,28) and gave 2 errors: the Implements error and ''Area' has Override, but Base has no Method like it'. Renaming at 30:Area made 1 edit and gave the Override error. The rename went through and left a program that doesn't check, and it isn't among the listed known limits.

Fix: In references() and rename, widen the symbol set to the Overrides, the overridden Method, and the Interface members implemented, using the links sema already has for override and implementation checks. Or refuse such a rename.

Status: fixed, with features-5

## front-6 (medium): check_files drops roots whose path's letter case differs from the first root's, and clears their diagnostics

compiler/src/driver.cpp:2771. check_files() finds a root's package by comparing its weakly_canonical path with package->root, part by part (is_under). On MinGW, weakly_canonical keeps the letter case it's given. A root spelled 'C:/...' when the first root was 'c:/...' (VS Code sends 'c%3A') is under no package folder, so it's silently left out. The std branch compares real_path()s and doesn't have this problem. In the LSP, when the workspace folder and a document arrive with different drive-letter case, an edit's dependents check gets mixed roots: the dropped file's diagnostics are published empty.

Shown by: Harness: `$S/harness.exe check - c:/.../front/p2/a.fb C:/.../front/p2/b.fb` (and the same with an upper-cased B.FB) lists only module app/a.fb, with one error. LSP: `python $S/case.py` makes package p3, where b.fb and c.fb import a.fb and each has an Int + String error. The workspace folder URI is lowercase c%3A, a.fb and b.fb are opened with C:, and a.fb is then edited.

Expected: Every root in the package is checked whatever the letter case of its path, and c.fb keeps its error "'+' can't combine Int32 and String".

Actual: After load, b.fb and c.fb both show the error. After the edit, 'c.fb': [], because c.fb's error is cleared by a check that never loaded it.

Skeptic: I ran harness check - with p2's a.fb spelled c:/... and b.fb spelled C:/.... It loaded only module app/a.fb and reported only a.fb's error, so b.fb was dropped. With case.py on p3, both b.fb and c.fb had "'+' can't combine Int32 and String" after load. After the edit of a.fb, c.fb's diagnostics were [] while b.fb kept its error. In driver.cpp:2769-2771, check_files compares weakly_canonical(file) with package->root using is_under(), but is_under's own comment requires real_path() on both sides. The std branch, which uses real_path, does it right.

Fix: Compare with real_path() on both sides, as the std branch does, or with std::filesystem::equivalent on the package root's ancestors, before program_path(). Add a driver test with mixed-case roots.

Status: fixed: check_files() finds a file's package with real_path() on both sides, as is_under() needs, and the main file's path in the program too; driver_tests' check_files test names files in other letter cases on Windows

## front-7 (medium): check_files hides fb check's error for top-level code in a file the main file imports

compiler/src/driver.cpp:1704. check_files always sets `testing`, so every file of the main package with top-level code, other than the main one, is other_main and has its code left out. That includes files the main file imports. fb check and fb build report "only the main module's top-level code runs, so this module only declares things" for such a file. Neither the package's background check nor the file's own check (where it's a main file) ever gives that error, so the LSP never shows an error that fb build stops on.

Shown by: Package $S/p4: main.fb is `Import std.core / Import app.tool / Print(T())`, and tool.fb is `Import std.core / Public Function T(): Int => 1 / Print("side")`. Run `fb check .` in p4. Run `$S/harness.exe check <p4>/main.fb <p4>/main.fb <p4>/tool.fb`, which is the whole-package check. Run `python $S/p4.py`, which opens both files in fb-lsp.

Expected: Diagnostics match fb check: tool.fb:3:1 "only the main module's top-level code runs, so this module only declares things".

Actual: fb check reports the error. check_files reports no diagnostics, and fb-lsp publishes main.fb [] and tool.fb [].

Skeptic: In p4, fb check . reports tool.fb:3:1 "only the main module's top-level code runs, so this module only declares things". The harness whole-package check (main=main.fb, files main.fb and tool.fb) printed no diagnostics, and p4.py had fb-lsp publish main.fb [] and tool.fb []. tool.fb is imported by main.fb, yet it's treated as another main file. The LSP therefore never shows an error that fb check and fb build stop on, against the milestone's 'shows errors as you type'. The listed limit, being checked again alone, is about cost and doesn't cover hiding the error.

Fix: Only mark a file other_main when it isn't reached from the check's main file. Or, when `request.main` is set, check the main's import closure without `testing` and keep `testing` for the files the closure doesn't reach. Add a driver test.

Status: fixed: a file with top-level code is another main file only when the main file doesn't import it, directly or not, so the check gives fb build's error, in fb test's build too, as fb build does; driver_tests' check_files test

## front-8 (medium): 'Super.' typed inside a Method, and other unnamed members, give errors that weren't there before

compiler/src/sema.cpp:25190. check_unnamed_member() checks the object before the '.', and member_expr_t with an empty name now reaches paths that used to see error_expr_t. The step meant diagnostics to stay as they were, and the sema test claims there's only the parser's error. Typing `Super.` in an Override (a common completion point) adds "'Super' only comes before a call". `x.(`, `Point.()`, and `collections.()` add errors that name an empty member, "''". An object with an error of its own (`nothing.`, `std.`, `p.M.`, `p.Nope.`, `Self.` outside a Method) now gets that error reported too. This was compared against build-m8-release's fb, whose parser.cpp and sema.cpp are the same as at 42d9528c.

Shown by: Files are in $S/em: s1.fb (`Super.` in an Override Method), and c8, c18, c19, c21, c29, c103, c110, c111, c115.fb (`collections.()`, `nothing.`, `Point.()`, `Self.`, `Super.`, `p.M.`, `std.`, `std.collections.`, `p.Nope.`). For each, compare `C:/Users/Hunter/Projects/build-m8-release/cli/fb.exe check f` with `C:/Users/Hunter/Projects/build-m8-lsp/cli/fb.exe check f`. fb build gives the same.

Expected: Only the parser's "expected identifier, found ...", as before.

Actual: s1.fb adds "8:9: error: 'Super' only comes before a call, like 'Super.Hurt(5)'". c8 adds "'' isn't declared in std.collections", and c19 adds "Point has no member named ''". c18, c21, c29, c103, c110, and c115 add, respectively, "'nothing' isn't declared", "'Self' can only be used in a Method or New", the Super error, "'M' is a method; call it with ()", "'std' isn't declared", and "Point has no member named 'Nope'".

Skeptic: build-m8-release was built from agent-af1b9ef6216eb12e9, whose parser.cpp and sema.cpp match 42d9528c byte for byte (diff -q). I ran old and new fb check on each em file. On s1.fb, new adds "8:9: 'Super' only comes before a call" to `Super.` in a valid Override. c8 adds "'' isn't declared in std.collections", and c19 adds "Point has no member named ''". c18, c21, c29, c103, c110, c111, and c115 each add an object error that the old build didn't report. tooling.md says the object of 'x.' is worked out 'without another error'.

Fix: In check_unnamed_member(), handle Super (take the base Class's symbol, without the call check). Check the object with diagnostics held back, or keep the old error_expr_t behavior for the object's own errors. Have call and qualified-name resolution return early on an empty member name. Add these cases to the sema test.

Status: fixed: check_unnamed_member checks the object quietly, Super has its own case, and a call whose callee is a member without its name checks the object and the arguments and says nothing else; sema_tests' "a member being typed..." has each case

## server-1 (medium): A check that finishes late writes an old import graph over a newer check's, so later edits skip the files that import them

lsp/src/workspace.cpp:604. run_job() checks the open document's version and the generation before it updates m_latest and before it publishes. It does not check either before it overwrites m_imports[key] (lines 601-613). So a check that started earlier and finishes later, usually the whole-package load on the background thread, puts back the imports of the old text. After that, dependents() no longer finds the file that now imports the edited one, and that file is never checked again. This happens in normal use: editors reopen files at startup, and a large package's load takes many seconds, so any Import a user adds during that time is lost.

Shown by: Scratch package C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/pkg6: 25 files fN.fb, each importing std.net.http, std.data.json, and std.text.regex (the whole check takes about 3.7 s), plus b.fb ('Public Function B(): Int => 1') and a.fb ('Public Function A(): Int => 1'). Script t11.py in that folder's parent, run with `python t11.py`, using lsp.py there. It initializes with the folder, opens a.fb right away, changes it to 'Import "./b"' with 'A() => B()', and the hover shows B. It waits for the load to finish, then opens b.fb and changes B to Bx. Control: t11b.py does the same steps but waits for the load before it edits a.fb.

Expected: a.fb, which is open and uses B, gets "'B' isn't declared", and its go to definition on B returns null. This is what t11b.py shows: 2 publications for a.fb, with that error.

Actual: t11.py: a.fb's diagnostics stay [], no diagnostics are published for a.fb after b.fb's edit, and textDocument/definition on B in a.fb still returns b.fb 1:16 from the old check.

Skeptic: Ran server/t11.py: a.fb's hover showed B (checked at 1.0 s), the load finished at 3.82 s, and after b.fb dropped B, a.fb's diagnostics stayed [], nothing was published for a.fb, and definition on B still returned b.fb 1:16. The control t11b.py (waits for the load first) gives "'B' isn't declared", 1 publication, and definition null. In the code, run_job() (workspace.cpp about lines 601-613) clears and rewrites m_imports[key] for every module of any finished check, with no generation or version check. Only the publish lambda below it checks whether the result is current, so a whole-package load that finishes late puts back the imports of the old text. I rated it medium, not high: it only happens when a file gains an Import during the first load, and the file's next check repairs the graph.

Fix: Record the generation that each m_imports entry comes from, as m_published does. Overwrite an entry only from a check whose generation is at least the recorded one, and that read the file's current version when the file is an open document. Add a workspace test where a whole-package load finishes after an edit that adds an Import.

Status: fixed: run_job() rewrites a file's imports only from a check of its current text that's at least as new as the one they're from (m_import_generations); editing_tests' "an Import added while a package is checked whole keeps its place in the graph"

## server-2 (medium): Incremental edits land 3 bytes early in a document whose text starts with a UTF-8 BOM

lsp/src/server.cpp:78. apply_change() works out offsets from a source_file_t built from the document's text. source_file_t's constructor removes a leading UTF-8 BOM, but text.replace() then applies those offsets to the original string, which still has the BOM. Every ranged change to such a document goes 3 bytes too early, so the server's copy of the text quietly drifts from the editor's.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t2.py (`python t2.py`). It does didOpen with text '\ufeffImport std.core\nLocal a = 1\nPrint(a)\n', then a didChange with range 1:10-1:11 and text '2', which replaces the '1'.

Expected: The document becomes 'Local a = 2', with no diagnostics.

Actual: documentSymbol returns a variable named 'a2', hover shows 'Local a2: Int32', and the diagnostics say "'a' isn't declared" at 2:6. The edit replaced the space before '=' instead of the '1'.

Skeptic: Ran server/t2.py: after a ranged change of 1:10-1:11 to '2' on a document that starts with U+FEFF, documentSymbol returned the variable 'a2', hover showed 'Local a2: Int32', and diagnostics gave "'a' isn't declared" at 2:6. In the code, apply_change() (server.cpp line 78) builds source_file_t from the text, which drops the BOM, takes offsets from that, then calls text.replace() on the original string, which still has the BOM. So every edit lands 3 bytes early.

Fix: In apply_change, take the BOM off before working out offsets and put it back afterwards, or use offsets from a view of the text without the BOM and add the BOM's length. Add a test with a ranged didChange on a document that has a BOM.

Status: fixed: apply_change() puts the BOM's 3 bytes back on its offsets; editing_tests' ranged changes test

## server-3 (medium): A project.toml created in an open workspace is never picked up: its files stay loose until the server restarts

lsp/src/workspace.cpp:309. For a project.toml, changed_on_disk() only rebuilds PACKAGE units that already exist at that root. A new project.toml, as fb new or a user makes in a folder already open in the editor, adds no package. It doesn't add one to m_loads either, and the files already in m_unit_of keep their LOOSE unit. Closing and reopening a file doesn't help, because m_unit_of is never cleared. docs/tooling.md says a changed project.toml's package is checked whole.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t5.py (`python t5.py`), on folder pkg2: two.fb ('Public Function Two()') and main.fb ('Import pkg2.two', 'Print(Two())'), with no project.toml. The workspace folder is pkg2 and main.fb is opened. Then the script writes pkg2/project.toml with name "pkg2", sends didChangeWatchedFiles (type 1), edits main.fb, and also closes and reopens it.

Expected: Once project.toml exists, main.fb is checked as part of package pkg2, so 'Import pkg2.two' resolves and the diagnostics are [], as a new server shows.

Actual: After the project.toml is made and after the edit, and again after a close and reopen, the diagnostics are still "there's no module pkg2.two ...". A new server on the same folder gives [].

Skeptic: server/t5.py as written didn't reproduce this, because an earlier run had left pkg2/project.toml behind. I fixed it in review-m8s4/verify-server/t5.py to delete project.toml first. Then: loose gives "there's no module pkg2.two", after writing project.toml plus didChangeWatchedFiles plus an edit it's the same error, after close and reopen it's the same, and a new server gives []. In the code, changed_on_disk() (workspace.cpp line 309) only rebuilds PACKAGE units whose root already matches. A new project.toml adds no unit, and m_unit_of keeps the LOOSE entry. tooling.md says a changed project.toml's package is checked whole.

Fix: When a project.toml appears, or a project.toml is deleted, drop the m_unit_of entries and the units for the files under that folder, so unit_index() works them out again. Add the new package to m_loads when it's in one of the client's folders. Add a test to the didChangeWatchedFiles workspace test.

Status: fixed: changed_manifest() works out the unit of every file under a project.toml's folder again when it's made, changed, or deleted, checks a new package whole when it's the workspace's or has a document open, and checks its documents again; editing_tests' "a project.toml made in the workspace makes its files a package"

## server-4 (medium): didChangeWorkspaceFolders ignores removed folders, so they stay the workspace's: renamed, in workspace/symbol, and checked

lsp/src/server.cpp:352. The server says it handles workspaceFolders.changeNotifications, but the handler only reads event.added. Removed folders stay in m_folders and m_units. owns() is still true for their files, so prepareRename and rename still change them, workspace/symbol still lists them, and they're still checked as dependents.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t10.py (`python t10.py`). It initializes with folders pkg5 and pkg1, then sends workspace/didChangeWorkspaceFolders with removed = [pkg5], then calls workspace/symbol and textDocument/prepareRename on pkg5/base.fb's Base.

Expected: After pkg5 is removed, workspace/symbol has only pkg1's symbols (['n']). prepareRename on a closed file of the removed folder returns null, since that file is no longer the workspace's to change.

Actual: workspace/symbol still gives ['Base', 'Mid', 'n'], and prepareRename returns {placeholder: 'Base', range 1:16-1:20}.

Skeptic: Ran server/t10.py: workspace/symbol gave ['Base','Mid','n'] both before and after removing pkg5, and prepareRename on pkg5/base.fb still returned {placeholder:'Base', 1:16-1:20}. In the code, the didChangeWorkspaceFolders handler (server.cpp around line 352) only reads event.added, while initialize advertises workspaceFolders changeNotifications: true (line 434). So rename still edits files of a folder that is no longer the workspace's, which goes against the intended call that rename changes only workspace files.

Fix: Handle event.removed: take the folder out of m_folders, and take out its units' m_loads and their entries in the candidate sets. Clear the published diagnostics of closed files that are no longer owned. Add a test.

Status: fixed: didChangeWorkspaceFolders' removed folders leave the workspace (remove_folders()), which stops their checks and takes back their closed files' diagnostics; editing_tests' "a folder the client removes isn't the workspace's"

## features-12 (low): Signature help's active parameter counts commas and parentheses inside character literals

lsp/src/features.cpp:556. The comma scanner skips only double-quoted strings. A `','` character literal adds a comma, and a `'('` adds depth, so activeParameter is wrong.

Shown by: S/t2/sig.fb (it builds and runs) has `Print(Join(',', "x", "y"))` and `Print(Join('(', "x", "y"))`. `PYTHONUTF8=1 python S/sig.py S/t2/sig.fb '"x", "y"))@1' '"x", "y"))@1#1'` asks with the cursor inside the "x" argument.

Expected: activeParameter 1 (a) in both calls.

Actual: activeParameter 2 for `','` and 0 for `'('`.

Skeptic: In t2/sig.fb, which builds and runs, the cursor inside "x" in Join(',', "x", "y") gives activeParameter 2, and in Join('(', "x", "y") it gives 0. Expected 1 in both. features.cpp:548-562 skips only '"' strings, with no char literals or escapes. Severity is low because a char literal holding a comma or a bracket is rare.

Fix: Skip single-quoted character literals as strings are skipped, and honour backslash escapes in both. Better, walk the call's argument expressions from the AST. Add a test.

Status: fixed: the active parameter counts the lexer's commas and brackets, so a string's and a character's aren't; editing_tests' signature help test

## features-14 (low): Module qualifiers and Import aliases have no hover or definition, and refusing to rename one says the workspace doesn't declare it

lsp/src/server.cpp:428. The qualifier in `collections.Stack<Int>()`, or an alias `S` from `Import "./shapes" As S` in `S.Point`, records no use. Hover and definition are null there, and the rename refusal says "only a name the workspace declares can be renamed" though the file declares S.

Shown by: `PYTHONUTF8=1 python S/t3.py S/t2/s.fb "collections.Stack@3"`. And S/pkg (main.fb has `Import "./shapes" As S`): `python S/multi.py S/pkg main.fb "S.Point(3@0@T"`.

Expected: Hover shows the Import, definition goes to the module's file, and a refused rename says module names and aliases can't be renamed.

Actual: Hover and definition are None and references are []. The rename error is -32803 "only a name the workspace declares can be renamed".

Skeptic: In t2/s.fb, hover, definition, and references on `collections` in collections.Stack are None, None, and []. In pkg/main.fb, hover on `S` in S.Point(3 is None with refs []. Rename there, and also at the alias in `As S`, which does hover as 'Import geo.shapes', returns -32803 'only a name the workspace declares can be renamed', which is misleading.

Fix: Record a use of the MODULE symbol for a qualifier. Give prepareRename/rename a message specific to modules.

Status: fixed: uses for a module's qualifier in code and in types, so it has a hover and a definition, which is its module's file, and a rename of one says modules and As names aren't renamed; names_tests' "module qualifiers have hovers and definitions, and aren't renamed"

## features-15 (low): Completion in a Static Method offers instance members it can't use

lsp/src/completion.cpp:645. add_self_members adds add_instance_members(owner) for any Method, Static ones included, although a Static Method has no Self.

Shown by: S/t2/st.fb: Class Enemy with `Field hp` and a Static Method Spawn. `FULL=1 PYTHONUTF8=1 python S/comp.py S/t2/st.fb "count += 1=>Local z = |"` lists 'hp'. S/t2/st2.fb shows that using hp there is the error "'hp' belongs to each Enemy, and a Static Method has no Self".

Expected: Only the statics (count, Spawn) in a Static Method.

Actual: hp is offered.

Skeptic: In t2/st.fb, completion inside Static Method Spawn at `Local z = |` lists 'hp' along with count and Spawn. Using hp there is an error, as the reviewer's st2.fb shows.

Fix: Skip add_instance_members when the function symbol is_static.

Status: fixed: a Static Method's completion has its type's Statics only, and a type's Fields aren't among the variables of its block; editing_tests' completion test

## features-16 (low): Hover on an Enum value's declaration calls it a Const

lsp/src/query.cpp:299. Enum values are CONSTANT symbols, so signature() shows them as a top-level Const.

Shown by: `PYTHONUTF8=1 python S/t3.py S/t2/e.fb "Green\n@0"`.

Expected: Something like `Color.Green = 1`.

Actual: ```fb Const Green: Int32 = 1```

Skeptic: Hover on the declaration of Green in t2/e.fb shows ```fb Const Green: Int32 = 1```, which is not how an Enum value is declared.

Fix: In signature(), show a CONSTANT whose declaration is an enum member as `Enum.Value = n`.

Status: fixed: an Enum's value hovers as "Color.Green = 1"; names_tests' Enum test

## features-17 (low): Semantic tokens leave builtin type names untagged; signatures drop type parameters' constraints

lsp/src/semantic.cpp:128. Builtin types like `Int` in `Method A(): Int` get no semantic token, though the legend has `type`. Hover and signature labels print `Max<T>` for `Max<T: Comparable>`, though query_t::signature says it shows the declaration as code does.

Shown by: `PYTHONUTF8=1 python S/sem.py S/t2/m.fb` has no token at line 3's `Int`. `python S/sig.py S/t2/sig3.fb "Max(1, @7"` gives 'Function Max<T>(a: T, b: T): T', while std/core/values.fb:15 declares `Max<T: Comparable>`.

Expected: `Int` is tagged `type` (defaultLibrary), and the label is `Function Max<T: Comparable>(a: T, b: T): T`.

Actual: No token for Int, and the constraint is missing.

Skeptic: sem.py on t2/m.fb emits no token at line 3's `Int` in `Method A(): Int`; tokens jump from 'A' at col 11 to line 4. semantic.cpp applies DEFAULT_LIBRARY only to BUILTIN_FUNCTION. sig.py on t2/sig3.fb's Max(1, gives 'Function Max<T>(a: T, b: T): T', while std/core/values.fb:15 declares Max<T: Comparable>. That contradicts query.hpp:85's 'declaration as code shows it'.

Fix: Tag identifier tokens that builtin_type() knows as type/defaultLibrary. Print each type parameter's constraints in signature().

Status: fixed: a built-in type's name is a type token with defaultLibrary, and signatures show type parameters' constraints, "Max<T: Comparable>"; editing_tests' and names_tests' tests

## features-18 (low): The feature tests don't cover declarations, writes, Enum values, statics, overrides, or invalid and conflicting renames

tests/lsp/feature_tests.cpp:147. The hover, references, and rename tests start only from uses of Functions, Structs, Fields, and a Method in the small geo package, so features-1 to 7 all pass the suite. Completion has no test for strings or comments, types, Super, or statics. Signature help has no test for overloads or character literals. The milestone's done-when, "each works in a large package as you type", is checked for diagnostics and timing in workspace_tests, but no feature answer is checked there.

Shown by: tests/lsp/feature_tests.cpp, and `ctest --test-dir C:/Users/Hunter/Projects/build-m8-lsp -R lsp` (it passes with the defects above).

Expected: Tests for each of features-1 to features-13.

Actual: None of them exist.

Skeptic: In tests/lsp/feature_tests.cpp, grep finds no Enum, Override, Super, Static, or overload cases. The hover, references, and rename TEST_CASEs at 147-290 start from uses in the geo package. ctest -R lsp passes (1/1) with all the defects above confirmed. CLAUDE.md requires a test for each fix.

Fix: Add a test for each fix, as CLAUDE.md requires.

Status: fixed: names_tests.cpp and editing_tests.cpp cover each of these, and uses_tests.cpp every kind of use

## features-8 (low): Completion in strings and comments gives every name and keyword

lsp/src/completion.cpp:672. completion() never checks whether the offset is inside a string literal (outside an interpolation), a quoted Import path, or a comment. It falls through to names(). After a '.', it returns an empty list only by accident, because no member expression is there.

Shown by: S/t1/a.fb has `Local s = "hello p. wor"` and `// a comment p. here`. `PYTHONUTF8=1 python S/t1.py` requests completion at the end of `wor` and of `here`. `python S/comp.py S/t2/c.fb 'Import std.core=>Import std.core\nImport "./|'` does the same in a quoted Import.

Expected: An empty list (or null) inside plain string text and comments. A quoted Import path gives nothing, or file paths.

Actual: 142 items in the string, 143 in the comment, and 143 in the quoted Import path, all names and keywords.

Skeptic: t1.py: completion at the end of `wor` inside the string gives 142 items, and at `here` in the comment it gives 143. comp.py: inside the quoted path of `Import "./|` it gives 143, and inside "te|xt" it gives 144, all names and keywords. After '.' in a string or comment the list is []. Severity is low: VS Code doesn't trigger quick suggestions in strings or comments by default, so this shows mostly on a manual invoke.

Fix: Lex the line, or reuse lex_layout's trivia and the tokens, to detect that the offset is in a STRING or STRING_START/MIDDLE/END token outside `{...}`, a CHARACTER, or a comment, and return an empty list there. Add tests.

Status: fixed: completion gives nothing in a string's text, a quoted Import, a character, or a comment, as the lexer's tokens and trivia say, though an interpolation's code is code; editing_tests' completion test

## front-10 (low): tooling.md and the uses test claim more than sema records

docs/tooling.md:121. tooling.md says "Sema records each use of a symbol (`analysis_t::uses`)", and sema.hpp says uses cover a name, a member, a type, or a call. Neither mentions the gaps in front-2, front-3, and front-4. The sema test "analysis keeps where each symbol is used" covers only fields, functions, constructors, and From lines, so none of these gaps fails a test. Also, Operator uses span the whole expression, so name_of() always drops them; that's intended but not stated where the doc says operators are recorded.

Shown by: Read docs/tooling.md:121 and tests/compiler/sema_tests.cpp's last test, then run `$S/harness.exe uses u/main.fb` and `$S/harness.exe uses w/main.fb`.

Expected: The docs and the test match what's recorded, or the gaps are closed.

Actual: Uses are missing for Extends and Extend types, Enum members, Static qualifiers, and Aliases, and no test notices.

Skeptic: tooling.md:121 says 'Sema records each use of a symbol (analysis_t::uses)', but the uses dumps show no uses for Extends/Extend types, Enum members, Static qualifiers, Aliases, or module aliases (front-2 to front-4). The test "analysis keeps where each symbol is used" (sema_tests.cpp:16280) covers only a Struct, Field, Function, constructor, parameter, local, and a From line, so none of these gaps fails a test. It largely restates front-2 to front-4 plus the test gap.

Fix: Close front-2 to front-4, and extend the test to cover each of the task's listed cases: Extends, Overrides, Interface Methods, Properties, Operators, constructors, Enum members, and Aliases. Otherwise, document the limits.

Status: fixed: tooling.md and symbol_use_t say what's recorded, operators' spans too, and uses_tests' program has each kind of use

## server-5 (low): Any opened document is checked as FutureBlitz code, project.toml included, which then also stops changes to it on disk from being seen

lsp/src/server.cpp:300. didOpen accepts every URI, whatever its languageId or extension, and the workspace checks it as a .fb root. The server itself publishes diagnostics for project.toml URIs, so a client that sends didOpen for project.toml is plausible (a generic client, or an extension whose selector covers project.toml). Such a project.toml is then parsed as FutureBlitz code and gets nonsense errors. It also becomes a document, so changed_on_disk() skips it, and saving the edited project.toml never rebuilds the package's unit or rechecks the package.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t15.py (`python t15.py`), on package pkg3. It does didOpen for pkg3/project.toml with languageId 'toml', then a didChange to '[project]\nname = 5\n'.

Expected: The project.toml document isn't checked as .fb code. Its diagnostics come from the manifest reader (for name = 5: "'name' is text, like \"game\""), and its package is checked again.

Actual: The published diagnostics for project.toml are 'expression has no effect', "'project' isn't declared", "'name' isn't declared", and "'version' isn't declared", from compiling the TOML as FutureBlitz code.

Skeptic: Ran server/t15.py: didOpen of pkg3/project.toml with languageId 'toml' published 'expression has no effect', "'project' isn't declared", "'name' isn't declared", and "'version' isn't declared". After the change to 'name = 5', it published the same kind of FutureBlitz errors rather than the manifest's error. In the code, didOpen (server.cpp around line 300) passes any URI to workspace->open(), and changed_on_disk() skips anything in m_documents, project.toml included. I rated it low because clients usually send didOpen only for their language's documents, and step 5's extension will select fb.

Fix: In didOpen and didChange, only check documents whose languageId is the fb language or whose path ends in .fb. Treat an open project.toml as a manifest change: rebuild the unit and add it to m_loads, reading the document's text in place of the disk's.

Status: fixed: only .fb documents are opened as code; a project.toml is read from disk, so its changes there are seen; editing_tests' "an open project.toml isn't checked as code..."

## server-6 (low): Opening a file from a package outside the workspace publishes diagnostics for all that package's files, and closing it never clears them

lsp/src/workspace.cpp:288. open() adds a package outside the client's folders to m_loads, and the whole-package check publishes diagnostics for every file in it. close() clears only the closed file's diagnostics, despite its comment that a file outside the workspace has nothing to say once it's closed. The package's other files keep their errors in the editor's Problems list for the rest of the session.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t12.py (`python t12.py`), on package pkg7: a.fb, which is fine, and broken.fb ('Public Function Broken(): Int => "no"'). There are no workspace folders. The script opens a.fb, waits 4 s, closes a.fb, and waits 2 s.

Expected: Once nothing from pkg7 is open, there are no diagnostics left for pkg7's files, which were never opened and aren't in the workspace. Or, at the least, broken.fb's are cleared when a.fb closes.

Actual: broken.fb, never opened, gets 'expected Int32, found String', and still has it after a.fb is closed. Only a.fb's diagnostics are cleared.

Skeptic: Ran server/t12.py with no workspace folders: opening pkg7/a.fb published 'expected Int32, found String' for broken.fb, which was never opened. After a.fb was closed, only a.fb's diagnostics were cleared and broken.fb's stayed. In the code, open() (workspace.cpp around line 245) adds an outside package to m_loads, the whole check publishes for every root, and close() (around line 288) clears only the closed key, although its comment says a file outside the workspace has nothing to say once it's closed. Stale Problems entries, so low.

Fix: Don't publish diagnostics for unopened files of units that aren't owned, and still keep their import graph. Or, when the last open document of a unit that isn't owned closes, publish empty diagnostics for each of that unit's files that has been published.

Status: fixed: when the last open document of a package outside the workspace closes, its files' diagnostics are taken back and its whole check is dropped (forget()); editing_tests' "a package outside the workspace keeps no diagnostics..."

## server-7 (low): A failed initialize still marks the server initialized, and a second initialize is accepted and resets the position encoding

lsp/src/server.cpp:366. initialize() sets m_initialized = true before it reads params. When it then throws (params null, or capabilities null), the client gets -32602 but the server answers later requests anyway. initialize isn't refused once the server is initialized, so a second one succeeds. It rewrites m_positions.unit, which the workspace threads read without a lock in publish(): this is a data race, and the encoding changes mid-session.

Shown by: C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/t14.py and t3.py (`python t14.py`). t14 sends initialize with {"capabilities": null}, then documentSymbol, then initialize again offering utf-8.

Expected: documentSymbol after the failed initialize gets -32002, and the second initialize is refused, for example with an INVALID_REQUEST error.

Actual: The first initialize fails with '[json.exception.type_error.306] cannot use value() with null'. documentSymbol is then answered with result null rather than -32002, and the second initialize succeeds, switching to utf-8 (t3.py also gets two successful initializes).

Skeptic: Ran server/t14.py: initialize with capabilities null got -32602 'cannot use value() with null', documentSymbol was then answered with result null rather than -32002, and a second initialize offering utf-8 succeeded. t3.py also shows two successful initializes. In the code, server_t::initialize() (server.cpp line 366) sets m_initialized = true before reading params, never refuses a repeat, and rewrites m_positions.unit, which publish() reads from the workspace threads.

Fix: Set m_initialized only after initialize has read its params. Answer an initialize that comes after a successful one with an error. Add both cases to server_tests.cpp.

Status: fixed: initialize sets the server up only once its params are read without a mistake, and a second initialize is refused with INVALID_REQUEST; capabilities of null are taken as none, which isn't a mistake; editing_tests' "initialize fails without initializing, and comes once"

## server-8 (low): No test covers incremental sync (ranged changes) or non-ASCII and UTF-16 surrogate-pair positions

tests/lsp/client.cpp:134. The server says it does incremental sync (textDocument.change = 2) and uses UTF-16 positions by default, but client_t::change() only sends whole-text changes. No test in tests/lsp sends a ranged contentChange, a change that spans CRLF lines, several changes in one notification, or text with characters outside the BMP. The BOM bug in server-2 got through because nothing tests apply_change(). By hand, ranged edits after an emoji and across CRLF lines worked in both encodings (t1.py), and diagnostic and semantic-token columns after an emoji were right (t13.py).

Shown by: grep -n 'range\|contentChanges' tests/lsp/*.cpp finds no ranged didChange. The scratch scripts t1.py and t13.py in C:/Users/Hunter/AppData/Local/Temp/claude/C--Users-Hunter-Projects-FutureBlitz/fea71702-0309-45a9-9566-b0f5ce94baf0/scratchpad/review-m8s4/server/ cover these cases by hand.

Expected: Tests that apply ranged didChanges in UTF-16 and UTF-8, including after a surrogate pair, across CRLF lines, with several changes in one notification, and on a document with a BOM, and that check diagnostic and semantic-token columns after an emoji.

Actual: No such tests exist.

Skeptic: client_t::change() (tests/lsp/client.cpp line 145) only sends {text} whole-document changes, and a grep of tests/lsp for contentChanges, positions_t, offset(, surrogate, and BOM finds no ranged didChange and no non-ASCII or surrogate position test. The server advertises change: 2 and UTF-16 by default. The BOM bug in server-2 went undetected because apply_change() has no test.

Fix: Add a ranged change(uri, range, text) to client_t, and add tests like t1.py, t2.py, and t13.py to tests/lsp.

Status: fixed: client_t::edit() sends ranged changes, which editing_tests' ranged changes test makes in UTF-16 after a surrogate pair, in UTF-8, across CRLF lines, several in one, and after a BOM
