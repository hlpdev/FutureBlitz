# FutureBlitz Language Specification (v0)

Status: **Locked** unless a section says otherwise.

Code examples use ```` ```vb ```` fences for approximate syntax highlighting.

| # | Chapter |
|---|---|
| 01 | [Lexical structure](01-lexical.md) |
| 02 | [Types, declarations, conversions](02-types.md) |
| 03 | [Operators](03-operators.md) |
| 04 | [Control flow](04-control-flow.md) |
| 05 | [Functions and closures](05-functions.md) |
| 06 | [Structs, classes, interfaces, enums](06-user-types.md) |
| 07 | [Generics](07-generics.md) |
| 08 | [Memory model and unsafe code](08-memory.md) |
| 09 | [Error handling](09-errors.md) |
| 10 | [Concurrency](10-concurrency.md) |
| 11 | [Modules and packages](11-modules.md) |
| 12 | [FFI](12-ffi.md) |
| 13 | [Compile-time features](13-compile-time.md) |
| 14 | [Strings and collections](14-strings-collections.md) |

## A taste

```vb
Import std.math

Class Enemy
    Public Field name: String
    Field hp: Int = 100

    New(name: String)
        Self.name = name
    End New

    Method Hurt(amount: Int)
        hp -= amount
        If hp <= 0 Then Print("{name} died")
    End Method
End Class

Local enemies: Enemy[] = [Enemy("grunt"), Enemy("boss")]

For e In enemies
    e.Hurt(150)
End For
```
