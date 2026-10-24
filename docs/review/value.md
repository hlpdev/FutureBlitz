# The review of M6 step 17's first unit, std.core's Value (data of any shape that every format reads into and writes from)

8 confirmed findings: 0 high, 2 medium, 6 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## change-1 (medium): Holds goes through a shared part once for each reference to it, so Add takes exponential time

Status: fixed in 2de27173 and 30195173: Holds walks with a stack of its own and marks each Value it looks through with that walk's own object, in the Value's Walked Field (first a Global Atomic's count, which 30195173 replaced, since a Global shows in every program), so a part held in several places is looked through once; core_value_deep adds 60 levels that each hold the one before twice, and panic_value_holds_itself_shared finds a 40-level one holding itself at once

Where: std/core/value.fb:414

CheckHeld calls value.Holds(Self), and Holds walks every element and entry with no record of what it has already visited. Sharing is allowed: one Value can sit in several places, which the docs call 'a change shows through every reference'. So a Value whose levels each hold the level below twice gets walked 2^n times. With 26 such levels, 52 Adds take about 5 s in release and 10 s in debug. 20 levels take 0.1 s and 28 take 24 s, about 4x more for every two levels, so 40 levels never finish. Building the same Value with New(elements), which runs no check, takes 0.17 s, so the cost is Holds alone. The docs' 'so each is a tree' (value.fb's header, stdlib.md, spec 14) hides this case: the Value here is a DAG.

```
Import std.core

// Each level holds the one before it twice: 26 levels are 52 Adds.
Local level = Value.Array()
For i = 1 To 26
    Local pair = Value.Array()
    pair.Add(level)
    pair.Add(level)
    level = pair
End For

Print(level.Count)
```

Expected: 2, printed at once (52 Adds of small Arrays)

Actual: 2, printed after about 5 s with --release and 10 s in debug; with 28 levels it takes 24 s, and each further level doubles the time

Suggested fix: Have Holds skip Values it has already looked through, by reference, not by ==. For example, walk with an explicit stack plus a set of visited references, so each distinct Value is visited once. The same change fixes change-2. Say in the docs that parts can be shared rather than 'each is a tree'.

Skeptic: I ran the reviewer's program (rv-value/idx/dag26.fb). Each level is an Array holding the level before it twice. With --release, 20 levels took 0.63 s, 26 took 5.4 s and 28 took 22.2 s, all wall time including the ~0.5 s compile, and each printed 2. Debug at 26 levels took 6.4 s. The same 26 levels built with Value([level, level]), which runs no check, took 0.48 s (dagnew.fb), so the time is CheckHeld's. value.fb:414-431: Holds recurses into every element and entry and keeps no record of what it has visited, so a shared part is walked once for each path to it. That is 2^n paths for a Value of only n+1 distinct Arrays. Sharing is allowed, and the docs point to it ('a change ... shows through every reference to it'). A reachability check like this can run in linear time with a visited-by-reference set.

## core-2 (medium): ToString, Clone, Hash, and == crash with a stack overflow for a Value nested about 2000 deep in debug, deeper in release

