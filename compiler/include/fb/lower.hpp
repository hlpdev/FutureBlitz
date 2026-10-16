#pragma once

#include "fb/ast.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"

namespace fb {

/// Turns an analyzed file into MIR: each Function becomes a MIR function, and the top-level code
/// becomes the entry, "main". The analysis must have no errors.
[[nodiscard]] mir_module_t lower(const ast_t& ast, const analysis_t& analysis);

} // namespace fb
