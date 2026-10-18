# Open Questions

## Needs a decision

| Question | Notes / recommendation |
|---|---|
| **Coroutine scheduling** | Proposed in [spec/10-concurrency.md](spec/10-concurrency.md): main scheduler ticked by `Flip()`, plus manually ticked custom schedulers. Not explicitly confirmed. M4 step 10 builds it that way, with the main scheduler ticked when top-level code ends and in `Wait()` until `Flip()` exists |
| **Milestone tracking** | Proposed: GitHub milestones M0–M15 with issues |
| **CI** | GitHub Actions matrix (Windows, Linux, macOS) not set up yet |
| **Name check** | "FutureBlitz" still contains "Blitz" (Blitz Basic 2, Blitz2D, Blitz Research). Fine for the honors project; check before any public release |
| **Self-contained toolchain** | M1 links through the C++ compiler that built the runtime. Shipping FutureBlitz to people without one needs bundled C runtime libraries (a sysroot) per platform. Decide by M5 or M7 |
| **Debug info** | Panics already report file, line, and column. Debugger support (DWARF/CodeView through LLVM's DIBuilder) isn't scheduled yet |
| **Engine API details** | Direction locked (modern objects); module-by-module design deferred to Phase 2 |
| **Standard library details** | Layout drafted in [stdlib.md](stdlib.md); depth deferred to M6 |
| **Asset system details** | Direction locked in [assets.md](assets.md); details at M12 |
| **Indexing collections** | `hp["boss"]` reads as a Map lookup, but needs an index Operator, which the spec doesn't have. Proposed: `Operator [](key: K): V?` and `Operator []=(key: K, value: V)`, with `Get` and `Add` until then |
| **Running ordinary functions at compile time** | `Const Function`s already do, on M3 step 10's MIR interpreter. Proposed: also run other functions when the compiler can prove they're pure (no I/O, globals, or allocation); what's left is that proof |
| **`-` and `/` in Const type arguments** | `Int[N + M]` and `Grid<W * 2>` work, since they stay positive. `N - 1` can reach 0. Proposed: allow them, and report an instance whose size works out below 1 where it's made |
| **Setting reflected members** | `p.Get(value)` and `s.Get()` read a Property or Static Field. Proposed: `p.Set(value, x)`, with a Where line like `Where p: Settable` to pick the ones with a Set or that aren't ReadOnly |

## Spec details to settle while implementing

These weren't explicitly discussed; the spec contains a reasonable default that may change.

| Item | Current default |
|---|---|
| Operator precedence table | in [spec/03-operators.md](spec/03-operators.md); bitwise binds tighter than comparison |
| Expression-bodied functions `Function F(): T => expr` | allowed |
| Property setter syntax `Set(value) ... End Set` | as in [spec/06-user-types.md](spec/06-user-types.md) |
| Default enum underlying type | `Int` |
