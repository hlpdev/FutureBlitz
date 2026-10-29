#pragma once

#include "fb/diagnostic.hpp"
#include "fb/source.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// The line width code is formatted to when project.toml's `[format] width` doesn't say.
inline constexpr std::uint32_t default_format_width = 100;

/// The file's text in FutureBlitz's one style, the rules in docs/tooling.md, with lines up to
/// `width` columns where a statement can be broken to fit. It changes only the space between
/// tokens, so the code means what it did. A file that doesn't lex or parse isn't formatted: the
/// errors that stop it come back instead, with spans in `file`.
[[nodiscard]] std::expected<std::string, std::vector<diagnostic_t>>
format_source(const source_file_t& file, std::uint32_t width = default_format_width);

/// format_source() without its check that the code means what it did, for the formatter's tests,
/// which check that themselves.
[[nodiscard]] std::expected<std::string, std::vector<diagnostic_t>>
format_unchecked(const source_file_t& file, std::uint32_t width = default_format_width);

/// Whether `formatted` means what `file` does, which format_source() checks of what it gives:
/// the same tokens but for line breaks, a ">=" as its '>' and '=', and the same tree without
/// spans, with each #If's lines left out, and no errors.
[[nodiscard]] bool same_meaning(const source_file_t& file, std::string_view formatted);

} // namespace fb
