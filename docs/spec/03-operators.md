# 03 · Operators

Operators follow **C syntax**. Only the type operators (`Is`, `As`) are words.

| Kind | Operators | Notes |
|---|---|---|
| Arithmetic | `+ - * / % **` | `/` follows operand types: `Int / Int` is integer division. `**` is power |
| Comparison | `== != < > <= >=` | |
| Logical | `&& \|\| !` | short-circuit, `Bool` operands only |
| Bitwise | `& \| ^ ~ << >>` | integers only, not `Bool`. `>>` is arithmetic on signed, logical on unsigned |
| Assignment | `= += -= *= /= %= &= \|= ^= <<= >>=` | assignment is a **statement**, not an expression |
| Increment | `++ --` | statements only; no pre/post value semantics (`x = i++` is invalid) |
| Ternary | `cond ? a : b` | |
| Null | `?.` `??` | safe member access, null-coalescing |
| Type | `Is` `As` `As?` | type test, cast, checked downcast |
| Range | `a..b` | half-open, used in slices |

`=` is always assignment and `==` is always comparison.

`>>`, `As?`, and the `?` of a nullable type `T?` are written without spaces. So `a > > b` is not
a shift, and in `x Is T ? a : b` the `?` is the ternary, not part of the type.

A shift's result has the left side's type, and the amount can be any integer type. An amount
known at compile time must be from 0 to the left side's width minus 1.

`+` on strings only joins `String` to `String`; use interpolation for everything else
(see [14](14-strings-collections.md)).

When the two operands have different numeric types, see
[02](02-types.md#mixed-numeric-types).

## Examples

```vb
If hp <= 0 && !godMode Then Die()
Local flags = Layer.World | Layer.Player
Local dmg = crit ? base * 2 : base
Local name = target?.name ?? "nobody"
If e Is Boss Then (e As Boss).Enrage()
Local b = e As? Boss               // Boss? (Null if e isn't a Boss)
i++
```

## Precedence (highest first)

| Level | Operators |
|---|---|
| 1 | `.` `?.` `()` `[]` (postfix) |
| 2 | `!` `~` `-` (unary) |
| 3 | `**` (right-associative) |
| 4 | `*` `/` `%` |
| 5 | `+` `-` |
| 6 | `<<` `>>` |
| 7 | `&` |
| 8 | `^` |
| 9 | `\|` |
| 10 | `Is` `As` `As?` |
| 11 | `<` `>` `<=` `>=` |
| 12 | `==` `!=` |
| 13 | `&&` |
| 14 | `\|\|` |
| 15 | `??` |
| 16 | `? :` (right-associative) |

Note: bitwise operators bind tighter than comparisons (unlike C), so `x & MASK == 0`
means `(x & MASK) == 0`.

## Operator overloading

Structs and classes may define operators. See [06-user-types.md](06-user-types.md#operator-overloading).
