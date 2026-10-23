# 01 · Lexical Structure

## Source files

- Extension `.fb`, UTF-8 encoded. A leading UTF-8 byte order mark is ignored.
- Invalid UTF-8 is a compile error.
- A line ends at `\n`, `\r\n`, or a lone `\r`.
- One file is one module (see [11-modules.md](11-modules.md)).

## Case

- The language is **case-sensitive**.
- Keywords are PascalCase (`Function`, `If`, `End`) and must be written exactly; `if` is an
  ordinary identifier. When a miscased keyword causes a syntax error, the compiler suggests
  the correct casing and the IDE can apply the fix.

## Statements and lines

- A newline ends a statement.
- A statement continues onto the next line when the line ends inside an open `(`, `[`, or
  `{`, or ends with a binary operator, `=`, a compound assignment, `,`, or `=>`.
- Inside a block body (for example a block lambda), newlines end statements again, even
  within brackets.
- No `:` statement separator, no `;` terminator.
- Only calls, assignments, and `++`/`--` can stand alone as statements. Any other expression
  on its own is an error, since it would do nothing.
- A list inside brackets (arguments, indexes, parameters) may end with a trailing comma.

```vb
Local total = base +
              bonus                // continues: the line ended with +

Spawn(Function()
    ParseLevel()                   // two statements, even inside Spawn(...)
    BuildNavMesh()
End Function)
```

## Comments

```vb
// line comment
/* block comment, /* nestable */ */
/// doc comment, attached to the next declaration
```

An unterminated `/*` is an error. A block comment counts as a space even when it spans lines,
so it never ends a statement.

## Blocks

Every block closes with `End <Kind>`:

`End If`, `End For`, `End While`, `End Loop`, `End Select`, `End Function`, `End Method`,
`End New`, `End Class`, `End Struct`, `End Interface`, `End Enum`, `End Try`, `End Lock`,
`End Unsafe`, `End Extern`, `End Extend`, `End Property`, `End Operator`, `End Get`, `End Set`.

Exception: `Repeat ... Until cond`.

There is no `Wend`, `Next`, or `EndIf`.

Blocks, expressions, and types nest up to 1000 deep, counting each block, operator, call, member,
index, lambda, and pair of brackets around a part, as in a chain `a + b + c` or `f()()`, and each
part of a type, like a `?`, a `[]`, or a list of type arguments. A Defer's statement and a
single-line If's count as blocks. Deeper code is an error that says to split it up, with Locals or
Functions. An #If's condition nests up to 1000 deep too.

## Identifiers

- ASCII letters, digits, and `_`; cannot start with a digit. Unicode identifiers may be
  allowed in a later version.
- **No type suffixes** (`%`, `#`, `$` do not exist).

## Literals

```vb
42            // Int
1_000_000     // digit separators
0xFF00        // hex
0b1010        // binary
0o755         // octal
1.5           // Float (32-bit)
1.5e-3        // exponent
'A'           // character literal: an Int code point
True  False   // Bool
Null          // the null reference
```

### Numbers

- `_` may only appear between two digits: `1_000` is valid; `_1`, `1_`, `1__0`, and `0x_FF`
  are errors.
- A decimal literal needs digits on both sides of `.` (`1.0`, not `1.` or `.5`), so `1..3`
  is a range, and `2.Seconds` is `2`, then `.`, then `Seconds`: a member of the literal, as
  `1.5.Minutes` is of `1.5`. An exponent also makes a decimal literal (`1e5`).
- Hex, binary, and octal literals are always integers.
- An integer literal must fit in 64 bits. Whether it fits its type is checked during type
  checking, so `-2147483648` is a valid `Int`.

Default literal types:

- An integer literal is `Int` (32-bit) unless context gives it another numeric type that holds
  its value (see [02](02-types.md#literals)).
- A decimal literal is `Float` (32-bit) unless context requires `Double`.

### Characters

- A character literal holds exactly one code point after escapes are applied; `''` and
  `'ab'` are errors.
- It uses the same escapes as strings. Braces are literal (`'{'`).

Strings are described in [14-strings-collections.md](14-strings-collections.md#strings).

## Escape sequences

Valid inside regular (non-raw) strings and character literals:

| Escape | Meaning |
|---|---|
| `\n` | newline |
| `\t` | tab |
| `\r` | carriage return |
| `\0` | null character |
| `\\` | backslash |
| `\"` | double quote |
| `\'` | single quote |
| `\xHH` | ASCII character by hex value, `00`–`7F` |
| `\u{H...}` | Unicode scalar value: at most `10FFFF`, no surrogates (`D800`–`DFFF`) |
| `{{` `}}` | literal braces (since `{` starts interpolation); strings only |

Strings are always valid UTF-8. Raw bytes belong in `UInt8[]`, not `String`.

## Directives and attributes

- A line whose first non-blank character is `#` is a conditional-compilation directive
  (`#If`, `#ElseIf`, `#Else`, `#End If`). See
  [13-compile-time.md](13-compile-time.md#conditional-compilation).
- `@Name` or `@Name(args)` is an attribute. Attribute names are ordinary identifiers, not
  keywords. See [13-compile-time.md](13-compile-time.md#attributes).

## Keywords (v0)

### Reserved

Can never be used as identifiers.

```
Abstract  As  Async  Await  Break  ByRef  Callback  Case  Catch  Class  Const  Continue
Default  Defer  Else  ElseIf  End  Enum  Extend  Extends  Extern  False  Field  Final
Finally  For  Func  Function  Global  If  Implements  Import  In  Interface  Is  Local
Lock  Loop  Method  New  Null  Operator  Override  Package  Parallel  Private  Property
Protected  Public  ReadOnly  Repeat  Return  Select  Self  Shared  Static  Struct  Super
Then  ThreadLocal  Throw  Throws  To  True  Try  Unsafe  Until  Where  While
```

### Contextual

Ordinary identifiers everywhere except in one position, where they act as keywords:

| Word | Keyword only |
|---|---|
| `Get`, `Set` | at the start of a line inside `Property ... End Property` |
| `Step` | after the bound in a `For` header, or a range's end |
| `Lib` | directly after `Extern "<convention>"` |
| `From` | at the start of an import line (`From std.math Import Vec3`) |
| `Weak` | inside a capture list (`[Weak Self]`); elsewhere it names the type `Weak<T>` |

So `Set<Int>()` and `particles[i].Step(dt)` are valid.
