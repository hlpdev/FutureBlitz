#include "fb/diagnostic_render.hpp"

#include "fb/utf8.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {
namespace {

std::string_view severity_name(const severity_t severity) {
    switch (severity) {
    case severity_t::ERROR:
        return "error";
    case severity_t::WARNING:
        return "warning";
    case severity_t::NOTE:
        return "note";
    }

    std::unreachable();
}

tone_t severity_tone(const severity_t severity) {
    switch (severity) {
    case severity_t::ERROR:
        return tone_t::ERROR;
    case severity_t::WARNING:
        return tone_t::WARNING;
    case severity_t::NOTE:
        return tone_t::NOTE;
    }

    std::unreachable();
}

/// "path:line:col: severity[code]: message", 1-based, with the column counted in `columns`. The
/// path and the message can hold source text, so their control characters are escaped, as a
/// snippet's are.
std::string header(const source_manager_t& sources, const span_t span, const severity_t severity,
                   const std::string_view code, const std::string_view message,
                   const style_t& style, const column_unit_t columns = column_unit_t::CODEPOINT) {
    const source_file_t& file = sources.get(span.file);
    const location_t location = file.location_of(span.begin, columns);

    std::string kind(severity_name(severity));
    if (!code.empty()) {
        kind += std::format("[{}]", code);
    }

    return std::format("{}:{}:{}: {}: {}\n", escape_controls(file.path()), location.line + 1,
                       location.column + 1, style.paint(kind, severity_tone(severity)),
                       style.paint(escape_controls(message), tone_t::BOLD));
}

/// One snippet row, "12 | text", with the number right-aligned to `width`.
std::string row(const std::string_view number, const std::string_view text, const std::size_t width,
                const style_t& style) {
    std::string out = style.paint(std::format("{:>{}} |", number, width), tone_t::GUTTER);
    if (!text.empty()) {
        out += ' ';
        out += text;
    }

    out += '\n';
    return out;
}

/// One character of a source line as a snippet shows it, `width` columns wide, at `offset` in
/// the file: a tab as itself, a control character escaped, like "\x1B", so it can't change the
/// terminal, a byte that isn't UTF-8 as U+FFFD, and anything else as itself.
struct cell_t {
    std::string text;
    std::size_t width = 0;
    std::uint32_t offset = 0;
};

std::vector<cell_t> cells_of(const std::string_view line, const std::uint32_t line_begin) {
    std::vector<cell_t> cells;
    for (std::size_t at = 0; at < line.size();) {
        const decoded_t decoded = decode_utf8(line, at);
        const char32_t c = decoded.code_point;
        cell_t cell{ .offset = line_begin + static_cast<std::uint32_t>(at) };
        if (c == U'\t') {
            cell.text = "\t";
            cell.width = 1;
        } else if (!decoded.valid()) {
            append_utf8(cell.text, U'�');
            cell.width = 1;
        } else if (is_control(c)) {
            cell.text = std::format("\\x{:02X}", static_cast<std::uint32_t>(c));
            cell.width = cell.text.size();
        } else {
            cell.text = line.substr(at, decoded.length);
            cell.width = static_cast<std::size_t>(display_width(c));
        }

        cells.push_back(std::move(cell));
        at += decoded.length;
    }

    return cells;
}

/// The span's first line with `marker`s under the span, by the columns the terminal shows: a
/// wide character takes two, and a combining mark none. A span over several lines is underlined
/// to the end of its first line. A line wider than 120 columns shows a part of it around the
/// span, with "..." for what's left out. A span in a file made from another's lines is shown in
/// that one, where its text is.
std::string snippet(const source_manager_t& sources, const span_t written, const char marker,
                    const tone_t marker_tone, const std::size_t width, const style_t& style) {
    const span_t span = sources.shown(written);
    constexpr std::size_t widest = 120;
    constexpr std::size_t context = 40;
    const source_file_t& file = sources.get(span.file);
    const location_t start = file.location_of(span.begin, column_unit_t::CODEPOINT);
    const std::string_view line = file.line_text(start.line);
    const std::uint32_t line_begin =
        file.offset_of({ .line = start.line, .column = 0 }, column_unit_t::UTF8);
    const auto line_end = static_cast<std::uint32_t>(line_begin + line.size());
    const std::vector<cell_t> cells = cells_of(line, line_begin);
    const auto index_of = [&](const std::uint32_t offset) {
        return static_cast<std::size_t>(
            std::ranges::find_if(cells, [&](const cell_t& cell) { return cell.offset >= offset; }) -
            cells.begin());
    };

    // At least one marker, so an empty span (like the end of the file) still shows where it is.
    const std::size_t first_marked = index_of(span.begin);
    const std::size_t end_marked = index_of(std::max(span.begin, std::min(span.end, line_end)));
    const auto columns = [&](const std::size_t from, const std::size_t to) {
        std::size_t total = 0;
        for (std::size_t i = from; i < to && i < cells.size(); ++i) {
            total += cells[i].width;
        }

        return total;
    };

    // The cells shown: all of them, or a window that starts some context before the span.
    std::size_t first = 0;
    std::size_t last = cells.size();
    if (columns(0, cells.size()) > widest) {
        first = first_marked;
        while (first > 0 && columns(first - 1, first_marked) <= context) {
            --first;
        }

        last = first;
        while (last < cells.size() && columns(first, last + 1) <= widest - 6) {
            ++last;
        }

        last = std::max(last, std::min(first_marked + 1, cells.size()));
    }

    const std::string_view cut = "...";
    std::string text(first > 0 ? cut : "");
    for (std::size_t i = first; i < last; ++i) {
        text += cells[i].text;
    }

    text += last < cells.size() ? cut : "";

    // Spaces up to the span, as wide as what they're under. Tabs are copied so the markers line
    // up whatever the terminal's tab width.
    std::string padding(first > 0 ? cut.size() : 0, ' ');
    for (std::size_t i = first; i < first_marked && i < last; ++i) {
        padding += cells[i].text == "\t" ? std::string("\t") : std::string(cells[i].width, ' ');
    }

    const std::size_t count =
        std::max<std::size_t>(1, columns(first_marked, std::min(end_marked, last)));
    std::string out = row("", "", width, style);
    out += row(std::to_string(start.line + 1), text, width, style);
    out += row("", padding + style.paint(std::string(count, marker), marker_tone), width, style);
    return out;
}

/// Digits in the largest line number the diagnostic shows, so every gutter lines up.
std::size_t gutter_width(const diagnostic_t& diagnostic, const source_manager_t& sources) {
    const auto line_of = [&](const span_t span) {
        return sources.get(span.file).location_of(span.begin, column_unit_t::UTF8).line;
    };

    std::uint32_t last = line_of(diagnostic.span);
    for (const auto& [span, message] : diagnostic.labels) {
        last = std::max(last, line_of(span));
    }

    return std::to_string(last + 1).size();
}

} // namespace

