#include "fb/source.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace fb {
namespace {

constexpr std::string_view utf8_bom = "\xEF\xBB\xBF";
/// Spans hold 32-bit offsets, which caps a file at 4 GiB.
constexpr std::size_t max_source_size = std::numeric_limits<std::uint32_t>::max();

/// How many columns of `unit` one character takes. Above U+FFFF, UTF-16 needs a surrogate pair.
std::uint32_t width_in(const column_unit_t unit, const decoded_t decoded) {
    switch (unit) {
    case column_unit_t::UTF8:
        return decoded.length;
    case column_unit_t::UTF16:
        return decoded.code_point > 0xFFFFU ? 2U : 1U;
    case column_unit_t::CODEPOINT:
        return 1;
    }

    std::unreachable();
}

} // namespace

source_file_t::source_file_t(const file_id_t id, std::string path, std::string text,
                             std::optional<line_origin_t> origin)
    : m_id(id), m_path(std::move(path)), m_text(std::move(text)), m_origin(std::move(origin)) {
    if (m_text.starts_with(utf8_bom)) {
        m_text.erase(0, utf8_bom.size());
    }
    assert(m_text.size() <= max_source_size);

    m_line_starts.push_back(0);
    const auto size = static_cast<std::uint32_t>(m_text.size());
    for (std::uint32_t i = 0; i < size; ++i) {
        // A line ends at "\n", "\r\n", or a lone "\r". Stepping onto the "\n" of a "\r\n"
        // makes it start one line, not two.
        if (m_text[i] == '\r' && i + 1 < size && m_text[i + 1] == '\n') {
            ++i;
        }

        if (m_text[i] == '\n' || m_text[i] == '\r') {
            m_line_starts.push_back(i + 1);
        }
    }
}

std::string_view source_file_t::slice(const span_t span) const {
    assert(span.file == m_id);
    assert(span.end <= m_text.size());
    return std::string_view(m_text).substr(span.begin, span.length());
}

std::string_view source_file_t::line_text(const std::uint32_t line) const {
    assert(line < line_count());
    const auto size = static_cast<std::uint32_t>(m_text.size());
    const std::uint32_t begin = m_line_starts[line];
    const std::uint32_t end = line + 1 < line_count() ? m_line_starts[line + 1] : size;

    std::string_view content = std::string_view(m_text).substr(begin, end - begin);
    // Drop the line break, whichever of the three it is.
    if (content.ends_with('\n')) {
        content.remove_suffix(1);
    }
    if (content.ends_with('\r')) {
        content.remove_suffix(1);
    }

    return content;
}

location_t source_file_t::location_of(const std::uint32_t offset, const column_unit_t unit) const {
    assert(offset <= m_text.size());

    // The offset is on the last line that starts at or before it.
    const auto next = std::ranges::upper_bound(m_line_starts, offset);
    const auto line = static_cast<std::uint32_t>(next - m_line_starts.begin() - 1);
    const std::uint32_t begin = m_line_starts[line];
    const std::string_view prefix = std::string_view(m_text).substr(begin, offset - begin);

    // Each byte of invalid UTF-8 decodes as its own U+FFFD, so it counts as one column.
    std::uint32_t column = 0;
    for (std::size_t pos = 0; pos < prefix.size();) {
        const decoded_t decoded = decode_utf8(prefix, pos);
        pos += decoded.length;
        column += width_in(unit, decoded);
    }

    if (m_origin && line < m_origin->lines.size()) {
        const location_t from = m_origin->lines[line];
        const bool made = line < m_origin->made.size() && m_origin->made[line];
        return { .line = from.line, .column = made ? from.column : from.column + column };
    }

    return { .line = line, .column = column };
}

std::uint32_t source_file_t::line_of(const std::uint32_t offset) const {
    assert(offset <= m_text.size());
    const auto next = std::ranges::upper_bound(m_line_starts, offset);
    return static_cast<std::uint32_t>(next - m_line_starts.begin() - 1);
}

std::uint32_t source_file_t::offset_of(const location_t location, const column_unit_t unit) const {
    if (location.line >= line_count()) {
        return static_cast<std::uint32_t>(m_text.size());
    }

    const std::string_view line = line_text(location.line);
    std::uint32_t column = 0;
    std::uint32_t pos = 0;

    // A column past the end stops before the line break. One inside a character, like the
    // second half of a surrogate pair, moves to the character's end.
    while (pos < line.size() && column < location.column) {
        const decoded_t decoded = decode_utf8(line, pos);
        pos += decoded.length;
        column += width_in(unit, decoded);
    }

    return m_line_starts[location.line] + pos;
}

file_id_t source_manager_t::add(std::string path, std::string text,
                                std::optional<line_origin_t> origin) {
    assert(!origin || std::to_underlying(origin->file) < m_files.size());
    const auto id = static_cast<file_id_t>(m_files.size());
    m_files.push_back(
        std::make_unique<source_file_t>(id, std::move(path), std::move(text), std::move(origin)));
    return id;
}

span_t source_manager_t::shown(const span_t span) const {
    const source_file_t& file = get(span.file);
    const std::optional<line_origin_t>& origin = file.origin();
    if (!origin) {
        return span;
    }

    // A column in bytes, which the ASCII before each line's origin column keeps.
    const source_file_t& shown = get(origin->file);
    const auto at = [&](const std::uint32_t offset) {
        return shown.offset_of(file.location_of(offset, column_unit_t::UTF8), column_unit_t::UTF8);
    };
    const std::uint32_t begin = at(span.begin);
    return { .file = origin->file, .begin = begin, .end = std::max(begin, at(span.end)) };
}

std::expected<file_id_t, std::string> source_manager_t::load(const std::filesystem::path& path) {
    std::string name = to_utf8(path);
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        return std::unexpected(std::format("cannot read '{}': {}", name, error.message()));
    }

    if (size > max_source_size) {
        return std::unexpected(std::format("'{}' is larger than 4 GiB", name));
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(std::format("cannot open '{}'", name));
    }

    std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    return add(std::move(name), std::move(text));
}

const source_file_t& source_manager_t::get(const file_id_t id) const {
    const auto index = std::to_underlying(id);
    assert(index < m_files.size());
    return *m_files[index];
}

} // namespace fb