Status: fixed in 2de27173: ToString (into parts the runtime's new fb_value_joined joins), Clone, Hash, and == walk with stacks of their own, Hash through std.core.value's Step, which keeps each Array's or Object's place and what it's added up; core_value_deep writes, clones, hashes, and compares Values 100000 deep, in debug and release

Where: std/core/value.fb:376

ToString, Clone, Hash, and == recurse once per level of nesting. In debug the program dies with 0xC00000FD, no panic and no message, at these depths: ToString by 2000, Clone and Hash by 2500, == by 4000. Release survives 4000 to 8000, so the same program crashes in debug and works in release. Nesting a few thousand deep is easy to reach in code, and it is the shape a format reader will build from nested input. Building one this deep with New(Value[]) works, and so does releasing it (10000 deep was fine); only these traversals crash.

```
Import std.core
Import std.collections

// An Array in an Array, 3000 deep, as nested JSON might read.
Local nested = Value(1)
For i = 1 To 3000
    nested = Value([nested])
End For

Local text = nested.ToString()
Print(text.Bytes.Count)
```

Expected: 6001 in both debug and release, or a panic that says how deep a Value can go

Actual: debug: error: deep.exe exited with code 0xC00000FD (nothing printed)
release: 6001 (release crashes the same way by 8000 deep for ToString and Clone, and by 20000 for Hash and ==)

Suggested fix: Walk the Value with an explicit stack of its own in ToString, Clone, Hash, and == (and Holds), not by recursion. Or cap the depth with a panic that names the limit, and have the format readers cap it too. Add a deep Value to the tests.

Skeptic: Ran c2.fb (3000 deep): debug 'error: c2.exe exited with code 0xC00000FD', release 6001. My own runs: Clone crashes in debug at 2500 (release survives 4000 and dies at 8000). Hash crashes in debug at 2500 (release is fine at 8000). == between two separately built 3000+ deep Values (e_eq_*.fb) crashes in debug at 4000 and in release by 20000. Add of a deep Value crashes too (e_add_*.fb, through CheckHeld/Holds): debug at 4000, release at 8000. Every crash prints no panic or message. Releasing a 10000-deep Value is fine, and a plain recursive user function (f_deep_list.fb) goes 3000 deep in debug with no trouble. So the crash comes from Value's recursive ToString, Clone, Hash, ==, and Holds (value.fb 300-431), whose debug frames are heavy. decisions.md 678 shows the project already counts 'a few thousand linked objects ran out of stack' as a bug, and fixed it in the runtime's release. value.fb's header says these 'go through to its end'.

## change-2 (low): Holds recurses once per level and overflows the stack when a Value nested about 4000 deep is added

Status: fixed in 2de27173, with core-2: Holds no longer recurses, so adding a Value 100000 deep works (core_value_deep, built both from the inside out and from the outside in)

Where: std/core/value.fb:414

Holds calls itself for every element and entry, so its recursion depth equals the Value's nesting depth. A chain built from the top runs fine, because each Add looks only at an empty Array. Adding that chain anywhere else makes CheckHeld walk the whole depth. The process then dies with a stack overflow (0xC00000FD) instead of adding or panicking. This happens between 3000 and 4000 levels in debug and between 5000 and 10000 in release. ToString, ==, Hash, and Clone also overflow at 5000 in debug, so the limit is the type's. But Holds runs on every []=, Add, and Insert, so a program that only builds a Value, without printing or comparing it, still crashes.

```
Import std.core

// A chain 4000 deep, built from the top, so each Add looks through one empty Array.
Local top = Value.Array()
Local at = top
For i = 1 To 4000
    Local next = Value.Array()
    at.Add(next)
    at = next
End For

Print("built")
Flush()
Local holder = Value.Array()
holder.Add(top)
Print("added")
Flush()
```

Expected: built
added

Actual: built
error: h4000.exe exited with code 0xC00000FD (debug; at 10000 levels release crashes the same way)

Suggested fix: Walk iteratively in Holds, with an explicit stack of Values still to look at (and the visited set from change-1). Alternatively, settle on a nesting limit that []=, Add, and Insert panic past, with a message, and that the walks and later readers share.

Skeptic: I built a chain 4000 deep from the top (h4000.fb) and added it to a new Array. Debug printed 'built', then 'error: h4000.exe exited with code 0xC00000FD'. Release survived 4000 levels ('added') but crashed the same way at 10000 (h10000.fb). It survived 3000 in debug. Building alone and letting the chain go is fine: 4000, 10000 and 100000 levels in debug, and 10000 in release, all exit cleanly, since the runtime releases objects one at a time (decisions.md:678). So the crash comes from Holds recursing once per level (value.fb:414). The docs say Add either adds or panics, so a silent stack-overflow crash goes against them. I rate it low because the limit is the type's as a whole: top == top.Clone() also overflows at 4000 in debug (print4000.fb) and passes in release. Making only Holds iterative wouldn't make deep Values usable; that needs a nesting limit or iterative walks across the type.

## change-5 (low): Spec 14's Value section says 'each is a tree', which shared parts disprove, on a line 113 columns wide

Status: fixed in 479cc43d: spec 14, stdlib.md, and value.fb's header say one Value can sit in several places, where a change to it shows in each, and that Clone copies it into each, and spec 14's line wraps at 100

Where: docs/spec/14-strings-collections.md:287

'None holds itself, inside it however deep, which a change that would make it panics at, so each is a tree.' The code allows one Value in several places of another, so the result is a DAG, not a tree. A change to the shared part shows in every place, and Clone splits the shared part into separate copies. stdlib.md ('so each is a tree') and value.fb's header comment say the same. Line 287 also runs to 113 columns while the rest of the section wraps at 100.

```
Import std.core

// One Value in two places: a change to it shows in both, so the Value is no tree.
Local part = Value.Array()
Local whole = Value.Array()
whole.Add(part)
whole.Add(part)
part.Add(Value(1))
Print(whole)
// Clone splits the shared part into two.
Local copy = whole.Clone()
(copy[0] ?? Value()).Add(Value(2))
Print(copy)
```

Expected: What the docs describe: a tree, in which a part sits in one place

Actual: [[1], [1]]
[[1, 2], [1]]

Suggested fix: Say instead that none holds itself, though one Value can sit in several places, and that Clone copies each place separately. Make the same change in stdlib.md and value.fb's header, and rewrap line 287 at 100 columns.

Skeptic: share.fb printed '[[1], [1]]' then '[[1, 2], [1]]'. One Array sits in two places, a change to it shows in both, and Clone splits it into two separate copies. So a Value the code allows is a DAG, not the tree that spec 14 (line 287: 'so each is a tree'), stdlib.md:239 and value.fb:5 call it. The 'so' presents 'tree' as following from 'none holds itself', which only gives acyclic. Also, awk shows line 287 at 113 columns, while every other prose line of the new Value section (270-288) is 98 or under: a missed rewrap.

## core-3 (low): stdlib.md says the getters' elements are copies, but AsArray's elements are the Value's own

Status: fixed in 479cc43d: stdlib.md and AsArray's comment say it's a new array of the Value's own elements, so a change to an element shows; core_value prints one

Where: docs/stdlib.md:240

stdlib.md says 'the getters' bytes and elements are copies'. AsArray does give a new array, but it holds the same Value objects, as Elements?.Clone() is shallow. So a change to an element made through AsArray's result shows in the original Value. The code matches value.fb's own comment ('in a new array, which the Value doesn't see changes to'), so the docs' wording is what's wrong.

