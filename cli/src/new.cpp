#include "fb/new.hpp"

#include "fb/commands.hpp"
#include "fb/manifest.hpp"
#include "fb/style.hpp"
#include "fb/terminal.hpp"
#include "fb/utf8.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fb {
namespace {

/// What a new package's main.fb says.
constexpr std::string_view main_file = "Import std.core\n"
                                       "\n"
                                       "Print(\"Hello, world!\")\n";

/// Writes to stderr, after what's gone to stdout.
void print_error(const std::string_view text) {
    std::fflush(stdout);
    std::fwrite(text.data(), 1, text.size(), stderr);
}

#ifdef _WIN32
/// Whether Windows keeps a name for a device: CON, PRN, AUX, NUL, COM1 to COM9, and LPT1 to
/// LPT9, in any letter case.
bool is_device_name(const std::string_view name) {
    std::string upper(name);
    std::ranges::transform(upper, upper.begin(), [](const char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    });
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") {
        return true;
    }

    return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) &&
           upper[3] >= '1' && upper[3] <= '9';
}
#endif

} // namespace

int new_command(const options_t& options) {
    const style_t style{ .color = use_color(options.color, stream_t::STANDARD_ERROR) };
    const auto error = [&](const std::string_view message) {
        print_error(std::format("{}: {}\n", style.paint("error", tone_t::ERROR),
                                style.paint(message, tone_t::BOLD)));
    };

    // The package is named by the folder's own name, however it's written: "game/", ".", and
    // "game/sub/.." are each a folder whose name is that of the folder they mean.
    const std::filesystem::path folder = options.input;
    std::error_code failed;
    std::filesystem::path named = std::filesystem::absolute(folder, failed).lexically_normal();
    if (!named.has_filename()) {
        named = named.parent_path();
    }

    const std::string name = to_utf8(named.filename());
    if (name.empty()) {
        error(std::format("{} has no name, which the package would take; give a folder in it",
                          to_utf8(folder)));
        return usage_exit_code;
    }

    if (const std::optional<std::string> wrong = package_name_error(name)) {
        error(std::format("{}; it's the folder's name", *wrong));
        return usage_exit_code;
    }

#ifdef _WIN32
    // Windows keeps these names for devices, which it finds in every folder.
    if (is_device_name(name)) {
        error(
            std::format("{} is the name of a device on Windows, which a folder can't have", name));
        return usage_exit_code;
    }
#endif

    if (std::filesystem::exists(folder, failed)) {
        if (!std::filesystem::is_directory(folder, failed)) {
            error(std::format("{} is a file, not a folder", to_utf8(folder)));
            return compile_errors_exit_code;
        }

        if (!std::filesystem::is_empty(folder, failed) || failed) {
            error(std::format("{} is there already, and isn't empty", to_utf8(folder)));
            return compile_errors_exit_code;
        }
    }

    std::filesystem::create_directories(folder, failed);
    if (failed) {
        error(std::format("couldn't make the folder {}: {}", to_utf8(folder), failed.message()));
        return compile_errors_exit_code;
    }

    const auto files = std::to_array<std::pair<std::string_view, std::string>>({
        { "project.toml", std::format("[project]\nname = \"{}\"\nversion = \"0.1.0\"\n", name) },
        { "main.fb", std::string(main_file) },
        { ".gitignore", "build/\n" },
    });
    for (const auto& [file, text] : files) {
        const std::filesystem::path path = folder / file;
        std::ofstream out(path, std::ios::binary);
        out << text;
        out.close();
        if (!out) {
            error(std::format("couldn't write {}", to_utf8(path)));
            return compile_errors_exit_code;
        }
    }

    print_error(std::format("{} package {} in {}\n",
                            style.paint(std::format("{:>12}", "Created"), tone_t::SUCCESS), name,
                            to_utf8(folder)));
    return 0;
}

} // namespace fb
