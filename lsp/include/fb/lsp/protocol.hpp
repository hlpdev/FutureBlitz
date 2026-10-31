#pragma once

#include "fb/source.hpp"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace fb::lsp {

using json_t = nlohmann::json;

/// The body of the next JSON-RPC message on `in`, after its "Content-Length" header. nullopt at
/// the end of the stream, or after a header without a length, which leaves nothing to read.
[[nodiscard]] std::optional<std::string> read_message(std::istream& in);

/// Writes `body` as one message, with its header, and flushes it.
void write_message(std::ostream& out, std::string_view body);

/// A file's "file://" URI, as an editor names it, with what a URI can't hold percent-encoded.
[[nodiscard]] std::string uri_of(const std::filesystem::path& path);

/// The file a "file://" URI names. nullopt for any other scheme.
[[nodiscard]] std::optional<std::filesystem::path> path_of(std::string_view uri);

/// What tells two files apart: an absolute path, with ".." and links worked out where they're
/// there, and on Windows and macOS, whose disks ignore letter case, in lower case.
[[nodiscard]] std::string file_key(const std::filesystem::path& path);

/// Forgets the keys file_key() worked out, which it keeps, as the disk may have changed.
void forget_file_keys();

/// What the client counts a position's character in: UTF-16 code units unless it said
/// otherwise.
struct positions_t {
    column_unit_t unit = column_unit_t::UTF16;

    /// An LSP Position, {"line", "character"}, of `offset` in `file`.
    [[nodiscard]] json_t position(const source_file_t& file, std::uint32_t offset) const;

    /// An LSP Range of `span`, which is in `file`.
    [[nodiscard]] json_t range(const source_file_t& file, span_t span) const;

    /// The offset in `file` of an LSP Position. A position past a line's end is at its end.
    [[nodiscard]] std::uint32_t offset(const source_file_t& file, const json_t& position) const;

    /// How many units `text` takes, as an LSP length or a column counts it.
    [[nodiscard]] std::uint32_t length(std::string_view text) const;
};

/// JSON-RPC's and the LSP's error codes.
enum class error_code_t : std::int32_t {
    PARSE_ERROR = -32700,
    INVALID_REQUEST = -32600,
    METHOD_NOT_FOUND = -32601,
    INVALID_PARAMS = -32602,
    INTERNAL_ERROR = -32603,
    SERVER_NOT_INITIALIZED = -32002,
    REQUEST_FAILED = -32803,
};

} // namespace fb::lsp