```
Import std.core
Import std.collections

// stdlib.md: "the getters' bytes and elements are copies". AsArray's array is new, but its
// elements are the Value's own, so a change to one shows in the Value.
Local list = Value([Value.Object()])
Local elements = list.AsArray() ?? []
elements[0]["changed"] = Value(True)
Print(list)
```

Expected: [{}] if the elements were copies, as stdlib.md says

Actual: [{"changed": true}]

Suggested fix: Reword stdlib.md: AsBytes gives a copy, and AsArray gives a new array of the same Values, so adding to or removing from it doesn't change the Value but a change to one of its elements does.

Skeptic: Ran c3.fb: debug and release both print [{"changed": true}]. AsArray (value.fb 205) is Elements?.Clone(), a shallow copy, so the array is new but its Values are the original's. stdlib.md's Value bullet says 'the getters' bytes and elements are copies', which reads as copied elements, and the behavior contradicts that. The code works as designed, so the fix is to the wording. value.fb 203's 'which the Value doesn't see changes to' and core_value.fb line 38's comment are just as loose: the Value doesn't see adds to or removals from the array, but it does see changes inside an element.

## core-4 (low): The docs say each Value is a tree, but a Value can be held twice, and traversals then take exponential time

Status: fixed in 479cc43d, with change-5: the docs say a Value holds no cycle, not that it's a tree, and stdlib.md says ToString, ==, Hash, and Clone go through a part held in several places once for each place

