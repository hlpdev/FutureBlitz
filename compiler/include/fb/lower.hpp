#pragma once

#include "fb/ast.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"

#include <vector>

namespace fb {

/// Turns an analyzed file into MIR: each Function becomes a MIR function, and the top-level code
/// becomes the entry, "main". The analysis must have no errors.
[[nodiscard]] mir_module_t lower(const ast_t& ast, const analysis_t& analysis);

/// One function and the functions it calls, for the compiler to run a call of a Const Function.
/// It's the module's first function. Only that code needs to have analyzed without errors.
[[nodiscard]] mir_module_t lower_function(const ast_t& ast, const analysis_t& analysis,
                                          symbol_id_t function,
                                          const std::vector<type_id_t>& type_arguments = {});

} // namespace fb
