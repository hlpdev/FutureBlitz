# Open Questions

## Needs a decision

| Question | Notes / recommendation |
|---|---|
| **Self-contained toolchain** | M1 links through the C++ compiler that built the runtime. Shipping FutureBlitz to people without one needs bundled C runtime libraries (a sysroot) per platform. Decide by M5 or M7 |
| **Debug info** | Panics already report file, line, and column. Debugger support (DWARF/CodeView through LLVM's DIBuilder) isn't scheduled yet |
| **Engine API details** | Direction locked (modern objects); module-by-module design deferred to Phase 2 |
| **Asset system details** | Direction locked in [assets.md](assets.md); details at M12 |
| **Running ordinary functions at compile time** | `Const Function`s already do, on M3 step 10's MIR interpreter. Proposed: also run other functions when the compiler can prove they're pure (no I/O, globals, or allocation); what's left is that proof |
| **Setting reflected members** | `p.Get(value)` and `s.Get()` read a Property or Static Field. Proposed: `p.Set(value, x)`, with a Where line like `Where p: Settable` to pick the ones with a Set or that aren't ReadOnly |

## Spec details to settle while implementing

These weren't explicitly discussed; the spec contains a reasonable default that may change.

| Item | Current default |
|---|---|
| Operator precedence table | in [spec/03-operators.md](spec/03-operators.md); bitwise binds tighter than comparison |
| Expression-bodied functions `Function F(): T => expr` | allowed |
| Property setter syntax `Set(value) ... End Set` | as in [spec/06-user-types.md](spec/06-user-types.md) |
| Default enum underlying type | `Int` |
