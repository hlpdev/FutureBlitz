#pragma once

#include "fb/ast.hpp"
#include "fb/mir.hpp"
#include "fb/sema.hpp"

#include <optional>
#include <string>
#include <vector>

namespace fb {

/// Turns an analyzed file into MIR: each Function becomes a MIR function, but a @Test Function,
/// and the top-level code becomes the entry, "main". The analysis must have no errors. With
/// `library`, a library package's name, it's that library's build, for its binary: every one of
/// its modules' Functions, Methods, and Classes but its tests, and an initializer for each
/// module's Globals, in place of main.
[[nodiscard]] mir_module_t lower(const ast_t& ast, const analysis_t& analysis,
                                 const std::optional<std::string>& library = std::nullopt);

/// One of the tests a test build runs: its @Test Function, the name fb test shows it by, like
/// "game.shapes.Area", and whether it runs on the main thread, as analysis_t's tests say.
struct test_case_t {
    symbol_id_t function{};
    std::string name;
    bool main_thread = false;
};

/// Like lower(), for fb test: main sets the Globals, then runs `tests`, by their index in it, with
/// the runtime's runner, in place of the main file's top-level code. Every other @Test Function
/// is left out, as lower() leaves them all out.
[[nodiscard]] mir_module_t lower_tests(const ast_t& ast, const analysis_t& analysis,
                                       const std::vector<test_case_t>& tests);

/// One function and the functions it calls, for the compiler to run a call of a Const Function.
/// It's the module's first function. Only that code needs to have analyzed without errors.
[[nodiscard]] mir_module_t lower_function(const ast_t& ast, const analysis_t& analysis,
                                          symbol_id_t function,
                                          const std::vector<type_id_t>& type_arguments = {});

} // namespace fb
