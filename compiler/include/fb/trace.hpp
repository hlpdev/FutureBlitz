#pragma once

#include <llvm-c/Types.h>
#include <string_view>

namespace fb {

/// Stack traces come from a table that every binary has, debug and release, which the runtime
/// reads (docs/tooling.md, decision 9): before each call, a marker of where its code is, and for
/// each marker, the call's function, file, and line, then those of each call it's inlined into.
/// This gives a program's or a library beside it's module the function that hands the runtime
/// its table as the binary loads, and the table, which fill_trace_table() fills in.
void add_trace_table(LLVMModuleRef module);

/// Fills in the table that add_trace_table() gave `module`, once it's optimized, right before it
/// becomes machine code for `triple`, so its calls are the ones the code has, each with the
/// places debug info gives it. A call whose function has no debug info gets a marker too, which
/// the runtime leaves out of a trace.
void fill_trace_table(LLVMModuleRef module, std::string_view triple);

} // namespace fb
