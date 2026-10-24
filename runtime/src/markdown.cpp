// std.text.markdown over md4c: CommonMark, with GitHub's tables, task lists, strikethrough, and
// autolinks as std asks, to HTML through md4c-html, and to the events std builds a tree of.

#include "fb/markdown.hpp"

#include "fb/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <md4c-html.h>
#include <md4c.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <entity.h>
}

namespace {

/// std's MarkdownKind: md4c's blocks, then its spans, then the kinds of text.
enum class kind_t : std::uint8_t {
    CODE_BLOCK = MD_BLOCK_CODE,
    HTML_BLOCK = MD_BLOCK_HTML,
    FIRST_SPAN = MD_BLOCK_TD + 1,
    CODE = FIRST_SPAN + MD_SPAN_CODE,
    TEXT = FIRST_SPAN + MD_SPAN_DEL + 1,
    SOFT_BREAK = TEXT + 1,
    HARD_BREAK = TEXT + 2,
    HTML = TEXT + 3,
};

enum class step_t : std::uint8_t {
    ENTER,
    LEAVE,
    TEXT,
};

struct event_t {
    step_t step = step_t::ENTER;
    std::int32_t kind = 0;
    std::int64_t number = 0;
    bool flag = false;
    /// The text, the URL or source, the title, the info string, and the language.
    std::array<std::string, 5> texts;
};

/// What fb_markdown_parse() reads: the events so far, and the blocks and spans entered and not
/// yet left, innermost last.
struct parse_t {
    std::vector<event_t> events;
    std::vector<std::size_t> open;
};

constexpr std::string_view replacement = "\xEF\xBF\xBD";

/// The code point as UTF-8, or U+FFFD for 0, a surrogate, or one past U+10FFFF, as CommonMark has
/// a numeric entity of them.
void append_code_point(std::string& out, const unsigned code_point) {
    if (code_point == 0 || code_point > 0x10FFFF ||
        (code_point >= 0xD800 && code_point <= 0xDFFF)) {
        out += replacement;
        return;
    }

    if (code_point <= 0x7F) {
        out += static_cast<char>(code_point);
    } else if (code_point <= 0x7FF) {
        out += static_cast<char>(0xC0U | (code_point >> 6U));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    } else if (code_point <= 0xFFFF) {
        out += static_cast<char>(0xE0U | (code_point >> 12U));
        out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (code_point >> 18U));
        out += static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    }
}

/// A decimal or hex digit's value.
unsigned digit_value(const char digit) {
    if (digit >= '0' && digit <= '9') {
        return static_cast<unsigned>(digit - '0');
    }

    return (static_cast<unsigned>(digit) | 0x20U) - 'a' + 10;
}

/// An entity md4c found, "&amp;" or "&#35;" or "&#x23;", as what it stands for, as md4c-html
/// renders it: one it doesn't know stays as it's written.
void append_entity(std::string& out, const std::string_view entity) {
    if (entity.size() > 3 && entity[1] == '#') {
        const bool hex = entity[2] == 'x' || entity[2] == 'X';
        const unsigned base = hex ? 16 : 10;
        unsigned code_point = 0;
        // CommonMark allows 7 decimal digits or 6 hex ones, which an unsigned holds.
        for (std::size_t i = hex ? 3 : 2; i + 1 < entity.size(); ++i) {
            code_point = (base * code_point) + digit_value(entity[i]);
        }

        append_code_point(out, code_point);
        return;
    }

    if (const ENTITY* found = entity_lookup(entity.data(), entity.size())) {
        append_code_point(out, found->codepoints[0]);
        if (found->codepoints[1] != 0) {
            append_code_point(out, found->codepoints[1]);
        }

        return;
    }

    out += entity;
}

/// A link's URL or title, or a code block's info, with its entities decoded.
std::string attribute(const MD_ATTRIBUTE& attribute) {
    std::string out;
    // An indented code block's info and language are all zeros, without even the offsets.
    if (attribute.substr_offsets == nullptr) {
        return out;
    }

    for (std::size_t i = 0; attribute.substr_offsets[i] < attribute.size; ++i) {
        const MD_OFFSET offset = attribute.substr_offsets[i];
        const std::string_view part(attribute.text + offset,
                                    attribute.substr_offsets[i + 1] - offset);
        if (attribute.substr_types[i] == MD_TEXT_ENTITY) {
            append_entity(out, part);
        } else {
            out += part;
        }
    }

    return out;
}

/// std's MarkdownExtensions as md4c's flags.
unsigned parser_flags(const std::int32_t extensions) {
    const auto bits = static_cast<std::uint32_t>(extensions);
    unsigned flags = 0;
    if ((bits & 1U) != 0) {
        flags |= MD_FLAG_TABLES;
    }

    if ((bits & 2U) != 0) {
        flags |= MD_FLAG_TASKLISTS;
    }

    if ((bits & 4U) != 0) {
        flags |= MD_FLAG_STRIKETHROUGH;
    }

    if ((bits & 8U) != 0) {
        flags |= MD_FLAG_PERMISSIVEAUTOLINKS;
    }

    return flags;
}

