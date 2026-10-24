# The review of M6 step 7, Text II: std.text.regex over PCRE2 with match limits and the String shortcuts, the compiler's check of literal patterns, Glob, WordWrap, Dedent, Indent, Levenshtein, fuzzy matching, Humanize, std.text.diff, and the overload fix for untyped lambdas (it sits on step 6, which is reviewed separately; review only step 7's change)

25 confirmed findings: 1 high, 11 medium, 13 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## S7-tools-1 (high): Diff.Merge turns an identical deletion on both sides into an empty conflict

Status: fixed in 87cce01 (a merge where both sides delete the same lines takes the deletion, as git's does), with text_diff_git.fb's four merges where both sides delete a line, a run, and the whole text, against git merge-file -p; two sides that made the same of their lines have them as lines both have, as git has no change for them, so they keep no conflicts apart that git joins

Where: std/text/diff.fb:428

When both sides delete the same base lines, Merge's region has both sides changed, Mapped gives [] for each, and Refine's first check `If mine.Count == 0 || others.Count == 0` makes it a CONFLICT before the `changes.Count == 0` check that would TAKE identical sides. The result is a conflict with nothing on either side, counted in Conflicts. git merge-file treats the two deletions as one clean change. This comes up all the time in real merges, for example when both branches remove the same debug line or a cherry-picked change is merged back. Differential runs against git merge-file hit it in about 1 in 100 random small merges. No test covers it.

```
// Both sides delete the same line: git merge-file gives "a\nc\n" with no conflict.
Import std.core
Import std.text.diff

Local merged = Diff.Merge("a\nb\nc\n", "a\nc\n", "a\nc\n")
Write(merged.Text)
Print(merged.Conflicts)
```

Expected: a
c
0

Actual: a
<<<<<<< ours
=======
>>>>>>> theirs
c
1
(the same in debug and release; Merge("a\n", "", "") likewise gives an empty conflict instead of "")

Suggested fix: In Refine, check for two empty sides (or more simply for mine == others) before the one-side-empty check, and add a TAKEN chunk then. Add both-sides-delete cases (one line, a run, the whole text) to text_diff_git.fb with git's results.

Skeptic: I ran the program in verify-tools/t1.fb. Debug and release both print a, then an empty <<<<<<< ours / ======= / >>>>>>> theirs block, then c and 1. Merge("a\n","","") also gives an empty conflict and 1. git merge-file -p on the same files gives "a\nc\n" and "" with rc 0. The cause is diff.fb:428: Refine checks `mine.Count == 0 || others.Count == 0` before the `changes.Count == 0` TAKEN check, so two identical empty sides become a CONFLICT. That contradicts the Merge doc's "what they both made of them" and the git merge-file promise. No test covers it.

## S7-compiler-1 (medium): A literal pattern in a Global or Static Field initializer isn't checked when the flags are left to their default

Status: fixed in c277e53 (a literal pattern in a global or static field's value is checked with regex's default flags too), with a sema test of a Global's Regex, a Static Field's, and a Global's Regex.Parse, each with the flags left out: the check makes sure Regex's defaults are checked before it reads them, as run_at_compile_time does

Where: compiler/src/sema.cpp:19763

check_pattern_argument reads Regex's flags from `symbol(function.parameters[1]).value` when the call leaves them out. run() calls initialize_globals() before check_defaults(), so while Globals and Static Fields are being initialized, that default isn't known yet. `bits` is then null and the check returns without reporting anything. Regex("[a-z") and Regex.Parse("x**") in a Global, and Regex(r"(\d+") in a Static Field, all compile, and the program panics when it starts. A precompiled Global Regex is the most common place for a pattern, so the done-when ('a bad literal pattern is a compile error at it') and stdlib.md ('The compiler checks any pattern it knows') both fail there. The String shortcuts are checked in a Global, because they take no flags. With an explicit flags argument, Global D = Regex("(", RegexFlags.NONE), the error is reported. No test puts a pattern in a Global or Static Field.

```
Import std.core
Import std.text.regex

Global Words = Regex("[a-z")

Class Lexer
    Static Field Number: Regex = Regex(r"(\d+")
End Class

Global Parsed = Try? Regex.Parse("x**")

Print(Words.Pattern)
Print(Lexer.Number.Pattern)
Print(Parsed == Null)
```

Expected: Three compile errors: f1.fb:4:26 "the pattern isn't valid: missing terminating ] for character class", f1.fb:7:46 "the pattern isn't valid: missing closing parenthesis", and f1.fb:10:36 "the pattern isn't valid: quantifier does not follow a repeatable item".

Actual: It compiles ("Finished debug build"), then at run time: panic: the pattern isn't valid: missing terminating ] for character class, at its byte 4 at f1.fb:4:16 (exit code 101). With line 4 fixed, it panics at f1.fb:7:34 instead.

Suggested fix: When the flags argument is left out, make sure the defaults are checked before reading them, as run_at_compile_time does. If `arguments.size() < 2 || !arguments[1]` and `function.declaration` is set, call `check_defaults(*function.declaration)` before reading `symbol(function.parameters[1]).value`. Add sema tests for a Global, a Static Field, and a Global Regex.Parse.

Skeptic: Ran f1.fb from the finding. Debug says "Finished debug build", then the program panics with "the pattern isn't valid: missing terminating ] for character class, at its byte 4 at f1.fb:4:16" (exit 101). f4.fb holds just the Static Field Regex(r"(\d+") and the Global Try? Regex.Parse("x**"). It also compiles in both debug and --release and panics at f4.fb:5:34. With explicit flags, Global D = Regex("(", RegexFlags.NONE) gives the compile error f2.fb:4:20. In a Local with default flags the error is reported too (a4.fb). Code: run() calls initialize_globals() (sema.cpp:419) before each_item(check_defaults) (sema.cpp:420). When the flags are left out, check_pattern_argument reads symbol(function.parameters[1]).value, gets nullopt, and returns quietly. run_at_compile_time works around the same ordering by calling check_defaults(*declared.declaration) first (around sema.cpp:19391). This breaks the milestones row's done-when ('a bad literal pattern is a compile error at it') and stdlib.md ('The compiler checks any pattern it knows'), since the default flags are a constant.

## S7-compiler-2 (medium): An untyped lambda counts as fitting an overload even when its body doesn't check as that Func, so valid calls are reported as ambiguous

Status: fixed in ab7b013 (an untyped lambda fits the overload whose func it checks as without errors over one it checks as with them), with sema tests of lambdas whose bodies check only as one overload's Func, by what they give and by what their parameter has, one with errors as both, which is still ambiguous, and one with only the one overload, whose body's error is reported: such a lambda fits a Func it has errors as ANY, below every other fit

Where: compiler/src/sema.cpp:21166

match() treats an untyped lambda as EXACT whenever `check(argument.value, type) == type`. The lambda check (check_expr for lambda_expr_t) always returns a Func built from the expected parameter types and the expected return type, whatever errors its body has. So every candidate with the right number of parameters fits. In H(Function(x) => "a"), the lambda can't be a Func(Int): Int, because its body gives a String where an Int is wanted. Spec 05 says it should fit only those whose parameter is "a Func it can be", and the lane's own rule is "a Func it checks as". The same happens with a body that only type-checks for one overload, such as G(Function(x) => x.Length) with G(Func(Int): Int) and G(Func(String): Int).

```
Import std.core

Function H(f: Func(Int): Int): String => "Int"
Function H(f: Func(Int): String): String => "String"

Print(H(Function(x) => "a"))
```

Expected: String

Actual: l3.fb:6:7: error: the call to 'H' is ambiguous, with notes 'could be this' at both overloads (lines 3 and 4).

Suggested fix: In match(), for an untyped lambda, compare error_count() before and after `check(argument.value, type)` (inside rank's quiet scratch bag). Rank a candidate whose check added errors below the error-free ones, for example as NONE when another candidate fits error-free. When none fits error-free, keep today's behavior, so that the body's own error is still reported for the one that fits by shape. Add sema tests for the return-type case and the body-member case.

Skeptic: l3.fb, H(Function(x) => "a") with H(Func(Int): Int) and H(Func(Int): String), gives "error: the call to 'H' is ambiguous" with both overloads noted. l3b.fb, G(Function(x) => x.Length) with G(Func(Int): Int) and G(Func(String): Int), is also reported ambiguous, though Int has no Length. Code: the lambda's check_expr (sema.cpp:21831) builds its parameters from the expected Func and its result from the expected return type. It returns m_analysis.types.function(types, ...) whatever errors check_lambda_body found. So match()'s new branch (`made == type` => EXACT) treats every candidate with the right arity as a fit. Spec 05's new sentence says the lambda fits only overloads whose parameter is 'a Func it can be', and the code's own comment says 'a Func it checks as'. A lambda whose body doesn't type-check as Func(Int): Int can't be one.

## S7-regex-1 (medium): MatchLimit bounds each start position's try, not the search, so a classic quadratic ReDoS hangs instead of throwing

Status: fixed in 9f26723 (a regex's matchlimit counts a search's steps over every place it tries, so a quadratic search throws instead of running on), with text_regex.fb's \s+$ over 3000 spaces and an x under a MatchLimit of 100000: a callout before each of the pattern's items (PCRE2_AUTO_CALLOUT) counts a search's steps over every place it tries, since PCRE2's own count starts again at each place, and PCRE2's own limit stays for each place; the compiler's check compiles with the same options. In an -O2 PCRE2, a search that tries many places runs about 1.5 to 2 times slower for it, and one PCRE2 skips through as before

Where: runtime/src/regex.cpp:197

fb_regex_match passes MatchLimit to pcre2_set_match_limit, but pcre2_match sets match_call_count back to 0 at each start position of its bumpalong (pcre2_match.c:7946 in the bundled 10.49), so the limit only bounds one start's try. An unanchored search can take MatchLimit times the text's length in steps and never throw. docs/decisions.md says 'a pathological pattern throws instead of hanging', docs/stdlib.md:292 says 'Each match takes at most MatchLimit steps', and regex.fb's header says it 'throws a RegexError instead of running on'. Trailing whitespace (\s+$) is the textbook ReDoS: it takes about n*n/2 steps over n spaces, yet it never throws. In a debug build, with MatchLimit = 100000 and 60000 spaces, it runs 21 s (about 1.8 billion steps) and returns False. FindAll, Replace, Split, and the shortcuts behave the same way, since each goes through Next.

```
Import std.core
Import std.text
Import std.text.regex

// Trailing whitespace, a well-known ReDoS: the try at each start backtracks over the spaces
// after it, about 1.25 billion steps in all, far past the 10 million of MatchLimit.
Local text = " ".Repeated(50000) + "x"
Try
    Print(Try Regex(r"\s+$").IsMatch(text))
Catch e As RegexError
    Print(e.Kind)
End Try
```

Expected: MATCH_LIMIT, printed at once (the search takes over 100 times the 10 million steps)

Actual: False, after about 10.8 s in a release build (fb run --release); about 21 s in debug with 60000 spaces and MatchLimit 100000

Suggested fix: Make the budget cover the whole call. One way is to count steps across start positions: drive the start positions in fb_regex_match with anchored tries, or count through a callout, and throw once the total passes MatchLimit. If the per-start behavior stays, docs/stdlib.md, decisions.md, and the MatchLimit and header comments in regex.fb must say the limit applies to each start position, so a search can take MatchLimit times the text's length. Add a test with an unanchored quadratic pattern.

Skeptic: Ran r1.fb, the finding's program: `fb run --release` printed False after about 13 s wall time, with no RegexError. Ran r1b.fb in debug: Regex(r"\s+$") with MatchLimit = 10000 over 3000 spaces + "x" (about 4.5M steps in total) printed False. In the same program the anchored ^(a+)+$ printed MATCH_LIMIT, so only a single start position's try is bounded. In the bundled _deps/pcre2-src/src/pcre2_match.c, line 873 checks `match_call_count++ >= match_limit` and line 7946 sets `mb->match_call_count = 0` before each start position's match() call. runtime/src/regex.cpp:197 passes MatchLimit to pcre2_set_match_limit with nothing that counts across start positions. This contradicts docs/stdlib.md:292 ("Each match takes at most MatchLimit steps"), regex.fb:234's MatchLimit comment ("The most steps one match takes"), and decisions.md:568 ("a pathological pattern throws instead of hanging"). An unanchored quadratic pattern can take MatchLimit times the text's length.

## S7-regex-2 (medium): After an empty match at the \r of \r\n, FindAll/Replace/Split skip the \n even when the pattern matches \n itself, losing or shortening matches

Status: fixed in 02be88e (after an empty match at a \r\n, a pattern with a \r or \n of its own looks again from the \n, as pcre2 and python do), with text_regex.fb's \n* over "a\r\n\nb", \n? replacing and splitting around "\r\n", and $ and x* over "\r\n", which still step over the whole of it: a Regex keeps PCRE2_INFO_HASCRORLF, from fb_regex_has_cr_or_lf

Where: std/text/regex.fb:529

After an empty match at position p, when no non-empty match starts at p, All moves on with CharAfter. CharAfter always steps over a whole "\r\n" when p is at a \r. That is right for patterns like $ or ^ under MULTILINE. For a pattern that matches CR or LF explicitly (\n*, \n?), though, it starts the next search past the \n, so a match that begins at that \n is lost or found shorter. Python, and PCRE2's own pcre2test, step over CRLF only when the pattern has no explicit CR/LF (PCRE2_INFO_HASCRORLF). Here, \n* over "a\r\n\nb" finds "\n" at 3-4 instead of "\n\n" at 2-4. Replace leaves the \n of a CRLF in place, and Split returns one part too few.

```
Import std.core
Import std.text
Import std.text.regex

// After an empty match at a "\r" before "\n", the next look starts past the "\n", so a match
// that starts at the "\n" is lost, though the pattern matches "\n" itself.
For found In Try Regex("\n*").FindAll("a\r\n\nb")
    Print(found.Start, found.Finish)
End For
Print(Try Regex("\n?").Replace("a\r\nb", "-").Contains("\n"))
Print(Try Regex("\n?").Split("x\r\ny").Count)
```

Expected: 0 0
1 1
2 4
4 4
5 5
False
6
(Python's re gives [(0,0),(1,1),(2,4),(4,4),(5,5)], '-a-\r--b-', and 6 parts)

Actual: 0 0
1 1
3 4
4 4
5 5
True
5

Suggested fix: Step over a whole "\r\n" only when the pattern has no explicit CR or LF. Expose PCRE2_INFO_HASCRORLF through a new fb_regex_ function, keep the answer on the Regex, and have All step a single character when it is set. Add these cases to text_regex.fb.

Skeptic: Ran r2.fb, the finding's program. It printed 0 0 / 1 1 / 3 4 / 4 4 / 5 5, then True, then 5, against Python's [(0,0),(1,1),(2,4),(4,4),(5,5)], '-a-\r--b-', and 6 parts (checked with python -I). Regex("\n*").Find("a\r\n\nb", 2) gives 2 4, so a non-empty match at 2 exists, but FindAll reports 3 4, which is not the leftmost match. [^\r]* over "a\r\nb" also gives 3 4 where it should give 2 4. Cause: regex.fb:529 CharAfter always steps over a whole \r\n after an empty match at the \r. PCRE2's own bumpalong (pcre2_match.c) skips the LF only when (re->flags & PCRE2_HASCRORLF) == 0, that is, when the pattern has no explicit CR or LF.

## S7-tools-2 (medium): Merge misses joining two conflicts when the lines between them span two SAME chunks

Status: fixed in 3e834f1 (a merge's kept lines next to each other are one run, so the conflicts around them join as git's do), with text_diff_git.fb's two merges with two runs of kept lines between their conflicts, against git merge-file -p

Where: std/text/diff.fb:459

Simplified joins a CONFLICT to the one before only when the chunk just before it is a single SAME. The lines between two conflicts are often two SAME chunks in a row: Refine's trailing common lines of one both-changed region, then the main loop's base lines before the next region (or a base SAME, then the next Refine's leading common lines). In that case simplified[Count-2] is a SAME, not a CONFLICT, so no join happens, even with 3 lines or fewer between. The docs say "two with at most 3 lines between them ... are one", and git merge-file joins these cases. Random differential runs against git hit it repeatedly; cases 72 and 268 of my noeol run are this bug.

```
// Two conflicts with 2 lines between them, one that both sides made of a base line and one that
// neither changed: git merge-file -p makes them one conflict.
Import std.core
Import std.text.diff

Local merged = Diff.Merge("1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\nA\nc\n4\nD\n6\n7\n8\n9\n",
                          "1\nB\nc\n4\nE\n6\n7\n8\n9\n")
Write(merged.Text)
Print(merged.Conflicts)
```

Expected: 1
<<<<<<< ours
A
c
4
D
=======
B
c
4
E
>>>>>>> theirs
6
7
8
9
1   (what git merge-file -p -L ours -L base -L theirs gives)

Actual: 1
<<<<<<< ours
A
=======
B
>>>>>>> theirs
c
4
<<<<<<< ours
D
=======
E
>>>>>>> theirs
6
7
8
9
2

Suggested fix: Before simplifying, combine adjacent SAME chunks into one (or have Simplified count the kept lines across every SAME chunk since the last CONFLICT). Add this case and its git result to text_diff_git.fb.

Skeptic: t2.fb prints two conflicts, A/B and D/E with "c\n4\n" between them, and Conflicts is 2 in debug and release. git merge-file -p gives one conflict holding A c 4 D / B c 4 E, rc 1. At diff.fb:456-460, Simplified only joins when simplified[-1] is SAME and simplified[-2] is CONFLICT. Here the chunks are CONFLICT, SAME[c] (Refine's trailing common line), SAME[4] (base), CONFLICT, so [-2] is a SAME and nothing is joined. That contradicts stdlib.md's "two with at most 3 lines between them ... are one".

## S7-tools-3 (medium): Myers' raw path isn't compacted, so Unified and Merge differ from diff -u, git diff, and git merge-file, with spurious conflicts

Status: fixed in ededd79 (diff searches in linear space as git's xdiff does, and slides each run of changes as git does, so its diffs and merges are git's), with text_diff_git.fb's unified diffs and a merge past repeated and blank lines, against git, and text_diff.fb's Myers example, which is git's and diff -u's now. Script goes as git's xdiff does: past the lines both start and end with, a line with no equal in the other text is changed without a search, xdl_split's linear-space Myers marks the rest, and xdl_change_compact, without the indent heuristic, slides each run of changes. 2800 random small diffs and merges, against git diff --no-indent-heuristic and git merge-file, gave git's result in each

Where: std/text/diff.fb:225

Script returns the greedy forward Myers path as it is. When a change could go in more than one place (repeated or blank lines), it often splits one change into two groups with a kept line between them. GNU diff (shift_boundaries) and git (xdl_change_compact) slide each group as far as the lines allow and merge groups that become adjacent. The docs promise Unified gives "what diff -u and git diff write" and Merge gives "what git merge-file makes", and the done-when says "diffs and merges match known results". In random tests, Unified differed from both diff -u and git diff --no-indent-heuristic in 21 of 300 small texts, and in 274 of the 961 pairs of texts up to 4 lines over two line values. Every difference was another minimal diff, so each diff is valid. Merge, though, then sees changes touch where git sees none: about 3 to 4% of random small merges give a conflict git doesn't, or a different one. Replacing a line next to blank lines is enough to trigger it.

```
// Ours makes the first line empty, theirs inserts T before the last: git merge-file merges them
// cleanly, "\n\n\nT\nx\n" with no conflict.
Import std.core
Import std.text.diff

Local merged = Diff.Merge("b0\n\n\nx\n", "\n\n\nx\n", "b0\n\n\nT\nx\n")
Write(merged.Text)
Print(merged.Conflicts)
Print("--")
Write(Diff.Unified("b0\n\n\nx\n", "\n\n\nx\n", context: 0))
Print("--")
Write(Diff.Unified("a\n", "b\na\na\n"))
```

Expected: 


T
x
0
--
--- a
+++ b
@@ -1 +1 @@
-b0
+
--
--- a
+++ b
@@ -1 +1,3 @@
+b
+a
 a
(git merge-file, diff -U0 and diff -u, and git diff with or without --no-indent-heuristic all agree)

Actual: 

<<<<<<< ours

=======
T
>>>>>>> theirs
x
1
--
--- a
+++ b
@@ -1 +0,0 @@
-b0
@@ -3,0 +3 @@
+
--
--- a
+++ b
@@ -1 +1,3 @@
+b
 a
+a

Suggested fix: After Script, compact the edit script as git's xdl_change_compact does without the indent heuristic: slide each run of deletions or insertions up while the line before it equals its last line, then down as far as it goes, merging runs that meet. Use it for Lines, Unified, and Merge's diffs, Refine's included. Add tests with repeated and blank lines against diff -u and git merge-file.

Skeptic: t3.fb reproduces all three outputs exactly. Merge gives a conflict (empty line vs T) and 1 where git merge-file gives "\n\n\nT\nx\n" with rc 0. Unified -U0 gives two hunks, '@@ -1 +0,0 @@ -b0' and '@@ -3,0 +3 @@ +', where diff -U0 and git diff -U0 both give '@@ -1 +1 @@ -b0 +'. Unified for a -> b,a,a gives '+b, a, +a' where diff -u, git diff, and git diff --no-indent-heuristic all give '+b +a a'. Script (diff.fb:225-300) returns the raw Myers path and never compacts or slides change groups. That breaks stdlib.md's "what diff -u and git diff write" and "what git merge-file makes", and the merge result is a spurious conflict, not just a different minimal diff.

## S7-tools-4 (medium): Humanize.Plural lowercases a word with capitals inside it

Status: fixed in 22bd1c5 (a plural keeps its word's capitals, as gameobjects does, and one in capitals has its ending in them), with text_humanize.fb's iPhone, GameObject, McDonald, "enemy SpaceShip", CITY, City, BoundingBox, Fish, and ChILD: the rules read the word lowercased and build on the word as given; an irregular plural keeps a first capital or all of them, as before

Where: std/text/humanize.fb:19

Plural lowercases the last word, pluralizes that, and Cased restores only all capitals or a first capital. Every other capital in the word is lost, so camelCase and PascalCase words, brand names, and type names come back wrong. For a game language, Plural(n, "GameObject") is a likely call. The doc comment says only that a word in capitals stays in them and one with a capital first keeps it. Lowercasing the rest of the word is a wrong result, not a documented choice.

```
Import std.core
Import std.text.humanize

Print(Humanize.Plural("iPhone"), Humanize.Plural("GameObject"), Humanize.Plural(3, "PlayStation"),
      Humanize.Plural("McDonald"), Humanize.Plural("enemy SpaceShip"))
```

Expected: iPhones GameObjects 3 PlayStations McDonalds enemy SpaceShips

Actual: iphones Gameobjects 3 Playstations Mcdonalds enemy Spaceships   (debug and release)

Suggested fix: Match lowercased for the lookups (SAME, IRREGULAR, the rule's tests), but build a regular plural from the original word: its stem as given plus the ending ("s", "es", or "y" -> "ies"), the ending in capitals only when the word is all capitals. Keep Cased for the irregular and same-plural words. Add mixed-case words to text_humanize.fb.

Skeptic: t4.fb prints "iphones Gameobjects 3 Playstations Mcdonalds enemy Spaceships" in debug and release. Plural (humanize.fb:19) pluralizes last.ToLower(), and Cased (145-150) only restores all capitals or capitalizes the first letter while lowercasing the rest. So any capital inside a word is lost, and iPhone loses nothing only because its first letter is lowercase. The doc only promises that capitals are kept, so silently lowercasing GameObject is a wrong result.

## S7-tools-5 (medium): Diff.Unified overflows Int on a large context: a panic inside std in debug, wrong hunks in release

Status: fixed in 7db1b92 (a unified diff takes any context up to the most an int holds, as diff -u and git diff do), with text_diff_git.fb's contexts of 1000000, 2^30, and Int's most, against git diff -U, in debug and release

Where: std/text/diff.fb:87

Int is 32-bit. The hunk-joining test computes `2 * context`, and the hunk end computes `changes[last].Finish + context`. For any context of 2^30 or more, the first overflows. Debug panics with an integer overflow, reported at the caller through @PanicsAtCaller even though the caller passed a valid, non-negative context. Release wraps instead, which gives negative ranges, splits one hunk into two, and repeats lines. A large -U to mean "the whole file" is common (git diff -U<huge>). Debug and release give different results.

```
// A context past the texts' length shows all of them in one hunk, as diff -U and git diff -U do.
Import std.core
Import std.text.diff

Local before = "1\n2\n3\n4\n5\n6\n7\n8\n9\n"
Local after = "1\n2\nthree\n4\n5\n6\n7\neight\n9\n"
Write(Diff.Unified(before, after, context: 1000000))
Print("--")
Write(Diff.Unified(before, after, context: 2147483647))
Print("--")
Write(Diff.Unified(before, after, context: 1073741824))
```

Expected: The same single hunk three times, in debug and release: "--- a\n+++ b\n@@ -1,9 +1,9 @@\n 1\n 2\n-3\n+three\n 4\n 5\n 6\n 7\n-8\n+eight\n 9\n", with "--" between.

Actual: Debug: the first hunk, "--", then "panic: integer overflow in '*' at unified_ctx.fb:9:7" (exit 101). Release: the first hunk is right. For 2147483647 it prints "@@ -1,-2147483646 +1,-2147483646 @@" over lines 1-3 and "@@ -1,-2147483641 +1,-2147483641 @@" over lines 1-8. For 1073741824 it prints two "@@ -1,9 +1,9 @@" hunks, each with all 9 lines, so every line shows twice.

Suggested fix: Clamp first: `Local reach = Min(context, from.Count + to.Count)`, and use it in the join test and the hunk bounds. Add a test with context Int's maximum.

Skeptic: t5.fb: in debug, the first hunk is right, then 'panic: integer overflow in '*' at t5.fb:8:7', exit 101, blamed on the caller through @PanicsAtCaller. In release, context 2147483647 gives '@@ -1,-2147483646 +1,-2147483646 @@' and '@@ -1,-2147483641 ...', and 1073741824 gives two full '@@ -1,9 +1,9 @@' hunks, so lines repeat. diff -U2147483647 and git diff -U2147483647 both give a single '@@ -1,9 +1,9 @@'. The cause is diff.fb:87 `2 * context` and :92 `changes[last].Finish + context`, which overflow Int (32-bit) on a valid non-negative context.

## S7-tools-6 (medium): Diff's Myers keeps every round's diagonals: memory grows with the square of the edits

Status: fixed in ededd79 (diff searches in linear space as git's xdiff does, and slides each run of changes as git does, so its diffs and merges are git's), with text_diff.fb's two texts of 50000 lines alike only in every 100th, which the old Script didn't finish: Myers' search keeps a row of diagonals each way, as xdl_split does, and a line with no equal in the other text isn't searched at all

Where: std/text/diff.fb:232

Script saves 2d+3 Ints for every round d so it can trace back, about D^2 Ints in all for D edits. That's fine for a few changes but not for texts that are mostly different. Two 4000-line texts with no line in common peaked at 246 MB. Two 10000-line ones peaked at 1.85 GB in a release build, 3.7 s. 20000 lines would need about 7 GB. Regenerated or minified files, or a file replaced wholesale, are ordinary diff inputs. git and GNU diff use linear space for the same inputs. The test's own comment ("a thousand changes take a few megabytes") is also low: 2000 edits are about 4 million Ints. Chars has the same cost on long strings.

```
// Two texts of N lines with no line in common: Myers' rounds keep d * d entries, with d = 2N.
Import std.core
Import std.text.diff
Import std.text.string

Const N = 10000
Local a = ""
Local b = ""
For i = 0 Until N
    a += "old {i}\n"
    b += "new {i}\n"
End For

Local runs = Diff.Lines(a, b)
Print(runs.Count, runs[0].Text.Count("\n"), runs[1].Text.Count("\n"))
```

Expected: 2 10000 10000, in memory a small multiple of the texts' size (git diff --no-index on the same two files needs a few MB)

Actual: 2 10000 10000, but the process's peak working set is 1851 MB (246 MB at N = 4000), so it grows as N squared

Suggested fix: Use Myers' linear-space refinement (find the middle snake, divide, and recurse), as git's xdl_split and GNU diff do, or cap the stored trace and fall back to it. Add a large dissimilar-input test that checks memory stays bounded.

Skeptic: I built the N-line no-common-lines program in release (t6_N.fb) and sampled PeakWorkingSet64. N=2000 peaked at about 78 MB (0.16 s), N=4000 at about 275 MB (0.50 s), and N=8000 at about 1163 MB (1.81 s). Every run printed the right '2 N N'. Memory grows as N^2 because Script (diff.fb:232-241) keeps a 2d+3-Int array for every round d, so about D^2 in all. text_diff.fb:43's 'a thousand changes take a few megabytes' understates it too: 2000 edits are about 4M Ints. A std diff that needs gigabytes for ordinary large dissimilar inputs is a defect.

## S7-tools-7 (medium): Diff.Merge writes \n conflict markers into \r\n text

Status: fixed in b00d1c1 (a merge's conflict markers end in \r\n where the texts' lines do, as git's do), with text_diff_git.fb's five merges of "\r\n" text, against git merge-file -p: the markers' break is is_cr_needed's, and a side without a last line break gets that one

Where: std/text/diff.fb:189

The module keeps \r\n line breaks, but the markers are always "<<<<<<< ours\n", "=======\n", and ">>>>>>> theirs\n". Ended also adds a bare \n to a side that ends without a line break. git merge-file picks \r\n for markers and added breaks when the texts use \r\n (is_cr_needed). So a merged Windows file comes out with mixed line endings, which differs from the git result the docs promise.

```
// git merge-file writes its markers with \r\n when the texts' lines end with \r\n.
Import std.core
Import std.text.diff
Import std.text.string

Local merged = Diff.Merge("a\r\nb\r\nc\r\n", "a\r\nB\r\nc\r\n", "a\r\nX\r\nc\r\n")
Print(merged.Text.Replace("\r", "\\r").Replace("\n", "\\n"))
Local ragged = Diff.Merge("a\r\nb", "a\r\nB", "a\r\nX")
Print(ragged.Text.Replace("\r", "\\r").Replace("\n", "\\n"))
```

Expected: a\r\n<<<<<<< ours\r\nB\r\n=======\r\nX\r\n>>>>>>> theirs\r\nc\r\n
a\r\n<<<<<<< ours\r\nB\r\n=======\r\nX\r\n>>>>>>> theirs\r\n
(git merge-file -p's bytes for the same files)

Actual: a\r\n<<<<<<< ours\nB\r\n=======\nX\r\n>>>>>>> theirs\nc\r\n
a\r\n<<<<<<< ours\nB\n=======\nX\n>>>>>>> theirs\n

Suggested fix: Choose the marker break as git does: \r\n when the line before the conflict on each side (or the first line) ends in \r\n and the base's first line does too, otherwise \n. Use the same break in Ended. Add a CRLF merge to text_diff_git.fb.

Skeptic: t7.fb shows a CRLF merge with bare \n after each marker line, and Ended adds a bare \n to sides without a break in the ragged case. git merge-file -p on the same bytes (checked with od -c) writes '<<<<<<< ours\r\n', '=======\r\n', and '>>>>>>> theirs\r\n', and for the ragged input 'B\r\n' and 'X\r\n'. diff.fb:189-190 hard-codes "\n" in the markers, and Ended (517-520) appends "\n". So the output has mixed line endings and differs from the git merge-file result stdlib.md promises.

## S7-tools-9 (medium): WordWrap breaks at a no-break space and replaces it with an ASCII space

Status: fixed in db172b9 (wordwrap doesn't break at a no-break space, and keeps the spaces between words that stay on a line), with text_layout.fb's U+00A0, U+202F, and U+2007 cases, and spaces between words that stay on a line, and the ones at a break, which go

Where: std/text/layout.fb:34

WordWrap rebuilds each line from SplitWhitespace, which splits at everything IsWhitespace accepts, U+00A0 and U+202F included, and joins words with " ". A no-break space therefore becomes a break point, and even when the line fits it comes back as an ordinary space. That changes the text: "100 km", French "« »" spacing, and the like. The same rebuild also turns every run of whitespace in a line (two spaces, a tab) into one space where no break is made, although the doc comment says only that the spaces at a break go.

```
// A no-break space, U+00A0, is neither a place to break nor a space to drop: "100\u{A0}km" stays
// whole and keeps its U+00A0.
Import std.core
Import std.text
Import std.text.layout

Print("100\u{A0}km away".WordWrap(80) == "100\u{A0}km away")
Print("[" + "go 100\u{A0}km".WordWrap(7).Replace("\u{A0}", "<nbsp>") + "]")
```

Expected: True
[go
100<nbsp>km]

Actual: False
[go 100
km]

Suggested fix: Split words only at breakable whitespace (leave out U+00A0, U+2007, U+202F, and U+2060), so a no-break space stays inside its word. Consider keeping a line's own spacing between words that stay on one line. Add these to text_layout.fb.

Skeptic: t9.fb: "100\u{A0}km away".WordWrap(80) == original is False, and "go 100\u{A0}km".WordWrap(7) gives "[go 100\nkm]". That is a break at the no-break space, which is then dropped. "a  b\tc".WordWrap(80) also comes back "a b c". layout.fb:34 rebuilds lines from SplitWhitespace, which splits at every Unicode whitespace, NBSP included, and joins with " ". A no-break space is by definition not a break opportunity (Python's textwrap only splits on ASCII whitespace), so this changes the text.

## S7-compiler-4 (low): A lambda without a return type doesn't fit an overload whose Func returns another type, though a single function takes it

Status: fixed in 3a42f73 (a lambda without a return type fits an overload whose func gives the type it would take, as one function's does), with sema tests of Function() => 1 and Function(n: Int) => 1 passed to overloads of a Func that gives a Double and a String, and of a lambda whose own type, Func(Int): Int, fits better than a Func(Int): Int64 it can be

Where: compiler/src/sema.cpp:21180

Spec 05: "A lambda returns what its `: Type` says, or else what the Func expected says." One(Function() => 1) with One(f: Func(): Double) compiles, because the lambda takes Double from the Func expected. When the function is overloaded, a lambda with no untyped parameters (zero parameters, or all of them typed) isn't covered by the new branch. It is matched by its natural type, Func(): Int32, which neither equals nor converts to Func(): Double, so no overload fits. That contradicts spec 05's new sentence, that passed to overloads the lambda fits those whose parameter is a Func it can be. This gap predates step 7.

```
Import std.core

Function One(f: Func(): Double): String => "{f()}"

Function Two(f: Func(): Double): String => "{f()}"
Function Two(f: String): String => f

Print(One(Function() => 1))
Print(Two(Function() => 1))
```

Expected: 1
1

Actual: l5.fb:9:7: error: no overload of 'Two' takes (Func(): Int32)

Suggested fix: Give the same treatment to any lambda without a `: Type` whose expected Func would decide its return type: in match(), check it against the candidate's Func type, not just against NONE. That is, widen is_untyped_lambda's test in match() to 'no return type, or a parameter without a type'. Add a sema test.

Skeptic: l5.fb: One(Function() => 1), not overloaded, prints 1. Two(Function() => 1) gives "no overload of 'Two' takes (Func(): Int32)". The inconsistency comes from step 7's own branch. l5b.fb, Two(Function(x) => 1) with Two(Func(Int): Double) and Two(String), compiles and prints 1, because the untyped-lambda branch checks it against the candidate's Func, so it returns Double. l5c.fb, the same call with the parameter typed as Function(x: Int) => 1, fails with "no overload of 'Two' takes (Func(Int32): Int32)". So writing the parameter's own type breaks the call. Spec 05 says a lambda without a ': Type' returns what the Func expected says. The new overload sentence says the lambda fits those overloads whose parameter is a Func it can be, and Function(x: Int) => 1 can be a Func(Int): Double. is_untyped_lambda only tests for parameters without a type, so a lambda whose return type comes from the expected Func falls back to its natural type in match().

## S7-compiler-5 (low): The pattern check also runs on a literal the lexer already rejected, adding a misleading second error

Status: fixed in 3da42cb (a literal pattern the lexer reported isn't checked as a pattern too), with a sema test of "\Qa(\E" and of a literal without its closing quote, and pattern_tests' cases of literal_is_whole, which tells them

Where: compiler/src/sema.cpp:19793

A bad escape in a regular string decodes to U+FFFD, and an unterminated string still yields a string_expr_t. check_pattern_argument compiles the result anyway and reports PCRE2's error for text the user never wrote. "\Qa(\E" (PCRE's \Q...\E written in a regular string) gets the two correct 'unknown escape sequence' errors, plus 'the pattern isn't valid: missing closing parenthesis'. That last error is wrong for the intended pattern "\\Qa(\\E", which is valid. Likewise Regex("ab( with no closing quote gets 'unterminated string' and also 'missing closing parenthesis'. Elsewhere this step avoids cascades: the Try change and choose_overload's check_alone.

```
Import std.core
Import std.text.regex

Local a = Regex("\Qa(\E")
Print("ok")
```

Expected: Two errors: a4.fb:4:18 and a4.fb:4:22, both "unknown escape sequence".

Actual: Three errors: the two "unknown escape sequence" errors, plus a4.fb:4:24: error: the pattern isn't valid: missing closing parenthesis

Suggested fix: Skip the pattern check for a literal with a lexical error. For example, literal_place (or a sibling) can report that a decode_escape in the literal had a non-empty `error`, or that the source doesn't end in its closing quote, and check_pattern_argument then returns without an error. Add a sema test for both cases.

Skeptic: a4.fb, Local a = Regex("\Qa(\E") with single backslashes, gives three errors. Two are 'unknown escape sequence', at 4:18 and 4:22. The third is 'a4.fb:4:24: error: the pattern isn't valid: missing closing parenthesis'. The intended pattern, written correctly in a7.fb as Regex("\\Qa(\\E"), compiles, and IsMatch("a(") prints True. So the third error describes text the user never wrote and is a pure follow-on from the lexer's error. The unterminated case, a5.fb with Regex("ab(, adds 'the pattern isn't valid' after 'unterminated string'. The project avoids such follow-on errors: decisions.md says 'a missing Import gives one error, not a cascade', and this step's own Try and check_alone changes do the same. check_pattern_argument runs on any string_expr_t value without checking for a lexical error in it.

## S7-regex-3 (low): Regex.Escape leaves PCRE2's non-ASCII pattern whitespace bare, so under EXTENDED its pattern doesn't match the text

Status: fixed in 163f343 (escape puts a backslash before the whitespace beyond ascii that extended skips too), with text_regex.fb's EXTENDED Regex of the Escape of U+0085, U+200E, U+200F, U+2028, U+2029, U+00A0, a space, and a #; its comment says text

Where: std/text/regex.fb:227

Escape adds a backslash only before ASCII characters other than letters, digits, and _. In UTF mode, PCRE2_EXTENDED also skips Unicode's Pattern_White_Space: U+0085, U+200E, U+200F, U+2028, and U+2029. So Regex(Regex.Escape(text), RegexFlags.EXTENDED) doesn't match text when text contains one of them, against Escape's promise that 'a Regex of it matches text itself'. In UTF mode PCRE2 reads a backslash before a non-ASCII character as that character, so escaping those five works: "a\\\u{2028}b" under EXTENDED matches "a\u{2028}b". Also, Escape's comment names its parameter 'pattern', but it is 'text'.

```
Import std.core
Import std.text
Import std.text.regex

// Escape's pattern matches text itself, under any flags.
For text In ["a b", "a#b", "a\u{2028}b", "a\u{85}b", "a\u{200E}b"]
    Print(Try Regex(Regex.Escape(text), RegexFlags.EXTENDED).IsMatch(text))
End For
```

Expected: True
True
True
True
True

Actual: True
True
False
False
False

Suggested fix: Also put a backslash before U+0085, U+200E, U+200F, U+2028, and U+2029, or before every character that isn't a letter, digit, or _. Change the comment's 'pattern with a backslash' to 'text with a backslash'. Add a test that runs Escape with EXTENDED.

Skeptic: Ran r3.fb. Regex(Regex.Escape(text), RegexFlags.EXTENDED).IsMatch(text) printed False for a\u{2028}b, a\u{85}b, a\u{200E}b, a\u{200F}b, and a\u{2029}b, and True for "a b", "a#b", U+00A0, and U+3000. Without EXTENDED, all of them printed True. Putting a backslash before the character ("a" + r"\" + "\u{2028}b" under EXTENDED) matches: True for U+2028 and U+0085. regex.fb:227 escapes only ASCII non-word characters, so PCRE2's non-ASCII Pattern_White_Space is skipped under EXTENDED. That breaks the comment's promise that a Regex of the result matches text itself, and Escape already escapes space and # for EXTENDED's sake. The comment at line 222 also says 'pattern with a backslash', but the parameter is text. Low, since only five unusual characters are affected.

## S7-regex-4 (low): A pattern's own (*LIMIT_MATCH/DEPTH/HEAP=n) throws with a message that says the Regex's limits (10 million steps, 64 MiB) were passed

Status: fixed in c7e580f (a search past a pattern's own (*limit_...) says so, and a depth limit isn't one of memory), with text_regex.fb's (*LIMIT_MATCH=10), (*LIMIT_DEPTH=5), and (*LIMIT_HEAP=1): fb_regex_match tells a pattern's own lower limit, from PCRE2_INFO_MATCHLIMIT, DEPTHLIMIT, and HEAPLIMIT, from the Regex's, and the Regex's own depth limit, which is its MatchLimit, is a MATCH_LIMIT

Where: std/text/regex.fb:348

PCRE2 lets a pattern lower its own limits with (*LIMIT_MATCH=n), (*LIMIT_DEPTH=n), and (*LIMIT_HEAP=n), and the runtime doesn't reject them. When one is hit, Next builds its message from the Regex's Steps or Bytes. For a match that stayed well below the Regex's limits, the message still claims 'more than the Regex's MemoryLimit of 67108864 bytes' or 'MatchLimit of 10000000 steps'. A depth limit, which is a count of backtracking levels, is also reported as MEMORY_LIMIT, with a message about bytes.

```
Import std.core
Import std.text
Import std.text.regex

Function Own(limit: String): String => "(*" + limit + ")^(a|b)*c"

// A pattern's own lower limits: the RegexError says the Regex's limits were passed, though the
// match stayed far below them.
For limit In ["LIMIT_MATCH=10", "LIMIT_DEPTH=5", "LIMIT_HEAP=1"]
    Try
        Print(Try (Try Regex.Parse(Own(limit))).IsMatch("ab".Repeated(50) + "dc"))
    Catch e As RegexError
        Print(e.Kind, e.Message)
    End Try
End For
```

Expected: Messages that don't claim the Regex's 10000000 steps or 67108864 bytes were passed. For example, they name the pattern's own limit, or say 'the match went past its limit'.

Actual: MATCH_LIMIT the match took more than the Regex's MatchLimit of 10000000 steps
MEMORY_LIMIT the match took more than the Regex's MemoryLimit of 67108864 bytes
MEMORY_LIMIT the match took more than the Regex's MemoryLimit of 67108864 bytes

Suggested fix: Have fb_regex_match return a separate code when the pattern's own limit is the lower one; pcre2_pattern_info gives PCRE2_INFO_MATCHLIMIT, DEPTHLIMIT, and HEAPLIMIT, so the runtime can compare. Then word the message for that case, or document that a pattern's (*LIMIT_...) is reported as the Regex's limit.

Skeptic: Ran r4.fb, the finding's program. It printed 'MATCH_LIMIT the match took more than the Regex's MatchLimit of 10000000 steps' for (*LIMIT_MATCH=10), and 'MEMORY_LIMIT the match took more than the Regex's MemoryLimit of 67108864 bytes' for both (*LIMIT_DEPTH=5) and (*LIMIT_HEAP=1). The same match without the pattern's own limits returns False, well under the Regex's limits, so each message is false. regex.fb:348-355 builds the message from Steps and Bytes whatever caused the error, and regex.cpp:214 maps DEPTHLIMIT to the same -2 code as HEAPLIMIT, so a depth limit comes out as a message about bytes.

## S7-regex-5 (low): docs/stdlib.md says FindAll's empty matches work 'as Python and Rust do', but Rust's differ, and FindAll follows Python's

Status: fixed in 74e63b8 (stdlib.md: findall's empty matches are python's, which rust's aren't), docs only

Where: docs/stdlib.md:297

FindAll reports an empty match right after a non-empty one: x* over "axxb" gives 0-0, 1-3, 3-3, 4-4, and Replace gives "-a--b-". That is Python 3.7+'s behavior, and text_regex.out checks it. Rust's regex crate skips an empty match that ends where the previous match ended. Its find_iter has a 'Don't accept empty matches immediately following a match' check in the old re_trait.rs, and handle_overlapping_empty_match in regex-automata. So Rust gives 0..0, 1..3, 4..4 and replace_all gives "-a-b-". The two languages disagree, so the doc's 'as Python and Rust do' is wrong.

```
Import std.core
Import std.text.regex

// docs/stdlib.md: FindAll finds an empty match once at each place, "as Python and Rust do".
For found In Try Regex("x*").FindAll("axxb")
    Print(found.Start, found.Finish)
End For
Print(Try Regex("x*").Replace("axxb", "-"))
```

Expected: The doc should name only the behavior FindAll has: Python's re gives (0,0),(1,3),(3,3),(4,4) and '-a--b-'. Rust's regex gives 0..0, 1..3, 4..4 and '-a-b-'.

Actual: 0 0
1 3
3 3
4 4
-a--b-

Suggested fix: Change line 297 to say 'as Python does' (an empty match can follow a non-empty one), or drop the comparison.

Skeptic: Ran r5.fb. FindAll of x* over "axxb" gives 0 0, 1 3, 3 3, 4 4, and Replace gives -a--b-. That is Python's result, and JavaScript's too: node gives -a--b-. Rust's regex crate (regex-automata's Searcher::advance) drops an empty match whose end equals last_match_end (handle_overlapping_empty_match), so it gives 0..0, 1..3, 4..4 and -a-b-. It has no empty match at 3. No Rust toolchain is installed here, so the Rust result is from its source logic, not from a run. docs/stdlib.md:297's 'as Python and Rust do' is therefore inaccurate for Rust. Doc-only.

## S7-regex-6 (low): No test covers MemoryLimit's 1 MiB panic or the runtime's rejection of \C

Status: fixed in 36f17d7 (tests for memorylimit's least and the runtime's refusal of \c), tests only: tests/programs/panic_regex_memory.fb, and text_regex.fb's Regex.Parse of r"a\Cb" when the program runs

Where: tests/programs/panic_regex_limit.fb:8

panic_regex_limit.fb tests only MatchLimit's check. The MemoryLimit setter's 'at least 1048576 bytes' panic, which is the 'MemoryLimit can't go below 1 MiB' decision, has no panic program. The '\C is off' decision is tested only in the compiler's check_pattern (tests/compiler/pattern_tests.cpp). No program test covers runtime/src/regex.cpp's PCRE2_NEVER_BACKSLASH_C for a pattern from Regex.Parse, so dropping that option from the runtime would fail no test. Both behaviors are right today. The program shows what the tests should pin down.

```
Import std.core
Import std.text
Import std.text.regex

Function Single(): String => r"a\Cb"

// \C is an error at run time too, not only in the compiler's check.
Try
    Print((Try Regex.Parse(Single())).Pattern)
Catch e As RegexError
    Print(e.Kind, e.Offset, e.Message)
End Try

// MemoryLimit below 1 MiB panics where it's set.
Local r = Regex("x")
r.MemoryLimit = 1048575
```

Expected: A test in tests/programs that checks: PATTERN 3 the pattern isn't valid: using \C is disabled by the application, at its byte 3
panic: MemoryLimit takes at least 1048576 bytes, not 1048575 at untested.fb:16:1

Actual: The output is correct, but no test in tests/programs checks either line.

Suggested fix: Add a panic_regex_memory.fb/.out like panic_regex_limit.fb, and add the runtime \C case (a Regex.Parse of a pattern built at run time) to text_regex.fb's Parse loop.

Skeptic: Searched tests/ for MemoryLimit, NEVER_BACKSLASH_C, and \C. MemoryLimit appears only in text_regex.fb:102-103, which sets it to exactly 1048576. No program sets it below 1 MiB, and panic_regex_limit.fb covers only MatchLimit. \C is tested only through the compiler: pattern_tests.cpp:36 and sema_tests.cpp:13054, both against compiler/src/pattern.cpp's own options. No program runs a run-time Regex.Parse of \C, so taking PCRE2_NEVER_BACKSLASH_C out of runtime/src/regex.cpp:94 would fail no test. untested.fb shows both behaviors are right today: it prints 'PATTERN 3 the pattern isn't valid: using \C is disabled by the application, at its byte 3' and then 'panic: MemoryLimit takes at least 1048576 bytes, not 1048575 at untested.fb:16:1'. It is a coverage gap for two lane decisions.

## S7-tools-10 (low): WordWrap counts a tab as 0 columns, so tab-indented lines run past the width

Status: fixed in eaee8a9 (wordwrap counts a tab to the next multiple of 8 columns, as a terminal shows it), with text_layout.fb's tab before a line's words and between them: a tab goes on to the next multiple of 8 columns from where it is

Where: std/text/layout.fb:26

Widths come from DisplayWidth, which gives 0 for a control character, a tab included. A line indented with a tab gets the full width as room, and its leading tab is put back on every line it becomes. In a terminal each of those lines is then 8 columns wider than width. The docs say WordWrap works "in a terminal's columns". Tab indentation is common in the code comments and text people wrap.

```
// A tab takes a terminal to its next multiple of 8 columns, so a line that starts with one has
// 8 columns gone before its words: no wrapped line should go past 10 columns.
Import std.core
Import std.text
Import std.text.layout

Local wrapped = "\tone two three four".WordWrap(10)
For line In wrapped.Split("\n")
    Local columns = 8 + DisplayWidth(line.TrimStart())
    Print("[" + line.Replace("\t", "\\t") + "]", columns)
End For
```

Expected: every printed column count at most 10

Actual: [\tone two] 15
[\tthree four] 18

Suggested fix: Measure the indent (and any tab) by tab stops: a tab moves to the next multiple of 8. Use that for room, or expand the tabs as Python's textwrap does. Add a tab-indented case to text_layout.fb.

Skeptic: t10.fb gives "[\tone two] 15" and "[\tthree four] 18" with width 10. Words of 7 and 10 columns fit after a leading tab, so room was 10, because DisplayWidth (unicode.fb:55-57) gives 0 for a control character and layout.fb:26 computes room = width - DisplayWidth(indent). That contradicts WordWrap's 'as a terminal shows them' and stdlib.md's 'in a terminal's columns', since a terminal advances a tab to the next multiple of 8. I put it at low because the doc comment also says 'with DisplayWidth', which partly documents the behavior.

## S7-tools-11 (low): Case styles drop combining marks and split words at them

Status: fixed in d14e1f5 (a case style keeps a word's combining marks in it), with text_humanize.fb's decomposed "café au lait", "naïve set", "caféBar", and "ÉCOLEName", and a mark first

Where: std/text/humanize.fb:177

WordsIn treats anything that isn't IsLetterOrDigit as a separator, and a combining mark (Mn/Mc) is neither. Decomposed text (NFD, as macOS file names are) therefore loses its accents, or is cut into two words at the mark. The function's comment says words are split at spaces, punctuation, and capitals, and a combining mark is none of those. Precomposed and decomposed forms of the same name give different results.

```
// "café" and "naïve" written with combining marks (NFD, as macOS file names are) keep their marks
// and stay one word, as the precomposed ones do.
Import std.core
Import std.text.humanize

Print(Humanize.SnakeCase("cafe\u{301} au lait"), Humanize.SnakeCase("caf\u{E9} au lait"))
Print(Humanize.PascalCase("nai\u{308}ve set"), Humanize.PascalCase("na\u{EF}ve set"))
```

Expected: café_au_lait café_au_lait   (the first with its U+0301 kept)
NaïveSet NaïveSet

Actual: cafe_au_lait café_au_lait
NaiVeSet NaïveSet

Suggested fix: In WordsIn, keep a mark (category Mn, Mc, or Me) that follows a letter or digit as part of the current word, or NFC-normalize the text first. Add an NFD case to text_humanize.fb.

Skeptic: t11.fb prints 'cafe_au_lait café_au_lait' and 'NaiVeSet NaïveSet'. WordsIn (humanize.fb:177) treats any non-IsLetterOrDigit Char as a separator, so a combining mark (Mn) is dropped or splits the word. The public doc says words split at spaces, punctuation, and capitals, and a mark is none of those. Decomposed identifiers are rare in case-style conversion, so I put it at low.

## S7-tools-12 (low): Glob's "**/" matches no folder even when the ** isn't a whole folder, so "a**/b" matches "ab"

Status: fixed in f81f215 (a glob's **/ can be no folder only where it's a whole one, as in bash's globstar), with text_glob.fb's a**/b, src**/main.fb, src/**/x, and **/x

Where: std/text/glob.fb:173

PiecesOf makes any "**/" a FOLDERS piece, which can match nothing, wherever it appears. Mid-segment, as in "a**/b" or "src**/x.fb", the / the pattern asks for then needs no / in the text. In bash's globstar, zsh, and gitignore, ** is "any folders, or none" only as a whole path segment. Elsewhere it's a star run and the / is literal: bash -O globstar gives nomatch for [[ ab == a**/b ]]. The header's "**/ can be no folder" is meant for the src/**/x shape.

```
// "**/" is no folder only where it's a whole folder, as in bash's globstar, zsh, and gitignore:
// "a**/b" still has a / to match, so it doesn't match "ab"; "src/**/x" matches "src/x".
Import std.core
Import std.text.glob

Print(Glob.Match("a**/b", "ab"), Glob.Match("src**/main.fb", "srcmain.fb"),
      Glob.Match("src/**/x", "src/x"))
```

Expected: False False True

Actual: True True True

Suggested fix: Make FOLDERS only when the ** starts the alternative or follows a /. Otherwise emit GLOBSTAR and a literal /. Add the case to text_glob.fb.

Skeptic: t12.fb prints 'True True True', so "a**/b" matches "ab" and "src**/main.fb" matches "srcmain.fb". In bash with globstar, a file 'ab' does not match `echo a**/b` (it prints the pattern), and [[ ab == a**/b ]] is nomatch, while src/**/x matches src/x. glob.fb:172-175 makes any '**/' a FOLDERS piece, even mid-segment, which drops a '/' the pattern asks for. The header's '"**/" can be no folder' is introduced by the src/**/*.fb example.

## S7-tools-14 (low): Merge's "no letter or digit" rule is Unicode's, but git merge-file's is ASCII's

Status: fixed in 6619a4c (a merge's letters and digits that keep conflicts apart are ascii's, as git's are), with text_diff_git.fb's four lines of "ä" between two conflicts, against git merge-file -p; stdlib.md says ASCII

Where: std/text/diff.fb:481

HasAlnum uses Char.IsLetterOrDigit, which is Unicode-wide. git's lines_contain_alnum uses its ASCII isalnum, so for git, lines of only non-ASCII letters such as "ä" have no letter and don't keep two conflicts apart. Diff.Merge is documented as "what git merge-file makes", but here it gives two conflicts where git gives one.

```
// git merge-file -p makes these one conflict: its lines_contain_alnum is ASCII's isalnum, so four
// lines of "ä" between two conflicts have no letter for it.
Import std.core
Import std.text.diff

Local merged = Diff.Merge("1\n2\nä\nä\nä\nä\n3\n4\n", "1\nA\nä\nä\nä\nä\nC\n4\n",
                          "1\nB\nä\nä\nä\nä\nD\n4\n")
Write(merged.Text)
Print(merged.Conflicts)
```

Expected: 1
<<<<<<< ours
A
ä
ä
ä
ä
C
=======
B
ä
ä
ä
ä
D
>>>>>>> theirs
4
1   (git merge-file's result)

Actual: 1
<<<<<<< ours
A
=======
B
>>>>>>> theirs
ä
ä
ä
ä
<<<<<<< ours
C
=======
D
>>>>>>> theirs
4
2

Suggested fix: Test ASCII letters and digits only, as git does, or say in docs/stdlib.md and the Merge comment that this rule is Unicode's and differs from git's there.

Skeptic: t14.fb gives two conflicts (Conflicts 2). git merge-file -p on the same files gives one conflict holding A ä ä ä ä C / B ä ä ä ä D, rc 1, because git's lines_contain_alnum uses ASCII isalnum. HasAlnum (diff.fb:481-489) uses the Unicode Char.IsLetterOrDigit. That literally fits stdlib.md's 'without a letter or digit' wording, but it contradicts the promise of 'what git merge-file makes' and the done-when's 'merges match known results'. Low: fix the code or document the difference.

## S7-tools-15 (low): Fuzzy.Match's score for a one-character pattern isn't fzf's own, as docs/stdlib.md says

Status: fixed in ebddc33 (a one-character fuzzy pattern takes its first place at a word's start, as fzf's does), with text_fuzzy.fb's "a" in "1.A aB", 32 at byte 2, as fzf gives, and three more one-character patterns

Where: std/text/fuzzy.fb:162

For a one-character pattern, fzf's FuzzyMatchV2 (algo.go, phase 2) stops at the first match whose bonus is at least bonusBoundary (`if forward && bonus >= bonusBoundary { break }`). Best always takes the highest-scoring place instead. docs/stdlib.md says Match gives "fzf's own score". I ported FuzzyMatchV2 to Python and compared 12,000 random cases. Without that shortcut, and without fzf's case-sensitive scan for the last pattern character in asciiFuzzyIndex, scores and positions matched in every case, so the DP is right. With the shortcut, one-character patterns differ, as below. (fzf itself isn't installed here; the expected value comes from its source and the port.)

```
// fzf's FuzzyMatchV2 (algo.go) takes the first match of a one-character pattern that has a
// boundary bonus and stops there: "a" in "1.A aB" scores 16 + 8 * 2 = 32 at byte 2, which is what
// fzf itself gives.
Import std.core
Import std.text.fuzzy

Local found = Fuzzy.Match("a", "1.A aB")
Print(found?.Score ?? -1, found?.Positions ?? [])
```

Expected: 32 [2]   (fzf's score and position)

Actual: 36 [4]

Suggested fix: Either mirror fzf's one-character shortcut (stop at the first match with bonus >= BONUS_BOUNDARY), or reword docs/stdlib.md and the module comment as fzf's scoring, with the best placement and without fzf's shortcuts.

Skeptic: t15.fb prints '36 [4]' in debug and release. I fetched fzf's src/algo/algo.go. In FuzzyMatchV2's phase 2, for M == 1, it sets maxScore and maxScorePos and then does `if forward && bonus >= bonusBoundary { break }`, and returns that early. For "1.A aB", 'A' at 2 follows '.', class charNonWord, so bonusBoundary is 8 and the score is 16 + 8*2 = 32. fzf stops there and gives 32 at [2]. Best (fuzzy.fb:162-165) takes the highest score anywhere, which contradicts stdlib.md's 'fzf's own score' for one-character patterns.

## S7-tools-16 (low): docs/stdlib.md's "So far, std/ holds" table leaves out step 7's std/text files

Status: fixed in 9e5f5ca (stdlib.md's table has step 7's std/text files), docs only

Where: docs/stdlib.md:41

The table of what std/ holds ends its std/text rows at encoding.fb. Step 7 added std/text/regex.fb, glob.fb, layout.fb, fuzzy.fb, humanize.fb, and diff.fb and changed only the std.text design bullets, so the table no longer matches the tree. (The program just shows those modules exist and import.)

```
// Step 7's std/text files, each of which docs/stdlib.md's "So far, std/ holds" table leaves out.
Import std.core
Import std.text.glob
Import std.text.layout
Import std.text.fuzzy
Import std.text.humanize
Import std.text.diff

Print(Glob.Match("*.fb", "a.fb"), Levenshtein("a", "b"), Fuzzy.Score("a", "a") ?? 0,
      Humanize.Ordinal(1), Diff.Lines("a", "a").Count)
```

Expected: True 1 36 1st 1, with a row in the table for each of std/text/regex.fb, glob.fb, layout.fb, fuzzy.fb, humanize.fb, and diff.fb

Actual: True 1 36 1st 1, and the table has none of those six rows

Suggested fix: Add rows after std/text/encoding.fb: regex.fb (Regex, RegexFlags, RegexError, Match, the String shortcuts), glob.fb (Glob), layout.fb (WordWrap, Dedent, Indent, Levenshtein), fuzzy.fb (Fuzzy, FuzzyMatch), humanize.fb (Humanize), and diff.fb (Diff, DiffKind, DiffRun, Merged).

Skeptic: t16.fb runs and prints 'True 1 36 1st 1'. docs/stdlib.md's 'So far, std/ holds' table (lines 41-101) ends its std/text rows at std/text/encoding.fb. It has no row for regex.fb, glob.fb, layout.fb, fuzzy.fb, humanize.fb, or diff.fb, and the step only changed the M6 design bullets. Rows for later steps' files, such as crypto, are present, so the table is kept per step and is now out of date.

## S7-tools-8 (low): Glob with ignoreCase fails text it matches case-sensitively when case folding changes a character count

Status: fixed in c6985bc (a glob that ignores case folds each character to one, so a ?, a set, or a range still takes what it takes without), with text_glob.fb's ?, sets, and ranges against ß, ẞ, İ, ς, and capitals: each character folds to one, its case folding where that's one character, or else its lowercase, and a set, negated too, has a letter in any case. A folding of two characters isn't two of the pattern's, so "STRASSE" no longer matches "straße", as the folding of whole strings had it

Where: std/text/glob.fb:41

New and Matches run CaseFold over the whole pattern and text, which is full Unicode folding: "ß" becomes "ss" and "İ" becomes "i" plus U+0307. A ?, a set, or a range then sees two characters in the text where the pattern means one. So turning on ignoreCase makes "stra?e" stop matching "straße" and "?.txt" stop matching "İ.txt". ignoreCase should only ever let more text match.

```
// ignoreCase only lets more text match: what a pattern matches as it is, it matches ignoring case.
Import std.core
Import std.text.glob

Print(Glob.Match("?", "ß"), Glob.Match("?", "ß", True))
Print(Glob.Match("[ß]", "ß"), Glob.Match("[ß]", "ß", True))
Print(Glob.Match("?.txt", "İ.txt"), Glob.Match("?.txt", "İ.txt", True))
Print(Glob.Match("stra?e", "straße"), Glob.Match("stra?e", "straße", True))
```

Expected: True True
True True
True True
True True

Actual: True False
True False
True False
True False

Suggested fix: Compare per character with a one-Char fold (simple case folding, or ToLower on both Chars) in LITERAL and SET, and test set ranges on the folded Char, instead of folding whole strings up front. Add ß and İ cases to text_glob.fb.

Skeptic: t8.fb prints 'True False' on all four lines in debug and release. glob.fb:41 and :52 run full CaseFold over the whole pattern and text, so 'ß' becomes 'ss' and 'İ' becomes 'i' plus U+0307. A ONE or SET piece then meets two characters in the text. Turning on ignoreCase therefore rejects text that matches without it, which is plainly wrong for an ignore-case option. Only ?, sets, and ranges against characters whose fold changes length are affected, so it's rare.

## Refuted by the skeptics (no action)

- S7-compiler-3: Untyped lambdas inside an array literal still fail when the function is overloaded. l8.fb reproduces: Chain([Function(x) => ...]) with an overloaded Chain gives "'x' needs a type", and Run(...) prints 20. But overload resolution treats every array literal by its natural type, not only arrays of lambdas. a8.fb has F(Double[]) and F(String). F([1, 2]) gives "no overload of 'F' takes (Int32[])", and F([]) gives "an empty array needs a type from where it's used". So the lambda array is handled like any array literal. Spec 05's new sentence covers a lambda passed as the argument, where the 'parameter there is a Func'; here the parameter is an array. A typed array of Funcs passed to the overloads works (a9.fb prints "funcs"). This is a broader, pre-existing limit of array literals in overload resolution. Nothing in the spec, stdlib.md, or the milestones row says step 7 must cover it.
- S7-compiler-6: No test covers an untyped lambda fitting a nullable Func overload by conversion. n6.fb, Twice(Function(n) => n * 3, 2) with Twice((Func(Int): Int)?, Int) and Twice(String, Int), prints 18. The behavior is correct. The sema test at sema_tests.cpp:13006 covers only the exact, arity-mismatch, and ambiguous cases. The only nullable Func parameter in std, Search's New(... goal: (Func(N): Bool)?) in std/algo/search.fb:216, isn't overloaded, so it never reaches match(). The finding is right that nothing exercises match()'s CONVERTIBLE path. But it reports a test gap, not wrong behavior or a contradiction of the spec, stdlib.md, or the milestones row, so it is refuted under this task's rules. Adding the CHECK is still worthwhile.
- S7-tools-13: Glob expands braces inside a [ ] set. I reproduced 'False False False'. It isn't a bug against the docs or the reference shell, though. In bash, brace expansion runs before globbing and doesn't know about brackets: `echo [{,}]` prints '[] []', exactly what Expanded produces here. The glob.fb header and stdlib.md don't say braces are literal inside a set, and the case (a comma and braces inside a set) is contrived. Refuted.
