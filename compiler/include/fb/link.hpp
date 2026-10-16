#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>

namespace fb {

/// Links object files and the runtime library into an executable at `output`, using lld through
/// the C++ compiler that built the runtime. Fails when the linker can't run or reports errors.
[[nodiscard]] std::expected<void, std::string>
link_program(std::span<const std::filesystem::path> objects, const std::filesystem::path& output);

} // namespace fb
