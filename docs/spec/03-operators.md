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
a shift, and in `x Is T ? a : b` the `?` is the ternary, not part of the type. In an
interpolation, a ternary's `:` has a space after it, since a `:` without one starts a format spec
(see [14-strings-collections.md](14-strings-collections.md#format-specs)).

A shift's result has the left side's type, and the amount can be any integer type. An amount
known at compile time must be from 0 to the left side's width minus 1.

A `Ptr<T>` steps by T's size, in Unsafe code: `p + n` and `p - n`, with the Ptr first and any
integer for `n`, and `p - q`, how many T's there are from `q` to `p`, an `Int64`; `+=` and `-=`
step it too. Ptrs compare their addresses with `==` and `!=`, in safe code too (see
[08](08-memory.md#pointers-and-unsafe-code)).

`+` on strings only joins `String` to `String`; use interpolation for everything else
(see [14](14-strings-collections.md)). Strings compare with `<` and the rest by code point,
the order their UTF-8 bytes have. `==` on arrays compares their elements.

Operands go left to right: an operator's operands, a call's arguments after the object of a
Method call, `?.` one included, in the order they're written even when they're named, the parts
of an interpolated string, and the elements of a tuple, array, or Map literal are each evaluated
before the next, so a variable, field, or element read in one keeps the value it had then, even
when a later one, like a call, changes it: `Pair(g, Bump())` passes `g` as it was, and
`"{s}{Grow()}"` shows `s` as it was. What could change it is a call, an Await, a Property's Get,
or a type's own code that an operator, `==`, or a value's text runs. The array in `a[i]`, and the
object a fixed array is in, are read before `i`, and a slice's array before its bounds; a fixed
array in a variable of its own is read in place, once `i` is known. An assignment's target goes
before its value: `holder.Name = Swap()` and `holder.Level = Swap()` set the Name or Level of the
object `holder` was, `g += Bump()` adds to the `g` there was, and a ByRef argument, like
`ByRef holder.count`, is the variable in the object there was.

When the two operands have different numeric types, see
[02](02-types.md#mixed-numeric-types).

## Examples

```vb
If retries <= 0 && !force Then Abort()
Local flags = Access.Read | Access.Write
Local dmg = crit ? base * 2 : base
Local name = target?.name ?? "nobody"
If u Is Admin Then (u As Admin).Grant()
Local a = u As? Admin              // Admin? (Null if u isn't an Admin)
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
| 16 | `To` `Until`, with `Step`: a range (see [04-control-flow.md](04-control-flow.md#ranges)) |
| 17 | `? :` (right-associative) |

Note: bitwise operators bind tighter than comparisons (unlike C), so `x & MASK == 0`
means `(x & MASK) == 0`.

## Operator overloading

Structs and classes may define operators. See
[06-user-types.md](06-user-types.md#operator-overloading).
