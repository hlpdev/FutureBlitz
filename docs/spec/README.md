# The FutureBlitz Language Reference

This reference describes the FutureBlitz language: its syntax, its types, and the rules the
compiler checks. The standard library, std, has its own documentation. This reference's rules
name a std type where the language itself relies on one, like Print, Error, or Iterator, and its
examples use std's types where they make an example shorter.

Code examples use ```` ```vb ```` fences for syntax highlighting.

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
| 12 | [C interop](12-ffi.md) |
| 13 | [Compile-time features](13-compile-time.md) |
| 14 | [Strings and collections](14-strings-collections.md) |

## A taste

```vb
Import std.core

Class Account
    Public Field owner: String
    Field balance: Int = 100

    New(owner: String)
        Self.owner = owner
    End New

    Method Withdraw(amount: Int)
        balance -= amount
        If balance < 0 Then Print("{owner} is overdrawn")
    End Method
End Class

Local accounts: Account[] = [Account("ada"), Account("grace")]

For a In accounts
    a.Withdraw(150)
End For
```
