#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace fb {

/// What fb-bindgen reads a C header with.
struct bindgen_options_t {
    std::filesystem::path header;
    /// The [native] library the Extern block's `Lib` names. Empty leaves `Lib` out.
    std::string library;
    /// The target to read the header for, like "x86_64-w64-mingw32", which decides how big a
    /// `long`, a `wchar_t`, and an enum are. Empty is the one this build makes programs for.
    std::string target;
    /// The C standard to read it with, like "c17" or "gnu23". Empty is the default of the C
    /// compiler that links programs, with GNU's extensions.
    std::string standard;
    /// -I folders, searched before the C library's.
    std::vector<std::string> includes;
    /// The C library's include folders. Empty is those of the C compiler that links programs,
    /// for this build's target, and none for another, whose come through `includes`.
    std::vector<std::string> system_includes;
    /// -D name or name=value.
    std::vector<std::string> defines;
};

/// A FutureBlitz module of a header's declarations, and a warning, "vecmath.h:12:5: skipped
/// ...", for each one it leaves out.
struct bindings_t {
    std::string text;
    std::vector<std::string> warnings;
    /// Each file clang read for the header: the header and what it #includes, directly or not,
    /// which the module mustn't be written over.
    std::vector<std::filesystem::path> files;
};

/// The header's own declarations, not those of what it #includes, as a module: its structs as
/// @CLayout Structs, its enums as Enums, or Consts when their values repeat, its number macros
/// as Consts of the types and values C gives them after the header, each in the header's
/// order, then its functions in one `Public Extern "C" Lib` block. C's names stay where they
/// can, and one that's a keyword, a type's, or taken gets a '_'. What can't map, like a union
/// or a varargs function, is a `// skipped:` comment. Fails, with clang's errors a line each,
/// when the header doesn't parse.
[[nodiscard]] std::expected<bindings_t, std::string>
generate_bindings(const bindgen_options_t& options);

} // namespace fb