parse_t& state_of(void* userdata) {
    return *static_cast<parse_t*>(userdata);
}

void enter(parse_t& state, event_t event) {
    state.open.push_back(state.events.size());
    state.events.push_back(std::move(event));
}

int leave(parse_t& state, const std::int32_t kind) {
    state.open.pop_back();
    state.events.push_back({ .step = step_t::LEAVE, .kind = kind });
    return 0;
}

int enter_block(const MD_BLOCKTYPE type, void* detail, void* userdata) {
    event_t event{ .kind = type };
    switch (type) {
    case MD_BLOCK_UL:
        event.flag = static_cast<MD_BLOCK_UL_DETAIL*>(detail)->is_tight != 0;
        break;
    case MD_BLOCK_OL: {
        const auto* list = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
        event.number = list->start;
        event.flag = list->is_tight != 0;
        break;
    }
    case MD_BLOCK_LI: {
        const auto* item = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
        if (item->is_task != 0) {
            event.number = item->task_mark == ' ' ? 1 : 2;
        }

        break;
    }
    case MD_BLOCK_H:
        event.number = static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
        break;
    case MD_BLOCK_CODE: {
        const auto* code = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        event.texts[3] = attribute(code->info);
        event.texts[4] = attribute(code->lang);
        break;
    }
    case MD_BLOCK_TH:
    case MD_BLOCK_TD:
        event.number = static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align;
        break;
    default:
        break;
    }

    enter(state_of(userdata), std::move(event));
    return 0;
}

int leave_block(const MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    return leave(state_of(userdata), type);
}

int enter_span(const MD_SPANTYPE type, void* detail, void* userdata) {
    event_t event{ .kind = static_cast<std::int32_t>(kind_t::FIRST_SPAN) + type };
    if (type == MD_SPAN_A) {
        const auto* link = static_cast<MD_SPAN_A_DETAIL*>(detail);
        event.texts[1] = attribute(link->href);
        event.texts[2] = attribute(link->title);
        event.flag = link->is_autolink != 0;
    } else if (type == MD_SPAN_IMG) {
        const auto* image = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        event.texts[1] = attribute(image->src);
        event.texts[2] = attribute(image->title);
    }

    enter(state_of(userdata), std::move(event));
    return 0;
}

int leave_span(const MD_SPANTYPE type, void* /*detail*/, void* userdata) {
    return leave(state_of(userdata), static_cast<std::int32_t>(kind_t::FIRST_SPAN) + type);
}

/// Whether a table's cell is among the blocks open.
bool in_cell(const parse_t& state) {
    return std::ranges::any_of(state.open, [&state](const std::size_t at) {
        const std::int32_t kind = state.events[at].kind;
        return kind == MD_BLOCK_TH || kind == MD_BLOCK_TD;
    });
}

/// A code span's text in a table cell, with each "\|" as a '|', as GitHub's tables have it, where
/// md4c keeps the backslash.
void append_cell_code(std::string& out, std::string_view part) {
    for (std::size_t at = part.find("\\|"); at != std::string_view::npos; at = part.find("\\|")) {
        out += part.substr(0, at);
        out += '|';
        part.remove_prefix(at + 2);
    }

    out += part;
}

/// Whether raw HTML's text goes on the Html before it, a tag or comment that md4c gives a line at
/// a time: its blanks and line break at a line's end, then the next line's part.
bool continues_html(const parse_t& state, const std::string_view part) {
    if (state.events.empty() || state.events.back().step != step_t::TEXT ||
        state.events.back().kind != static_cast<std::int32_t>(kind_t::HTML)) {
        return false;
    }

    const std::string& before = state.events.back().texts[0];
    return (!before.empty() && before.back() == '\n') ||
           part.find_first_not_of(" \t\n") == std::string_view::npos;
}

