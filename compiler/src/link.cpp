#include "fb/link.hpp"

#include "fb/build_config.hpp"
#include "fb/process.hpp"

#include <format>
#include <string_view>
#include <vector>

namespace fb {

/// The C++ driver that built the runtime knows where the C and C++ runtime libraries are; -B
/// points it at lld.
std::expected<void, std::string> link_program(const std::span<const std::filesystem::path> objects,
                                              const std::filesystem::path& output) {
    std::vector<std::string> arguments;
    for (const std::filesystem::path& object : objects) {
        arguments.push_back(object.string());
    }

    arguments.emplace_back(build_config::runtime_library);
    arguments.emplace_back("-o");
    arguments.push_back(output.string());
    arguments.emplace_back("-fuse-ld=lld");
    arguments.push_back(std::format("-B{}/", build_config::lld_directory));
    // Per-platform options from compiler/CMakeLists.txt, like static linking on MinGW.
    for (const std::string_view option : build_config::link_options) {
        arguments.emplace_back(option);
    }

    const std::expected<process_result_t, std::string> result =
        run_process(build_config::linker_driver, arguments);
    if (!result) {
        return std::unexpected(result.error());
    }

    if (result->exit_code != 0) {
        return std::unexpected(std::format("linking failed:\n{}", result->output));
    }

    return {};
}

} // namespace fb