std::string render_short(const diagnostic_t& diagnostic, const source_manager_t& sources) {
    return header(sources, diagnostic.span, diagnostic.severity, diagnostic.code,
                  diagnostic.message, {});
}

std::string render_pretty(const diagnostic_t& diagnostic, const source_manager_t& sources,
                          const render_options_t options) {
    const std::size_t width = gutter_width(diagnostic, sources);
    const style_t style{ .color = options.color };

    std::string out = header(sources, diagnostic.span, diagnostic.severity, diagnostic.code,
                             diagnostic.message, style, options.columns);
    out += snippet(sources, diagnostic.span, '^', severity_tone(diagnostic.severity), width, style);

    for (const label_t& label : diagnostic.labels) {
        out += header(sources, label.span, severity_t::NOTE, "", label.message, style,
                      options.columns);
        out += snippet(sources, label.span, '-', tone_t::GUTTER, width, style);
    }

    for (const std::string& note : diagnostic.notes) {
        out += style.paint(std::format("{:>{}} =", "", width), tone_t::GUTTER);
        out += std::format(" {}: {}\n", style.paint("note", tone_t::BOLD), escape_controls(note));
    }

    // What an editor's quick fixes would do.
    for (const suggestion_t& suggestion : diagnostic.suggestions) {
        out += style.paint(std::format("{:>{}} =", "", width), tone_t::GUTTER);
        out += std::format(" {}: {}\n", style.paint("help", tone_t::BOLD),
                           escape_controls(suggestion.title));
    }

    return out;
}

} // namespace fb