int text(const MD_TEXTTYPE type, const MD_CHAR* text, const MD_SIZE size, void* userdata) {
    parse_t& state = state_of(userdata);
    const std::string_view part(text, size);
    // A code block's, a code span's, or an HTML block's text is its own.
    if (!state.open.empty()) {
        event_t& inside = state.events[state.open.back()];
        if (inside.kind == static_cast<std::int32_t>(kind_t::CODE) && in_cell(state)) {
            append_cell_code(inside.texts[0], part);
            return 0;
        }

        if (inside.kind == static_cast<std::int32_t>(kind_t::CODE_BLOCK) ||
            inside.kind == static_cast<std::int32_t>(kind_t::HTML_BLOCK) ||
            inside.kind == static_cast<std::int32_t>(kind_t::CODE)) {
            inside.texts[0] += part;
            return 0;
        }
    }

    if (type == MD_TEXT_HTML && continues_html(state, part)) {
        state.events.back().texts[0] += part;
        return 0;
    }

    kind_t kind = kind_t::TEXT;
    if (type == MD_TEXT_BR) {
        kind = kind_t::HARD_BREAK;
    } else if (type == MD_TEXT_SOFTBR) {
        kind = kind_t::SOFT_BREAK;
    } else if (type == MD_TEXT_HTML) {
        kind = kind_t::HTML;
    }

    // Text next to text is one.
    if (kind != kind_t::TEXT || state.events.empty() || state.events.back().step != step_t::TEXT ||
        state.events.back().kind != static_cast<std::int32_t>(kind_t::TEXT)) {
        state.events.push_back({ .step = step_t::TEXT, .kind = static_cast<std::int32_t>(kind) });
    }

    std::string& into = state.events.back().texts[0];
    if (type == MD_TEXT_ENTITY) {
        append_entity(into, part);
    } else if (kind == kind_t::TEXT || kind == kind_t::HTML) {
        into += part;
    }

    return 0;
}

const event_t& event_at(const void* events, const std::int64_t index) {
    return static_cast<const parse_t*>(events)->events[static_cast<std::size_t>(index)];
}

void append_html(const MD_CHAR* text, const MD_SIZE size, void* userdata) {
    static_cast<std::string*>(userdata)->append(text, size);
}

/// The Markdown md4c reads: the String's bytes, with each NUL as U+FFFD, as CommonMark has it,
/// and without a byte order mark at its start, as cmark and md4c's md2html skip it. md4c, which
/// would replace NULs itself, misses a code span's and repeats a code block's.
std::string markdown_of(const fb_string_t* text) {
    std::string_view bytes(reinterpret_cast<const char*>(text + 1),
                           static_cast<std::size_t>(text->length));
    if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.remove_prefix(3);
    }

    std::string markdown;
    markdown.reserve(bytes.size());
    for (const char c : bytes) {
        if (c == '\0') {
            markdown += replacement;
        } else {
            markdown += c;
        }
    }

    // md4c counts its text's bytes in 32 bits.
    if (markdown.size() > std::numeric_limits<MD_SIZE>::max()) {
        fb_panic("std.text.markdown reads less than 4 GiB of Markdown", nullptr);
    }

    return markdown;
}

} // namespace

extern "C" {

fb_string_t* fb_markdown_html(const fb_string_t* text, const std::int32_t extensions) {
    const std::string markdown = markdown_of(text);
    std::string html;
    if (md_html(markdown.data(), static_cast<MD_SIZE>(markdown.size()), append_html, &html,
                parser_flags(extensions), 0) != 0) {
        fb_panic("std.text.markdown ran out of memory", nullptr);
    }

    return fb_string_from_bytes(html.data(), static_cast<std::int64_t>(html.size()));
}

void* fb_markdown_parse(const fb_string_t* text, const std::int32_t extensions) {
    auto* state = new parse_t;
    const MD_PARSER parser{
        .abi_version = 0,
        .flags = parser_flags(extensions),
        .enter_block = enter_block,
        .leave_block = leave_block,
        .enter_span = enter_span,
        .leave_span = leave_span,
        .text = ::text,
        .debug_log = nullptr,
        .syntax = nullptr,
    };
    const std::string markdown = markdown_of(text);
    if (md_parse(markdown.data(), static_cast<MD_SIZE>(markdown.size()), &parser, state) != 0) {
        fb_panic("std.text.markdown ran out of memory", nullptr);
    }

    return state;
}

void fb_markdown_free(void* events) {
    delete static_cast<parse_t*>(events);
}

std::int64_t fb_markdown_count(const void* events) {
    return static_cast<std::int64_t>(static_cast<const parse_t*>(events)->events.size());
}

std::int32_t fb_markdown_step(const void* events, const std::int64_t index) {
    return static_cast<std::int32_t>(event_at(events, index).step);
}

std::int32_t fb_markdown_kind(const void* events, const std::int64_t index) {
    return event_at(events, index).kind;
}

std::int64_t fb_markdown_number(const void* events, const std::int64_t index) {
    return event_at(events, index).number;
}

std::int32_t fb_markdown_flag(const void* events, const std::int64_t index) {
    return event_at(events, index).flag ? 1 : 0;
}

fb_string_t* fb_markdown_text(const void* events, const std::int64_t index,
                              const std::int32_t which) {
    const std::string& text = event_at(events, index).texts.at(static_cast<std::size_t>(which));
    return fb_string_from_bytes(text.data(), static_cast<std::int64_t>(text.size()));
}

} // extern "C"
