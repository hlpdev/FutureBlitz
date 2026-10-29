#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

/// A file's index in its source_manager_t, in the order files were added.
enum class file_id_t : std::uint32_t {};

/// Half-open byte range [begin, end) in one source file.
struct span_t {
    file_id_t file{};
    std::uint32_t begin = 0;
    std::uint32_t end = 0;

    [[nodiscard]] constexpr std::uint32_t length() const {
        return end - begin;
    }

    [[nodiscard]] constexpr bool empty() const {
        return begin == end;
    }

    [[nodiscard]] constexpr bool contains(const std::uint32_t offset) const {
        return offset >= begin && offset < end;
    }

    constexpr bool operator==(const span_t&) const = default;
};

/// The smallest span covering both. They must be in the same file.
[[nodiscard]] constexpr span_t merge(const span_t first, const span_t second) {
    assert(first.file == second.file);
    return {
        .file = first.file,
        .begin = std::min(first.begin, second.begin),
        .end = std::max(first.end, second.end),
    };
}

/// What a column counts.
enum class column_unit_t : std::uint8_t {
    /// Bytes.
    UTF8,
    /// UTF-16 code units, which LSP clients count in by default.
    UTF16,
    /// Code points, which diagnostics show.
    CODEPOINT,
};

/// 0-based line and column. Add 1 when showing them to people.
struct location_t {
    std::uint32_t line = 0;
    std::uint32_t column = 0;

    constexpr bool operator==(const location_t&) const = default;
};

/// For a file made from another's lines, as a doc test is from its doc comment's example: that
/// file, and for each of its own lines, where its first column is in that file. Every line's
/// text before that column there is ASCII, so a column moves by the same in every unit.
struct line_origin_t {
    file_id_t file{};
    std::vector<location_t> lines;
    /// For each line, whether it was made for the file, and has no text of the origin's, so every
    /// column of it shows as its line's place there.
    std::vector<bool> made;
};

/// One source file's path and text, with where each line starts. A UTF-8 BOM is dropped, and
/// lines end at "\n", "\r\n", or a lone "\r". The text must be smaller than 4 GiB.
class source_file_t {
public:
    source_file_t(file_id_t id, std::string path, std::string text,
                  std::optional<line_origin_t> origin = std::nullopt);

    [[nodiscard]] file_id_t id() const {
        return m_id;
    }

    [[nodiscard]] const std::string& path() const {
        return m_path;
    }

    [[nodiscard]] std::string_view text() const {
        return m_text;
    }

    [[nodiscard]] std::uint32_t line_count() const {
        return static_cast<std::uint32_t>(m_line_starts.size());
    }

    /// The text `span` covers. It must be a span in this file.
    [[nodiscard]] std::string_view slice(span_t span) const;

    /// A 0-based line's text, without its line break.
    [[nodiscard]] std::string_view line_text(std::uint32_t line) const;

    /// `offset` may equal text().size() (end of file). In a file with an origin, it's where the
    /// offset's text is in the origin's file, as messages and stack traces show it.
    [[nodiscard]] location_t location_of(std::uint32_t offset, column_unit_t unit) const;

    /// The reverse of location_of(), in the file itself, whatever its origin. A line past the end
    /// gives the end of the file, and a column past the end of its line gives the end of that
    /// line.
    [[nodiscard]] std::uint32_t offset_of(location_t location, column_unit_t unit) const;

    /// The 0-based line `offset` is on in the file itself, whatever its origin.
    [[nodiscard]] std::uint32_t line_of(std::uint32_t offset) const;

    /// Where its lines come from, for a file made from another's.
    [[nodiscard]] const std::optional<line_origin_t>& origin() const {
        return m_origin;
    }

private:
    file_id_t m_id;
    std::string m_path;
    std::string m_text;
    std::vector<std::uint32_t> m_line_starts;
    std::optional<line_origin_t> m_origin;
};

/// Owns the source files of a compilation. Each file lives on the heap, so references and views
/// into it stay valid while the manager grows or moves.
class source_manager_t {
public:
    /// Adds a file whose text is already in memory. `path` is only used to name it. A file made
    /// from another's lines has their `origin`, whose file has to be in this manager.
    [[nodiscard]] file_id_t add(std::string path, std::string text,
                                std::optional<line_origin_t> origin = std::nullopt);
    /// Reads a file from disk. Its path is kept as UTF-8 with forward slashes. Fails when it
    /// can't be read or is 4 GiB or larger.
    [[nodiscard]] std::expected<file_id_t, std::string> load(const std::filesystem::path& path);
    /// `id` must come from this manager.
    [[nodiscard]] const source_file_t& get(file_id_t id) const;

    /// Where a span's text is shown: in a file with an origin, the same text in the origin's
    /// file, and elsewhere the span itself.
    [[nodiscard]] span_t shown(span_t span) const;

    /// How many files it has. Their ids run from 0 up to it.
    [[nodiscard]] std::size_t size() const {
        return m_files.size();
    }

private:
    std::vector<std::unique_ptr<source_file_t>> m_files;
};

} // namespace fb
