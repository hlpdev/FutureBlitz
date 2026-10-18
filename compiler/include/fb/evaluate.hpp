#pragma once

#include "fb/const_value.hpp"
#include "fb/mir.hpp"
#include "fb/source.hpp"

#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace fb {

struct evaluation_error_t {
    std::string message;
    std::optional<span_t> span;
};

[[nodiscard]] std::expected<const_value_t, evaluation_error_t>
evaluate(const mir_module_t& module, function_id_t function,
         const std::vector<const_value_t>& arguments);

} // namespace fb