Where: docs/spec/14-strings-collections.md:287

Spec 14, stdlib.md (line 239), and value.fb's header (line 5) say that because none holds itself, 'each is a tree'. CheckHeld only rules out cycles, so one Value can sit in an Array or Object any number of times: the Value is a DAG, and a change to the shared Value shows in each place. ToString, ==, Hash, and Clone visit a shared Value once for each place it sits, with no memo. So a chain of 23 Values made with v = Value([v, v]) is about 4 million visits: Clone() == v takes about 3.5 s in release, and each extra level doubles it. Clone also gives back an unshared tree of that size.

```
Import std.core
Import std.collections

// The docs say each Value is a tree, but one Value can sit in another twice.
Local shared = Value.Array()
Local outer = Value.Array()
outer.Add(shared)
outer.Add(shared)
shared.Add(Value(1))
Print(outer)

// So 23 Values can be 4 million nodes to Clone, Hash, ==, and ToString.
Local v = Value(1)
For i = 1 To 22
    v = Value([v, v])
End For
Print(v.Clone() == v)
```

Expected: Docs that match the shape: a Value holds no cycle, but a Value may appear in several places

Actual: [[1], [1]]
True (after about 3.5 s in release; each extra level doubles the time)

Suggested fix: Say 'holds no cycle' rather than 'is a tree' in spec 14, stdlib.md, and value.fb's header. Note that a Value held in several places is written, compared, hashed, and cloned once for each place. Or refuse a Value that is already held elsewhere, if trees are what's intended.

