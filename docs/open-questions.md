# Open Questions

## Needs a decision

| Question | Notes / recommendation |
|---|---|
| **Coroutine scheduling** | Proposed in [spec/10-concurrency.md](spec/10-concurrency.md): main scheduler ticked by `Flip()`, plus manually ticked custom schedulers. Not explicitly confirmed |
| **Milestone tracking** | Proposed: GitHub milestones M0–M15 with issues |
| **CI** | GitHub Actions matrix (Windows, Linux, macOS) not set up yet |
| **Name check** | "FutureBlitz" still contains "Blitz" (Blitz Basic 2, Blitz2D, Blitz Research). Fine for the honors project; check before any public release |
| **Self-contained toolchain** | M1 links through the C++ compiler that built the runtime. Shipping FutureBlitz to people without one needs bundled C runtime libraries (a sysroot) per platform. Decide by M5 or M7 |
| **Debug info** | Panics already report file, line, and column. Debugger support (DWARF/CodeView through LLVM's DIBuilder) isn't scheduled yet |
| **Engine API details** | Direction locked (modern objects); module-by-module design deferred to Phase 2 |
| **Standard library details** | Layout drafted in [stdlib.md](stdlib.md); depth deferred to M6 |
| **Asset system details** | Direction locked in [assets.md](assets.md); details at M12 |
| **Captures across threads** (M4) | Proposed in [spec/10-concurrency.md](spec/10-concurrency.md): since closures capture by reference, a closure run on another thread can only capture `Const`s holding `Sendable` values |
| **Structs holding class references** (M4) | Is such a struct `Sendable`? Proposed: a struct is `Sendable` only when all its fields are, so it isn't |
| **Running ordinary functions at compile time** | `Const Function`s already do. Proposed: also run other functions when the compiler can prove they're pure (no I/O, globals, or allocation). Needs a compile-time interpreter, so after M3 |

## Spec details to settle while implementing

These weren't explicitly discussed; the spec contains a reasonable default that may change.

| Item | Current default |
|---|---|
| Operator precedence table | in [spec/03-operators.md](spec/03-operators.md); bitwise binds tighter than comparison |
| Expression-bodied functions `Function F(): T => expr` | allowed |
| Property setter syntax `Set(value) ... End Set` | as in [spec/06-user-types.md](spec/06-user-types.md) |
| Default enum underlying type | `Int` |
