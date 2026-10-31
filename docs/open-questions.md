# Open Questions

## Needs a decision

| Question | Notes / recommendation |
|---|---|
| **Engine API details** | Direction locked (modern objects); each module's design is in its milestone, E0 to E14 ([engine.md](engine.md)) |
| **Build steps and tools in packages** | The engine's cooker and typed references (E8) run at build time. Proposed: a package can name a tool of its own that `fb build` runs before compiling, whose output files are part of the build |
| **Re-exports** | A package can't hand on another package's names, so there's no umbrella package. Proposed: `Public Import X`, which gives the importer X's Public names too |
| **Asset system details** | Direction locked in [assets.md](assets.md); details in the engine phase |
| **Running ordinary functions at compile time** | `Const Function`s already do, on M3 step 10's MIR interpreter. Proposed: also run other functions when the compiler can prove they're pure (no I/O, globals, or allocation); what's left is that proof |

## Spec details to settle while implementing

These weren't explicitly discussed; the spec contains a reasonable default that may change.

| Item | Current default |
|---|---|
| Operator precedence table | in [spec/03-operators.md](spec/03-operators.md); bitwise binds tighter than comparison |
| Expression-bodied functions `Function F(): T => expr` | allowed |
| Property setter syntax `Set(value) ... End Set` | as in [spec/06-user-types.md](spec/06-user-types.md) |
| Default enum underlying type | `Int` |
