#include "fb/diagnostic_render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

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

/// "path:line:col: severity[code]: message", 1-based, with the column counted in code points.
std::string header(const source_manager_t& sources, const span_t span, const severity_t severity,
                   const std::string_view code, const std::string_view message,
                   const style_t& style) {
    const source_file_t& file = sources.get(span.file);
    const location_t location = file.location_of(span.begin, column_unit_t::CODEPOINT);

    std::string kind(severity_name(severity));
    if (!code.empty()) {
        kind += std::format("[{}]", code);
    }

    return std::format("{}:{}:{}: {}: {}\n", file.path(), location.line + 1, location.column + 1,
                       style.paint(kind, severity_tone(severity)),
                       style.paint(message, tone_t::BOLD));
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

/// The span's first line with `marker`s under the span. A span over several lines is
/// underlined to the end of its first line.
std::string snippet(const source_manager_t& sources, const span_t span, const char marker,
                    const tone_t marker_tone, const std::size_t width, const style_t& style) {
    const source_file_t& file = sources.get(span.file);
    const location_t start = file.location_of(span.begin, column_unit_t::CODEPOINT);
    const std::string_view line = file.line_text(start.line);
    const std::uint32_t line_begin =
        file.offset_of({ .line = start.line, .column = 0 }, column_unit_t::UTF8);
    const auto line_end = static_cast<std::uint32_t>(line_begin + line.size());

    // Spaces up to the span, one per code point. Tabs are copied so the markers line up
    // whatever the terminal's tab width.
    std::string padding;
    for (const char c : line.substr(0, span.begin - line_begin)) {
        if (c == '\t') {
            padding += '\t';
        } else if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) { // skip continuation bytes
            padding += ' ';
        }
    }

    // At least one marker, so an empty span (like the end of the file) still shows where it is.
    const std::uint32_t end = std::max(span.begin, std::min(span.end, line_end));
    const std::uint32_t end_column = file.location_of(end, column_unit_t::CODEPOINT).column;
    const std::size_t count = std::max<std::size_t>(1, end_column - start.column);

    std::string out = row("", "", width, style);
    out += row(std::to_string(start.line + 1), line, width, style);
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
                             diagnostic.message, style);
    out += snippet(sources, diagnostic.span, '^', severity_tone(diagnostic.severity), width, style);

    for (const label_t& label : diagnostic.labels) {
        out += header(sources, label.span, severity_t::NOTE, "", label.message, style);
        out += snippet(sources, label.span, '-', tone_t::GUTTER, width, style);
    }

    for (const std::string& note : diagnostic.notes) {
        out += style.paint(std::format("{:>{}} =", "", width), tone_t::GUTTER);
        out += std::format(" {}: {}\n", style.paint("note", tone_t::BOLD), note);
    }

    return out;
}

} // namespace fb