Skeptic: Ran c4.fb: it prints [[1], [1]] and then True. Adding one Value to an Array twice is allowed, because CheckHeld/Holds (value.fb 409-431) only rules out cycles, so a Value can be a DAG. Spec 14 (lines 286-287), stdlib.md's bullet, and value.fb's header (line 5) all say 'none holds itself ... so each is a tree', and that inference is false. The 22-level v = Value([v, v]) chain took about 7.3 s for `fb run --release`, compile included (an 18-level one took 3.8 s in debug). That shows each shared Value is visited once per place, with no memo, and that Clone unfolds the sharing. The time cost is inherent to treating a DAG as a tree (Python's json.dumps does the same), so the real defect is the docs' claim of a tree. Low severity.

## core-5 (low): stdlib.md and spec 14 leave out Keys and Contains, and say ToString writes JSON though bytes come out as <hex>

Status: fixed in 479cc43d: stdlib.md and spec 14 name Keys() and Contains(key), and say bytes are written in hex between angle brackets, which JSON doesn't have

Where: docs/stdlib.md:229

Value's Public Keys() and Contains(key) appear in neither stdlib.md's Value bullet nor spec 14's Value section. stdlib.md ends with '`ToString` writes it as JSON on one line', but a Value holding bytes writes <01ff>, which isn't JSON. Only value.fb's comment mentions the hex form, and neither doc does. Also, spec 14's line 287 runs to 113 columns, where the rest of the new section wraps at 100.

```
Import std.core
Import std.collections

// stdlib.md says ToString writes JSON, but bytes come out as <hex>; and Keys and Contains,
// which are Public, aren't in stdlib.md's or spec 14's list.
Local config = Value.Object()
config["blob"] = Value([1 As UInt8, 255])
Print(config, config.Keys(), config.Contains("blob"))
```

Expected: Docs that name Keys() and Contains() and say that bytes are written as <hex>

Actual: {"blob": <01ff>} [blob] True (all of it works, but the docs don't describe the Keys, Contains, or bytes text)

Suggested fix: Add Keys() and Contains(key) to stdlib.md's bullet (and spec 14's list). Say that ToString writes bytes in hex in angle brackets, so it is JSON except for bytes. Rewrap spec 14's line 287 at 100 columns.

Skeptic: Ran c5.fb: both builds print {"blob": <01ff>} [blob] True. Keys() and Contains(key) are Public (value.fb 208, 214), yet stdlib.md's Value bullet (lines 229-242), which lists every other member, names neither. Its closing '`ToString` writes it as JSON on one line' doesn't mention that bytes are written as <hex>, which isn't JSON. That form is the lane's choice, and only value.fb's comment states it. awk shows spec 14's line 287 at 113 columns. The section's other prose wraps at 100, and only table rows run longer.

## core-7 (low): milestones.md still puts Value in step 19's std.data I and has no row for std.core's Value

Status: handed over: milestones.md and decisions.md are the coordinator's. Row 19's `Value` belongs to step 17 (std.core's Value, with Load<T> and Save<T>), and decisions.md needs the 2026-10-23 call, one dynamic Value in std.core with built-in Load<T> and Save<T>; lane4-value.md has the row's text

Where: docs/milestones.md:187

Row 19 still begins 'std.data I: `Value`; JSON ...', and no row (step 17's included) lists std.core's Value. But Value now lives in std.core, a program reaches it with Import std.core alone, and stdlib.md was changed to say every format reads into std.core's Value. So milestones.md disagrees with stdlib.md and the code about where Value is and when it lands. The user's 2026-10-23 decision (one dynamic Value in std.core, with built-in Load<T> and Save<T>) also isn't in decisions.md.

```
Import std.core

// Value is std.core's now, with no std.data Import.
Print(Value(1).Kind, Value.Object())
```

Expected: milestones.md listing Value as std.core's, in the step that builds it, with row 19 building on it

Actual: INT {} (the code has Value in std.core, while milestones.md row 19 still lists `Value` under std.data I)

Suggested fix: Move Value from row 19's scope to the row of the step that builds it (std.core), and add the 2026-10-23 decision to decisions.md.

Skeptic: Ran c7.fb: 'INT {}', with Value reached through Import std.core alone. milestones.md line 187 (row 19) still begins 'std.data I: `Value`;', and no row names std.core's Value. Row 17, the step this unit is said to belong to, is std.os and doesn't mention it either. This change's stdlib.md edits move Value into std.core and rewrite std.data's first bullet as 'Every format reads its text into std.core's `Value`', so the two docs now disagree. A grep of decisions.md finds no row for the 2026-10-23 call (one dynamic Value in std.core, built-in Load<T>/Save<T>), though other calls from that date are recorded '(the user's call on 2026-10-23)'. master's decisions.md (f6211258) lacks it too.

## Refuted by the skeptics (no action)

- core-1: ToString writes a NaN with its sign, so one program prints -nan in debug and nan in release, and NaN and infinities aren't JSON. Ran the program (scratchpad/rv-value/core-refute/c1.fb). Debug prints [-nan, inf, -inf] and release prints [nan, inf, -inf], so the difference is real. It is not Value's, though. c1b.fb has no Value at all, just Print(nan, "{nan}", 1.0 / zero), and it shows the same split: debug '-nan -nan inf', release 'nan nan inf'. c1c.fb computes the NaN at run time, so nothing can fold it, and both builds print '-nan -nan nan [-nan]'. So the sign comes from the shared Double text, runtime/src/string.cpp from_number (std::to_chars, which writes NaN's sign bit). The release build differs only because the constant folder makes a +NaN. DoubleText just hands back that text, as the lane decision says ('Value(1.0e300) prints C's 1e+300 until step 5's float text lands'). Step 5's row ('what Format writes Parse reads back, every Float64 included') is where a NaN's text gets settled. JSON has no NaN or infinity, and no doc names a text for them, so ToString's 'as JSON' wording can't be held to one. This is a pre-existing Double-text issue, not this change's. The coordinator may still want it tracked: a NaN's printed sign depends on constant folding.
- core-6: core_value.fb never checks AsInt's range check: its only Int64 INT, 3000000, fits an Int. Ran c6.fb: both builds print 'True True 2147483647 -2147483648' and 'UINT True'. AsInt's range check (value.fb 177-182) and New(UInt64)'s switch at 2^63 (value.fb 73-80) are both correct, so there is no wrong behavior and nothing contradicts the docs. The gap the reviewer describes is accurate. core_value.fb line 30's Value(3000000 As Int64) fits an Int (its .out line 29 shows 3000000 in the AsInt column), and no INT outside Int32's range reaches AsInt. 4000000000 is used only in the kinds list, and 2^63 itself isn't tested. That's a missing test, not a bug, but adding it is cheap.
- change-3: Path panics with 'isn't a path' for a digits index past Int64, though the text is a path. Reproduced (path.fb, debug and release): '[5]' and '[9223372036854775807]' give "none", and '[9223372036854775808]' panics with the 'isn't a path' message at path.fb:7:7. But the lane chose this on purpose and tests it: tests/runtime/value_tests.cpp:123 checks that '[9223372036854775807]' is a path, and line 135 puts '[99999999999999999999]' among the wrongs. runtime.hpp's 'like an empty key or an index that isn't digits' and value.fb's 'like "a..b" or "a[x]"' give examples; neither says these are the only cases. The spec and stdlib.md say nothing about indices past an Int64. The path is the program's own literal text, so an index that can't be written as an Int64 being treated as malformed is a defensible call. Nothing in the docs is plainly contradicted, so I refute it.
- change-4: The holds-itself checks in Insert and []=(index), and the wrong-kind panics of Insert, RemoveAt, and []=(index), are untested. The gap is real: only core_value.fb's ordinary calls use Insert and RemoveAt, and no .out file has the Insert, RemoveAt or [index]= wrong-kind messages, or a holds-itself panic through Insert or [index]=. But every path behaves correctly. I ran each one in debug: inner[0] = array, inner.Insert(0, array) and inner.Insert(0, inner) give 'a Value can't hold itself, inside it or as it is' at the caller (c1.fb:6:6, c2.fb:6:1, c8.fb:6:1). Object()[0]= and Value(1)[0]= give "a Value's [index] = works on an Array, not an Object/an integer" (c3.fb:7:2, c10.fb:6:9). Value("x").Insert and Value("x").RemoveAt give 'Value.Insert/RemoveAt works on an Array, not a String' at 6:1. RemoveAt(1) and Insert(2, ...) on a 1-element Array give 'index N is out of range for 1 element' at the caller's line. A holds-itself check through an Object's entry also panics (c9.fb). With no wrong behavior and nothing in the spec, stdlib.md or step 19's row that it contradicts, this is a coverage gap, not a bug.
- change-6: RemoveAt has no comment, and Insert's doesn't say it panics at an index outside the Array. insdoc.fb printed '[1, 2]' then 'panic: index 2 is out of range for 2 elements at insdoc.fb:6:1', which is correct behavior. RemoveAt (value.fb:290-293) does lack a comment, and Add/Insert's comment doesn't mention the index panic. But no rule in the spec, stdlib.md or the milestones row is broken: stdlib.md lists RemoveAt as one of the methods that change an Array. std doesn't consistently name index panics in comments either: ImmutableArray.RemoveAt's comment (immutable.fb:96) is just 'A copy without the value at index.' And several Public members in this same file (Kind, IsNull, IsBool and the other Is properties) have no comment. This is a minor comment gap, not a bug, so I refute it.
