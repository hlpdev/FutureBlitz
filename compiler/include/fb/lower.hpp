#pragma once

#include "fb/ast.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fb {

/// Turns an analyzed file into MIR: each Function becomes a MIR function, and the top-level code
/// becomes the entry, "main". The analysis must have no errors. With `library`, a library
/// package's name, it's that library's build, for its binary: every one of its modules' Functions,
/// Methods, and Classes, and an initializer for each module's Globals, in place of main.
[[nodiscard]] mir_module_t lower(const ast_t& ast, const analysis_t& analysis,
                                 const std::optional<std::string>& library = std::nullopt);

/// One function and the functions it calls, for the compiler to run a call of a Const Function.
/// It's the module's first function. Only that code needs to have analyzed without errors.
[[nodiscard]] mir_module_t lower_function(const ast_t& ast, const analysis_t& analysis,
                                          symbol_id_t function,
                                          const std::vector<type_id_t>& type_arguments = {});

} // namespace fb
