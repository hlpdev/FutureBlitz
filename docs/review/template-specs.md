# The review of M6 step 8's template format specs: {{price:.2}} in std.text.template over step 5's format specs

2 confirmed findings: 0 high, 1 medium, 1 low. Each was reproduced by a skeptic. Fix every one with a test that fails without the fix, highs first, and set its Status line: fixed in <commit>, or dropped with why (only if it turns out not to reproduce, or the spec settles it the other way).

## specs-1 (medium): An integer formatted by a Double's spec loses its exact value past 2^53

Status: fixed in 73abbb35: an integer whose own Format doesn't take the spec goes to the runtime's new fb_format_exact, fb::format_exact in text_format.hpp, which writes it as a float of its value would be written but from its own digits: fixed with the precision's zeros, '%' with two zeros more, and 'e' and 'E' rounded half to even, as to_chars rounds, so UInt64's largest and 2^53 + 1 keep every digit; text_format_tests checks those, the halves, a carry, and that each small integer comes out as its float does by eleven specs, and text_template prints the review's cases

Where: std/text/template.fb:711

SpecText falls back to `(number As Double).Format(spec)` for INT and UINT values when the integer's own Format rejects the spec, as it does for `.N`, `%`, `e`, and `f`. Converting to Double rounds any integer above 2^53. So `{{u:.0}}` for UInt64's largest value prints 18446744073709551616, a different number, and `{{n:.0}}` for 9007199254740993 prints 9007199254740992. That defeats Value's UINT kind, which exists so JSON's integers stay exact (docs/stdlib.md). Interpolation can't do this at all: "{u:.0}" is a compile error ('UInt64 doesn't take a precision'). The fallback is the template's own addition, so the template is where the rounding comes from. Debug and release builds print the same.

```
Import std.core
Import std.collections
Import std.text.template

Local d = Value.Object()
d["u"] = Value(18446744073709551615 As UInt64)
d["n"] = Value(9007199254740993 As Int64)
Print(Try Template(r"[{{u:.0}}] [{{n:.0}}] [{{n:.1}}] [{{n:%}}]").Render(d))
```

Expected: [18446744073709551615] [9007199254740993] [9007199254740993.0] [900719925474099300%]

Actual: [18446744073709551616] [9007199254740992] [9007199254740992.0] [900719925474099200%]

Suggested fix: For an integer whose spec has no kind or 'f' and a precision, format it exactly: take the integer's text from its own Format of the spec without the precision (keeping sign, grouping, and zero-fill), append '.' and the precision's zeros, then pad to the width. Use the Double fallback only for 'e'/'E' and '%' (scale '%' as an exact integer times 100 when it fits). Failing that, document that integers past 2^53 round under a Double's spec. Add the UInt64-max and 2^53+1 cases to tests/programs/text_template.fb.

Skeptic: Ran the finding's program (scratchpad/rv-tplspec/refute/s1.fb) in debug and release. Both printed `[18446744073709551616] [9007199254740992] [9007199254740992.0] [900719925474099200%]`, so UInt64's max and 2^53+1 come out as different numbers. std/text/template.fb SpecText (INT and UINT cases) does `number.Format(part.Spec) ?? (number As Double).Format(part.Spec)`, and the cast to Double rounds anything past 2^53. stdlib.md does say an integer is formatted 'by a Double's spec' when its own type rejects the spec, but nothing there allows the digits to change. Silently printing a different integer goes against Value's UINT and INT kinds, which exist to keep integers exact. Interpolation can't hit this, because it rejects the spec at compile time.

## specs-2 (low): A spec's BAD_VALUE at Render doesn't say the line, column, or template

Status: fixed in 3be05740: the reader keeps the line and column it has read to, and a VALUE with a spec keeps where its tag is, as BAD_TEMPLATE says it, which BAD_VALUE's message starts with; an indented partial's columns leave out its indentation; text_template checks a lone Template's, a named template's on its second line, and an indented partial's

Where: std/text/template.fb:723

The lane's intended behavior is that a spec a value's type doesn't take throws a FormatError BAD_VALUE naming the template's line and column. SpecText's message gives only the path, the value, and the spec. The Part keeps no position (Opens is set only for blocks) and no template name, so neither can be reported. With the same tag used twice, or inside a Templates partial, the message can't tell which tag or template failed. Every other template error (BAD_TEMPLATE) gives the template's name, line, and column. The test (`Failure(r"{{name:x}}", kinds)` in tests/programs/text_template.out) records the message without a position, so the test doesn't check the intended behavior.

```
Import std.core
Import std.collections
Import std.text.template

Local d = Value.Object()
d["s"] = Value("abc")
Local set = Templates()
Try set.Add("page", "one\n" + r"two {{s:+}}")
Try
    Print(Try set.Render("page", d))
Catch e As FormatError
    Print("{e.Kind}: {e.Message}")
End Try
```

Expected: BAD_VALUE: page, line 2, column 5: s's value, abc, doesn't take the format spec "+"

Actual: BAD_VALUE: s's value, abc, doesn't take the format spec "+"

Suggested fix: In ValuePart, store the tag's line and column (or the prefix that Bad builds, with the template's name) on the Part, and put it at the front of SpecText's message. Update the test's .out line to show the position, and add a case that goes through Templates so the template's name is checked too.

Skeptic: Ran the finding's program (scratchpad/rv-tplspec/refute/s2.fb) in debug and release. Both printed `BAD_VALUE: s's value, abc, doesn't take the format spec "+"`, with no template name, line, or column. SpecText's Throw builds its message from only the path, the value's text, and the spec, and the Part it gets doesn't carry a position. tests/programs/text_template.out:52 records the message without a position too. This contradicts the lane's stated behavior that BAD_VALUE names the template's line and column. stdlib.md only says BAD_VALUE is thrown 'at Render' and doesn't require a position, so this is a diagnostics gap, not wrong output. Low severity.
